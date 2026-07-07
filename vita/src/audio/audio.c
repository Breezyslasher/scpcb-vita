#include "audio.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <stdio.h>

/* Boot log on the card for on-device diagnosis. */
static FILE *logFile;

static void alog(const char *fmt, ...) {
    if (!logFile) {
        logFile = fopen("ux0:data/scpcb-ue/port_log.txt", "w");
        if (!logFile) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logFile, fmt, ap);
    va_end(ap);
    fputc(10, logFile);
    fflush(logFile);
}

/* The MAIN port only accepts 48000 Hz (44100 fails with
 * SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ); per-channel rate
 * conversion handles the 44.1 kHz sources. */
#define OUT_RATE 48000
#define GRAIN 256
#define MAX_SOUNDS 224   /* headroom for the on-demand ambient one-shots */
#define MAX_CHANNELS 12
#define AMBIENCE_CHANNEL MAX_CHANNELS       /* extra looping slot */
#define MUSIC_CHANNEL (MAX_CHANNELS + 1)    /* extra looping slot */
#define LAST_CHANNEL MUSIC_CHANNEL

typedef struct {
    char *path;
    int16_t *volatile pcm; /* interleaved; NULL until decoded */
    uint32_t frames;
    int channels;        /* 1 or 2 */
    int rate;
    int failed;          /* decode failed; don't rehit the disk */
    volatile int wanted; /* queued for the decoder thread */
} Sound;

typedef struct {
    int sound;           /* -1 = free */
    uint64_t posFx;      /* fixed-point source frame position (48.16);
                            32 bits overflowed after ~1.5 s of audio */
    uint32_t stepFx;     /* rate conversion step */
    float volL, volR;
    int loop;
} Channel;

static Sound sounds[MAX_SOUNDS];
static int soundCount;
static Channel channels[LAST_CHANNEL + 1];
static int port = -1;
static SceUID mixThread = -1;
static volatile int running;

/* ---- streamed music ----
 * Music decodes from disk on the mixer thread instead of being held
 * as PCM (a 3-minute track is ~30 MB decoded). The main thread posts
 * requests through a mailbox; the mixer opens/closes the vorbis
 * handle itself, so there is no cross-thread lifetime to manage. */
#define MUSIC_CHUNK 1024
static stb_vorbis *musicV;
static int musicSrcChans, musicSrcRate;
static int musicLoop;
static float musicBaseVol = 1.0f;
static float musicVol = 1.0f;
static int16_t musicChunk[MUSIC_CHUNK * 2];
static int musicChunkLen, musicChunkPos; /* frames */
static int16_t musicPrevL, musicPrevR, musicCurL, musicCurR;
static uint32_t musicFracFx, musicStepFx;
static int musicEmptyReads;

static volatile int musicReq;            /* 0 idle, 1 start, 2 stop */
static char musicReqPath[256];
static float musicReqVol;
static int musicReqLoop;

/* Mixer thread only. Returns 0 at end of stream (after loop wrap). */
static int musicNextFrame(int16_t *l, int16_t *r) {
    while (musicChunkPos >= musicChunkLen) {
        int outCh = musicSrcChans >= 2 ? 2 : 1;
        int n = stb_vorbis_get_samples_short_interleaved(
            musicV, outCh, musicChunk, MUSIC_CHUNK * outCh);
        if (n <= 0) {
            /* Zero-length priming frames occur mid-open; two empty
             * reads mean real EOF. */
            if (++musicEmptyReads >= 2) {
                if (!musicLoop) return 0;
                stb_vorbis_seek_start(musicV);
                musicEmptyReads = 0;
            }
            continue;
        }
        musicEmptyReads = 0;
        if (outCh == 1) { /* expand mono in place, back to front */
            for (int i = n - 1; i >= 0; i--) {
                musicChunk[i * 2] = musicChunk[i];
                musicChunk[i * 2 + 1] = musicChunk[i];
            }
        }
        musicChunkLen = n;
        musicChunkPos = 0;
    }
    *l = musicChunk[musicChunkPos * 2];
    *r = musicChunk[musicChunkPos * 2 + 1];
    musicChunkPos++;
    return 1;
}

/* Mixer thread only: handle start/stop requests, then mix GRAIN
 * output frames of music into acc. */
static void musicService(int32_t *acc) {
    if (musicReq) {
        if (musicV) {
            stb_vorbis_close(musicV);
            musicV = NULL;
        }
        if (musicReq == 1) {
            int err = 0;
            musicV = stb_vorbis_open_filename(musicReqPath, &err, NULL);
            if (musicV) {
                stb_vorbis_info info = stb_vorbis_get_info(musicV);
                musicSrcChans = info.channels;
                musicSrcRate = (int)info.sample_rate;
                musicStepFx = (uint32_t)(((uint64_t)musicSrcRate << 16)
                                         / OUT_RATE);
                musicBaseVol = musicReqVol;
                musicLoop = musicReqLoop;
                musicChunkLen = musicChunkPos = 0;
                musicFracFx = 0;
                musicEmptyReads = 0;
                musicPrevL = musicPrevR = musicCurL = musicCurR = 0;
            } else {
                alog("music OPEN-FAIL %s err=%d", musicReqPath, err);
            }
        }
        musicReq = 0;
    }
    if (!musicV) return;

    float vL = musicBaseVol * musicVol, vR = vL;
    for (int i = 0; i < GRAIN; i++) {
        musicFracFx += musicStepFx;
        while (musicFracFx >= 0x10000u) {
            musicFracFx -= 0x10000u;
            musicPrevL = musicCurL;
            musicPrevR = musicCurR;
            if (!musicNextFrame(&musicCurL, &musicCurR)) {
                stb_vorbis_close(musicV);
                musicV = NULL;
                return;
            }
        }
        int32_t l = musicPrevL
                  + (((musicCurL - musicPrevL) * (int32_t)musicFracFx) >> 16);
        int32_t r = musicPrevR
                  + (((musicCurR - musicPrevR) * (int32_t)musicFracFx) >> 16);
        acc[i * 2] += (int32_t)(l * vL);
        acc[i * 2 + 1] += (int32_t)(r * vR);
    }
}

/* The main thread starts channels; the mixer thread advances and
 * retires them. `sound` is written last when starting (with the rest
 * already set), so a torn read at worst plays one stale grain. */

static int mixerLoop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    /* Ping-pong buffers: the hardware may still read the submitted
     * buffer after sceAudioOutOutput returns; reusing one buffer
     * produces constant static. */
    static int16_t out[2][GRAIN * 2];
    static int32_t acc[GRAIN * 2];
    int flip = 0;

    while (running) {
        memset(acc, 0, sizeof(acc));
        for (int c = 0; c <= LAST_CHANNEL; c++) {
            Channel *ch = &channels[c];
            int si = ch->sound;
            if (si < 0 || si >= soundCount) continue;
            const Sound *s = &sounds[si];
            uint64_t endFx = (uint64_t)s->frames << 16;
            for (int i = 0; i < GRAIN; i++) {
                if (ch->posFx >= endFx) {
                    if (ch->loop && endFx > 0) {
                        ch->posFx -= endFx;
                    } else {
                        ch->sound = -1;
                        break;
                    }
                }
                uint32_t f = (uint32_t)(ch->posFx >> 16);
                uint32_t f2 = f + 1 < s->frames ? f + 1 : f;
                int32_t frac = (int32_t)(ch->posFx & 0xFFFF);
                int32_t l, r;
                if (s->channels == 2) {
                    l = s->pcm[f * 2]
                      + (((s->pcm[f2 * 2] - s->pcm[f * 2]) * frac) >> 16);
                    r = s->pcm[f * 2 + 1]
                      + (((s->pcm[f2 * 2 + 1] - s->pcm[f * 2 + 1]) * frac)
                         >> 16);
                } else {
                    l = s->pcm[f]
                      + (((s->pcm[f2] - s->pcm[f]) * frac) >> 16);
                    r = l;
                }
                acc[i * 2] += (int32_t)(l * ch->volL);
                acc[i * 2 + 1] += (int32_t)(r * ch->volR);
                ch->posFx += ch->stepFx;
            }
        }
        musicService(acc);
        for (int i = 0; i < GRAIN * 2; i++) {
            int32_t v = acc[i];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[flip][i] = (int16_t)v;
        }
        sceAudioOutOutput(port, out[flip]); /* blocks until queueable */
        flip ^= 1;
    }
    return 0;
}

static int initState; /* 0 not tried, >0 ok, <0 failed step */

int audioInit(void) {
    alog("audioInit");
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE,
                               SCE_AUDIO_OUT_MODE_STEREO);
    alog("port=%d", port);
    if (port < 0) {
        initState = -1;
        return 0;
    }
    int vols[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
    sceAudioOutSetVolume(port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                         vols);
    for (int c = 0; c <= LAST_CHANNEL; c++) channels[c].sound = -1;
    running = 1;
    mixThread = sceKernelCreateThread("audio_mix", mixerLoop, 64, 0x10000,
                                      0, 0, NULL);
    if (mixThread < 0) {
        initState = -2;
        return 0;
    }
    sceKernelStartThread(mixThread, 0, NULL);
    /* PCM decoder: low priority so it never starves the mixer or the
     * render loop; a lost thread just means decodes stay boot-only. */
    SceUID decThread = sceKernelCreateThread("audio_decode", decoderLoop,
                                             160, 0x40000, 0, 0, NULL);
    if (decThread >= 0) sceKernelStartThread(decThread, 0, NULL);
    initState = 1;
    return 1;
}

int audioStatus(void) {
    return initState;
}

int audioSoundCount(void) {
    return soundCount;
}

static int loadFopenFails, loadDecodeFails;

int audioLoadFopenFails(void) { return loadFopenFails; }
int audioLoadDecodeFails(void) { return loadDecodeFails; }

/* Full PCM decode of one registered sound. Runs on the decoder thread
 * (or on the main thread only during the boot loading screen): a long
 * OGG costs 0.1-3 s on the Vita CPU, which froze gameplay frames when
 * it ran inside audioPlay on first play. */
static int decodeSoundNow(int idx) {
    Sound *s = &sounds[idx];
    if (s->pcm) return 1;
    if (s->failed) return 0;

    /* Decode manually: stb_vorbis_decode_filename() treats a
     * zero-sample frame as end-of-stream, but some encoders (the menu
     * music) emit a zero-length priming frame first, which made the
     * whole file decode to 0 frames. Only two consecutive empty reads
     * mean real EOF. */
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_filename(s->path, &err, NULL);
    if (!v) {
        s->failed = 1;
        loadDecodeFails++;
        alog("decode DECODE-FAIL %s open err=%d", s->path, err);
        return 0;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int chans = info.channels >= 2 ? 2 : 1;
    int rate = (int)info.sample_rate;
    uint32_t cap = stb_vorbis_stream_length_in_samples(v);
    if (cap < 4096) cap = 4096;
    short *pcm = (short *)malloc((size_t)cap * chans * sizeof(short));
    uint32_t total = 0;
    int emptyReads = 0;
    while (pcm) {
        if (total == cap) {
            cap *= 2;
            short *grown2 = (short *)realloc(
                pcm, (size_t)cap * chans * sizeof(short));
            if (!grown2) break;
            pcm = grown2;
        }
        int n = stb_vorbis_get_samples_short_interleaved(
            v, chans, pcm + (size_t)total * chans,
            (int)((cap - total) * chans));
        if (n <= 0) {
            if (++emptyReads >= 2) break;
            continue;
        }
        emptyReads = 0;
        total += (uint32_t)n;
    }
    stb_vorbis_close(v);
    int frames = (int)total;
    if (frames <= 0 || !pcm) {
        free(pcm);
        s->failed = 1;
        loadDecodeFails++;
        alog("decode DECODE-FAIL %s frames=%d", s->path, frames);
        return 0;
    }

    /* Publish everything before pcm: readers gate on pcm != NULL. */
    s->frames = (uint32_t)frames;
    s->channels = chans;
    s->rate = rate;
    __sync_synchronize();
    s->pcm = pcm;
    return 1;
}

/* Main-thread check: never decodes. A miss queues the sound for the
 * decoder thread and reports not-ready; the caller skips or defers. */
static int ensureDecoded(int idx) {
    Sound *s = &sounds[idx];
    if (s->pcm) {
        __sync_synchronize();
        return 1;
    }
    if (s->failed) return 0;
    s->wanted = 1;
    return 0;
}

/* Decoder thread: scans for wanted sounds and decodes them off the
 * render loop. Priority sits below the mixer so decoding never starves
 * audio output. */
static int decoderLoop(SceSize argSize, void *argp) {
    (void)argSize;
    (void)argp;
    while (running) {
        int worked = 0;
        int count = soundCount;
        for (int i = 0; i < count; i++) {
            if (sounds[i].wanted && !sounds[i].pcm && !sounds[i].failed) {
                decodeSoundNow(i);
                sounds[i].wanted = 0;
                worked = 1;
            }
        }
        if (!worked) sceKernelDelayThread(10 * 1000);
    }
    return sceKernelExitDeleteThread(0);
}

void audioPredecodeSmall(unsigned maxFileBytes, unsigned maxTotalPcmBytes) {
    /* Boot-time warmup for the small, hot SFX (steps, doors, buttons):
     * their first play must not wait on the decoder thread. Bounded by
     * file size and by a total PCM budget so this can never recreate
     * the decode-everything heap exhaustion. */
    unsigned total = 0;
    for (int i = 0; i < soundCount && total < maxTotalPcmBytes; i++) {
        if (sounds[i].pcm || sounds[i].failed) continue;
        FILE *f = fopen(sounds[i].path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz <= 0 || (unsigned)sz > maxFileBytes) continue;
        if (decodeSoundNow(i)) {
            total += sounds[i].frames * (unsigned)sounds[i].channels * 2u;
        }
    }
    alog("predecode: %u KB PCM resident", total / 1024);
}

int audioLoad(const char *path) {
    for (int i = 0; i < soundCount; i++) {
        if (strcmp(sounds[i].path, path) == 0) return i;
    }
    if (soundCount >= MAX_SOUNDS) {
        alog("load FULL %s", path);
        return -1;
    }

    /* Registration only confirms the file exists; PCM decodes lazily on
     * first play (see ensureDecoded). */
    FILE *probe = fopen(path, "rb");
    if (!probe) {
        loadFopenFails++;
        alog("load OPEN-FAIL %s", path);
        return -1;
    }
    fclose(probe);

    Sound *s = &sounds[soundCount];
    s->path = strdup(path);
    s->pcm = NULL;
    s->frames = 0;
    s->channels = 1;
    s->rate = OUT_RATE;
    s->failed = 0;
    s->wanted = 0;
    if (!s->path) {
        alog("load OOM %s", path);
        return -1;
    }
    /* Publish the entry before the decoder thread can see the count. */
    __sync_synchronize();
    return soundCount++;
}

static void startChannel(int c, int sound, float volL, float volR, int loop) {
    Channel *ch = &channels[c];
    ch->sound = -1;
    ch->posFx = 0;
    ch->stepFx = (uint32_t)(((uint64_t)sounds[sound].rate << 16) / OUT_RATE);
    ch->volL = volL;
    ch->volR = volR;
    ch->loop = loop;
    ch->sound = sound;
}

static float sfxVol = 1.0f;

void audioSetSfxVolume(float vol) {
    sfxVol = vol;
}

void audioSetMusicVolume(float vol) {
    musicVol = vol; /* mixer applies it per grain */
}

void audioPlay(int sound, float vol, float pan) {
    if (sound < 0 || sound >= soundCount) return;
    if (!ensureDecoded(sound)) return;
    vol *= sfxVol;
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    float volL = vol * (pan <= 0 ? 1.0f : 1.0f - pan);
    float volR = vol * (pan >= 0 ? 1.0f : 1.0f + pan);
    for (int c = 0; c < MAX_CHANNELS; c++) {
        if (channels[c].sound < 0) {
            startChannel(c, sound, volL, volR, 0);
            return;
        }
    }
}

void audioPlay3D(int sound, const float pos[3], const float listener[3],
                 float yaw, float range) {
    float dx = pos[0] - listener[0];
    float dz = pos[2] - listener[2];
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist >= range) return;
    float vol = 1.0f - dist / range;
    /* Pan from the angle between facing and the source. */
    float fx = sinf(yaw), fz = -cosf(yaw);
    float rx = -fz, rz = fx; /* right vector */
    float pan = dist > 1.0f ? (dx * rx + dz * rz) / dist : 0.0f;
    audioPlay(sound, vol, pan * 0.7f);
}

/* Ambience whose PCM is still decoding: remembered here and started by
 * audioService once the decoder thread finishes, so entering a room
 * with a new emitter never stalls a frame (it just fades in late). */
static volatile int ambPending = -1;
static float ambPendingVol;

void audioLoopAmbience(int sound, float vol) {
    ambPending = -1;
    if (sound < 0 || sound >= soundCount) {
        channels[AMBIENCE_CHANNEL].sound = -1;
        return;
    }
    if (!ensureDecoded(sound)) {
        channels[AMBIENCE_CHANNEL].sound = -1; /* silent while decoding */
        if (!sounds[sound].failed) {
            ambPending = sound;
            ambPendingVol = vol;
        }
        return;
    }
    vol *= sfxVol;
    startChannel(AMBIENCE_CHANNEL, sound, vol, vol, 1);
}

void audioService(void) {
    int p = ambPending;
    if (p >= 0) {
        if (sounds[p].failed) {
            ambPending = -1;
        } else if (sounds[p].pcm) {
            __sync_synchronize();
            ambPending = -1;
            float vol = ambPendingVol * sfxVol;
            startChannel(AMBIENCE_CHANNEL, p, vol, vol, 1);
        }
    }
}

void audioStreamMusic(const char *path, float vol, int loop) {
    snprintf(musicReqPath, sizeof(musicReqPath), "%s", path);
    musicReqVol = vol;
    musicReqLoop = loop;
    musicReq = 1;
}

void audioStopMusic(void) {
    musicReq = 2;
}

int audioMusicPlaying(void) {
    /* A pending start counts as playing so a just-started clip is not
     * reported finished before the mixer thread opens it; a non-looping
     * stream clears musicV at EOF. */
    return musicReq == 1 || musicV != NULL;
}
