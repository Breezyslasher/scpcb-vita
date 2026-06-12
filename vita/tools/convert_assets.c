/*
 * Device data packager for the Vita port.
 *
 * Stages the game's data into an output directory laid out exactly as it
 * is installed on the Vita (ux0:data/scpcb-ue/). World textures are
 * downscaled to a budget cap (the full set is ~1.1 GB of RGBA8 at native
 * size — see vita/PORTING.md); UI art, sounds and geometry are copied
 * verbatim. Filenames and formats are preserved so .rmesh/.b3d texture
 * references keep resolving.
 *
 * Usage:
 *   convert_assets <out-dir> --cap <px> <dir>... [--copy <dir>...]
 *
 * Example (the CI invocation):
 *   convert_assets staging --cap 256 GFX/Map GFX/NPCs \
 *                          --copy Data SFX GFX
 * Directories are processed in order; the first rule that staged a file
 * wins, so listing GFX last copies everything not already downscaled.
 *
 * Build (host):
 *   gcc -O2 -Wall -o convert_assets vita/tools/convert_assets.c \
 *       vita/src/formats/texture.c -lm
 */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../src/formats/stb_image_write.h"

#include "../src/formats/texture.h"

#define JPEG_QUALITY 90

static const char *skipExts[] = { "wmv", "exe", "dll", "cwm" };

static struct {
    unsigned downscaled, copied, skipped;
    unsigned long long bytesIn, bytesOut;
} stats;

static int failures;
static const char *outRoot;

static const char *extOf(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static int extEqNoCase(const char *name, const char *ext) {
    const char *e = extOf(name);
    while (*e && *ext) {
        char a = (char)((*e >= 'A' && *e <= 'Z') ? *e + 32 : *e);
        if (a != *ext) return 0;
        e++;
        ext++;
    }
    return *e == '\0' && *ext == '\0';
}

static int shouldSkip(const char *name) {
    for (size_t i = 0; i < sizeof(skipExts) / sizeof(skipExts[0]); i++) {
        if (extEqNoCase(name, skipExts[i])) return 1;
    }
    return 0;
}

/* mkdir -p for the directory part of an output path. */
static int ensureParentDirs(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return 0;
            *p = '/';
        }
    }
    return 1;
}

static long long fileSize(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static int alreadyStaged(const char *relPath) {
    char out[4096];
    snprintf(out, sizeof(out), "%s/%s", outRoot, relPath);
    struct stat st;
    return stat(out, &st) == 0;
}

static void copyFile(const char *relPath) {
    char out[4096];
    snprintf(out, sizeof(out), "%s/%s", outRoot, relPath);
    if (!ensureParentDirs(out)) goto fail;

    FILE *src = fopen(relPath, "rb");
    if (!src) goto fail;
    FILE *dst = fopen(out, "wb");
    if (!dst) {
        fclose(src);
        goto fail;
    }
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            goto fail;
        }
    }
    int err = ferror(src);
    fclose(src);
    if (fclose(dst) != 0 || err) goto fail;

    stats.copied++;
    long long sz = fileSize(relPath);
    if (sz > 0) {
        stats.bytesIn += (unsigned long long)sz;
        stats.bytesOut += (unsigned long long)sz;
    }
    return;

fail:
    printf("FAIL copy %s\n", relPath);
    failures = 1;
}

/* Downscale-or-copy for an image under a --cap directory. Output keeps
 * the source name and format. */
static void stageImage(const char *relPath, uint32_t cap) {
    char err[256];
    TextureImage *img = textureLoadFile(relPath, 0, err, sizeof(err));
    if (!img) {
        printf("FAIL read %s: %s\n", relPath, err);
        failures = 1;
        return;
    }
    if (img->width <= cap && img->height <= cap) {
        textureFree(img);
        copyFile(relPath);
        return;
    }
    textureFree(img);
    img = textureLoadFile(relPath, cap, err, sizeof(err));
    if (!img) {
        printf("FAIL scale %s: %s\n", relPath, err);
        failures = 1;
        return;
    }

    char out[4096];
    snprintf(out, sizeof(out), "%s/%s", outRoot, relPath);
    if (!ensureParentDirs(out)) {
        textureFree(img);
        printf("FAIL mkdir %s\n", out);
        failures = 1;
        return;
    }

    int ok;
    if (extEqNoCase(relPath, "jpg") || extEqNoCase(relPath, "jpeg")) {
        ok = stbi_write_jpg(out, (int)img->width, (int)img->height, 4,
                            img->pixels, JPEG_QUALITY);
    } else if (extEqNoCase(relPath, "bmp")) {
        ok = stbi_write_bmp(out, (int)img->width, (int)img->height, 4,
                            img->pixels);
    } else {
        /* .png — and anything else lands here as PNG content with its
         * original name; the engine sniffs content, not extensions. */
        ok = stbi_write_png(out, (int)img->width, (int)img->height, 4,
                            img->pixels, (int)img->width * 4);
    }
    textureFree(img);
    if (!ok) {
        printf("FAIL write %s\n", out);
        failures = 1;
        return;
    }

    stats.downscaled++;
    long long in = fileSize(relPath), outSz = fileSize(out);
    if (in > 0) stats.bytesIn += (unsigned long long)in;
    if (outSz > 0) stats.bytesOut += (unsigned long long)outSz;
}

static int isResizableImage(const char *name) {
    return extEqNoCase(name, "png") || extEqNoCase(name, "jpg")
        || extEqNoCase(name, "jpeg") || extEqNoCase(name, "bmp");
}

static void walk(const char *path, uint32_t cap) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("FAIL stat %s\n", path);
        failures = 1;
        return;
    }
    if (S_ISREG(st.st_mode)) {
        if (shouldSkip(path)) {
            stats.skipped++;
            return;
        }
        if (alreadyStaged(path)) return;
        if (cap > 0 && isResizableImage(path)) {
            stageImage(path, cap);
        } else {
            copyFile(path);
        }
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;

    DIR *d = opendir(path);
    if (!d) {
        printf("FAIL open %s\n", path);
        failures = 1;
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        walk(child, cap);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <out-dir> --cap <px> <dir>... [--copy <dir>...]\n",
                argv[0]);
        return 2;
    }
    outRoot = argv[1];

    uint32_t cap = 0;
    int mode = 0; /* 0 = none, 1 = cap, 2 = copy */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--cap") == 0) {
            if (i + 1 >= argc) return 2;
            cap = (uint32_t)atoi(argv[++i]);
            mode = 1;
        } else if (strcmp(argv[i], "--copy") == 0) {
            mode = 2;
        } else if (mode == 1) {
            walk(argv[i], cap);
        } else if (mode == 2) {
            walk(argv[i], 0);
        } else {
            fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
    }

    printf("\nStaged into %s:\n", outRoot);
    printf("  downscaled=%u copied=%u skipped=%u\n",
           stats.downscaled, stats.copied, stats.skipped);
    printf("  input=%.1f MB output=%.1f MB\n",
           stats.bytesIn / (1024.0 * 1024.0),
           stats.bytesOut / (1024.0 * 1024.0));
    return failures ? 1 : 0;
}
