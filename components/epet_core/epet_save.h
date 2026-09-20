#pragma once
#include <stddef.h>
#include "epet.h"
#include "epet_event.h"

/* Saved state.
 *
 * Two independent records:
 *   "pet"  -- the creature: class, home, stats, age
 *   "mods" -- which modules were installed, by id
 *
 * The class is stored BY NAME, not by index or pointer: indices shift when a
 * module is installed or removed, and pointers are meaningless across a
 * reboot. If the saved class is no longer available -- its module was removed
 * -- the load fails cleanly rather than resurrecting the wrong creature. */

#define EPET_SAVE_VERSION 1

typedef enum {
    EPET_LOAD_OK = 0,
    EPET_LOAD_NONE,        /* nothing saved yet */
    EPET_LOAD_NO_STORE,    /* no platform storage attached */
    EPET_LOAD_CORRUPT,     /* bad magic, length or checksum */
    EPET_LOAD_VERSION,     /* written by a different firmware */
    EPET_LOAD_NO_SPECIES,  /* the saved class is not installed any more */
} epet_load_result_t;

const char *epet_load_result_name(epet_load_result_t r);

bool               epet_save_pet(const epet_t *p);
epet_load_result_t epet_load_pet(epet_t *p);
bool               epet_clear_pet(void);

/* Installed-module list, so sub-programs come back after a restart. */
bool epet_save_modules(void);
/* Installs every saved id that is currently available. Returns how many. */
uint8_t epet_restore_modules(epet_bus_t *bus);

/* ---- loaded module packs --------------------------------------------
 * A pack installed from a browser is stored whole, keyed by its id, and
 * re-parsed on boot. That is what makes a loaded sub-program survive a
 * restart. */
#define EPET_PACK_MAX_BYTES 65536

bool    epet_save_pack(const char *id, const uint8_t *data, size_t len);
bool    epet_erase_pack(const char *id);
/* Parses and PROVIDES every stored pack (does not install). Returns how many
 * were provided; `failed` counts packs that would not parse. */
typedef enum {
    EPET_PACKS_OK = 0,
    EPET_PACKS_NO_STORE,
    EPET_PACKS_NO_LIST,     /* nothing has ever been installed */
    EPET_PACKS_NO_MEMORY,   /* could not allocate the read buffer */
} epet_packs_result_t;

const char *epet_packs_result_name(epet_packs_result_t r);
uint8_t epet_provide_saved_packs(uint8_t *failed);
/* Same, but says why nothing came back. */
uint8_t epet_provide_saved_packs_ex(uint8_t *failed, epet_packs_result_t *why);
/* Frees everything epet_provide_saved_packs() allocated. */
void    epet_release_saved_packs(void);

/* Convenience: save when something meaningful changed, at most every
 * min_interval_ms. Call once per loop; it decides. */
typedef struct {
    uint32_t last_ms;
    uint32_t min_interval_ms;
    bool     dirty;
} epet_autosave_t;

void epet_autosave_init(epet_autosave_t *a, epet_bus_t *bus, uint32_t min_interval_ms);
/* Returns true if it wrote. */
bool epet_autosave_tick(epet_autosave_t *a, const epet_t *p, uint32_t dt_ms);
