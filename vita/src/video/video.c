/*
 * sceAvPlayer -> vitaGL video player. See video.h.
 *
 * The integration follows the verified vitaGL pattern (OpenFMV): a ring
 * of GL textures whose underlying SceGxmTexture handles are re-pointed
 * at each decoded frame with sceGxmTextureInitLinear, using the
 * YVU420P2_CSC1 format so the GPU does YUV->RGB. The decoder's frame
 * buffers are CDRAM allocated through the memoryReplacement callbacks
 * and mapped for GXM. Audio is pumped on a dedicated thread to the BGM
 * port (the game mixer owns the MAIN port).
 */

#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include <vitaGL.h>

#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>

#include "../input.h"

#define VID_W 960
#define VID_H 544
#define FB_ALIGN 0x40000 /* CDRAM allocations round up to 256 KB */
#define VIDEO_FRAME_BUFFERS 5

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* Diagnostics -> ux0:data/scpcb-ue/video_log.txt (its own file so it
 * never races the audio log). Flushed per line so a crash keeps it. */
static void vlog(const char *fmt, ...) {
    static FILE *lf;
    if (!lf) lf = fopen("ux0:data/scpcb-ue/video_log.txt", "w");
    if (!lf) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fputc('\n', lf);
    fflush(lf);
}

/* ---- memoryReplacement callbacks ---- */

static void *mem_alloc(void *arg, uint32_t align, uint32_t size) {
    (void)arg;
    return memalign(align, size);
}

static void mem_free(void *arg, void *ptr) {
    (void)arg;
    free(ptr);
}

/* Decoder frame buffers live in CDRAM so GXM can sample them directly. */
static void *tex_alloc(void *arg, uint32_t align, uint32_t size) {
    (void)arg;
    if (align < FB_ALIGN) align = FB_ALIGN;
    size = ALIGN_UP(size, align);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = align;
    SceUID blk = sceKernelAllocMemBlock("scpcb_video",
                                        SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                        size, &opt);
    if (blk < 0) return NULL;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(blk, &base) < 0) {
        sceKernelFreeMemBlock(blk);
        return NULL;
    }
    sceGxmMapMemory(base, size,
                    SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    return base;
}

static void tex_free(void *arg, void *ptr) {
    (void)arg;
    if (!ptr) return;
    glFinish(); /* never free a buffer the GPU may still be sampling */
    SceUID blk = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    if (blk >= 0) sceKernelFreeMemBlock(blk);
}

/* ---- fileReplacement callbacks ----
 * The built-in AvPlayer loader rejects our ux0: path (AddSource returns
 * SCE_AVPLAYER_ERROR_INVALID_PARAM), so we hand it an sceIo reader on
 * the exact path fopen already proved openable. One file plays at a
 * time, so a single global fd is enough; objectPointer stays NULL. */
static SceUID gVidFd = -1;

static int file_open(void *p, const char *filename) {
    (void)p;
    gVidFd = sceIoOpen(filename, SCE_O_RDONLY, 0);
    return gVidFd < 0 ? -1 : 0;
}

static int file_close(void *p) {
    (void)p;
    int r = gVidFd >= 0 ? sceIoClose(gVidFd) : 0;
    gVidFd = -1;
    return r < 0 ? -1 : 0;
}

static int file_readOffset(void *p, uint8_t *buffer, uint64_t position,
                           uint32_t length) {
    (void)p;
    if (gVidFd < 0) return -1;
    if (sceIoLseek(gVidFd, (SceOff)position, SCE_SEEK_SET) < 0) return -1;
    return sceIoRead(gVidFd, buffer, length);
}

static uint64_t file_size(void *p) {
    (void)p;
    if (gVidFd < 0) return 0;
    SceOff cur = sceIoLseek(gVidFd, 0, SCE_SEEK_CUR);
    SceOff end = sceIoLseek(gVidFd, 0, SCE_SEEK_END);
    sceIoLseek(gVidFd, cur, SCE_SEEK_SET);
    return (uint64_t)end;
}

/* ---- audio pump thread ---- */

static SceAvPlayerHandle gPlayer;
static volatile int gAudioRun;

static int audioThread(SceSize argSize, void *argp) {
    (void)argSize;
    (void)argp;
    int port = -1;
    while (gAudioRun && sceAvPlayerIsActive(gPlayer)) {
        SceAvPlayerFrameInfo frame;
        memset(&frame, 0, sizeof(frame));
        if (sceAvPlayerGetAudioData(gPlayer, &frame)) {
            if (port < 0) {
                int chans = frame.details.audio.channelCount;
                port = sceAudioOutOpenPort(
                    SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024,
                    frame.details.audio.sampleRate,
                    chans == 1 ? SCE_AUDIO_OUT_MODE_MONO
                               : SCE_AUDIO_OUT_MODE_STEREO);
            }
            if (port >= 0) sceAudioOutOutput(port, frame.pData);
        } else {
            sceKernelDelayThread(1000);
        }
    }
    if (port >= 0) {
        sceAudioOutOutput(port, NULL); /* drain */
        sceAudioOutReleasePort(port);
    }
    return sceKernelExitDeleteThread(0);
}

/* ---- texture ring (created once) ---- */

static GLuint gRing[VIDEO_FRAME_BUFFERS];
static SceGxmTexture *gRingGxm[VIDEO_FRAME_BUFFERS];
static int gRingReady;

static void ensureRing(void) {
    if (gRingReady) return;
    glGenTextures(VIDEO_FRAME_BUFFERS, gRing);
    for (int i = 0; i < VIDEO_FRAME_BUFFERS; i++) {
        glBindTexture(GL_TEXTURE_2D, gRing[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        gRingGxm[i] = vglGetGxmTexture(GL_TEXTURE_2D);
        vglFree(vglGetTexDataPointer(GL_TEXTURE_2D)); /* drop dummy backing */
    }
    gRingReady = 1;
}

/* ---- init ---- */

static int gSysmoduleOk = -1; /* -1 untried, 0 failed, 1 loaded */

int videoInit(void) {
    if (gSysmoduleOk < 0) {
        int r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        gSysmoduleOk = r >= 0 ? 1 : 0;
        vlog("videoInit: sceSysmoduleLoadModule(AVPLAYER)=0x%08X -> %s",
             (unsigned)r, gSysmoduleOk ? "ok" : "FAILED");
    }
    return gSysmoduleOk;
}

/* ---- draw one frame, letterboxed ---- */

static void drawFrameQuad(GLuint tex, int vw, int vh) {
    if (vw <= 0 || vh <= 0) return;
    float sw = (float)VID_W, sh = (float)VID_H;
    float va = (float)vw / (float)vh, sa = sw / sh;
    float w, h;
    if (va > sa) { w = sw; h = sw / va; }
    else { h = sh; w = sh * va; }
    float x = (sw - w) * 0.5f, y = (sh - h) * 0.5f;
    GLfloat verts[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    GLfloat uv[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };

    glBindTexture(GL_TEXTURE_2D, tex);
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glEnableClientState(GL_COLOR_ARRAY);
}

static int skipPressed(void) {
    float tx, ty;
    return inputHit(ACTION_INTERACT) || inputHit(ACTION_MENU)
        || inputHit(ACTION_CROUCH) || inputHit(ACTION_INVENTORY)
        || inputHit(ACTION_USE_ITEM) || inputHit(ACTION_SAVE)
        || inputDpadDownHit() || inputTouchTap(&tx, &ty);
}

/* ---- play one file ---- */

int videoPlayFile(const char *path) {
    vlog("---- videoPlayFile: %s", path);
    if (!videoInit()) { vlog("  videoInit failed"); return 0; }

    /* Confirm the file is actually on the device (the #1 failure is the
     * data package not carrying the .mp4 yet). */
    FILE *probe = fopen(path, "rb");
    if (!probe) {
        vlog("  fopen FAILED - file not present on device");
        return 0;
    }
    fseek(probe, 0, SEEK_END);
    long fsz = ftell(probe);
    fclose(probe);
    vlog("  file present, %ld bytes", fsz);

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = mem_alloc;
    init.memoryReplacement.deallocate = mem_free;
    init.memoryReplacement.allocateTexture = tex_alloc;
    init.memoryReplacement.deallocateTexture = tex_free;
    init.fileReplacement.objectPointer = NULL;
    init.fileReplacement.open = file_open;
    init.fileReplacement.close = file_close;
    init.fileReplacement.readOffset = file_readOffset;
    init.fileReplacement.size = file_size;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = VIDEO_FRAME_BUFFERS;
    init.autoStart = 1;

    gPlayer = sceAvPlayerInit(&init);
    vlog("  sceAvPlayerInit -> handle=%d", gPlayer);
    if (gPlayer < 0) return 0;
    int addRc = sceAvPlayerAddSource(gPlayer, path);
    vlog("  sceAvPlayerAddSource -> 0x%08X", (unsigned)addRc);
    if (addRc < 0) {
        sceAvPlayerClose(gPlayer);
        return 0;
    }

    ensureRing();
    vlog("  ring ready, entering play loop");

    /* Audio on its own thread (the video is pumped here). */
    gAudioRun = 1;
    SceUID athread = sceKernelCreateThread("scpcb_video_audio", audioThread,
                                           0x10000100, 0x4000, 0, 0, NULL);
    if (athread >= 0) sceKernelStartThread(athread, 0, NULL);

    /* 2D, textured, no depth/blend for the duration. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glViewport(0, 0, VID_W, VID_H);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0.0f, (float)VID_W, (float)VID_H, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    int idx = 0, haveFrame = 0, lastW = 0, lastH = 0;
    int loops = 0, frames = 0, skipped = 0, timedOut = 0;
    uint64_t startUs = sceKernelGetProcessTimeWide();
    uint64_t nextLogUs = startUs + 1000000ull;
    while (sceAvPlayerIsActive(gPlayer)) {
        loops++;
        inputUpdate();
        if (skipPressed()) { skipped = 1; break; }

        SceAvPlayerFrameInfo frame;
        memset(&frame, 0, sizeof(frame));
        if (sceAvPlayerGetVideoData(gPlayer, &frame)) {
            idx = (idx + 1) % VIDEO_FRAME_BUFFERS;
            sceGxmTextureInitLinear(gRingGxm[idx], frame.pData,
                                    SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                    frame.details.video.width,
                                    frame.details.video.height, 0);
            sceGxmTextureSetMinFilter(gRingGxm[idx],
                                      SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(gRingGxm[idx],
                                      SCE_GXM_TEXTURE_FILTER_LINEAR);
            lastW = (int)frame.details.video.width;
            lastH = (int)frame.details.video.height;
            if (!haveFrame) {
                vlog("  first video frame: %dx%d pData=%p", lastW, lastH,
                     (void *)frame.pData);
            }
            haveFrame = 1;
            frames++;
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (haveFrame) drawFrameQuad(gRing[idx], lastW, lastH);
        vglSwapBuffers(GL_FALSE);

        uint64_t nowUs = sceKernelGetProcessTimeWide();
        if (nowUs >= nextLogUs) {
            vlog("  +%llus loops=%d frames=%d active=%d",
                 (nowUs - startUs) / 1000000ull, loops, frames,
                 (int)sceAvPlayerIsActive(gPlayer));
            nextLogUs += 1000000ull;
        }
        /* Wall-clock safety net against a stalled stream (2 min). */
        if (nowUs - startUs > 120000000ull) { timedOut = 1; break; }
    }
    vlog("  loop end: reason=%s loops=%d frames=%d haveFrame=%d",
         skipped ? "skip" : (timedOut ? "timeout" : "stream-ended"), loops,
         frames, haveFrame);

    /* Restore GL matrices. */
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glEnable(GL_DEPTH_TEST);

    /* Stop audio thread, then tear the player down. */
    gAudioRun = 0;
    if (athread >= 0) {
        sceKernelWaitThreadEnd(athread, NULL, NULL);
    }
    sceAvPlayerStop(gPlayer);
    sceAvPlayerClose(gPlayer);
    gPlayer = 0;
    vlog("  closed, returning played=%d", frames > 0);
    /* Report "played" only if real frames were shown; otherwise let the
     * caller fall back (e.g. a decodable container that yielded nothing). */
    return frames > 0;
}
