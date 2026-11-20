#pragma once

#include <stdint.h>

typedef struct {
    uint8_t width;    // advance = bitmap width
    uint32_t offset;  // into alpha[]
} font_glyph_t;

typedef struct {
    uint8_t height, ascent;
    const font_glyph_t *glyphs;  // ASCII 32..126
    const uint8_t *alpha;        // 8-bit coverage, row-major, width x height
} font_t;

extern const font_t FONT_STATUS;  // 37 px: status bands, picker buttons (font.c)
extern const font_t FONT_LIST;    // 28 px: song list (font_list.c)
