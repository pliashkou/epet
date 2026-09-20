#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epet_sprite.h"

/* Drawing primitives, available to pages so a sub-program can render whatever
 * UI or animation it likes straight into the framebuffer.
 *
 * The framebuffer is native-endian RGB565, EPET_W * EPET_H, row-major.
 * Every primitive clips to the screen, so out-of-range coordinates are safe. */

/* Colours are stored in the byte order the ST7789 wants: RGB565,
 * most-significant byte first.
 *
 * The framebuffer used to hold native-endian values and the firmware swapped
 * all 57600 pixels before every transfer -- 9.5 ms per frame, 38% of the
 * frame budget, purely to reorder bytes. Storing them panel-ready deletes
 * that pass entirely and lets the device render straight into the DMA
 * buffer. The cost moves to the simulator, which swaps once per frame when
 * uploading to SDL, where it is free.
 *
 * Only code that DECOMPOSES a colour has to care: see epet_mix(). */
#define EPET_RGB565(r, g, b) \
    ((uint16_t)__builtin_bswap16( \
        (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))))

/* Shared palette. Pages may of course use their own colours. */
#define EPET_C_BLACK   EPET_RGB565(0, 0, 0)
#define EPET_C_WHITE   EPET_RGB565(255, 255, 255)
#define EPET_C_SKY     EPET_RGB565(88, 166, 224)
#define EPET_C_NIGHT   EPET_RGB565(24, 30, 72)
#define EPET_C_GROUND  EPET_RGB565(116, 186, 108)
#define EPET_C_GROUNDN EPET_RGB565(40, 70, 50)
#define EPET_C_BODY    EPET_RGB565(246, 214, 92)
#define EPET_C_BODYHI  EPET_RGB565(255, 240, 160)
#define EPET_C_BODYILL EPET_RGB565(170, 190, 120)
#define EPET_C_POOP    EPET_RGB565(110, 76, 40)
#define EPET_C_PANEL   EPET_RGB565(16, 22, 34)
#define EPET_C_BARBG   EPET_RGB565(30, 40, 55)
#define EPET_C_GOOD    EPET_RGB565(90, 210, 110)
#define EPET_C_WARN    EPET_RGB565(240, 190, 70)
#define EPET_C_BAD     EPET_RGB565(230, 80, 80)
#define EPET_C_MENU    EPET_RGB565(22, 30, 46)
#define EPET_C_MENUSEL EPET_RGB565(58, 120, 190)
#define EPET_C_DIM     EPET_RGB565(120, 132, 150)

void epet_fill(uint16_t *fb, uint16_t c);
void epet_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
void epet_frame(uint16_t *fb, int x, int y, int w, int h, int t, uint16_t c);
void epet_disc(uint16_t *fb, int cx, int cy, int r, uint16_t c);
void epet_hline(uint16_t *fb, int x, int y, int w, uint16_t c);
void epet_vline(uint16_t *fb, int x, int y, int h, uint16_t c);

/* 5x7 glyphs: digits, A-Z (case-insensitive), '%', '.', '-', ':' and space.
 * `scale` multiplies both axes; advance per character is 6 * scale. */
void epet_glyph(uint16_t *fb, int x, int y, char ch, uint16_t c, int scale);
void epet_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int scale);
void epet_number(uint16_t *fb, int x, int y, int v, uint16_t c, int scale);
int  epet_text_width(const char *s, int scale);
/* Raw 5x7 bitmap for a character (5 columns, bit N = row N), or NULL if the
 * glyph is unmapped. Lets a caller render text into a buffer with a
 * different stride -- the simulator uses it to label its on-screen buttons. */
const uint8_t *epet_font_glyph(char ch);

/* Horizontal meter used by the status bars; pages may reuse it. */
/* Alpha blending, for overlays drawn on top of the scene.
 * `alpha` is how much of `over` shows through, 0 = none, 255 = opaque. */
uint16_t epet_mix(uint16_t under, uint16_t over, uint8_t alpha);
void epet_shade(uint16_t *fb, int x, int y, int w, int h, uint16_t c, uint8_t alpha);
void epet_shade_disc(uint16_t *fb, int cx, int cy, int r, uint16_t c, uint8_t alpha);

/* ---- menu icons ------------------------------------------------------
 * Pixel art rather than composed shapes: circles and rectangles give lumpy
 * silhouettes at 20x20. Index 1 is the body, 2 a secondary tone, 3 a
 * highlight, each coloured per selection state. */
typedef struct {
    const char         *name;
    const epet_frame_t *frame;
} epet_icon_t;

extern const epet_icon_t EPET_ICONS[];
extern const uint8_t     EPET_ICON_COUNT;

const epet_frame_t *epet_icon(const char *name);
/* Draw one centred, tinted for its state. */
void epet_icon_draw(uint16_t *fb, int cx, int cy, const epet_frame_t *f,
                    bool selected);

void epet_bar(uint16_t *fb, int x, int y, int w, int h, float pct, uint16_t c);
uint16_t epet_level_colour(float pct);
