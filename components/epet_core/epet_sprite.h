#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Sprite frames, poses and the little player that runs them.
 *
 * A FRAME is an 8bpp palette-indexed bitmap. Index 0 is transparent, so
 * frames composite over whatever the scene already drew. Indices 1..15 look
 * up in the species palette, which means two species can share frame data
 * and still look completely different.
 *
 * A POSE is a named sequence of frames with per-frame durations: "idle",
 * "birth", "happy", "sad". Poses are found BY NAME, so adding one is just
 * another entry in the species' pose array -- no enum to edit, no switch to
 * extend anywhere in the codebase. */

#define EPET_PAL_SIZE 16

typedef struct {
    uint16_t c[EPET_PAL_SIZE];   /* c[0] is never read; index 0 is transparent */
} epet_palette_t;

typedef struct {
    uint8_t        w, h;
    const uint8_t *px;           /* w*h indices, row-major */
} epet_frame_t;

typedef struct {
    const epet_frame_t *frame;
    uint16_t            hold_ms;
} epet_key_t;

typedef struct {
    const char        *name;
    const epet_key_t  *keys;
    uint8_t            n_keys;
    bool               loop;
} epet_pose_t;

/* Well-known pose names. Any other string works just as well. */
#define EPET_POSE_IDLE  "idle"
#define EPET_POSE_BIRTH "birth"
#define EPET_POSE_HAPPY "happy"
#define EPET_POSE_SAD   "sad"

/* ---- blitting -------------------------------------------------------- */

/* Top-left placement. `scale` replicates pixels; 0 and 1 both mean 1:1. */
void epet_blit(uint16_t *fb, int x, int y, const epet_frame_t *f,
               const epet_palette_t *pal, int scale);
/* Centred on (cx, cy) -- what the pet drawing actually wants. */
void epet_blit_centred(uint16_t *fb, int cx, int cy, const epet_frame_t *f,
                       const epet_palette_t *pal, int scale);
/* As above but every opaque pixel is drawn in one flat colour: cheap
 * silhouettes for shadows, flashes and death fades. */
void epet_blit_tinted(uint16_t *fb, int cx, int cy, const epet_frame_t *f,
                      uint16_t colour, uint8_t alpha, int scale);
