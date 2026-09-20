#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epet_event.h"
#include "epet_species.h"

/* Platform-independent core: no ESP-IDF, no SDL. Compiles for device and host. */

#define EPET_W 240
#define EPET_H 240

/* Blank the display after this much time with no button activity.
 * 0 disables the timeout. Platform layers may override it after epet_init(). */
#define EPET_DISPLAY_TIMEOUT_MS_DEFAULT 30000
/* How long a dead pet must be shaken to start a new one. The core does not
 * do the shaking -- it only needs the figure so the death screen can say
 * what to do. Platforms overwrite it with their own setting. */
#define EPET_REVIVE_HOLD_MS_DEFAULT 5000
/* How long one age level lasts. The pet's age level runs 1..EPET_AGE_MAX and
 * drives which growth stage it draws; see epet_species.h. Age itself keeps
 * climbing past the top level, it just stops changing how the pet looks.
 * The default puts a full childhood inside the fast development tuning;
 * raise it along with the decay rates for real play. */
#define EPET_AGE_LEVEL_MS_DEFAULT 3000

/* Framebuffer holds panel-ready RGB565 (MSB first); see epet_draw.h. */
#define EPET_RGB(r, g, b) \
    ((uint16_t)__builtin_bswap16( \
        (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))))

/* Buttons are named by physical position, not meaning: two down the left of
 * the screen, two down the right. What each one DOES lives in one place,
 * the action map below, so the layout can be re-assigned without touching
 * the simulation. */
typedef enum {
    EPET_BTN_LT = 0,   /* left  top    */
    EPET_BTN_LB,       /* left  bottom */
    EPET_BTN_RT,       /* right top    */
    EPET_BTN_RB,       /* right bottom */
    EPET_BTN_COUNT
} epet_btn_t;

/* What the pet can be asked to do. Buttons no longer map to these directly;
 * the UI (menu + pages) decides, and calls epet_apply_action(). */
typedef enum {
    EPET_ACT_NONE = 0,
    EPET_ACT_FEED,
    EPET_ACT_PLAY,
    EPET_ACT_CLEAN,
    EPET_ACT_REVIVE,
    EPET_ACT_COUNT
} epet_action_t;

const char *epet_button_name(epet_btn_t b);
const char *epet_action_name(epet_action_t a);

typedef enum {
    EPET_MOOD_HAPPY,
    EPET_MOOD_NEUTRAL,
    EPET_MOOD_SAD,
    EPET_MOOD_SICK,
    EPET_MOOD_ASLEEP,
    EPET_MOOD_DEAD
} epet_mood_t;

typedef struct {
    float    hunger;      /* 0 = full .. 100 = starving */
    float    happiness;   /* 0 = miserable .. 100 = delighted */
    float    energy;      /* 0 = exhausted .. 100 = rested */
    float    hygiene;     /* 0 = filthy .. 100 = clean */
    float    health;      /* 0 = dead .. 100 = healthy */
    bool     asleep;
    bool     alive;
    uint32_t age_ms;
    uint8_t  poop;        /* number of piles on screen */

    /* Display power. The pet keeps living while the panel is blanked; only
     * the output sleeps. The press that wakes the display is swallowed so it
     * cannot also trigger an action. */
    bool     display_on;
    uint32_t idle_ms;
    uint32_t display_timeout_ms;
    uint32_t revive_hold_ms;   /* shown on the death screen */
    uint32_t age_level_ms;     /* wall time per age level; see the default */

    /* Event bus, optional. NULL means events are simply not raised, so the
     * core still works standalone (tests, minimal builds). */
    epet_bus_t *bus;

    /* Internal edge-tracking for event generation -- do not poke directly. */
    uint32_t ev_minute;
    bool     ev_hungry, ev_sad, ev_dirty, ev_sick;

    /* Character class: drives artwork AND stat decay via its temperament. */
    const epet_species_t *species;
    epet_actor_t          actor;
    uint8_t               backdrop;   /* index into species->backdrops */

    /* presentation-only state */
    uint32_t anim_ms;
    uint32_t feed_flash_ms;
    uint32_t play_flash_ms;
    uint32_t clean_flash_ms;
} epet_t;

/* Longest simulation step epet_advance() will take in one go. Integration
 * and periodic events (poop, sleep transitions) stay accurate below this. */
#define EPET_MAX_STEP_MS 100

/* Apply an action. Returns false if it was refused (asleep, dead, nothing
 * to clean). Raises the matching event. */
bool        epet_apply_action(epet_t *p, epet_action_t a);

/* Small xorshift PRNG, owned by the core so species selection behaves the
 * same on device and in the simulator. It starts from a FIXED seed, which
 * keeps tests reproducible; platforms call epet_seed_random() with real
 * entropy at startup so a real pet is not always the same class. */
/* The pet a background handler should act on. Set once by the platform;
 * loaded modules receive events without any pointer of their own. */
void        epet_set_active(epet_t *p);
epet_t     *epet_active(void);

void        epet_seed_random(uint32_t seed);
uint32_t    epet_random(void);
uint32_t    epet_random_below(uint32_t n);

void        epet_init(epet_t *p);
/* Swap the character class. Keeps the pet's stats; restarts the animation.
 * Pass play_birth to replay the hatching sequence. */
void        epet_set_species(epet_t *p, const epet_species_t *sp, bool play_birth);
/* Current age level, 1..EPET_AGE_MAX. Clamped at the top: a pet older than
 * that keeps ageing but stops growing. Drives the main-screen size and which
 * growth stage's artwork is drawn. */
uint8_t epet_age_level(const epet_t *p);

/* Shortcut for pages: play a pose and fall back to the mood-driven loop. */
void        epet_pet_emote(epet_t *p, const char *pose);
/* If the pet's class is no longer in the installed pool -- its module was
 * removed -- start a fresh creature from what IS available. Returns true if
 * it had to do that. Call after installing or removing modules. */
bool        epet_validate_species(epet_t *p);
/* The backdrop this pet is currently living in; NULL if the class has none. */
const epet_backdrop_t *epet_pet_backdrop(const epet_t *p);
/* Cycle to the next backdrop this class offers. */
void        epet_pet_next_backdrop(epet_t *p);
/* Attach after epet_init(). Survives death/rebirth. */
void        epet_attach_bus(epet_t *p, epet_bus_t *bus);
/* Force the display on for roughly show_ms, then let it blank as usual.
 * Used by alert handlers so the pet can ask for attention. */
void        epet_display_nudge(epet_t *p, uint32_t show_ms);
/* Advance time, decay needs, drive display power, raise events.
 *
 * btn_edge[] = went down this frame; btn_down[] = currently held.
 * Returns a bitmask of the buttons whose edge SURVIVED: a press that merely
 * woke a blanked display is swallowed and will not appear. Feed that mask to
 * the UI so the waking press cannot also trigger a menu action.
 * Buttons themselves do nothing here -- see epet_apply_action(). */
uint32_t    epet_update(epet_t *p, uint32_t dt_ms,
                        const bool btn_down[EPET_BTN_COUNT],
                        const bool btn_edge[EPET_BTN_COUNT]);
#define EPET_BTN_BIT(b) (1u << (b))
/* Advance by an arbitrary elapsed time in bounded steps. Use after a long
 * gap -- e.g. waking from light sleep -- instead of one huge epet_update().
 * The elapsed time happened BEFORE the button press, so the edges are applied
 * to the final step only. */
uint32_t    epet_advance(epet_t *p, uint32_t elapsed_ms,
                         const bool btn_down[EPET_BTN_COUNT],
                         const bool btn_edge[EPET_BTN_COUNT]);
epet_mood_t epet_mood(const epet_t *p);
const char *epet_mood_name(epet_mood_t m);
void        epet_render(const epet_t *p, uint16_t *fb);
/* Main screen including the menu; ui may be NULL. */
struct epet_ui;
void        epet_render_main(const struct epet_ui *ui, const epet_t *p, uint16_t *fb);
