/*
 * The vita2d shipped in the VitaSDK image references SceSharedFb, but the
 * toolchain provides no stub library for that module. Those functions are
 * only called when vita2d runs in shared-framebuffer mode (system
 * applications), which this app never enables, so weak error-returning
 * fallbacks are enough to satisfy the linker. If real stubs appear in a
 * future SDK, they take precedence over these weak definitions.
 */

#include <stdint.h>

#define SHAREDFB_COMPAT_STUB __attribute__((weak))

SHAREDFB_COMPAT_STUB int32_t _sceSharedFbOpen(int index, int sysver) {
    (void)index;
    (void)sysver;
    return -1;
}

SHAREDFB_COMPAT_STUB int32_t sceSharedFbClose(int32_t uid) {
    (void)uid;
    return -1;
}

SHAREDFB_COMPAT_STUB int32_t sceSharedFbGetInfo(int32_t uid, void *info) {
    (void)uid;
    (void)info;
    return -1;
}

SHAREDFB_COMPAT_STUB int32_t sceSharedFbBegin(int32_t uid, void *info) {
    (void)uid;
    (void)info;
    return -1;
}

SHAREDFB_COMPAT_STUB int32_t sceSharedFbEnd(int32_t uid) {
    (void)uid;
    return -1;
}
