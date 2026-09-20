#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epet_species.h"
#include "epet_ui.h"

/* Modules.
 *
 * A module is a bundle of content: characters (with their animations and
 * backdrops) and menu items (sub-programs). Everything shipped in the
 * firmware lives in the "core" module; nothing is special about it beyond
 * being installed first.
 *
 * Installing a module adds its characters to the pool that birth rolls from,
 * and its pages to the menu. Removing it takes them away again.
 *
 * ---------------------------------------------------------------------
 * On loading modules over USB/Bluetooth from a browser:
 *
 * Characters are PURE DATA -- frames, palettes, pose tables, temperament --
 * so a character module can be parsed from a downloaded blob at runtime.
 * That is the path this struct is shaped for.
 *
 * Sub-programs are CODE. `epet_page_t` holds function pointers, so a page
 * cannot be loaded from a data blob without either a bytecode interpreter or
 * signed native code and a loader. Until one of those exists, modules that
 * carry pages must be compiled in. Do not design the transfer protocol as if
 * pages were data.
 * --------------------------------------------------------------------- */

#define EPET_MAX_MODULES 8

typedef struct epet_module {
    const char *id;       /* stable, e.g. "core"; used for lookup and removal */
    const char *name;     /* shown to the user */
    uint16_t    version;

    const epet_species_t *const *species;
    uint8_t                      n_species;

    /* NULL for a data-loaded module; see the note above. */
    epet_page_t *const          *pages;
    uint8_t                      n_pages;

    /* Optional lifecycle hooks. A module may subscribe to the bus here. */
    void (*on_install)(const struct epet_module *self, epet_bus_t *bus);
    void (*on_remove) (const struct epet_module *self);
    void *ctx;
} epet_module_t;

/* Availability. A module must be PROVIDED before it can be installed by id
 * on boot: compiled-in modules provide themselves at startup, and a module
 * parsed from a download would provide itself once decoded. Providing does
 * not install. */
bool                 epet_modules_provide(const epet_module_t *m);
const epet_module_t *epet_modules_available(const char *id);
uint8_t              epet_modules_available_count(void);
const epet_module_t *epet_modules_available_at(uint8_t i);

/* Registry. Modules are referenced, not copied, so they must outlive the
 * registry -- static for compiled-in ones, heap for loaded ones. */
void                 epet_modules_reset(void);
bool                 epet_modules_install(const epet_module_t *m, epet_bus_t *bus);
bool                 epet_modules_remove(const char *id);
uint8_t              epet_modules_count(void);
const epet_module_t *epet_modules_get(uint8_t i);
const epet_module_t *epet_modules_find(const char *id);

/* Aggregated views across every installed module, in install order. */
uint8_t               epet_modules_species_count(void);
const epet_species_t *epet_modules_species(uint8_t i);
/* Which module a species came from, for display. NULL if not found. */
const epet_module_t  *epet_modules_owner_of(const epet_species_t *sp);

/* Register every installed module's pages into the menu, in install order. */
void epet_modules_populate_ui(epet_ui_t *ui);

/* The module holding everything built into the firmware. */
/* The module holding everything built into the firmware. Call this rather
 * than taking the address of a global: its counts are filled in at first use
 * because the page and species arrays live in other translation units. */
const epet_module_t *epet_module_core_get(void);
