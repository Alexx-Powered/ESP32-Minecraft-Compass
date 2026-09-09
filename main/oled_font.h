/*
 * Printable ASCII 5x7 font used by the OLED display adapter.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_FONT_FIRST 0x20U
#define OLED_FONT_LAST 0x7EU
#define OLED_FONT_GLYPH_WIDTH 5U
#define OLED_FONT_GLYPH_HEIGHT 7U

extern const uint8_t oled_font_5x7[];

/**
 * @brief Return the five column bytes for a printable ASCII character.
 *
 * Each byte is one column, with bit 0 at the top of the glyph.
 */
const uint8_t *oled_font_5x7_glyph(unsigned char character);

#ifdef __cplusplus
}
#endif
