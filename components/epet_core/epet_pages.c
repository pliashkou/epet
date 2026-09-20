#include "epet_pages.h"
#include "epet_species.h"
#include "epet_module.h"
#include "epet_draw.h"
#include <string.h>
#include <stdio.h>

/* ---- menu icons ------------------------------------------------------
 * Each page draws its own, so a new sub-program brings its own artwork. */

static void icon_feed(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("feed"), sel);
}

static void icon_play(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("play"), sel);
}

static void icon_wash(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("wash"), sel);
}

static void icon_stats(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("stats"), sel);
}

static void icon_sleep(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("sleep"), sel);
}

static void icon_about(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("about"), sel);
}

/* ---- shared chrome --------------------------------------------------- */

static void page_header(uint16_t *fb, const char *title, uint16_t accent)
{
    epet_fill(fb, EPET_C_PANEL);
    epet_rect(fb, 0, 0, EPET_W, 30, accent);
    int tw = epet_text_width(title, 2);
    epet_text(fb, (EPET_W - tw) / 2, 8, title, EPET_C_WHITE, 2);
}

static void page_footer(uint16_t *fb, const char *hint)
{
    epet_rect(fb, 0, EPET_H - 16, EPET_W, 16, EPET_C_MENU);
    epet_text(fb, 6, EPET_H - 12, hint, EPET_C_DIM, 1);
}

/* ---- an action page: does one thing, shows it, then closes ----------- */

typedef struct {
    epet_page_t   base;
    epet_action_t action;
    uint16_t      accent;
    const char   *ok_text;
    const char   *no_text;
    uint32_t      shown_ms;
    bool          succeeded;
    bool          done;
} action_page_t;

#define ACTION_HOLD_MS 1400

static void action_enter(epet_page_t *self, epet_ui_t *ui, epet_t *pet)
{
    (void)ui;
    action_page_t *a = (action_page_t *)self;
    a->shown_ms = 0;
    a->done = false;
    a->succeeded = epet_apply_action(pet, a->action);
}

static bool action_update(epet_page_t *self, epet_ui_t *ui, epet_t *pet,
                          uint32_t dt_ms, const bool edge[EPET_BTN_COUNT])
{
    (void)ui; (void)pet; (void)edge;
    action_page_t *a = (action_page_t *)self;
    a->shown_ms += dt_ms;
    return a->shown_ms < ACTION_HOLD_MS;      /* auto-close */
}

/* A small bouncing token so each action reads as an animation, not a flash. */
static void action_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    action_page_t *a = (action_page_t *)self;
    page_header(fb, self->title, a->accent);

    if (!a->succeeded) {
        const char *why = a->no_text;
        int tw = epet_text_width(why, 1);
        epet_text(fb, (EPET_W - tw) / 2, 120, why, EPET_C_BAD, 1);
        page_footer(fb, "RB BACK");
        return;
    }

    /* token arcs from the left toward the pet */
    float t = (float)a->shown_ms / (float)ACTION_HOLD_MS;
    if (t > 1.0f) t = 1.0f;
    int x = 40 + (int)(t * 120.0f);
    int y = 150 - (int)(70.0f * (1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f)));

    epet_disc(fb, 180, 150, 34, EPET_C_BODY);          /* the pet, simplified */
    epet_disc(fb, 172, 142, 6, EPET_C_BLACK);
    epet_disc(fb, 192, 142, 6, EPET_C_BLACK);
    epet_disc(fb, x, y, 10, a->accent);
    epet_disc(fb, x, y, 6, EPET_C_WHITE);

    int tw = epet_text_width(a->ok_text, 1);
    epet_text(fb, (EPET_W - tw) / 2, 200, a->ok_text, EPET_C_WHITE, 1);
    (void)pet;
}

#define ACTION_PAGE(ident, name, act, col, ic, ok, no)      \
    static action_page_t ident = {                          \
        .base = { .title = name,                            \
                  .enter = action_enter,                    \
                  .update = action_update,                  \
                  .render = action_render,                  \
                  .icon = ic },                             \
        .action = act, .accent = col,                       \
        .ok_text = ok, .no_text = no,                       \
    }

ACTION_PAGE(page_feed,  "FEED", EPET_ACT_FEED,  EPET_RGB565(230, 150, 60),
            icon_feed, "YUM", "NOT NOW - ASLEEP");
ACTION_PAGE(page_play,  "PLAY", EPET_ACT_PLAY,  EPET_RGB565(120, 190, 230),
            icon_play, "FUN", "NOT NOW - ASLEEP");
ACTION_PAGE(page_wash,  "WASH", EPET_ACT_CLEAN, EPET_RGB565(110, 200, 150),
            icon_wash, "CLEAN", "ALREADY CLEAN");

/* ---- stats: a read-only page with its own layout --------------------- */

static void stats_row(uint16_t *fb, int y, const char *label, float pct)
{
    epet_text(fb, 10, y + 1, label, EPET_C_WHITE, 1);
    epet_bar(fb, 62, y, 130, 9, pct, epet_level_colour(pct));
    epet_number(fb, 200, y + 1, (int)(pct + 0.5f), EPET_C_DIM, 1);
}

static void stats_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    page_header(fb, self->title, EPET_RGB565(90, 110, 170));
    stats_row(fb, 44,  "FED", 100.0f - pet->hunger);
    stats_row(fb, 60,  "JOY", pet->happiness);
    stats_row(fb, 76,  "PEP", pet->energy);
    stats_row(fb, 92,  "WSH", pet->hygiene);
    stats_row(fb, 108, "HP",  pet->health);

    epet_text(fb, 10, 132, "AGE", EPET_C_WHITE, 1);
    epet_number(fb, 62, 132, (int)(pet->age_ms / 1000u), EPET_C_DIM, 1);
    epet_text(fb, 110, 132, "SEC", EPET_C_DIM, 1);

    epet_text(fb, 10, 150, "MESS", EPET_C_WHITE, 1);
    epet_number(fb, 62, 150, pet->poop, EPET_C_DIM, 1);

    epet_text(fb, 10, 168, "MOOD", EPET_C_WHITE, 1);
    epet_text(fb, 62, 168, epet_mood_name(epet_mood(pet)), EPET_C_DIM, 1);

    page_footer(fb, "RB BACK");
}

static epet_page_t page_stats = { .title = "STATS", .render = stats_render,
                                  .icon = icon_stats };

/* ---- sleep: shows rest state, RT toggles the lights ------------------ */

typedef struct { epet_page_t base; bool lights_off; } sleep_page_t;

static bool sleep_update(epet_page_t *self, epet_ui_t *ui, epet_t *pet,
                         uint32_t dt_ms, const bool edge[EPET_BTN_COUNT])
{
    (void)ui; (void)dt_ms;
    sleep_page_t *s = (sleep_page_t *)self;
    if (edge[EPET_BTN_RT]) {
        s->lights_off = !s->lights_off;
        /* Turning the lights off blanks the panel right away. */
        if (s->lights_off) epet_display_nudge(pet, 0);
    }
    return true;
}

static void sleep_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    sleep_page_t *s = (sleep_page_t *)self;
    page_header(fb, self->title, EPET_RGB565(60, 70, 130));

    epet_text(fb, 10, 48, "PEP", EPET_C_WHITE, 1);
    epet_bar(fb, 62, 47, 150, 10, pet->energy, epet_level_colour(pet->energy));

    const char *state = pet->asleep ? "SLEEPING" : "AWAKE";
    int tw = epet_text_width(state, 2);
    epet_text(fb, (EPET_W - tw) / 2, 90, state, EPET_C_WHITE, 2);

    if (pet->asleep) {
        epet_text(fb, 150, 70, "Z", EPET_C_DIM, 2);
        epet_text(fb, 172, 56, "Z", EPET_C_DIM, 1);
    }

    epet_disc(fb, 120, 155, 34, pet->asleep ? EPET_C_BODYILL : EPET_C_BODY);
    epet_rect(fb, 104, 150, 14, 3, EPET_C_BLACK);
    epet_rect(fb, 124, 150, 14, 3, EPET_C_BLACK);

    page_footer(fb, s->lights_off ? "RT LIGHTS ON   RB BACK"
                                  : "RT LIGHTS OFF  RB BACK");
}

static sleep_page_t page_sleep = {
    .base = { .title = "SLEEP", .update = sleep_update, .render = sleep_render,
              .icon = icon_sleep },
};

/* ---- class picker: browse the roster and adopt one -------------------- */

typedef struct {
    epet_page_t base;
    uint8_t     shown;       /* which species is being previewed */
    epet_actor_t preview;    /* its own animator, independent of the pet */
} class_page_t;

static void class_enter(epet_page_t *self, epet_ui_t *ui, epet_t *pet)
{
    (void)ui;
    class_page_t *c = (class_page_t *)self;
    c->shown = 0;
    for (uint8_t i = 0; i < epet_species_count(); i++) {
        if (epet_species_builtin(i) == pet->species) c->shown = i;
    }
    epet_actor_init(&c->preview, epet_species_builtin(c->shown));
    epet_actor_play(&c->preview, EPET_POSE_HAPPY, EPET_POSE_IDLE);
}

static bool class_update(epet_page_t *self, epet_ui_t *ui, epet_t *pet,
                         uint32_t dt_ms, const bool edge[EPET_BTN_COUNT])
{
    (void)ui;
    class_page_t *c = (class_page_t *)self;
    uint8_t n = epet_species_count();

    int move = (edge[EPET_BTN_LB] ? 1 : 0) - (edge[EPET_BTN_LT] ? 1 : 0);
    if (move) {
        c->shown = (uint8_t)(((int)c->shown + move + n) % n);
        epet_actor_init(&c->preview, epet_species_builtin(c->shown));
        epet_actor_play(&c->preview, EPET_POSE_BIRTH, EPET_POSE_IDLE);
    }
    if (edge[EPET_BTN_RT]) {
        /* Adopting replays birth, so the change reads as a real event. */
        epet_set_species(pet, epet_species_builtin(c->shown), true);
        return false;
    }
    epet_actor_tick(&c->preview, dt_ms);
    return true;
}

static void class_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    class_page_t *c = (class_page_t *)self;
    const epet_species_t *sp = epet_species_builtin(c->shown);
    page_header(fb, self->title, EPET_RGB565(120, 100, 180));

    epet_actor_draw(&c->preview, fb, 120, 108);

    int tw = epet_text_width(sp->name, 2);
    epet_text(fb, (EPET_W - tw) / 2, 150, sp->name, EPET_C_WHITE, 2);
    tw = epet_text_width(sp->blurb, 1);
    epet_text(fb, (EPET_W - tw) / 2, 170, sp->blurb, EPET_C_DIM, 1);

    if (sp == pet->species) {
        epet_text(fb, 96, 190, "ADOPTED", EPET_C_GOOD, 1);
    }
    epet_number(fb, 196, 170, sp->n_backdrops, EPET_C_DIM, 1);
    epet_text(fb, 204, 170, "BG", EPET_C_DIM, 1);

    epet_number(fb, 8, 190, c->shown + 1, EPET_C_DIM, 1);
    epet_text(fb, 20, 190, "/", EPET_C_DIM, 1);
    epet_number(fb, 28, 190, epet_species_count(), EPET_C_DIM, 1);

    page_footer(fb, "LT LB BROWSE  RT ADOPT  RB BACK");
}

static void icon_class(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("class"), sel);
}

static class_page_t page_class = {
    .base = { .title = "CLASS", .enter = class_enter, .update = class_update,
              .render = class_render, .icon = icon_class },
};

/* ---- home: pick which of this class's backdrops to live in ------------ */

static bool home_update(epet_page_t *self, epet_ui_t *ui, epet_t *pet,
                        uint32_t dt_ms, const bool edge[EPET_BTN_COUNT])
{
    (void)self; (void)ui; (void)dt_ms;
    if (edge[EPET_BTN_LB] || edge[EPET_BTN_LT] || edge[EPET_BTN_RT]) {
        epet_pet_next_backdrop(pet);
    }
    return true;
}

static void home_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    const epet_backdrop_t *bd = epet_pet_backdrop(pet);

    /* Show it full-screen: this page IS the preview. */
    epet_backdrop_draw(bd, fb, false);
    epet_actor_draw(&pet->actor, fb, 120, 150);

    epet_shade(fb, 0, 0, EPET_W, 30, EPET_C_BLACK, 170);
    int tw = epet_text_width(self->title, 2);
    epet_text(fb, (EPET_W - tw) / 2, 8, self->title, EPET_C_WHITE, 2);

    if (bd && bd->name) {
        epet_shade(fb, 0, 34, EPET_W, 18, EPET_C_BLACK, 150);
        tw = epet_text_width(bd->name, 1);
        epet_text(fb, (EPET_W - tw) / 2, 39, bd->name, EPET_C_WHITE, 1);
    }

    uint8_t n = pet->species ? pet->species->n_backdrops : 0;
    epet_shade(fb, 0, EPET_H - 16, EPET_W, 16, EPET_C_BLACK, 170);
    epet_number(fb, 6, EPET_H - 12, pet->backdrop + 1, EPET_C_DIM, 1);
    epet_text(fb, 18, EPET_H - 12, "/", EPET_C_DIM, 1);
    epet_number(fb, 26, EPET_H - 12, n, EPET_C_DIM, 1);
    epet_text(fb, 52, EPET_H - 12, "ANY KEY NEXT  RB BACK", EPET_C_DIM, 1);
}

static void icon_home(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("home"), sel);
}

static epet_page_t page_home = { .title = "HOME", .update = home_update,
                                 .render = home_render, .icon = icon_home };

/* ---- modules: what is installed --------------------------------------- */

typedef struct { epet_page_t base; uint8_t top; } modules_page_t;

static void modules_enter(epet_page_t *self, epet_ui_t *ui, epet_t *pet)
{
    (void)ui; (void)pet;
    ((modules_page_t *)self)->top = 0;
}

static bool modules_update(epet_page_t *self, epet_ui_t *ui, epet_t *pet,
                           uint32_t dt_ms, const bool edge[EPET_BTN_COUNT])
{
    (void)ui; (void)pet; (void)dt_ms;
    modules_page_t *m = (modules_page_t *)self;
    uint8_t n = epet_modules_count();
    if (edge[EPET_BTN_LB] && n) m->top = (uint8_t)((m->top + 1) % n);
    if (edge[EPET_BTN_LT] && n) m->top = (uint8_t)((m->top + n - 1) % n);
    return true;
}

static void modules_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    (void)pet;
    modules_page_t *m = (modules_page_t *)self;
    page_header(fb, self->title, EPET_RGB565(70, 130, 150));

    uint8_t n = epet_modules_count();
    int y = 40;
    for (uint8_t row = 0; row < 4 && row < n; row++) {
        const epet_module_t *mod = epet_modules_get((uint8_t)((m->top + row) % n));
        if (!mod) continue;
        bool first = (row == 0);
        if (first) epet_shade(fb, 4, y - 3, EPET_W - 8, 40, EPET_C_MENUSEL, 120);

        epet_text(fb, 10, y, mod->name, first ? EPET_C_WHITE : EPET_C_DIM, 1);
        epet_text(fb, 10, y + 13, "V", EPET_C_DIM, 1);
        epet_number(fb, 18, y + 13, mod->version, EPET_C_DIM, 1);

        epet_number(fb, 60, y + 13, mod->n_species, EPET_C_GOOD, 1);
        epet_text(fb, 68, y + 13, "CHR", EPET_C_DIM, 1);
        epet_number(fb, 110, y + 13, mod->n_pages, EPET_C_GOOD, 1);
        epet_text(fb, 118, y + 13, "APP", EPET_C_DIM, 1);
        epet_text(fb, 160, y + 13, mod->id, EPET_C_DIM, 1);
        y += 44;
    }

    epet_text(fb, 10, 196, "TOTAL", EPET_C_WHITE, 1);
    epet_number(fb, 52, 196, n, EPET_C_GOOD, 1);
    epet_text(fb, 66, 196, "MODULES", EPET_C_DIM, 1);
    epet_number(fb, 140, 196, epet_species_count(), EPET_C_GOOD, 1);
    epet_text(fb, 154, 196, "CHARS", EPET_C_DIM, 1);

    page_footer(fb, "LT LB SCROLL   RB BACK");
}

static void icon_modules(epet_page_t *self, uint16_t *fb, int cx, int cy,
                      uint16_t tint, bool sel)
{
    (void)self; (void)tint;
    epet_icon_draw(fb, cx, cy, epet_icon("mods"), sel);
}

static modules_page_t page_modules_impl = {
    .base = { .title = "MODS", .enter = modules_enter, .update = modules_update,
              .render = modules_render, .icon = icon_modules },
};
#define page_modules (page_modules_impl.base)

/* ---- about: proves a page can be purely cosmetic --------------------- */

static void about_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    page_header(fb, self->title, EPET_RGB565(150, 90, 160));
    epet_text(fb, 16, 44, "EPET", EPET_C_WHITE, 3);
    epet_text(fb, 16, 76, "ESP32-S3 POCKET PET", EPET_C_DIM, 1);

    if (pet->species) {
        const epet_module_t *owner = epet_modules_owner_of(pet->species);
        epet_text(fb, 16, 100, "CLASS", EPET_C_WHITE, 1);
        epet_text(fb, 64, 100, pet->species->name, EPET_C_GOOD, 1);
        if (owner) epet_text(fb, 140, 100, owner->id, EPET_C_DIM, 1);
        epet_text(fb, 16, 116, pet->species->blurb, EPET_C_DIM, 1);
        epet_text(fb, 16, 134, "POSE", EPET_C_WHITE, 1);
        epet_text(fb, 64, 134, epet_actor_pose_name(&pet->actor), EPET_C_DIM, 1);
    }

    epet_text(fb, 16, 158, "LT LB  SCROLL MENU", EPET_C_DIM, 1);
    epet_text(fb, 16, 172, "RT     SELECT", EPET_C_DIM, 1);
    epet_text(fb, 16, 186, "RB     BACK", EPET_C_DIM, 1);
    page_footer(fb, "RB BACK");
}

static epet_page_t page_about = { .title = "ABOUT", .render = about_render,
                                  .icon = icon_about };

/* ---- registration ---------------------------------------------------- */

static epet_page_t *const BUILTIN[] = {
    &page_feed.base,
    &page_play.base,
    &page_wash.base,
    &page_stats,
    &page_sleep.base,
    &page_class.base,
    &page_home,
    &page_modules,
    &page_about,
};

epet_page_t *const *epet_pages_builtin(uint8_t *n_out)
{
    if (n_out) *n_out = (uint8_t)(sizeof(BUILTIN) / sizeof(BUILTIN[0]));
    return BUILTIN;
}

void epet_pages_register_builtin(epet_ui_t *ui)
{
    /* Keep standalone callers (tests, minimal builds) working: make sure the
     * core module is installed, then register from the registry so module
     * order is the same as it would be in the firmware. */
    if (!epet_modules_find("core")) {
        epet_modules_install(epet_module_core_get(), 0);
    }
    epet_modules_populate_ui(ui);
}
