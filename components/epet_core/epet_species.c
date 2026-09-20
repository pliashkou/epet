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

/* ---- player ---------------------------------------------------------- */

void epet_actor_init(epet_actor_t *a, const epet_species_t *sp)
{
    a->sp = sp;
    a->pose = epet_species_pose_or_idle(sp, EPET_POSE_IDLE);
    a->resume = EPET_POSE_IDLE;
    a->key = 0;
    a->t_ms = 0;
    a->finished = false;
}

void epet_actor_play(epet_actor_t *a, const char *pose, const char *resume)
{
    const epet_pose_t *p = epet_species_pose_or_idle(a->sp, pose);
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
