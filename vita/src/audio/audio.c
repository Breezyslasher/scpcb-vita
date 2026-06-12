#include "audio.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define OUT_RATE 44100
#define GRAIN 256
#define MAX_SOUNDS 64
#define MAX_CHANNELS 12
#define AMBIENCE_CHANNEL MAX_CHANNELS /* extra looping slot */

typedef struct {
    char *path;
    int16_t *pcm;        /* interleaved */
    uint32_t frames;
    int channels;        /* 1 or 2 */
    int rate;
} Sound;

typedef struct {
    int sound;           /* -1 = free */
    uint32_t posFx;      /* fixed-point source frame position (16.16) */
    uint32_t stepFx;     /* rate conversion step */
    float volL, volR;
    int loop;
} Channel;

static Sound sounds[MAX_SOUNDS];
static int soundCount;
static Channel channels[MAX_CHANNELS + 1];
static int port = -1;
static SceUID mixThread = -1;
static volatile int running;

/* The main thread starts channels; the mixer thread advances and
 * retires them. `sound` is written last when starting (with the rest
 * already set), so a torn read at worst plays one stale grain. */

static int mixerLoop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    static int16_t out[GRAIN * 2];
    static int32_t acc[GRAIN * 2];

    while (running) {
        memset(acc, 0, sizeof(acc));
        for (int c = 0; c <= MAX_CHANNELS; c++) {
            Channel *ch = &channels[c];
            int si = ch->sound;
            if (si < 0 || si >= soundCount) continue;
            const Sound *s = &sounds[si];
            uint32_t endFx = s->frames << 16;
            for (int i = 0; i < GRAIN; i++) {
                if (ch->posFx >= endFx) {
                    if (ch->loop && endFx > 0) {
                        ch->posFx -= endFx;
                    } else {
                        ch->sound = -1;
                        break;
                    }
                }
                uint32_t f = ch->posFx >> 16;
                int16_t l, r;
                if (s->channels == 2) {
                    l = s->pcm[f * 2];
                    r = s->pcm[f * 2 + 1];
                } else {
                    l = r = s->pcm[f];
                }
                acc[i * 2] += (int32_t)(l * ch->volL);
                acc[i * 2 + 1] += (int32_t)(r * ch->volR);
                ch->posFx += ch->stepFx;
            }
        }
        for (int i = 0; i < GRAIN * 2; i++) {
            int32_t v = acc[i];
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out[i] = (int16_t)v;
        }
        sceAudioOutOutput(port, out); /* blocks until consumed */
    }
    return 0;
}

static int initState; /* 0 not tried, >0 ok, <0 failed step */

int audioInit(void) {
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE,
                               SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        initState = -1;
        return 0;
    }
    int vols[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
    sceAudioOutSetVolume(port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                         vols);
    for (int c = 0; c <= MAX_CHANNELS; c++) channels[c].sound = -1;
    running = 1;
    mixThread = sceKernelCreateThread("audio_mix", mixerLoop, 64, 0x10000,
                                      0, 0, NULL);
    if (mixThread < 0) {
        initState = -2;
        return 0;
    }
    sceKernelStartThread(mixThread, 0, NULL);
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

int audioLoad(const char *path) {
    for (int i = 0; i < soundCount; i++) {
        if (strcmp(sounds[i].path, path) == 0) return i;
    }
    if (soundCount >= MAX_SOUNDS) return -1;

    FILE *probe = fopen(path, "rb");
    if (!probe) {
        loadFopenFails++;
        return -1;
    }
    fclose(probe);

    int chans = 0, rate = 0;
    short *pcm = NULL;
    int frames = stb_vorbis_decode_filename(path, &chans, &rate, &pcm);
    if (frames <= 0 || !pcm) {
        loadDecodeFails++;
        return -1;
    }

    Sound *s = &sounds[soundCount];
    s->path = strdup(path);
    s->pcm = pcm;
    s->frames = (uint32_t)frames;
    s->channels = chans >= 2 ? 2 : 1;
    s->rate = rate;
    if (!s->path) return -1;
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

void audioPlay(int sound, float vol, float pan) {
    if (sound < 0 || sound >= soundCount) return;
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

void audioLoopAmbience(int sound, float vol) {
    if (sound < 0 || sound >= soundCount) {
        channels[AMBIENCE_CHANNEL].sound = -1;
        return;
    }
    startChannel(AMBIENCE_CHANNEL, sound, vol, vol, 1);
}
