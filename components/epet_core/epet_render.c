#include "epet.h"
#include "epet_draw.h"
#include "epet_ui.h"
#include "epet_species.h"

/* Main screen: status panel across the top, scrolling menu down the left,
 * the pet in the remaining area. Pages draw their own screens instead. */

#define PANEL_H   18
#define PET_CX    (EPET_W / 2)
/* The pet is anchored by its FEET, not its centre, because it changes size
 * as it ages. PET_GROUND is where the bottom edge of the sprite sits; at the
 * old fixed scale of 2 that is the same place the old centre of 156 put it,
 * so a young pet looks exactly as it always did. */
#define PET_GROUND 212
#define PET_R     44

/* One line for all five needs. At 240px the labels have to be single
 * letters -- the STATS page carries the full names and numbers. */
static void stat_cell(uint16_t *fb, int x, const char *letter,
                      float pct, bool invert)
{
    float shown = invert ? (100.0f - pct) : pct;
    epet_text(fb, x, 5, letter, EPET_C_WHITE, 1);
    epet_bar(fb, x + 7, 5, 25, 7, shown, epet_level_colour(shown));
}

static void draw_pet(uint16_t *fb, const epet_t *p, epet_mood_t mood)
{
    int bob = 0;
    if (mood != EPET_MOOD_DEAD) {
        uint32_t phase = (p->anim_ms / 250) % 4;
        bob = (phase == 1) ? -2 : (phase == 3) ? 2 : 0;
    }
    const int cx = PET_CX, gy = PET_GROUND + bob;

    /* Contact shadow, sized with the creature -- a fixed 60px ellipse under
     * a baby looks like it is standing on a manhole cover. */
    int w = epet_actor_grown_width(&p->actor);
    int sw = w ? (w * 2) / 3 : 60;
    epet_shade(fb, cx - sw / 2, gy - 16, sw, 7, EPET_C_BLACK, 60);

    /* The dead pose has its own artwork -- a slump with crossed eyes and a
     * departing spirit -- so it is drawn normally rather than tinted grey.
     * draw_grown() picks the artwork and the size for the pet's age. */
    epet_actor_draw_grown(&p->actor, fb, cx, gy);

    if (mood == EPET_MOOD_ASLEEP) {
        /* Track the top of the creature, which moves as it grows. */
        int top = gy - epet_actor_grown_height(&p->actor);
        epet_text(fb, cx + sw / 2 + 4, top + 8,  "Z", EPET_C_WHITE, 2);
        epet_text(fb, cx + sw / 2 + 20, top + 22, "Z", EPET_C_WHITE, 1);
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
    stat_cell(fb, 3,   "F", p->hunger, true);      /* fed, not hungry */
    stat_cell(fb, 39,  "J", p->happiness, false);
    stat_cell(fb, 75,  "P", p->energy, false);
    stat_cell(fb, 111, "W", p->hygiene, false);
    stat_cell(fb, 147, "H", p->health, false);

    /* mood right-aligned against the age, so neither jumps as they change */
    const char *mn = epet_mood_name(mood);
    epet_text(fb, 182, 5, mn, EPET_C_WHITE, 1);
    epet_number(fb, 216, 5, (int)(p->age_ms / 1000u), EPET_C_DIM, 1);

    if (mood == EPET_MOOD_DEAD) {
        /* Centred on the panel, and it says what to actually do. The figure
         * comes from the pet rather than a constant here, because the hold
         * is a platform setting. */
        const int bw = 188, bh = 58;
        const int bx = (EPET_W - bw) / 2, by = (EPET_H - bh) / 2;
        epet_shade(fb, bx, by, bw, bh, EPET_C_BLACK, 205);
        epet_frame(fb, bx, by, bw, bh, 2, EPET_C_BAD);

        int tw = epet_text_width("GONE", 2);
        epet_text(fb, (EPET_W - tw) / 2, by + 10, "GONE", EPET_C_BAD, 2);

        /* "SHAKE 5S TO RESTART", laid out in pieces so it stays centred
         * whatever the number is. */
        uint32_t secs = (p->revive_hold_ms + 999u) / 1000u;
        if (secs < 1) secs = 1;
        const char *pre = "SHAKE ", *post = "S TO RESTART";
        int digits = 1;
        for (uint32_t t = secs; t >= 10; t /= 10) digits++;
        int total = epet_text_width(pre, 1) + digits * 6 +
                    epet_text_width(post, 1);
        int tx = (EPET_W - total) / 2, ty = by + 36;
        epet_text(fb, tx, ty, pre, EPET_C_WHITE, 1);
        tx += epet_text_width(pre, 1);
        epet_number(fb, tx, ty, (int)secs, EPET_C_GOOD, 1);
        tx += digits * 6;
        epet_text(fb, tx, ty, post, EPET_C_WHITE, 1);
    }

    if (ui) epet_ui_render_menu(ui, fb);
}

/* Kept so callers that do not use the UI still get a screen. */
void epet_render(const epet_t *p, uint16_t *fb)
{
    epet_render_main(0, p, fb);
}
