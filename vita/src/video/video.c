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

#include <vitaGL.h>

#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "../input.h"

#define VID_W 960
#define VID_H 544
#define FB_ALIGN 0x40000 /* CDRAM allocations round up to 256 KB */
#define VIDEO_FRAME_BUFFERS 5

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

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
        gSysmoduleOk =
            sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) >= 0 ? 1 : 0;
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
    if (!videoInit()) return 0;

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = mem_alloc;
    init.memoryReplacement.deallocate = mem_free;
    init.memoryReplacement.allocateTexture = tex_alloc;
    init.memoryReplacement.deallocateTexture = tex_free;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = VIDEO_FRAME_BUFFERS;
    init.autoStart = 1;

    gPlayer = sceAvPlayerInit(&init);
    if (gPlayer < 0) return 0;
    if (sceAvPlayerAddSource(gPlayer, path) < 0) {
        sceAvPlayerClose(gPlayer);
        return 0;
    }

    ensureRing();

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
    uint64_t startUs = sceKernelGetProcessTimeWide();
    while (sceAvPlayerIsActive(gPlayer)) {
        inputUpdate();
        if (skipPressed()) break;

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
            haveFrame = 1;
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (haveFrame) drawFrameQuad(gRing[idx], lastW, lastH);
        vglSwapBuffers(GL_FALSE);

        /* Wall-clock safety net against a stalled stream (2 min). */
        if (sceKernelGetProcessTimeWide() - startUs > 120000000ull) break;
    }

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
    return 1;
}
