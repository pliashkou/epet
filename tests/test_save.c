/* Persistence: pet round-trip, module list, and the ways a load must fail
 * cleanly rather than resurrecting the wrong creature. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "epet.h"
#include "epet_module.h"
#include "epet_save.h"
#include "epet_store.h"

static int failures = 0;
#define CHECK(cond, ...) do {                           \
    if (!(cond)) {                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__); printf("\n");              \
        failures++;                                     \
    }                                                   \
} while (0)

static const bool NONE[EPET_BTN_COUNT] = {false};

/* ---- an in-memory store ---------------------------------------------- */

#define SLOTS 4
static struct { char key[16]; uint8_t buf[512]; size_t len; bool used; } mem[SLOTS];

static bool mem_read(void *ctx, const char *key, void *buf, size_t *len)
{
    (void)ctx;
    for (int i = 0; i < SLOTS; i++) {
        if (!mem[i].used || strcmp(mem[i].key, key)) continue;
        if (*len < mem[i].len) return false;
        memcpy(buf, mem[i].buf, mem[i].len);
        *len = mem[i].len;
        return true;
    }
    return false;
}
static bool mem_write(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    if (len > sizeof mem[0].buf) return false;
    for (int i = 0; i < SLOTS; i++) {
        if (mem[i].used && strcmp(mem[i].key, key)) continue;
        snprintf(mem[i].key, sizeof mem[i].key, "%s", key);
        memcpy(mem[i].buf, buf, len);
        mem[i].len = len;
        mem[i].used = true;
        return true;
    }
    return false;
}
static bool mem_erase(void *ctx, const char *key)
{
    (void)ctx;
    for (int i = 0; i < SLOTS; i++) {
        if (mem[i].used && !strcmp(mem[i].key, key)) { mem[i].used = false; return true; }
    }
    return true;
}
static const epet_store_t MEM_STORE = { mem_read, mem_write, mem_erase, NULL };

static void wipe(void) { memset(mem, 0, sizeof mem); }

/* a second module, to test that a removed class fails the load cleanly */
static const uint8_t PX[4] = { 1, 1, 1, 1 };
static const epet_frame_t FR = { 2, 2, PX };
static const epet_key_t KEYS[] = { { &FR, 500 } };
static const epet_pose_t POSES[] = { { EPET_POSE_IDLE, KEYS, 1, true } };
static const epet_species_t S_EXTRA = {
    .name = "EXTRA", .blurb = "TEMPORARY.",
    .palette = { { 0, 0xFFFF } }, .poses = POSES, .n_poses = 1, .scale = 1,
    .temper = { 1.f, 1.f, 1.f, 1.f },
};
static const epet_species_t *const EXTRA_SP[] = { &S_EXTRA };
static const epet_module_t extra_module = {
    .id = "extra", .name = "EXTRA PACK", .version = 1,
    .species = EXTRA_SP, .n_species = 1,
};

int main(void)
{
    printf("no store\n");

    epet_store_set(NULL);
    epet_modules_reset();
    epet_modules_install(epet_module_core_get(), NULL);
    epet_t p; epet_init(&p);
    CHECK(!epet_save_pet(&p), "saving without a store must fail, not crash");
    CHECK(epet_load_pet(&p) == EPET_LOAD_NO_STORE, "load should say NO_STORE");

    printf("round trip\n");

    wipe();
    epet_store_set(&MEM_STORE);
    CHECK(epet_load_pet(&p) == EPET_LOAD_NONE, "nothing saved yet");

    epet_init(&p);
    p.display_timeout_ms = 0;
    for (int i = 0; i < 600; i++) epet_update(&p, 50, NONE, NONE);   /* 30 s */
    epet_apply_action(&p, EPET_ACT_FEED);

    const epet_species_t *want_sp = p.species;
    uint8_t  want_bd   = p.backdrop;
    float    want_hun  = p.hunger, want_joy = p.happiness, want_hp = p.health;
    uint32_t want_age  = p.age_ms;
    uint8_t  want_poop = p.poop;

    CHECK(epet_save_pet(&p), "save should succeed");

    epet_t q;
    epet_init(&q);                       /* a different, freshly hatched pet */
    CHECK(epet_load_pet(&q) == EPET_LOAD_OK, "load should succeed");

    CHECK(q.species == want_sp, "class should survive: %s vs %s",
          q.species->name, want_sp->name);
    CHECK(q.backdrop == want_bd, "home should survive: %d vs %d",
          q.backdrop, want_bd);
    CHECK(fabsf(q.hunger - want_hun) < 0.01f, "hunger %.2f vs %.2f",
          q.hunger, want_hun);
    CHECK(fabsf(q.happiness - want_joy) < 0.01f, "joy %.2f vs %.2f",
          q.happiness, want_joy);
    CHECK(fabsf(q.health - want_hp) < 0.01f, "health %.2f vs %.2f",
          q.health, want_hp);
    CHECK(q.age_ms == want_age, "age %u vs %u", q.age_ms, want_age);
    CHECK(q.poop == want_poop, "mess %d vs %d", q.poop, want_poop);

    /* a resumed pet is not newborn: it must not replay the birth animation */
    CHECK(strcmp(epet_actor_pose_name(&q.actor), EPET_POSE_BIRTH) != 0,
          "a resumed pet should not replay birth, got %s",
          epet_actor_pose_name(&q.actor));

    /* nor should it replay every minute it has ever lived */
    epet_bus_t bus; epet_bus_init(&bus);
    int minutes = 0;
    epet_t r; epet_init(&r); epet_attach_bus(&r, &bus);
    CHECK(epet_load_pet(&r) == EPET_LOAD_OK, "reload with a bus");
    CHECK(r.bus == &bus, "loading must not detach the bus");
    epet_update(&r, 50, NONE, NONE);
    epet_bus_dispatch(&bus);
    (void)minutes;
    CHECK(r.ev_minute == r.age_ms / 60000u,
          "the minute counter should resume, not replay from zero");

    printf("corruption\n");

    /* flip a byte in the middle of the record */
    for (int i = 0; i < SLOTS; i++) {
        if (mem[i].used && !strcmp(mem[i].key, "pet")) mem[i].buf[20] ^= 0xFF;
    }
    CHECK(epet_load_pet(&q) == EPET_LOAD_CORRUPT,
          "a flipped byte must be caught by the checksum");

    /* truncated record */
    for (int i = 0; i < SLOTS; i++) {
        if (mem[i].used && !strcmp(mem[i].key, "pet")) mem[i].len -= 4;
    }
    CHECK(epet_load_pet(&q) == EPET_LOAD_CORRUPT, "a short record must be rejected");

    wipe();
    CHECK(epet_load_pet(&q) == EPET_LOAD_NONE, "after wiping, nothing saved");

    printf("class removed between runs\n");

    /* save a pet whose class belongs to a module, then uninstall it */
    epet_modules_install(&extra_module, NULL);
    epet_init(&p);
    epet_set_species(&p, &S_EXTRA, false);
    CHECK(epet_save_pet(&p), "save the guest-class pet");
    CHECK(epet_load_pet(&q) == EPET_LOAD_OK, "loads while the module is here");

    epet_modules_remove("extra");
    CHECK(epet_load_pet(&q) == EPET_LOAD_NO_SPECIES,
          "with the class gone the load must fail cleanly, not pick another");

    printf("module list\n");

    wipe();
    epet_modules_reset();
    epet_modules_provide(epet_module_core_get());
    epet_modules_provide(&extra_module);
    epet_modules_install(epet_module_core_get(), NULL);
    epet_modules_install(&extra_module, NULL);
    CHECK(epet_save_modules(), "save the installed list");

    /* simulate a restart: registry cleared, modules re-provided */
    epet_modules_reset();
    epet_modules_provide(epet_module_core_get());
    epet_modules_provide(&extra_module);
    CHECK(epet_modules_count() == 0, "nothing installed right after a restart");

    uint8_t n = epet_restore_modules(NULL);
    CHECK(n == 2, "both modules should be restored, got %d", n);
    CHECK(epet_modules_find("core") && epet_modules_find("extra"),
          "both should be installed by id");
    CHECK(epet_species_by_name("EXTRA") == &S_EXTRA,
          "the restored module's character should be available again");

    /* a module that is no longer part of the build is skipped, not fatal */
    epet_modules_reset();
    epet_modules_provide(epet_module_core_get());     /* extra not provided */
    n = epet_restore_modules(NULL);
    CHECK(n == 1, "only the available module should restore, got %d", n);
    CHECK(!epet_modules_find("extra"), "the missing module must not appear");

    printf("autosave\n");

    wipe();
    epet_modules_reset();
    epet_modules_install(epet_module_core_get(), NULL);
    epet_bus_init(&bus);
    epet_autosave_t as;
    epet_autosave_init(&as, &bus, 5000);
    epet_init(&p); epet_attach_bus(&p, &bus);
    p.display_timeout_ms = 0;

    CHECK(!epet_autosave_tick(&as, &p, 10000),
          "nothing changed, so nothing should be written");

    /* A change after a long quiet spell writes at once: the interval is
     * there to limit flash wear, not to delay a save that is already due. */
    epet_apply_action(&p, EPET_ACT_FEED);
    epet_bus_dispatch(&bus);
    CHECK(as.dirty, "an action should mark the save dirty");
    CHECK(epet_autosave_tick(&as, &p, 0),
          "a change after a long idle should write immediately");
    CHECK(!as.dirty, "writing should clear the dirty flag");
    CHECK(as.last_ms == 0, "writing should restart the interval");

    /* Now the interval really does defer the next one. */
    CHECK(!epet_autosave_tick(&as, &p, 60000),
          "clean state should not write however long passes");

    epet_apply_action(&p, EPET_ACT_PLAY);
    epet_bus_dispatch(&bus);
    CHECK(as.dirty, "second action marks dirty");
    as.last_ms = 0;                       /* pretend we just wrote */
    CHECK(!epet_autosave_tick(&as, &p, 1000),
          "dirty but only 1s since the last write: must defer");
    CHECK(as.dirty, "deferring must not lose the dirty flag");
    CHECK(epet_autosave_tick(&as, &p, 5000),
          "once past the interval it should write");
    CHECK(!as.dirty, "and clear the flag");

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
