#include "epet_module.h"
#include "epet_pages.h"

/* The default module: everything built into the firmware.
 * Nothing here is privileged -- it is installed first, that is all. */

extern const epet_species_t *const epet_core_species[];
extern const uint8_t              epet_core_species_count;

static epet_page_t *const *core_pages(uint8_t *n) { return epet_pages_builtin(n); }

/* n_pages is filled in at install time because the page array lives in
 * epet_pages.c and its length is not a compile-time constant here. */
static uint8_t core_n_pages(void)
{
    uint8_t n = 0;
    (void)core_pages(&n);
    return n;
}

static epet_page_t *const *g_pages;
static uint8_t             g_n_pages;

static void core_on_install(const epet_module_t *self, epet_bus_t *bus)
{
    (void)self; (void)bus;
}

const epet_module_t epet_module_core = {
    .id        = "core",
    .name      = "EPET CORE",
    .version   = 1,
    .species   = epet_core_species,
    /* set below via the initialiser helper */
    .n_species = 0,
    .pages     = 0,
    .n_pages   = 0,
    .on_install = core_on_install,
};

/* The struct above cannot reference counts that live in other translation
 * units at compile time, so it is completed here before first use. */
const epet_module_t *epet_module_core_get(void)
{
    static epet_module_t built;
    static bool done;
    if (!done) {
        built = epet_module_core;
        built.n_species = epet_core_species_count;
        built.pages     = core_pages(&g_n_pages);
        built.n_pages   = core_n_pages();
        g_pages = built.pages;
        done = true;
    }
    return &built;
}
