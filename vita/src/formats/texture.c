#include "texture.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

static void setErr(char *err, size_t errLen, const char *msg) {
    if (err && errLen > 0) {
        snprintf(err, errLen, "%s", msg);
    }
}

/* Halve an RGBA8 image in place with a 2x2 box filter. Odd edges keep
 * the last row/column. */
static void halveImage(TextureImage *img) {
    uint32_t nw = img->width > 1 ? img->width / 2 : 1;
    uint32_t nh = img->height > 1 ? img->height / 2 : 1;
    for (uint32_t y = 0; y < nh; y++) {
        for (uint32_t x = 0; x < nw; x++) {
            uint32_t sx0 = x * 2;
            uint32_t sy0 = y * 2;
            uint32_t sx1 = sx0 + 1 < img->width ? sx0 + 1 : sx0;
            uint32_t sy1 = sy0 + 1 < img->height ? sy0 + 1 : sy0;
            for (int c = 0; c < 4; c++) {
                uint32_t sum =
                    img->pixels[(sy0 * img->width + sx0) * 4 + c] +
                    img->pixels[(sy0 * img->width + sx1) * 4 + c] +
                    img->pixels[(sy1 * img->width + sx0) * 4 + c] +
                    img->pixels[(sy1 * img->width + sx1) * 4 + c];
                img->pixels[(y * nw + x) * 4 + c] = (uint8_t)(sum / 4);
            }
        }
    }
    img->width = nw;
    img->height = nh;
}

/* ---- DDS (DXT1/3/5) decoding; the game ships one DXT5 texture ---- */

static void decodeColorBlock(const uint8_t *block, uint8_t out[16][4], int dxt1) {
    uint16_t c0 = (uint16_t)(block[0] | block[1] << 8);
    uint16_t c1 = (uint16_t)(block[2] | block[3] << 8);
    uint8_t colors[4][4];

    colors[0][0] = (uint8_t)(((c0 >> 11) & 0x1F) * 255 / 31);
    colors[0][1] = (uint8_t)(((c0 >> 5) & 0x3F) * 255 / 63);
    colors[0][2] = (uint8_t)((c0 & 0x1F) * 255 / 31);
    colors[0][3] = 255;
    colors[1][0] = (uint8_t)(((c1 >> 11) & 0x1F) * 255 / 31);
    colors[1][1] = (uint8_t)(((c1 >> 5) & 0x3F) * 255 / 63);
    colors[1][2] = (uint8_t)((c1 & 0x1F) * 255 / 31);
    colors[1][3] = 255;

    if (!dxt1 || c0 > c1) {
        for (int c = 0; c < 3; c++) {
            colors[2][c] = (uint8_t)((2 * colors[0][c] + colors[1][c]) / 3);
            colors[3][c] = (uint8_t)((colors[0][c] + 2 * colors[1][c]) / 3);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for (int c = 0; c < 3; c++) {
            colors[2][c] = (uint8_t)((colors[0][c] + colors[1][c]) / 2);
            colors[3][c] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0; /* 1-bit transparency */
    }

    uint32_t bits = (uint32_t)block[4] | (uint32_t)block[5] << 8
                  | (uint32_t)block[6] << 16 | (uint32_t)block[7] << 24;
    for (int i = 0; i < 16; i++) {
        memcpy(out[i], colors[(bits >> (i * 2)) & 3], 4);
    }
}

static void decodeAlphaDxt5(const uint8_t *block, uint8_t alpha[16]) {
    uint8_t a0 = block[0], a1 = block[1];
    uint8_t table[8];
    table[0] = a0;
    table[1] = a1;
    if (a0 > a1) {
        for (int i = 0; i < 6; i++) {
            table[2 + i] = (uint8_t)(((6 - i) * a0 + (i + 1) * a1) / 7);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            table[2 + i] = (uint8_t)(((4 - i) * a0 + (i + 1) * a1) / 5);
        }
        table[6] = 0;
        table[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= (uint64_t)block[2 + i] << (i * 8);
    }
    for (int i = 0; i < 16; i++) {
        alpha[i] = table[(bits >> (i * 3)) & 7];
    }
}

static TextureImage *loadDds(const char *path, char *err, size_t errLen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        setErr(err, errLen, "could not open file");
        return NULL;
    }
    uint8_t header[128];
    if (fread(header, 1, 128, f) != 128 || memcmp(header, "DDS ", 4) != 0) {
        fclose(f);
        setErr(err, errLen, "not a DDS file");
        return NULL;
    }
    uint32_t height = (uint32_t)header[12] | (uint32_t)header[13] << 8
                    | (uint32_t)header[14] << 16 | (uint32_t)header[15] << 24;
    uint32_t width  = (uint32_t)header[16] | (uint32_t)header[17] << 8
                    | (uint32_t)header[18] << 16 | (uint32_t)header[19] << 24;
    const uint8_t *fourcc = header + 84;

    int dxt1 = memcmp(fourcc, "DXT1", 4) == 0;
    int dxt3 = memcmp(fourcc, "DXT3", 4) == 0;
    int dxt5 = memcmp(fourcc, "DXT5", 4) == 0;
    if (!dxt1 && !dxt3 && !dxt5) {
        fclose(f);
        setErr(err, errLen, "unsupported DDS pixel format");
        return NULL;
    }
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        fclose(f);
        setErr(err, errLen, "bad DDS dimensions");
        return NULL;
    }

    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    size_t blockSize = dxt1 ? 8 : 16;
    size_t dataSize = (size_t)bw * bh * blockSize;
    uint8_t *data = (uint8_t *)malloc(dataSize);
    uint8_t *pixels = (uint8_t *)calloc((size_t)width * height, 4);
    if (!data || !pixels || fread(data, 1, dataSize, f) != dataSize) {
        free(data);
        free(pixels);
        fclose(f);
        setErr(err, errLen, "truncated DDS data");
        return NULL;
    }
    fclose(f);

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *block = data + ((size_t)by * bw + bx) * blockSize;
            uint8_t texels[16][4];
            uint8_t alpha[16];
            if (dxt1) {
                decodeColorBlock(block, texels, 1);
            } else if (dxt3) {
                decodeColorBlock(block + 8, texels, 0);
                for (int i = 0; i < 16; i++) {
                    uint8_t nib = (uint8_t)((block[i / 2] >> ((i % 2) * 4)) & 0xF);
                    texels[i][3] = (uint8_t)(nib * 17);
                }
            } else {
                decodeAlphaDxt5(block, alpha);
                decodeColorBlock(block + 8, texels, 0);
                for (int i = 0; i < 16; i++) {
                    texels[i][3] = alpha[i];
                }
            }
            for (int i = 0; i < 16; i++) {
                uint32_t px = bx * 4 + (uint32_t)(i % 4);
                uint32_t py = by * 4 + (uint32_t)(i / 4);
                if (px < width && py < height) {
                    memcpy(pixels + ((size_t)py * width + px) * 4, texels[i], 4);
                }
            }
        }
    }
    free(data);

    TextureImage *img = (TextureImage *)malloc(sizeof(TextureImage));
    if (!img) {
        free(pixels);
        setErr(err, errLen, "out of memory");
        return NULL;
    }
    img->width = width;
    img->height = height;
    img->pixels = pixels;
    return img;
}

static int hasDdsMagic(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4] = {0};
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    return n == 4 && memcmp(magic, "DDS ", 4) == 0;
}

TextureImage *textureLoadFile(const char *path, uint32_t maxDim,
                              char *err, size_t errLen) {
    if (hasDdsMagic(path)) {
        TextureImage *img = loadDds(path, err, errLen);
        if (!img) return NULL;
        if (maxDim > 0) {
            while (img->width > maxDim || img->height > maxDim) {
                halveImage(img);
            }
        }
        return img;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc *pixels = stbi_load(path, &w, &h, &comp, 4);
    if (!pixels) {
        setErr(err, errLen, stbi_failure_reason());
        return NULL;
    }

    TextureImage *img = (TextureImage *)malloc(sizeof(TextureImage));
    if (!img) {
        stbi_image_free(pixels);
        setErr(err, errLen, "out of memory");
        return NULL;
    }
    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    img->pixels = pixels;

    if (maxDim > 0) {
        while (img->width > maxDim || img->height > maxDim) {
            halveImage(img);
        }
    }
    return img;
}

void textureFree(TextureImage *img) {
    if (!img) return;
    stbi_image_free(img->pixels);
    free(img);
}

static int strEqNoCase(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (char)((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a);
        char cb = (char)((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b);
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Compare name stems (everything before the last dot), ignoring case. */
static int stemEqNoCase(const char *a, const char *b) {
    const char *aDot = strrchr(a, '.');
    const char *bDot = strrchr(b, '.');
    size_t aLen = aDot ? (size_t)(aDot - a) : strlen(a);
    size_t bLen = bDot ? (size_t)(bDot - b) : strlen(b);
    if (aLen != bLen || aLen == 0) return 0;
    for (size_t i = 0; i < aLen; i++) {
        char ca = (char)((a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : a[i]);
        char cb = (char)((b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i]);
        if (ca != cb) return 0;
    }
    return 1;
}

/* Directory-listing cache. On the Vita's storage every resolve
 * otherwise pays opendir/readdir over 300+ entry directories
 * (milliseconds of I/O each), and the room-streaming path resolves
 * several names per frame - a measured contributor to the fps drop
 * while rooms load. Each directory is read once and matched from
 * memory afterwards; the asset set never changes mid-run. */
#define DIRCACHE_MAX 16

typedef struct {
    char *path;
    char **names;
    size_t count;
} DirListing;

static DirListing dirCache[DIRCACHE_MAX];
static size_t dirCacheCount;

static const DirListing *dirListingGet(const char *path) {
    for (size_t i = 0; i < dirCacheCount; i++) {
        if (strcmp(dirCache[i].path, path) == 0) return &dirCache[i];
    }
    if (dirCacheCount >= DIRCACHE_MAX) return NULL;
    /* Read it once; a directory that fails to open caches as empty so
     * the filesystem is not retried on every resolve. */
    char **names = NULL;
    size_t count = 0, cap = 0;
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 64;
                char **grown =
                    (char **)realloc(names, ncap * sizeof(char *));
                if (!grown) break;
                names = grown;
                cap = ncap;
            }
            names[count] = strdup(de->d_name);
            if (!names[count]) break;
            count++;
        }
        closedir(d);
    }
    DirListing *dl = &dirCache[dirCacheCount];
    dl->path = strdup(path);
    if (!dl->path) {
        for (size_t i = 0; i < count; i++) free(names[i]);
        free(names);
        return NULL;
    }
    dl->names = names;
    dl->count = count;
    dirCacheCount++;
    return dl;
}

int textureResolve(const char *name, const char *const *searchDirs,
                   size_t searchDirCount, char *out, size_t outLen) {
    /* Texture names may carry Windows path separators; only the final
     * component is meaningful. */
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    if (*base == '\0') return 0;

    /* Pass 1: exact (case-insensitive) filename match, like the game.
     * Pass 2: same stem with any extension — some shipped rooms
     * reference e.g. metalpanels2.jpg while the file is .png; the
     * original engine left those surfaces untextured. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < searchDirCount; i++) {
            const DirListing *dl = dirListingGet(searchDirs[i]);
            if (dl) {
                for (size_t n = 0; n < dl->count; n++) {
                    int match = pass == 0
                                    ? strEqNoCase(dl->names[n], base)
                                    : stemEqNoCase(dl->names[n], base);
                    if (match) {
                        snprintf(out, outLen, "%s/%s", searchDirs[i],
                                 dl->names[n]);
                        return 1;
                    }
                }
                continue;
            }
            /* Cache table full/OOM: fall back to a direct scan. */
            DIR *d = opendir(searchDirs[i]);
            if (!d) continue;
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                int match = pass == 0 ? strEqNoCase(de->d_name, base)
                                      : stemEqNoCase(de->d_name, base);
                if (match) {
                    snprintf(out, outLen, "%s/%s", searchDirs[i], de->d_name);
                    closedir(d);
                    return 1;
                }
            }
            closedir(d);
        }
    }
    return 0;
}
