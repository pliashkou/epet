#pragma once
#include "epet_sprite.h"

/* A character class.
 *
 * Adding a pose later means appending to `poses` -- nothing else changes,
 * because poses are resolved by name at runtime. Adding a species means
 * another epet_species_t and registering it. */

typedef struct {
    /* Per-stat decay multipliers, 1.0 = the baseline in epet_state.c.
     * This is what makes a class more than a recolour. */
    float hunger, happiness, energy, hygiene;
} epet_temperament_t;

/* The main-screen background. A species characteristic: each class lives
 * somewhere different.
 *
 * The image is a low-resolution indexed bitmap scaled up to fill the screen,
 * which costs a few KB instead of the ~58 KB a full 240x240 frame would.
 * It carries its OWN palette -- a backdrop's colours have nothing to do with
 * the creature's body colours. Leave `image` NULL and the flat sky/ground
 * colours are used instead, which is what a species with no art gets. */
typedef struct {
    const char           *name;        /* "MEADOW", "BEACH" */
    const epet_frame_t   *image;
    const epet_palette_t *palette;
    uint8_t               scale;       /* image->w * scale should cover 240 */
    uint16_t              sky, ground; /* fallback when image is NULL */
    uint8_t               horizon_y;   /* screen row the ground starts at */
    uint16_t              night;       /* tint blended over it while asleep */
    uint8_t               night_alpha;
} epet_backdrop_t;

/* Fills the whole screen. Safe with a zeroed backdrop. */
void epet_backdrop_draw(const epet_backdrop_t *bd, uint16_t *fb, bool night);

typedef struct epet_species {
    const char         *name;        /* "BLOB", "SPROUT" */
    const char         *blurb;       /* one line, shown on the ABOUT page */
    epet_palette_t      palette;
    const epet_pose_t  *poses;
    uint8_t             n_poses;
    uint8_t             scale;       /* sprite pixels -> screen pixels */
    /* One or more places this class lives. Which one a given pet gets is
     * rolled at birth alongside the class. */
    const epet_backdrop_t *backdrops;
    uint8_t                n_backdrops;
    /* What this creature leaves behind. NULL falls back to a generic pile,
     * so a species without art still works. Drawn with the body palette. */
    const epet_frame_t    *poop;
    epet_temperament_t  temper;
} epet_species_t;

/* Wraps the index; returns NULL only if the species has no backdrops. */
const epet_backdrop_t *epet_species_backdrop(const epet_species_t *sp, uint8_t i);

/* NULL if this species has no such pose. */
const epet_pose_t *epet_species_pose(const epet_species_t *sp, const char *name);
/* Resolves with a fallback chain, so a species need only define what it
 * cares about: birth -> idle, happy/sad -> idle. Never returns NULL for a
 * species that defines "idle". */
const epet_pose_t *epet_species_pose_or_idle(const epet_species_t *sp, const char *name);

/* Built-in roster. */
const epet_species_t *epet_species_builtin(uint8_t i);
uint8_t               epet_species_count(void);
const epet_species_t *epet_species_by_name(const char *name);

/* ---- the player ------------------------------------------------------ */

typedef struct {
    const epet_species_t *sp;
    const epet_pose_t    *pose;
    const char           *resume;   /* pose to return to when a one-shot ends */
    uint8_t               key;
    uint32_t              t_ms;
    bool                  finished; /* a non-looping pose has run out */
} epet_actor_t;

void epet_actor_init(epet_actor_t *a, const epet_species_t *sp);
/* Start a pose by name. `resume` is the pose to fall back to when this one
 * finishes; pass NULL for a pose that should hold on its last frame. */
void epet_actor_play(epet_actor_t *a, const char *pose, const char *resume);
/* Only starts it if a different pose is running -- safe to call every frame. */
void epet_actor_ensure(epet_actor_t *a, const char *pose);
void epet_actor_tick(epet_actor_t *a, uint32_t dt_ms);
void epet_actor_draw(const epet_actor_t *a, uint16_t *fb, int cx, int cy);
/* Same, at an explicit scale rather than the species' own -- info screens
 * want the character small enough to sit in a list. */
void epet_actor_draw_scaled(const epet_actor_t *a, uint16_t *fb,
                            int cx, int cy, int scale);
/* Silhouette, for shadows and flashes. */
void epet_actor_draw_tinted(const epet_actor_t *a, uint16_t *fb, int cx, int cy,
                            uint16_t colour, uint8_t alpha);
const char *epet_actor_pose_name(const epet_actor_t *a);
bool        epet_actor_finished(const epet_actor_t *a);
