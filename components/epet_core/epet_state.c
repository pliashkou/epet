#include <stddef.h>
#include "epet.h"
#include "epet_module.h"

/* Decay rates are per second, tuned fast so a dev session shows a full
 * life-cycle in minutes rather than days. Slow these down for real play. */
#define HUNGER_RATE     1.20f
#define HAPPY_RATE      0.60f
#define ENERGY_RATE     0.45f
#define HYGIENE_RATE    0.35f

#define SLEEP_ENERGY    2.50f   /* energy regained per second asleep */
#define SLEEP_ENTER     15.0f   /* falls asleep below this energy */
#define SLEEP_EXIT      95.0f   /* wakes above this energy */

#define POOP_INTERVAL   12000   /* ms between piles once fed */
#define FLASH_MS        450

/* ---- randomness ------------------------------------------------------ */

static epet_t *g_active;

void epet_set_active(epet_t *p) { g_active = p; }
epet_t *epet_active(void) { return g_active; }

static uint32_t rng_state = 0x6D2B79F5u;   /* fixed: tests stay reproducible */

void epet_seed_random(uint32_t seed)
{
    rng_state = seed ? seed : 0x6D2B79F5u;  /* xorshift dies on zero */
    /* Mix, then warm up. Without this, xorshift32 seeded with small
     * sequential integers leaks the seed's low bit straight into the first
     * output -- seeds 1,2,3,4 alternated classes perfectly. */
    rng_state ^= 0x9E3779B9u;
    rng_state *= 0x85EBCA6Bu;
    rng_state ^= rng_state >> 15;
    if (rng_state == 0) rng_state = 0x6D2B79F5u;
    for (int i = 0; i < 8; i++) (void)epet_random();
}

uint32_t epet_random(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

uint32_t epet_random_below(uint32_t n)
{
    /* Take the high bits: xorshift's low bits are the weakest. */
    return n ? (epet_random() >> 16) % n : 0;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- button layout -------------------------------------------------- */

const char *epet_button_name(epet_btn_t b)
{
    switch (b) {
    case EPET_BTN_LT: return "LT";
    case EPET_BTN_LB: return "LB";
    case EPET_BTN_RT: return "RT";
    case EPET_BTN_RB: return "RB";
    default: return "?";
    }
}

const char *epet_action_name(epet_action_t a)
{
    switch (a) {
    case EPET_ACT_FEED:    return "FEED";
    case EPET_ACT_PLAY:    return "PLAY";
    case EPET_ACT_CLEAN:   return "CLEAN";
    case EPET_ACT_REVIVE:  return "REVIVE";
    default: return "NONE";
    }
}

/* Reset the stats but keep the bus wiring, so a rebirth does not silently
 * unsubscribe every module. */
uint8_t epet_age_level(const epet_t *p)
{
    if (!p) return 1;
    uint32_t per = p->age_level_ms ? p->age_level_ms : EPET_AGE_LEVEL_MS_DEFAULT;
    uint32_t lvl = p->age_ms / per + 1u;      /* a newborn is age 1, not 0 */
    return (uint8_t)(lvl > EPET_AGE_MAX ? EPET_AGE_MAX : lvl);
}

static void reset_stats(epet_t *p)
{
    epet_bus_t *bus = p->bus;
    const epet_species_t *sp = p->species;
    uint32_t revive_ms = p->revive_hold_ms;
    uint32_t blank_ms  = p->display_timeout_ms;
    uint32_t agelvl_ms = p->age_level_ms;
    *p = (epet_t){
        .hunger    = 20.0f,
        .happiness = 80.0f,
        .energy    = 90.0f,
        .hygiene   = 100.0f,
        .health    = 100.0f,
        .asleep    = false,
        .alive     = true,
        .display_on = true,
        .idle_ms    = 0,
        .display_timeout_ms = EPET_DISPLAY_TIMEOUT_MS_DEFAULT,
        .revive_hold_ms     = EPET_REVIVE_HOLD_MS_DEFAULT,
        .age_level_ms       = EPET_AGE_LEVEL_MS_DEFAULT,
    };
    p->bus = bus;
    if (revive_ms) p->revive_hold_ms = revive_ms;
    if (blank_ms)  p->display_timeout_ms = blank_ms;
    if (agelvl_ms) p->age_level_ms = agelvl_ms;
    (void)sp;
    /* A birth is a new creature: roll its class from every installed
     * module's characters. The CLASS page can still override afterwards, but
     * the next birth rolls again. */
    if (epet_species_count() == 0) {
        /* Nothing installed yet -- a bare epet_init() should still work. */
        epet_modules_install(epet_module_core_get(), 0);
    }
    const epet_species_t *rolled =
        epet_species_builtin((uint8_t)epet_random_below(epet_species_count()));
    p->species = rolled;
    if (rolled) {
        epet_actor_init(&p->actor, rolled);
        epet_actor_play(&p->actor, EPET_POSE_BIRTH, EPET_POSE_IDLE);
        /* and roll where it lives */
        p->backdrop = (uint8_t)epet_random_below(rolled->n_backdrops
                                                 ? rolled->n_backdrops : 1);
    }
}

void epet_init(epet_t *p)
{
    /* reset_stats() PRESERVES these across a rebirth, which means it reads
     * them before clearing the struct. On a first call that would be
     * uninitialised stack, so clear every preserved field here first --
     * a stray value once made the death screen ask for a 180 second shake. */
    p->bus = NULL;
    p->species = NULL;
    p->revive_hold_ms = 0;
    p->display_timeout_ms = 0;
    p->age_level_ms = 0;
    reset_stats(p);         /* rolls the class and starts the birth pose */
}

void epet_set_species(epet_t *p, const epet_species_t *sp, bool play_birth)
{
    if (!sp) return;
    p->species = sp;
    p->backdrop = (uint8_t)epet_random_below(sp->n_backdrops ? sp->n_backdrops : 1);
    epet_actor_init(&p->actor, sp);
    if (play_birth) {
        epet_actor_play(&p->actor, EPET_POSE_BIRTH, EPET_POSE_IDLE);
    }
}

void epet_pet_emote(epet_t *p, const char *pose)
{
    epet_actor_play(&p->actor, pose, EPET_POSE_IDLE);
}

const epet_backdrop_t *epet_pet_backdrop(const epet_t *p)
{
    return epet_species_backdrop(p->species, p->backdrop);
}

void epet_pet_next_backdrop(epet_t *p)
{
    if (p->species && p->species->n_backdrops) {
        p->backdrop = (uint8_t)((p->backdrop + 1) % p->species->n_backdrops);
    }
}

/* Temperament multipliers, 1.0 when no species is set. */
static epet_temperament_t temper_of(const epet_t *p)
{
    if (p->species) return p->species->temper;
    return (epet_temperament_t){ 1.0f, 1.0f, 1.0f, 1.0f };
}

void epet_attach_bus(epet_t *p, epet_bus_t *bus)
{
    p->bus = bus;
}

void epet_display_nudge(epet_t *p, uint32_t show_ms)
{
    p->display_on = true;
    /* Leave just show_ms of the idle budget so it blanks again shortly. */
    p->idle_ms = (p->display_timeout_ms > show_ms)
               ? p->display_timeout_ms - show_ms : 0;
}

static void emit(epet_t *p, epet_event_type_t t, int32_t a, int32_t b)
{
    epet_bus_publish(p->bus, t, p->age_ms, a, b);
}

/* Latching threshold with hysteresis: fires once on entry, re-arms only
 * after the value recovers past the clear point. */
static bool cross(bool *latched, float value, float set, float clear, bool rising)
{
    if (!*latched) {
        if (rising ? (value >= set) : (value <= set)) { *latched = true; return true; }
    } else if (rising ? (value <= clear) : (value >= clear)) {
        *latched = false;
    }
    return false;
}

epet_mood_t epet_mood(const epet_t *p)
{
    if (!p->alive)          return EPET_MOOD_DEAD;
    if (p->asleep)          return EPET_MOOD_ASLEEP;
    if (p->health < 40.0f)  return EPET_MOOD_SICK;
    if (p->happiness < 30.0f || p->hunger > 75.0f) return EPET_MOOD_SAD;
    if (p->happiness > 65.0f && p->hunger < 40.0f) return EPET_MOOD_HAPPY;
    return EPET_MOOD_NEUTRAL;
}

const char *epet_mood_name(epet_mood_t m)
{
    switch (m) {
    case EPET_MOOD_HAPPY:   return "HAPPY";
    case EPET_MOOD_NEUTRAL: return "OK";
    case EPET_MOOD_SAD:     return "SAD";
    case EPET_MOOD_SICK:    return "SICK";
    case EPET_MOOD_ASLEEP:  return "ZZZ";
    case EPET_MOOD_DEAD:    return "GONE";
    }
    return "?";
}

static void tick_down(uint32_t *ms, uint32_t dt)
{
    *ms = (*ms > dt) ? *ms - dt : 0;
}

uint32_t epet_update(epet_t *p, uint32_t dt_ms,
                     const bool btn_down[EPET_BTN_COUNT],
                     const bool btn_edge[EPET_BTN_COUNT])
{
    (void)btn_down;
    const float dt = dt_ms / 1000.0f;

    p->anim_ms += dt_ms;
    tick_down(&p->feed_flash_ms,  dt_ms);
    tick_down(&p->play_flash_ms,  dt_ms);
    tick_down(&p->clean_flash_ms, dt_ms);

    /* --- display power ------------------------------------------------ */
    bool any_edge = false;
    for (int i = 0; i < EPET_BTN_COUNT; i++) {
        if (btn_edge[i]) any_edge = true;
    }

    bool wake_press = false;
    if (any_edge) {
        p->idle_ms = 0;
        if (!p->display_on) {
            p->display_on = true;
            wake_press = true;   /* this press only wakes; it does not act */
            emit(p, EPET_EV_DISPLAY_ON, 0, 0);
        }
    } else {
        p->idle_ms += dt_ms;
        if (p->display_timeout_ms && p->idle_ms >= p->display_timeout_ms) {
            if (p->display_on) emit(p, EPET_EV_DISPLAY_OFF, 0, 0);
            p->display_on = false;
        }
    }

    /* Edges that survive the wake-swallow. Returned so the UI can trust them. */
    bool edge[EPET_BTN_COUNT];
    uint32_t surviving = 0;
    for (int i = 0; i < EPET_BTN_COUNT; i++) {
        edge[i] = wake_press ? false : btn_edge[i];
        if (edge[i]) surviving |= EPET_BTN_BIT(i);
    }

    for (int i = 0; i < EPET_BTN_COUNT; i++) {
        if (edge[i]) emit(p, EPET_EV_BUTTON, i, 0);
    }

    if (!p->alive) {
        /* Covers every route into death: dying now, loading a dead save, or
         * a module removing the class out from under a corpse. */
        epet_actor_ensure(&p->actor, EPET_POSE_DEAD);
        epet_actor_set_age(&p->actor, epet_age_level(p));
        epet_actor_tick(&p->actor, dt_ms);
        return surviving;
    }

    p->age_ms += dt_ms;

    uint32_t minute = p->age_ms / 60000u;
    while (p->ev_minute < minute) {
        p->ev_minute++;
        emit(p, EPET_EV_MINUTE, (int32_t)p->ev_minute, 0);
    }

    /* --- sleep cycle -------------------------------------------------- */
    if (p->asleep) {
        p->energy = clampf(p->energy + SLEEP_ENERGY * dt, 0.0f, 100.0f);
        if (p->energy >= SLEEP_EXIT) {
            p->asleep = false;
            emit(p, EPET_EV_WOKE, 0, 0);
        }
    } else {
        p->energy = clampf(p->energy - ENERGY_RATE * temper_of(p).energy * dt,
                           0.0f, 100.0f);
        if (p->energy <= SLEEP_ENTER) {
            p->asleep = true;
            emit(p, EPET_EV_FELL_ASLEEP, 0, 0);
        }
    }

    /* --- needs drift -------------------------------------------------- */
    const epet_temperament_t tp = temper_of(p);
    p->hunger  = clampf(p->hunger + HUNGER_RATE * tp.hunger * dt, 0.0f, 100.0f);
    p->hygiene = clampf(p->hygiene - HYGIENE_RATE * tp.hygiene * dt * (1.0f + p->poop),
                        0.0f, 100.0f);
    if (!p->asleep) {
        p->happiness = clampf(p->happiness - HAPPY_RATE * tp.happiness * dt,
                              0.0f, 100.0f);
    }

    /* --- poop --------------------------------------------------------- */
    if (p->poop < 4 && p->age_ms > 0 &&
        (p->age_ms % POOP_INTERVAL) < dt_ms && p->hunger < 90.0f) {
        p->poop++;
        emit(p, EPET_EV_POOPED, (int32_t)p->poop, 0);
    }

    /* --- health ------------------------------------------------------- */
    float strain = 0.0f;
    if (p->hunger    > 80.0f) strain += (p->hunger - 80.0f) * 0.05f;
    if (p->hygiene   < 25.0f) strain += (25.0f - p->hygiene) * 0.04f;
    if (p->happiness < 15.0f) strain += (15.0f - p->happiness) * 0.03f;

    if (strain > 0.0f) {
        p->health = clampf(p->health - strain * dt, 0.0f, 100.0f);
    } else {
        p->health = clampf(p->health + 1.5f * dt, 0.0f, 100.0f);
    }

    /* Need thresholds, latched with hysteresis so they fire once per episode. */
    if (cross(&p->ev_hungry, p->hunger,    75.0f, 60.0f, true))
        emit(p, EPET_EV_HUNGRY, (int32_t)p->hunger, 0);
    if (cross(&p->ev_sad,    p->happiness, 30.0f, 45.0f, false))
        emit(p, EPET_EV_SAD, (int32_t)p->happiness, 0);
    if (cross(&p->ev_dirty,  p->hygiene,   25.0f, 40.0f, false))
        emit(p, EPET_EV_DIRTY, (int32_t)p->hygiene, 0);

    bool was_sick = p->ev_sick;
    if (cross(&p->ev_sick,   p->health,    40.0f, 55.0f, false))
        emit(p, EPET_EV_SICK, (int32_t)p->health, 0);
    if (was_sick && !p->ev_sick)
        emit(p, EPET_EV_RECOVERED, (int32_t)p->health, 0);

    if (p->health <= 0.0f) {
        p->alive  = false;
        p->asleep = false;
        epet_actor_play(&p->actor, EPET_POSE_DEAD, 0);
        emit(p, EPET_EV_DIED, (int32_t)(p->age_ms / 1000u), 0);
    }

    /* Looping poses follow the mood; a one-shot (birth, happy) plays out
     * first and returns here on its own via the actor's resume pose. */
    if (p->actor.pose && p->actor.pose->loop) {
        epet_mood_t m = epet_mood(p);
        epet_actor_ensure(&p->actor,
            (m == EPET_MOOD_SAD || m == EPET_MOOD_SICK) ? EPET_POSE_SAD
                                                        : EPET_POSE_IDLE);
    }
    epet_actor_set_age(&p->actor, epet_age_level(p));
    epet_actor_tick(&p->actor, dt_ms);

    return surviving;
}

bool epet_apply_action(epet_t *p, epet_action_t a)
{
    if (a == EPET_ACT_REVIVE) {
        if (p->alive) return false;
        reset_stats(p);
        emit(p, EPET_EV_REBORN, 0, 0);
        return true;
    }
    if (!p->alive) return false;

    switch (a) {
    case EPET_ACT_FEED:
        if (p->asleep) return false;
        p->hunger    = clampf(p->hunger - 25.0f, 0.0f, 100.0f);
        p->happiness = clampf(p->happiness + 4.0f, 0.0f, 100.0f);
        p->feed_flash_ms = FLASH_MS;
        epet_pet_emote(p, EPET_POSE_HAPPY);
        emit(p, EPET_EV_FED, (int32_t)p->hunger, 0);
        return true;

    case EPET_ACT_PLAY:
        if (p->asleep) return false;
        p->happiness = clampf(p->happiness + 18.0f, 0.0f, 100.0f);
        p->energy    = clampf(p->energy - 8.0f, 0.0f, 100.0f);
        p->hunger    = clampf(p->hunger + 5.0f, 0.0f, 100.0f);
        p->play_flash_ms = FLASH_MS;
        epet_pet_emote(p, EPET_POSE_HAPPY);
        emit(p, EPET_EV_PLAYED, (int32_t)p->happiness, 0);
        return true;

    case EPET_ACT_CLEAN:
        if (p->poop == 0 && p->hygiene > 95.0f) return false;
        if (p->poop > 0) p->poop--;
        p->hygiene = clampf(p->hygiene + 30.0f, 0.0f, 100.0f);
        p->clean_flash_ms = FLASH_MS;
        emit(p, EPET_EV_CLEANED, (int32_t)p->poop, 0);
        return true;

    default:
        return false;
    }
}

bool epet_validate_species(epet_t *p)
{
    if (!p->species) { reset_stats(p); return true; }

    for (uint8_t i = 0; i < epet_species_count(); i++) {
        if (epet_species_builtin(i) == p->species) {
            /* Still here, but its backdrop list may have changed under us. */
            if (p->species->n_backdrops &&
                p->backdrop >= p->species->n_backdrops) {
                p->backdrop = 0;
            }
            return false;
        }
    }

    /* The class went away with its module. Nothing about this creature is
     * recoverable, so start a new one from what is installed. */
    reset_stats(p);
    emit(p, EPET_EV_REBORN, 0, 0);
    return true;
}

uint32_t epet_advance(epet_t *p, uint32_t elapsed_ms,
                      const bool btn_down[EPET_BTN_COUNT],
                      const bool btn_edge[EPET_BTN_COUNT])
{
    static const bool none[EPET_BTN_COUNT] = {false};

    uint32_t remaining = elapsed_ms;
    while (remaining > EPET_MAX_STEP_MS) {
        epet_update(p, EPET_MAX_STEP_MS, none, none);
        remaining -= EPET_MAX_STEP_MS;
    }
    /* Final step carries the input: the gap elapsed before the press. */
    return epet_update(p, remaining, btn_down, btn_edge);
}
