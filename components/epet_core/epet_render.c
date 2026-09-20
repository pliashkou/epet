#include "epet.h"
#include "epet_draw.h"
#include "epet_ui.h"
#include "epet_species.h"

/* Main screen: status panel across the top, scrolling menu down the left,
 * the pet in the remaining area. Pages draw their own screens instead. */

#define PANEL_H   46
#define PET_CX    (EPET_W / 2)
/* Sprites carry headroom above the body (SPROUT's leaf), so the centre
 * sits lower than the visual middle to keep the feet on the ground. */
#define PET_CY    156
#define PET_R     44

static void stat_bar(uint16_t *fb, int x, int y, const char *label,
                     float pct, bool invert)
{
    float shown = invert ? (100.0f - pct) : pct;
    epet_text(fb, x, y, label, EPET_C_WHITE, 1);
    epet_bar(fb, x + 26, y, 78, 7, shown, epet_level_colour(shown));
}

static void draw_pet(uint16_t *fb, const epet_t *p, epet_mood_t mood)
{
    int bob = 0;
    if (mood != EPET_MOOD_DEAD) {
        uint32_t phase = (p->anim_ms / 250) % 4;
        bob = (phase == 1) ? -2 : (phase == 3) ? 2 : 0;
    }
    const int cx = PET_CX, cy = PET_CY + bob;

    /* soft contact shadow so the sprite is not floating */
    epet_shade(fb, cx - 30, cy + 40, 60, 7, EPET_C_BLACK, 60);

    if (mood == EPET_MOOD_DEAD) {
        epet_actor_draw_tinted(&p->actor, fb, cx, cy, EPET_RGB565(120, 120, 130), 220);
        return;
    }
    epet_actor_draw(&p->actor, fb, cx, cy);

    if (mood == EPET_MOOD_ASLEEP) {
        epet_text(fb, cx + 34, cy - 44, "Z", EPET_C_WHITE, 2);
        epet_text(fb, cx + 50, cy - 30, "Z", EPET_C_WHITE, 1);
    }
}

void epet_render_main(const epet_ui_t *ui, const epet_t *p, uint16_t *fb)
{
    epet_mood_t mood = epet_mood(p);
    bool night = p->asleep;

    /* The backdrop is a species characteristic: each class lives somewhere. */
    epet_backdrop_draw(epet_pet_backdrop(p), fb, night);

    for (int i = 0; i < p->poop; i++) {
        int px = EPET_MENU_W + 16 + i * 40;   /* keep clear of the menu rail */
        const epet_frame_t *art = p->species ? p->species->poop : 0;
        if (art) {
            epet_blit_centred(fb, px, 202, art, &p->species->palette,
                              p->species->scale);
        } else {
            epet_disc(fb, px, 206, 6, EPET_C_POOP);
            epet_disc(fb, px, 200, 4, EPET_C_POOP);
        }
    }

    draw_pet(fb, p, mood);

    /* status panel first; the menu overlay is drawn last so it floats on top */
    epet_rect(fb, 0, 0, EPET_W, PANEL_H, EPET_C_PANEL);
    stat_bar(fb, 6,   5, "FED", p->hunger, true);
    stat_bar(fb, 6,  16, "JOY", p->happiness, false);
    stat_bar(fb, 6,  27, "PEP", p->energy, false);
    stat_bar(fb, 122, 5, "WSH", p->hygiene, false);
    stat_bar(fb, 122, 16, "HP", p->health, false);
    epet_text(fb, 122, 27, epet_mood_name(mood), EPET_C_WHITE, 1);
    epet_number(fb, 190, 27, (int)(p->age_ms / 1000u), EPET_C_WHITE, 1);
    epet_text(fb, 220, 27, "S", EPET_C_WHITE, 1);

    if (mood == EPET_MOOD_DEAD) {
        epet_rect(fb, 54, 108, EPET_W - 68, 46, EPET_C_PANEL);
        epet_frame(fb, 54, 108, EPET_W - 68, 46, 2, EPET_C_BAD);
        epet_text(fb, 74, 116, "GONE", EPET_C_BAD, 2);
        epet_text(fb, 64, 138, "REVIVE IN STATS", EPET_C_WHITE, 1);
    }

    if (ui) epet_ui_render_menu(ui, fb);
}

/* Kept so callers that do not use the UI still get a screen. */
void epet_render(const epet_t *p, uint16_t *fb)
{
    epet_render_main(0, p, fb);
}
