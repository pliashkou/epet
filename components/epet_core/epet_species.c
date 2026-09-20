#include "epet_species.h"
#include "epet_draw.h"
#include "epet.h"
#include "epet_module.h"
#include <string.h>

void epet_backdrop_draw(const epet_backdrop_t *bd, uint16_t *fb, bool night)
{
    static const uint16_t FALLBACK_SKY    = EPET_RGB565(88, 166, 224);
    static const uint16_t FALLBACK_GROUND = EPET_RGB565(116, 186, 108);

    if (bd && bd->image && bd->palette) {
        int scale = bd->scale ? bd->scale : 1;
        /* Centre it, so an image that does not divide 240 exactly still
         * covers the middle rather than leaving a gap on one side. */
        int w = bd->image->w * scale, h = bd->image->h * scale;
        epet_blit(fb, (EPET_W - w) / 2, (EPET_H - h) / 2,
                  bd->image, bd->palette, scale);
    } else {
        uint16_t sky    = (bd && bd->sky)    ? bd->sky    : FALLBACK_SKY;
        uint16_t ground = (bd && bd->ground) ? bd->ground : FALLBACK_GROUND;
        int hy = (bd && bd->horizon_y) ? bd->horizon_y : 198;
        epet_rect(fb, 0, 0, EPET_W, hy, sky);
        epet_rect(fb, 0, hy, EPET_W, EPET_H - hy, ground);
    }

    if (night) {
        uint16_t tint = (bd && bd->night_alpha) ? bd->night
                                                : EPET_RGB565(20, 26, 70);
        uint8_t  a    = (bd && bd->night_alpha) ? bd->night_alpha : 150;
        epet_shade(fb, 0, 0, EPET_W, EPET_H, tint, a);
    }
}


const epet_backdrop_t *epet_species_backdrop(const epet_species_t *sp, uint8_t i)
{
    if (!sp || sp->n_backdrops == 0) return 0;
    return &sp->backdrops[i % sp->n_backdrops];
}

const epet_pose_t *epet_species_pose(const epet_species_t *sp, const char *name)
{
    if (!sp || !name) return 0;
    for (uint8_t i = 0; i < sp->n_poses; i++) {
        if (strcmp(sp->poses[i].name, name) == 0) return &sp->poses[i];
    }
    return 0;
}

const epet_pose_t *epet_species_pose_or_idle(const epet_species_t *sp, const char *name)
{
    const epet_pose_t *p = epet_species_pose(sp, name);
    if (p) return p;
    p = epet_species_pose(sp, EPET_POSE_IDLE);
    if (p) return p;
    /* last resort: whatever the species does have */
    return (sp && sp->n_poses) ? &sp->poses[0] : 0;
}

/* ---- growth ---------------------------------------------------------- */

static uint8_t clamp_age(uint8_t age)
{
    if (age < 1) return 1;
    if (age > EPET_AGE_MAX) return EPET_AGE_MAX;
    return age;
}

/* Index of the stage covering `age`, or -1. Stages are ordered by from_age,
 * so this is the last one that has started. */
static int stage_index(const epet_species_t *sp, uint8_t age)
{
    if (!sp || sp->n_stages == 0) return -1;
    int found = -1;
    for (uint8_t i = 0; i < sp->n_stages; i++) {
        if (sp->stages[i].from_age <= age) found = (int)i;
        else break;
    }
    /* Younger than the first stage declares: use the first. A species whose
     * stages start at age 3 should still draw something at age 1. */
    return found < 0 ? 0 : found;
}

const epet_growth_t *epet_species_stage(const epet_species_t *sp, uint8_t age)
{
    int i = stage_index(sp, clamp_age(age));
    return i < 0 ? 0 : &sp->stages[i];
}

int epet_species_scale_q8_at(const epet_species_t *sp, uint8_t age)
{
    if (!sp) return 256;
    uint8_t base = sp->scale ? sp->scale : 1;

    age = clamp_age(age);
    int i = stage_index(sp, age);
    if (i < 0) return base * 256;               /* no stages: never changes */

    const epet_growth_t *cur = &sp->stages[i];
    uint16_t from_pct = cur->scale_pct ? cur->scale_pct : (uint16_t)(base * 100);

    /* Last stage, or age below the first: hold this size. */
    if (i + 1 >= (int)sp->n_stages || age < cur->from_age)
        return (int)((uint32_t)from_pct * 256u / 100u);

    const epet_growth_t *next = &sp->stages[i + 1];
    uint16_t to_pct = next->scale_pct ? next->scale_pct : from_pct;

    /* Interpolate across the ages this stage covers, so each level is its
     * own size instead of the size stepping at a handful of thresholds. */
    uint16_t span = (uint16_t)(next->from_age - cur->from_age);
    if (span == 0) return (int)((uint32_t)from_pct * 256u / 100u);
    uint16_t into = (uint16_t)(age - cur->from_age);

    int32_t pct = (int32_t)from_pct +
                  ((int32_t)to_pct - (int32_t)from_pct) * (int32_t)into / (int32_t)span;
    if (pct < 1) pct = 1;
    return (int)((uint32_t)pct * 256u / 100u);
}

/* Look in this stage, then walk back through earlier stages, then the
 * species itself. Walking back is what lets a stage declare only the poses
 * that actually change -- an ELDER that just stoops needs a new "idle" and
 * inherits "happy" from whoever last defined it. */
static const epet_pose_t *stage_pose(const epet_species_t *sp, int from_stage,
                                     const char *name)
{
    for (int i = from_stage; i >= 0; i--) {
        const epet_growth_t *g = &sp->stages[i];
        for (uint8_t k = 0; k < g->n_poses; k++) {
            if (strcmp(g->poses[k].name, name) == 0) return &g->poses[k];
        }
    }
    return 0;
}

const epet_pose_t *epet_species_pose_at(const epet_species_t *sp, uint8_t age,
                                        const char *name)
{
    if (!sp || !name) return 0;
    int i = stage_index(sp, clamp_age(age));
    if (i >= 0) {
        const epet_pose_t *p = stage_pose(sp, i, name);
        if (p) return p;
        /* This stage does not draw that pose. Prefer the species' own
         * version of it over this stage's idle: a pose is about what the
         * creature is DOING, and the wrong action reads far more wrongly
         * than slightly wrong proportions. So a baby with only "idle" and
         * "birth" still bounces for "happy", drawn from the base art at the
         * baby's size, rather than standing there. */
        p = epet_species_pose(sp, name);
        if (p) return p;
        /* Nobody defines it at all -- this stage's idle, then the usual
         * species-level fallback chain. */
        p = stage_pose(sp, i, EPET_POSE_IDLE);
        if (p) return p;
    }
    return epet_species_pose_or_idle(sp, name);
}

/* ---- player ---------------------------------------------------------- */

void epet_actor_init(epet_actor_t *a, const epet_species_t *sp)
{
    a->sp = sp;
    a->age = 0;
    a->pose = epet_species_pose_or_idle(sp, EPET_POSE_IDLE);
    a->resume = EPET_POSE_IDLE;
    a->key = 0;
    a->t_ms = 0;
    a->finished = false;
}

void epet_actor_set_age(epet_actor_t *a, uint8_t age)
{
    if (!a || a->age == age) return;
    const epet_growth_t *before = epet_species_stage(a->sp, a->age);
    a->age = age;
    const epet_growth_t *after = epet_species_stage(a->sp, age);
    if (before == after || !a->pose) return;

    /* Crossed into a stage with different art. Re-resolve the SAME pose name
     * there and keep the frame index and phase, so a creature that grows up
     * mid-blink finishes the blink instead of snapping back to frame 0. */
    const epet_pose_t *p = epet_species_pose_at(a->sp, age, a->pose->name);
    if (!p || p == a->pose) return;
    a->pose = p;
    if (a->key >= p->n_keys) a->key = p->n_keys ? (uint8_t)(p->n_keys - 1) : 0;
}

void epet_actor_play(epet_actor_t *a, const char *pose, const char *resume)
{
    const epet_pose_t *p = epet_species_pose_at(a->sp, a->age, pose);
    if (!p) return;
    a->pose = p;
    a->resume = resume;
    a->key = 0;
    a->t_ms = 0;
    a->finished = false;
}

void epet_actor_ensure(epet_actor_t *a, const char *pose)
{
    if (a->pose && strcmp(a->pose->name, pose) == 0) return;
    epet_actor_play(a, pose, 0);
}

void epet_actor_tick(epet_actor_t *a, uint32_t dt_ms)
{
    const epet_pose_t *p = a->pose;
    if (!p || p->n_keys == 0) return;

    a->t_ms += dt_ms;
    /* while(), not if(): a long light-sleep gap can span many frames. */
    while (a->t_ms >= p->keys[a->key].hold_ms) {
        uint16_t hold = p->keys[a->key].hold_ms;
        if (hold == 0) break;                 /* guard against a 0 ms frame */
        a->t_ms -= hold;

        if (a->key + 1 < p->n_keys) {
            a->key++;
            continue;
        }
        if (p->loop) {
            a->key = 0;
            continue;
        }
        /* one-shot finished */
        a->finished = true;
        if (a->resume) {
            const char *r = a->resume;
            epet_actor_play(a, r, 0);
            a->resume = 0;
            p = a->pose;
            if (!p || p->n_keys == 0) return;
            continue;
        }
        a->t_ms = 0;                          /* hold on the last frame */
        return;
    }
}

static const epet_frame_t *current_frame(const epet_actor_t *a)
{
    if (!a->pose || a->pose->n_keys == 0) return 0;
    uint8_t k = a->key < a->pose->n_keys ? a->key : (uint8_t)(a->pose->n_keys - 1);
    return a->pose->keys[k].frame;
}

void epet_actor_draw(const epet_actor_t *a, uint16_t *fb, int cx, int cy)
{
    const epet_frame_t *f = current_frame(a);
    if (!f || !a->sp) return;
    epet_blit_centred(fb, cx, cy, f, &a->sp->palette, a->sp->scale);
}

void epet_actor_draw_scaled(const epet_actor_t *a, uint16_t *fb,
                            int cx, int cy, int scale)
{
    const epet_frame_t *f = current_frame(a);
    if (!f || !a->sp) return;
    epet_blit_centred(fb, cx, cy, f, &a->sp->palette, scale);
}

void epet_actor_draw_tinted(const epet_actor_t *a, uint16_t *fb, int cx, int cy,
                            uint16_t colour, uint8_t alpha)
{
    const epet_frame_t *f = current_frame(a);
    if (!f || !a->sp) return;
    epet_blit_tinted(fb, cx, cy, f, colour, alpha, a->sp->scale);
}

void epet_actor_draw_grown(const epet_actor_t *a, uint16_t *fb,
                           int cx, int ground_y)
{
    const epet_frame_t *f = current_frame(a);
    if (!f || !a->sp) return;
    epet_blit_bottom_q8(fb, cx, ground_y, f, &a->sp->palette,
                        epet_species_scale_q8_at(a->sp, a->age));
}

int epet_actor_grown_width(const epet_actor_t *a)
{
    const epet_frame_t *f = current_frame(a);
    if (!f || !a->sp) return 0;
    return (f->w * epet_species_scale_q8_at(a->sp, a->age)) >> 8;
}

int epet_actor_grown_height(const epet_actor_t *a)
{
    const epet_frame_t *f = current_frame(a);
    if (!f || !a->sp) return 0;
    return (f->h * epet_species_scale_q8_at(a->sp, a->age)) >> 8;
}

const char *epet_actor_pose_name(const epet_actor_t *a)
{
    return (a->pose && a->pose->name) ? a->pose->name : "?";
}

bool epet_actor_finished(const epet_actor_t *a) { return a->finished; }

/* The roster is the union of every installed module's characters, so
 * installing a module adds to the pool that birth rolls from. */
uint8_t epet_species_count(void)
{
    return epet_modules_species_count();
}

const epet_species_t *epet_species_builtin(uint8_t i)
{
    const epet_species_t *sp = epet_modules_species(i);
    if (sp) return sp;
    return epet_modules_species(0);   /* may still be NULL if nothing installed */
}

const epet_species_t *epet_species_by_name(const char *name)
{
    if (!name) return 0;
    for (uint8_t i = 0; i < epet_species_count(); i++) {
        const epet_species_t *sp = epet_modules_species(i);
        if (sp && strcmp(sp->name, name) == 0) return sp;
    }
    return 0;
}
