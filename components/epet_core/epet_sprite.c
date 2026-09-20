#include "epet.h"
#include "epet_sprite.h"
#include "epet_draw.h"

void epet_blit(uint16_t *fb, int x, int y, const epet_frame_t *f,
               const epet_palette_t *pal, int scale)
{
    if (!f || !f->px || !pal) return;
    if (scale < 1) scale = 1;

    for (int sy = 0; sy < f->h; sy++) {
        for (int sx = 0; sx < f->w; sx++) {
            uint8_t idx = f->px[sy * f->w + sx];
            if (idx == 0) continue;                  /* transparent */
            if (idx >= EPET_PAL_SIZE) idx = EPET_PAL_SIZE - 1;
            uint16_t c = pal->c[idx];

            int dx = x + sx * scale, dy = y + sy * scale;
            for (int ry = 0; ry < scale; ry++) {
                int py = dy + ry;
                if (py < 0 || py >= EPET_H) continue;
                for (int rx = 0; rx < scale; rx++) {
                    int px = dx + rx;
                    if (px < 0 || px >= EPET_W) continue;
                    fb[py * EPET_W + px] = c;
                }
            }
        }
    }
}

void epet_blit_centred(uint16_t *fb, int cx, int cy, const epet_frame_t *f,
                       const epet_palette_t *pal, int scale)
{
    if (!f) return;
    if (scale < 1) scale = 1;
    epet_blit(fb, cx - (f->w * scale) / 2, cy - (f->h * scale) / 2, f, pal, scale);
}

void epet_blit_tinted(uint16_t *fb, int cx, int cy, const epet_frame_t *f,
                      uint16_t colour, uint8_t alpha, int scale)
{
    if (!f || !f->px) return;
    if (scale < 1) scale = 1;
    int x = cx - (f->w * scale) / 2, y = cy - (f->h * scale) / 2;

    for (int sy = 0; sy < f->h; sy++) {
        for (int sx = 0; sx < f->w; sx++) {
            if (f->px[sy * f->w + sx] == 0) continue;
            int dx = x + sx * scale, dy = y + sy * scale;
            for (int ry = 0; ry < scale; ry++) {
                int py = dy + ry;
                if (py < 0 || py >= EPET_H) continue;
                for (int rx = 0; rx < scale; rx++) {
                    int px = dx + rx;
                    if (px < 0 || px >= EPET_W) continue;
                    uint16_t *dst = &fb[py * EPET_W + px];
                    *dst = epet_mix(*dst, colour, alpha);
                }
            }
        }
    }
}
