#include "epet.h"
#include "epet_draw.h"

void epet_fill(uint16_t *fb, uint16_t c)
{
    for (int i = 0; i < EPET_W * EPET_H; i++) fb[i] = c;
}

void epet_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= EPET_H) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= EPET_W) continue;
            fb[row * EPET_W + col] = c;
        }
    }
}

void epet_hline(uint16_t *fb, int x, int y, int w, uint16_t c) { epet_rect(fb, x, y, w, 1, c); }
void epet_vline(uint16_t *fb, int x, int y, int h, uint16_t c) { epet_rect(fb, x, y, 1, h, c); }

void epet_frame(uint16_t *fb, int x, int y, int w, int h, int t, uint16_t c)
{
    epet_rect(fb, x, y, w, t, c);
    epet_rect(fb, x, y + h - t, w, t, c);
    epet_rect(fb, x, y, t, h, c);
    epet_rect(fb, x + w - t, y, t, h, c);
}

void epet_disc(uint16_t *fb, int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= EPET_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int x = cx + dx;
            if (x < 0 || x >= EPET_W) continue;
            fb[y * EPET_W + x] = c;
        }
    }
}

/* ---- 5x7 font -------------------------------------------------------- */

static const uint8_t F_DIGIT[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
};

static const uint8_t F_ALPHA[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

/* punctuation: . - : ! ? / + < > */
static const uint8_t F_DOT[5]   = {0x00,0x60,0x60,0x00,0x00};
static const uint8_t F_DASH[5]  = {0x08,0x08,0x08,0x08,0x08};
static const uint8_t F_COLON[5] = {0x00,0x36,0x36,0x00,0x00};
static const uint8_t F_BANG[5]  = {0x00,0x00,0x5F,0x00,0x00};
static const uint8_t F_QUERY[5] = {0x02,0x01,0x51,0x09,0x06};
static const uint8_t F_SLASH[5] = {0x20,0x10,0x08,0x04,0x02};
static const uint8_t F_PLUS[5]  = {0x08,0x08,0x3E,0x08,0x08};
static const uint8_t F_LT[5]    = {0x08,0x14,0x22,0x41,0x00};
static const uint8_t F_GT[5]    = {0x00,0x41,0x22,0x14,0x08};

const uint8_t *epet_font_glyph(char ch)
{
    if      (ch >= '0' && ch <= '9') return F_DIGIT[ch - '0'];
    else if (ch >= 'A' && ch <= 'Z') return F_ALPHA[ch - 'A'];
    else if (ch >= 'a' && ch <= 'z') return F_ALPHA[ch - 'a'];
    else if (ch == '.') return F_DOT;
    else if (ch == '-') return F_DASH;
    else if (ch == ':') return F_COLON;
    else if (ch == '!') return F_BANG;
    else if (ch == '?') return F_QUERY;
    else if (ch == '/') return F_SLASH;
    else if (ch == '+') return F_PLUS;
    else if (ch == '<') return F_LT;
    else if (ch == '>') return F_GT;
    return 0;
}

void epet_glyph(uint16_t *fb, int x, int y, char ch, uint16_t c, int scale)
{
    const uint8_t *g = 0;
    if      (ch >= '0' && ch <= '9') g = F_DIGIT[ch - '0'];
    else if (ch >= 'A' && ch <= 'Z') g = F_ALPHA[ch - 'A'];
    else if (ch >= 'a' && ch <= 'z') g = F_ALPHA[ch - 'a'];
    else if (ch == '.') g = F_DOT;
    else if (ch == '-') g = F_DASH;
    else if (ch == ':') g = F_COLON;
    else if (ch == '!') g = F_BANG;
    else if (ch == '?') g = F_QUERY;
    else if (ch == '/') g = F_SLASH;
    else if (ch == '+') g = F_PLUS;
    else if (ch == '<') g = F_LT;
    else if (ch == '>') g = F_GT;
    else if (ch == '%') {
        epet_rect(fb, x, y, scale, scale, c);
        epet_rect(fb, x + 4 * scale, y + 6 * scale, scale, scale, c);
        for (int i = 0; i < 7; i++)
            epet_rect(fb, x + (4 - (i * 4) / 6) * scale, y + i * scale, scale, scale, c);
        return;
    }
    else return;   /* space and anything unmapped */

    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1 << row)) {
                epet_rect(fb, x + col * scale, y + row * scale, scale, scale, c);
            }
        }
    }
}

void epet_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int scale)
{
    for (; *s; s++) {
        epet_glyph(fb, x, y, *s, c, scale);
        x += 6 * scale;
    }
}

int epet_text_width(const char *s, int scale)
{
    int n = 0;
    for (; *s; s++) n++;
    return n * 6 * scale;
}

void epet_number(uint16_t *fb, int x, int y, int v, uint16_t c, int scale)
{
    char buf[12];
    int n = 0;
    bool neg = v < 0;
    if (neg) v = -v;
    if (v == 0) buf[n++] = '0';
    while (v > 0 && n < 10) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg) buf[n++] = '-';
    for (int i = n - 1; i >= 0; i--) {
        epet_glyph(fb, x, y, buf[i], c, scale);
        x += 6 * scale;
    }
}

uint16_t epet_mix(uint16_t under, uint16_t over, uint8_t alpha)
{
    if (alpha == 0)   return under;
    if (alpha == 255) return over;

    /* Pixels are stored panel-ready (byte-swapped), so unpack before touching
     * the channels and repack afterwards. This is the only place that pays
     * for the storage order, and it runs on blended pixels only -- a few
     * thousand per frame for the menu overlay, against the 57600 the old
     * whole-framebuffer swap touched every time. */
    uint16_t u = __builtin_bswap16(under);
    uint16_t o = __builtin_bswap16(over);

    uint32_t ia = 255u - alpha;
    uint32_t r = (((u >> 11) & 0x1F) * ia + ((o >> 11) & 0x1F) * alpha) / 255u;
    uint32_t g = (((u >> 5)  & 0x3F) * ia + ((o >> 5)  & 0x3F) * alpha) / 255u;
    uint32_t b = (( u        & 0x1F) * ia + ( o        & 0x1F) * alpha) / 255u;
    return __builtin_bswap16((uint16_t)((r << 11) | (g << 5) | b));
}

void epet_shade(uint16_t *fb, int x, int y, int w, int h, uint16_t c, uint8_t alpha)
{
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= EPET_H) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= EPET_W) continue;
            uint16_t *px = &fb[row * EPET_W + col];
            *px = epet_mix(*px, c, alpha);
        }
    }
}

void epet_shade_disc(uint16_t *fb, int cx, int cy, int r, uint16_t c, uint8_t alpha)
{
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= EPET_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int x = cx + dx;
            if (x < 0 || x >= EPET_W) continue;
            uint16_t *px = &fb[y * EPET_W + x];
            *px = epet_mix(*px, c, alpha);
        }
    }
}

#include <string.h>

const epet_frame_t *epet_icon(const char *name)
{
    for (uint8_t i = 0; i < EPET_ICON_COUNT; i++) {
        if (strcmp(EPET_ICONS[i].name, name) == 0) return EPET_ICONS[i].frame;
    }
    return 0;
}

void epet_icon_draw(uint16_t *fb, int cx, int cy, const epet_frame_t *f,
                    bool selected)
{
    if (!f) return;
    /* Two palettes, one per state. Selected reads bright and solid;
     * unselected sits back without becoming illegible. */
    static const epet_palette_t SEL = { {
        0,
        EPET_RGB565(255, 255, 255),   /* body      */
        EPET_RGB565(150, 200, 245),   /* secondary */
        EPET_RGB565( 70, 130, 200),   /* highlight */
    } };
    static const epet_palette_t DIM = { {
        0,
        EPET_RGB565(132, 146, 166),
        EPET_RGB565( 92, 104, 122),
        EPET_RGB565( 62, 72,  88),
    } };
    epet_blit_centred(fb, cx, cy, f, selected ? &SEL : &DIM, 1);
}

uint16_t epet_level_colour(float pct)
{
    if (pct > 60.0f) return EPET_C_GOOD;
    if (pct > 30.0f) return EPET_C_WARN;
    return EPET_C_BAD;
}

void epet_bar(uint16_t *fb, int x, int y, int w, int h, float pct, uint16_t c)
{
    epet_rect(fb, x, y, w, h, EPET_C_BARBG);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    int fillw = (int)((pct / 100.0f) * (w - 2));
    epet_rect(fb, x + 1, y + 1, fillw, h - 2, c);
}
