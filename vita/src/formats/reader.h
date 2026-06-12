#ifndef VITA_FORMATS_READER_H
#define VITA_FORMATS_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounds-checked little-endian reader shared by the format loaders.
 * On any out-of-bounds read the reader latches into a failed state;
 * callers check rdOk() once after a batch of reads. */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    bool failed;
} Reader;

static inline void rdInit(Reader *r, const void *data, size_t size) {
    r->data = (const uint8_t *)data;
    r->size = size;
    r->pos = 0;
    r->failed = false;
}

static inline bool rdOk(const Reader *r) {
    return !r->failed;
}

static inline bool rdHas(Reader *r, size_t n) {
    if (r->failed || n > r->size - r->pos) {
        r->failed = true;
        return false;
    }
    return true;
}

static inline uint8_t rdU8(Reader *r) {
    if (!rdHas(r, 1)) return 0;
    return r->data[r->pos++];
}

static inline int32_t rdI32(Reader *r) {
    if (!rdHas(r, 4)) return 0;
    uint32_t v = (uint32_t)r->data[r->pos]
               | (uint32_t)r->data[r->pos + 1] << 8
               | (uint32_t)r->data[r->pos + 2] << 16
               | (uint32_t)r->data[r->pos + 3] << 24;
    r->pos += 4;
    return (int32_t)v;
}

static inline float rdF32(Reader *r) {
    union { uint32_t i; float f; } u;
    u.i = (uint32_t)rdI32(r);
    return u.f;
}

/* Blitz3D ReadString: int32 length followed by raw bytes. Returns a
 * malloc'd NUL-terminated copy, or NULL on failure. */
static inline char *rdBlitzString(Reader *r) {
    int32_t len = rdI32(r);
    if (r->failed || len < 0 || !rdHas(r, (size_t)len)) {
        r->failed = true;
        return NULL;
    }
    char *s = (char *)malloc((size_t)len + 1);
    if (!s) {
        r->failed = true;
        return NULL;
    }
    memcpy(s, r->data + r->pos, (size_t)len);
    s[len] = '\0';
    r->pos += (size_t)len;
    return s;
}

/* C-style NUL-terminated string (used by .b3d). */
static inline char *rdCString(Reader *r) {
    if (r->failed) return NULL;
    size_t start = r->pos;
    while (r->pos < r->size && r->data[r->pos] != 0) {
        r->pos++;
    }
    if (r->pos >= r->size) {
        r->failed = true;
        return NULL;
    }
    size_t len = r->pos - start;
    char *s = (char *)malloc(len + 1);
    if (!s) {
        r->failed = true;
        return NULL;
    }
    memcpy(s, r->data + start, len);
    s[len] = '\0';
    r->pos++; /* skip NUL */
    return s;
}

/* Read an entire file into a malloc'd buffer. */
static inline void *readWholeFile(const char *path, size_t *outSize) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    void *buf = malloc(sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outSize = (size_t)sz;
    return buf;
}

#endif
