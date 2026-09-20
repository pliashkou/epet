#include "epet_dynpage.h"
#include "epet_draw.h"
#include "epet_module.h"
#include "epet_species.h"
#include <string.h>
#include <stdio.h>

bool epet_src_number(epet_src_t s, const epet_t *pet, float *out)
{
    const epet_species_t *sp = pet->species;
    switch (s) {
    case EPET_SRC_HUNGER:     *out = pet->hunger; return true;
    case EPET_SRC_FED:        *out = 100.0f - pet->hunger; return true;
    case EPET_SRC_HAPPINESS:  *out = pet->happiness; return true;
    case EPET_SRC_ENERGY:     *out = pet->energy; return true;
    case EPET_SRC_HYGIENE:    *out = pet->hygiene; return true;
    case EPET_SRC_HEALTH:     *out = pet->health; return true;
    case EPET_SRC_AGE_S:      *out = (float)(pet->age_ms / 1000u); return true;
    case EPET_SRC_POOP:       *out = (float)pet->poop; return true;
    case EPET_SRC_N_BACKDROPS: *out = sp ? sp->n_backdrops : 0; return true;
    case EPET_SRC_N_POSES:    *out = sp ? sp->n_poses : 0; return true;
    case EPET_SRC_SPRITE_SCALE: *out = sp ? sp->scale : 0; return true;
    /* Temperament as a percentage of baseline, so 100 means "normal". */
    case EPET_SRC_TEMPER_HUNGER: *out = sp ? sp->temper.hunger * 100.f : 0; return true;
    case EPET_SRC_TEMPER_HAPPY:  *out = sp ? sp->temper.happiness * 100.f : 0; return true;
    case EPET_SRC_TEMPER_ENERGY: *out = sp ? sp->temper.energy * 100.f : 0; return true;
    case EPET_SRC_TEMPER_HYGIENE:*out = sp ? sp->temper.hygiene * 100.f : 0; return true;
    case EPET_SRC_SPECIES_COUNT: *out = epet_species_count(); return true;
    case EPET_SRC_MODULE_COUNT:  *out = epet_modules_count(); return true;
    default: return false;
    }
}

const char *epet_src_string(epet_src_t s, const epet_t *pet)
{
    const epet_species_t *sp = pet->species;
    switch (s) {
    case EPET_SRC_SPECIES_NAME:  return sp ? sp->name : "?";
    case EPET_SRC_SPECIES_BLURB: return sp ? sp->blurb : "";
    case EPET_SRC_POSE_NAME:     return epet_actor_pose_name(&pet->actor);
    case EPET_SRC_MOOD:          return epet_mood_name(epet_mood(pet));
    case EPET_SRC_BACKDROP_NAME: {
        const epet_backdrop_t *bd = epet_pet_backdrop(pet);
        return (bd && bd->name) ? bd->name : "-";
    }
    case EPET_SRC_MODULE_ID: {
        const epet_module_t *m = epet_modules_owner_of(sp);
        return m ? m->id : "-";
    }
    default: return "";
    }
}

static void dyn_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    epet_dynpage_t *p = (epet_dynpage_t *)self;

    epet_fill(fb, EPET_C_PANEL);
    epet_rect(fb, 0, 0, EPET_W, 28, EPET_RGB565(100, 140, 180));
    int tw = epet_text_width(p->title, 2);
    epet_text(fb, (EPET_W - tw) / 2, 7, p->title, EPET_C_WHITE, 2);

    int y = 36;
    for (uint8_t i = 0; i < p->n_rows && y < EPET_H - 18; i++) {
        const epet_dynrow_t *r = &p->rows[i];
        char label[EPET_ROW_LABEL_MAX + 1];
        memcpy(label, r->label, EPET_ROW_LABEL_MAX);
        label[EPET_ROW_LABEL_MAX] = 0;

        switch (r->kind) {
        case EPET_ROW_GAP:
            y += 8;
            continue;

        case EPET_ROW_TEXT:
            epet_text(fb, 10, y, label, EPET_C_WHITE, 1);
            break;

        case EPET_ROW_VALUE: {
            float v = 0;
            epet_text(fb, 10, y, label, EPET_C_WHITE, 1);
            if (epet_src_number(r->src, pet, &v)) {
                epet_number(fb, 110, y, (int)(v + 0.5f), EPET_C_GOOD, 1);
            } else {
                epet_text(fb, 110, y, "-", EPET_C_DIM, 1);
            }
            break;
        }

        case EPET_ROW_BAR: {
            float v = 0;
            epet_text(fb, 10, y, label, EPET_C_WHITE, 1);
            if (epet_src_number(r->src, pet, &v)) {
                epet_bar(fb, 98, y - 1, 104, 9, v, epet_level_colour(v));
                epet_number(fb, 208, y, (int)(v + 0.5f), EPET_C_DIM, 1);
            }
            break;
        }

        case EPET_ROW_STRING:
            epet_text(fb, 10, y, label, EPET_C_WHITE, 1);
            epet_text(fb, 98, y, epet_src_string(r->src, pet), EPET_C_GOOD, 1);
            break;

        case EPET_ROW_SPRITE:
            /* Source resolution, not the species' own scale: on a list page
             * the full-size sprite is 112px tall and swallows the rows. */
            epet_actor_draw_scaled(&pet->actor, fb, 120, y + 28, 1);
            y += 60;
            continue;

        default:
            break;
        }
        y += 14;
    }

    epet_rect(fb, 0, EPET_H - 16, EPET_W, 16, EPET_C_MENU);
    epet_text(fb, 6, EPET_H - 12, "RB BACK", EPET_C_DIM, 1);
}

/* A handful of built-in icons, chosen by id: a loaded module cannot ship
 * code, so it picks from these rather than drawing its own. */
static void dyn_icon(epet_page_t *self, uint16_t *fb, int cx, int cy,
                     uint16_t tint, bool sel)
{
    (void)sel;
    epet_dynpage_t *p = (epet_dynpage_t *)self;
    switch (p->icon) {
    case 1:   /* card */
        epet_rect(fb, cx - 11, cy - 8, 22, 16, tint);
        epet_shade(fb, cx - 9, cy - 6, 18, 12, EPET_C_BLACK, 140);
        epet_rect(fb, cx - 7, cy - 3, 8, 2, tint);
        epet_rect(fb, cx - 7, cy + 1, 12, 2, tint);
        break;
    case 2:   /* magnifier */
        epet_disc(fb, cx - 2, cy - 2, 8, tint);
        epet_shade(fb, cx - 2 - 5, cy - 2 - 5, 10, 10, EPET_C_BLACK, 130);
        epet_rect(fb, cx + 3, cy + 3, 7, 3, tint);
        break;
    case 3:   /* star */
        for (int i = -8; i <= 8; i++) {
            int h = 8 - (i < 0 ? -i : i);
            epet_rect(fb, cx + i, cy - h, 1, h * 2, tint);
        }
        epet_rect(fb, cx - 9, cy - 1, 19, 3, tint);
        break;
    default:  /* plug: "this came from a module" */
        epet_rect(fb, cx - 7, cy - 6, 14, 10, tint);
        epet_rect(fb, cx - 4, cy - 10, 3, 5, tint);
        epet_rect(fb, cx + 2, cy - 10, 3, 5, tint);
        epet_rect(fb, cx - 2, cy + 4, 5, 6, tint);
        break;
    }
}

void epet_dynpage_bind(epet_dynpage_t *p)
{
    p->base.title  = p->title;
    p->base.render = dyn_render;
    p->base.icon   = dyn_icon;
    p->base.enter  = 0;
    p->base.update = 0;
    p->base.leave  = 0;
    p->base.ctx    = 0;
}
