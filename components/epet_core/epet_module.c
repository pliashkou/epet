#include "epet_module.h"
#include <string.h>

static const epet_module_t *g_mod[EPET_MAX_MODULES];
static uint8_t              g_n;

/* Known but not necessarily installed. */
static const epet_module_t *g_avail[EPET_MAX_MODULES];
static uint8_t              g_n_avail;

bool epet_modules_provide(const epet_module_t *m)
{
    if (!m || !m->id) return false;
    if (epet_modules_available(m->id)) return true;   /* already known */
    if (g_n_avail >= EPET_MAX_MODULES) return false;
    g_avail[g_n_avail++] = m;
    return true;
}

const epet_module_t *epet_modules_available(const char *id)
{
    if (!id) return 0;
    for (uint8_t i = 0; i < g_n_avail; i++) {
        if (strcmp(g_avail[i]->id, id) == 0) return g_avail[i];
    }
    return 0;
}

uint8_t epet_modules_available_count(void) { return g_n_avail; }

const epet_module_t *epet_modules_available_at(uint8_t i)
{
    return i < g_n_avail ? g_avail[i] : 0;
}

void epet_modules_reset(void)
{
    for (uint8_t i = 0; i < g_n; i++) {
        if (g_mod[i]->on_remove) g_mod[i]->on_remove(g_mod[i]);
    }
    g_n = 0;
    g_n_avail = 0;
}

bool epet_modules_install(const epet_module_t *m, epet_bus_t *bus)
{
    if (!m || !m->id || g_n >= EPET_MAX_MODULES) return false;
    if (epet_modules_find(m->id)) return false;      /* ids are unique */

    epet_modules_provide(m);        /* installed implies available */
    g_mod[g_n++] = m;
    if (m->on_install) m->on_install(m, bus);
    return true;
}

bool epet_modules_remove(const char *id)
{
    for (uint8_t i = 0; i < g_n; i++) {
        if (strcmp(g_mod[i]->id, id) != 0) continue;
        if (g_mod[i]->on_remove) g_mod[i]->on_remove(g_mod[i]);
        for (uint8_t j = i; j + 1 < g_n; j++) g_mod[j] = g_mod[j + 1];
        g_n--;
        return true;
    }
    return false;
}

uint8_t epet_modules_count(void) { return g_n; }

const epet_module_t *epet_modules_get(uint8_t i)
{
    return i < g_n ? g_mod[i] : 0;
}

const epet_module_t *epet_modules_find(const char *id)
{
    if (!id) return 0;
    for (uint8_t i = 0; i < g_n; i++) {
        if (strcmp(g_mod[i]->id, id) == 0) return g_mod[i];
    }
    return 0;
}

uint8_t epet_modules_species_count(void)
{
    uint16_t n = 0;
    for (uint8_t i = 0; i < g_n; i++) n += g_mod[i]->n_species;
    return n > 255 ? 255 : (uint8_t)n;
}

const epet_species_t *epet_modules_species(uint8_t i)
{
    for (uint8_t m = 0; m < g_n; m++) {
        if (i < g_mod[m]->n_species) return g_mod[m]->species[i];
        i = (uint8_t)(i - g_mod[m]->n_species);
    }
    return 0;
}

const epet_module_t *epet_modules_owner_of(const epet_species_t *sp)
{
    for (uint8_t m = 0; m < g_n; m++) {
        for (uint8_t s = 0; s < g_mod[m]->n_species; s++) {
            if (g_mod[m]->species[s] == sp) return g_mod[m];
        }
    }
    return 0;
}

void epet_modules_populate_ui(epet_ui_t *ui)
{
    for (uint8_t m = 0; m < g_n; m++) {
        for (uint8_t p = 0; p < g_mod[m]->n_pages; p++) {
            epet_ui_register(ui, g_mod[m]->pages[p]);
        }
    }
}
