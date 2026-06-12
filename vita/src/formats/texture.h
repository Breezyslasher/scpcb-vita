#ifndef VITA_FORMATS_TEXTURE_H
#define VITA_FORMATS_TEXTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Texture loading for the port: decodes the game's PNG/JPG/BMP textures
 * to RGBA8 and optionally downscales them to a budget cap (the Vita has
 * 512 MB total, so desktop-resolution textures cannot ship as-is).
 *
 * Also handles reference resolution: .rmesh files store bare texture
 * names that the game looks up first next to the room file and then in
 * GFX/Map/Textures (Map_Core.bb / Texture_Cache_Core.bb). The original
 * ran on a case-insensitive filesystem, so lookup here is
 * case-insensitive too.
 */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;        /* RGBA8, width * height * 4 bytes */
} TextureImage;

/* Decode a texture file. If maxDim > 0 and either dimension exceeds it,
 * the image is box-filtered down (halved repeatedly) until it fits.
 * Returns NULL on failure (err filled in when non-NULL). */
TextureImage *textureLoadFile(const char *path, uint32_t maxDim,
                              char *err, size_t errLen);

void textureFree(TextureImage *img);

/* Resolve a texture reference name to an on-disk path, checking each of
 * searchDirs in order with case-insensitive filename matching. Writes
 * the resolved path into out and returns 1, or returns 0 if the name is
 * not found anywhere. */
int textureResolve(const char *name, const char *const *searchDirs,
                   size_t searchDirCount, char *out, size_t outLen);

#endif
