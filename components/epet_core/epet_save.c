#include "epet_save.h"
#include "epet_store.h"
#include "epet_module.h"
#include "epet_modblob.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define KEY_PET  "pet"
#define KEY_MODS  "mods"
#define KEY_PACKS "packs"
#define MAGIC    0x45504554u   /* "EPET" */

#define NAME_MAX 16
#define MODS_MAX EPET_MAX_MODULES
#define ID_MAX   12

/* Packed on purpose: this crosses a reboot and, later, a wire. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;          /* of this struct, so a size change is detected */

    char     species[NAME_MAX];
    uint8_t  backdrop;

    float    hunger, happiness, energy, hygiene, health;
    uint32_t age_ms;
    uint8_t  poop;
    uint8_t  asleep;
    uint8_t  alive;
    uint8_t  pad;

    uint32_t crc;           /* over everything above */
} pet_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    char     id[MODS_MAX][ID_MAX];
    uint32_t crc;
} mods_record_t;

const char *epet_load_result_name(epet_load_result_t r)
{
    switch (r) {
    case EPET_LOAD_OK:         return "OK";
    case EPET_LOAD_NONE:       return "NONE";
    case EPET_LOAD_NO_STORE:   return "NO_STORE";
    case EPET_LOAD_CORRUPT:    return "CORRUPT";
    case EPET_LOAD_VERSION:    return "VERSION";
    case EPET_LOAD_NO_SPECIES: return "NO_SPECIES";
    }
    return "?";
}

/* FNV-1a: small, adequate for spotting a truncated or torn write. */
static uint32_t crc_of(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void copy_name(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    for (; i < cap; i++) dst[i] = 0;
}

/* ---- pet -------------------------------------------------------------- */

bool epet_save_pet(const epet_t *p)
{
    if (!epet_store_available() || !p || !p->species) return false;

    pet_record_t r;
    memset(&r, 0, sizeof r);
    r.magic   = MAGIC;
    r.version = EPET_SAVE_VERSION;
    r.size    = (uint16_t)sizeof r;
    copy_name(r.species, NAME_MAX, p->species->name);
    r.backdrop  = p->backdrop;
    r.hunger    = p->hunger;
    r.happiness = p->happiness;
    r.energy    = p->energy;
    r.hygiene   = p->hygiene;
    r.health    = p->health;
    r.age_ms    = p->age_ms;
    r.poop      = p->poop;
    r.asleep    = p->asleep ? 1 : 0;
    r.alive     = p->alive ? 1 : 0;
    r.crc = crc_of(&r, sizeof r - sizeof r.crc);

    const epet_store_t *st = epet_store_get();
    return st->write(st->ctx, KEY_PET, &r, sizeof r);
}

epet_load_result_t epet_load_pet(epet_t *p)
{
    if (!epet_store_available()) return EPET_LOAD_NO_STORE;

    pet_record_t r;
    size_t len = sizeof r;
    const epet_store_t *st = epet_store_get();
    if (!st->read(st->ctx, KEY_PET, &r, &len)) return EPET_LOAD_NONE;
    if (len != sizeof r)                        return EPET_LOAD_CORRUPT;
    if (r.magic != MAGIC)                       return EPET_LOAD_CORRUPT;
    if (r.crc != crc_of(&r, sizeof r - sizeof r.crc)) return EPET_LOAD_CORRUPT;
    if (r.version != EPET_SAVE_VERSION || r.size != sizeof r)
        return EPET_LOAD_VERSION;

    /* The class is resolved by name: its module may no longer be installed. */
    r.species[NAME_MAX - 1] = 0;
    const epet_species_t *sp = epet_species_by_name(r.species);
    if (!sp) return EPET_LOAD_NO_SPECIES;

    epet_bus_t *bus = p->bus;          /* keep the wiring the caller set up */
    epet_init(p);
    p->bus = bus;

    epet_set_species(p, sp, false);    /* no birth animation: it is not new */
    p->backdrop  = (uint8_t)(sp->n_backdrops ? r.backdrop % sp->n_backdrops : 0);
    p->hunger    = r.hunger;
    p->happiness = r.happiness;
    p->energy    = r.energy;
    p->hygiene   = r.hygiene;
    p->health    = r.health;
    p->age_ms    = r.age_ms;
    p->ev_minute = r.age_ms / 60000u;  /* do not replay every past minute */
    p->poop      = r.poop;
    p->asleep    = r.asleep != 0;
    p->alive     = r.alive != 0;
    return EPET_LOAD_OK;
}

bool epet_clear_pet(void)
{
    if (!epet_store_available()) return false;
    const epet_store_t *st = epet_store_get();
    return st->erase ? st->erase(st->ctx, KEY_PET) : false;
}

/* ---- modules ---------------------------------------------------------- */

bool epet_save_modules(void)
{
    if (!epet_store_available()) return false;

    mods_record_t r;
    memset(&r, 0, sizeof r);
    r.magic = MAGIC;
    r.version = EPET_SAVE_VERSION;

    uint8_t n = epet_modules_count();
    for (uint8_t i = 0; i < n && r.count < MODS_MAX; i++) {
        const epet_module_t *m = epet_modules_get(i);
        if (!m || !m->id) continue;
        copy_name(r.id[r.count], ID_MAX, m->id);
        r.count++;
    }
    r.crc = crc_of(&r, sizeof r - sizeof r.crc);

    const epet_store_t *st = epet_store_get();
    return st->write(st->ctx, KEY_MODS, &r, sizeof r);
}

uint8_t epet_restore_modules(epet_bus_t *bus)
{
    if (!epet_store_available()) return 0;

    mods_record_t r;
    size_t len = sizeof r;
    const epet_store_t *st = epet_store_get();
    if (!st->read(st->ctx, KEY_MODS, &r, &len)) return 0;
    if (len != sizeof r || r.magic != MAGIC ||
        r.version != EPET_SAVE_VERSION ||
        r.crc != crc_of(&r, sizeof r - sizeof r.crc)) {
        return 0;
    }

    uint8_t installed = 0;
    for (uint16_t i = 0; i < r.count && i < MODS_MAX; i++) {
        r.id[i][ID_MAX - 1] = 0;
        const epet_module_t *m = epet_modules_available(r.id[i]);
        if (!m) continue;                     /* no longer part of this build */
        if (epet_modules_install(m, bus)) installed++;
    }
    return installed;
}

/* ---- loaded packs ----------------------------------------------------- */

/* Key layout: "p_<id>" holds the blob, and the id list lives in the same
 * "mods" record the installed set uses. */
static void pack_key(char *out, size_t cap, const char *id)
{
    snprintf(out, cap, "p_%s", id);
}

/* The set of STORED packs is tracked separately from the set of INSTALLED
 * modules. They drifted apart once: a pack failed to restore on one boot, so
 * it was not installed, so epet_save_modules() wrote a list without it -- and
 * the blob was orphaned in storage with nothing left pointing at it. */
static bool read_pack_list(mods_record_t *r)
{
    if (!epet_store_available()) return false;
    size_t len = sizeof *r;
    const epet_store_t *st = epet_store_get();
    if (!st->read(st->ctx, KEY_PACKS, r, &len)) return false;
    if (len != sizeof *r || r->magic != MAGIC ||
        r->crc != crc_of(r, sizeof *r - sizeof r->crc)) return false;
    return true;
}

static bool write_pack_list(mods_record_t *r)
{
    r->magic = MAGIC;
    r->version = EPET_SAVE_VERSION;
    r->crc = crc_of(r, sizeof *r - sizeof r->crc);
    const epet_store_t *st = epet_store_get();
    return st->write(st->ctx, KEY_PACKS, r, sizeof *r);
}

bool epet_save_pack(const char *id, const uint8_t *data, size_t len)
{
    if (!epet_store_available() || !id || !data) return false;
    if (len > EPET_PACK_MAX_BYTES) return false;
    char key[24];
    pack_key(key, sizeof key, id);
    const epet_store_t *st = epet_store_get();
    if (!st->write(st->ctx, key, data, len)) return false;

    mods_record_t list;
    if (!read_pack_list(&list)) memset(&list, 0, sizeof list);
    for (uint16_t i = 0; i < list.count && i < MODS_MAX; i++) {
        if (strcmp(list.id[i], id) == 0) return true;      /* already listed */
    }
    if (list.count < MODS_MAX) {
        copy_name(list.id[list.count], ID_MAX, id);
        list.count++;
        write_pack_list(&list);
    }
    return true;
}

bool epet_erase_pack(const char *id)
{
    if (!epet_store_available() || !id) return false;
    char key[24];
    pack_key(key, sizeof key, id);
    const epet_store_t *st = epet_store_get();
    bool ok = st->erase ? st->erase(st->ctx, key) : false;

    mods_record_t list;
    if (read_pack_list(&list)) {
        uint16_t w = 0;
        for (uint16_t i = 0; i < list.count && i < MODS_MAX; i++) {
            if (strcmp(list.id[i], id) == 0) continue;
            if (w != i) memcpy(list.id[w], list.id[i], ID_MAX);
            w++;
        }
        if (w != list.count) { list.count = w; write_pack_list(&list); }
    }
    return ok;
}

/* Track what we allocated so it can be released on shutdown or re-scan. */
#define PACKS_MAX EPET_MAX_MODULES
static epet_module_t *g_packs[PACKS_MAX];
static uint8_t        g_n_packs;

void epet_release_saved_packs(void)
{
    for (uint8_t i = 0; i < g_n_packs; i++) {
        if (g_packs[i]) {
            epet_modules_remove(g_packs[i]->id);
            epet_modblob_free(g_packs[i]);
            g_packs[i] = 0;
        }
    }
    g_n_packs = 0;
}

const char *epet_packs_result_name(epet_packs_result_t r)
{
    switch (r) {
    case EPET_PACKS_OK:        return "ok";
    case EPET_PACKS_NO_STORE:  return "no storage";
    case EPET_PACKS_NO_LIST:   return "no saved list";
    case EPET_PACKS_NO_MEMORY: return "out of memory";
    }
    return "?";
}

uint8_t epet_provide_saved_packs(uint8_t *failed)
{
    return epet_provide_saved_packs_ex(failed, 0);
}

uint8_t epet_provide_saved_packs_ex(uint8_t *failed, epet_packs_result_t *why)
{
    if (failed) *failed = 0;
    if (why) *why = EPET_PACKS_OK;
    if (!epet_store_available()) {
        if (why) *why = EPET_PACKS_NO_STORE;
        return 0;
    }

    /* Read the PACK list, not the installed-module list: a pack that failed
     * to install once must still be found on the next boot. */
    mods_record_t r;
    const epet_store_t *st = epet_store_get();
    if (!read_pack_list(&r)) {
        if (why) *why = EPET_PACKS_NO_LIST;
        return 0;
    }

    uint8_t *buf = malloc(EPET_PACK_MAX_BYTES);
    if (!buf) {
        /* 64 KB is a lot on a device that also holds two framebuffers and,
         * once UPDATE mode runs, a Bluetooth stack. */
        if (why) *why = EPET_PACKS_NO_MEMORY;
        return 0;
    }

    uint8_t provided = 0;
    for (uint16_t i = 0; i < r.count && i < MODS_MAX; i++) {
        r.id[i][ID_MAX - 1] = 0;
        if (epet_modules_available(r.id[i])) continue;   /* compiled in */
        if (g_n_packs >= PACKS_MAX) break;

        char key[24];
        pack_key(key, sizeof key, r.id[i]);
        size_t got = EPET_PACK_MAX_BYTES;
        if (!st->read(st->ctx, key, buf, &got)) continue;

        epet_module_t *m = 0;
        if (epet_modblob_parse(buf, got, &m) != EPET_BLOB_OK || !m) {
            if (failed) (*failed)++;
            continue;
        }
        epet_modules_provide(m);
        g_packs[g_n_packs++] = m;
        provided++;
    }
    free(buf);
    return provided;
}

/* ---- autosave --------------------------------------------------------- */

static void autosave_on_event(const epet_event_t *ev, void *ctx)
{
    (void)ev;
    ((epet_autosave_t *)ctx)->dirty = true;
}

void epet_autosave_init(epet_autosave_t *a, epet_bus_t *bus, uint32_t min_interval_ms)
{
    a->last_ms = 0;
    a->min_interval_ms = min_interval_ms;
    a->dirty = false;
    /* Anything that changes the creature meaningfully marks it dirty. */
    const uint32_t watched =
        EPET_EV_MASK(EPET_EV_MINUTE)  | EPET_EV_MASK(EPET_EV_FED)     |
        EPET_EV_MASK(EPET_EV_PLAYED)  | EPET_EV_MASK(EPET_EV_CLEANED) |
        EPET_EV_MASK(EPET_EV_POOPED)  | EPET_EV_MASK(EPET_EV_DIED)    |
        EPET_EV_MASK(EPET_EV_REBORN)  | EPET_EV_MASK(EPET_EV_FELL_ASLEEP) |
        EPET_EV_MASK(EPET_EV_WOKE);
    epet_bus_subscribe(bus, watched, autosave_on_event, a, "autosave");
}

bool epet_autosave_tick(epet_autosave_t *a, const epet_t *p, uint32_t dt_ms)
{
    a->last_ms += dt_ms;
    if (!a->dirty || a->last_ms < a->min_interval_ms) return false;
    if (!epet_save_pet(p)) return false;
    a->dirty = false;
    a->last_ms = 0;
    return true;
}
