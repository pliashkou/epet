#include "epet.h"
#include "epet_sprite.h"
#include "epet_draw.h"
#include <stddef.h>

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

/* ---- fractional scaling ---------------------------------------------- */

/* The integer blitter above walks the SOURCE and replicates each pixel into
 * a scale x scale block, which only ever produces whole-number sizes. Growth
 * needs sizes in between -- a pet that creeps up through fifty age levels
 * two integer steps apart would visibly jump twice and sit still the rest of
 * the time.
 *
 * So this one walks the DESTINATION instead and samples back into the source
 * (nearest neighbour), stepping with a 16.16 accumulator so the inner loop
 * is an add and a shift rather than a divide.
 *
 * At a whole-number scale it is pixel-identical to epet_blit(): dest pixel d
 * maps to source floor(d * 65536/s / 65536) = floor(d/s), which is exactly
 * the block replication above. That matters -- the main screen switched to
 * this path, and it must not quietly resample art that used to be crisp. */
void epet_blit_q8(uint16_t *fb, int x, int y, const epet_frame_t *f,
                  const epet_palette_t *pal, int scale_q8)
{
    if (!f || !f->px || !pal) return;
    if (scale_q8 < 16) scale_q8 = 16;        /* 1/16 of life size, a floor */

    int dw = (f->w * scale_q8) >> 8;
    int dh = (f->h * scale_q8) >> 8;
    if (dw <= 0 || dh <= 0) return;

    /* Bresenham, not a fixed-point step.
     *
     * The obvious version precomputes step = (w << 16) / dw and accumulates
     * it. That is exact only when dw divides 65536: at scale 2 it is, at
     * scale 3 it is not -- 65536/3 truncates to 21845, and by the third
     * destination pixel the sample point has already drifted a whole source
     * pixel too low. The result looked almost right, which is the dangerous
     * kind of wrong. tests/test_character.c compares this against
     * epet_blit() at every whole scale for exactly that reason.
     *
     * Splitting the step into a whole part and a remainder carried against
     * dw keeps sx equal to floor(dx * w / dw) exactly, with no division in
     * the loop and no drift at any scale. */
    const int stepx_i = f->w / dw, stepx_r = f->w % dw;
    const int stepy_i = f->h / dh, stepy_r = f->h % dh;

    int sy = 0, sy_err = 0;
    for (int dy = 0; dy < dh; dy++) {
        const int src_row = sy;
        sy += stepy_i;
        sy_err += stepy_r;
        if (sy_err >= dh) { sy_err -= dh; sy++; }

        int py = y + dy;
        if (py < 0) continue;
        if (py >= EPET_H) break;

        const uint8_t *row = f->px + (size_t)src_row * f->w;
        uint16_t *dst = fb + (size_t)py * EPET_W;

        int sx = 0, sx_err = 0;
        for (int dx = 0; dx < dw; dx++) {
            const int src_col = sx;
            sx += stepx_i;
            sx_err += stepx_r;
            if (sx_err >= dw) { sx_err -= dw; sx++; }

            int px = x + dx;
            if (px < 0) continue;
            if (px >= EPET_W) break;
            uint8_t idx = row[src_col];
            if (idx == 0) continue;                  /* transparent */
            if (idx >= EPET_PAL_SIZE) idx = EPET_PAL_SIZE - 1;
            dst[px] = pal->c[idx];
        }
    }
}

void epet_blit_centred_q8(uint16_t *fb, int cx, int cy, const epet_frame_t *f,
                          const epet_palette_t *pal, int scale_q8)
{
    if (!f) return;
    if (scale_q8 < 16) scale_q8 = 16;
    epet_blit_q8(fb, cx - ((f->w * scale_q8) >> 8) / 2,
                     cy - ((f->h * scale_q8) >> 8) / 2, f, pal, scale_q8);
}

void epet_blit_bottom_q8(uint16_t *fb, int cx, int ground_y,
                         const epet_frame_t *f, const epet_palette_t *pal,
                         int scale_q8)
{
    if (!f) return;
    if (scale_q8 < 16) scale_q8 = 16;
    /* Anchored by the FEET, not the centre. Scaling about the middle would
     * sink a growing pet through the floor and lift a small one off it. */
    epet_blit_q8(fb, cx - ((f->w * scale_q8) >> 8) / 2,
                     ground_y - ((f->h * scale_q8) >> 8), f, pal, scale_q8);
}
