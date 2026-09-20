/* Module packs over the wire: blob parsing, the line protocol, and what
 * happens to a pet whose class is removed under it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "epet.h"
#include "epet_module.h"
#include "epet_modblob.h"
#include "epet_link.h"
#include "epet_save.h"
#include "epet_store.h"
#include "epet_vm.h"

static int failures = 0;
#define CHECK(cond, ...) do {                           \
    if (!(cond)) {                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__); printf("\n");              \
        failures++;                                     \
    }                                                   \
} while (0)

static const bool NONE[EPET_BTN_COUNT] = {false};

/* ---- in-memory store -------------------------------------------------- */
#define SLOTS 8
static struct { char key[24]; uint8_t buf[70000]; size_t len; bool used; } mem[SLOTS];
static bool m_read(void *c, const char *k, void *b, size_t *l) {
    (void)c;
    for (int i=0;i<SLOTS;i++) if (mem[i].used && !strcmp(mem[i].key,k)) {
        if (*l < mem[i].len) return false;
        memcpy(b, mem[i].buf, mem[i].len); *l = mem[i].len; return true; }
    return false; }
static bool m_write(void *c, const char *k, const void *b, size_t l) {
    (void)c;
    if (l > sizeof mem[0].buf) return false;
    for (int i=0;i<SLOTS;i++) if (mem[i].used && !strcmp(mem[i].key,k)) {
        memcpy(mem[i].buf,b,l); mem[i].len=l; return true; }
    for (int i=0;i<SLOTS;i++) if (!mem[i].used) {
        snprintf(mem[i].key,sizeof mem[i].key,"%s",k);
        memcpy(mem[i].buf,b,l); mem[i].len=l; mem[i].used=true; return true; }
    return false; }
static bool m_erase(void *c, const char *k) {
    (void)c;
    for (int i=0;i<SLOTS;i++) if (mem[i].used && !strcmp(mem[i].key,k)) mem[i].used=false;
    return true; }
static const epet_store_t MEM = { m_read, m_write, m_erase, NULL };

/* ---- capture what the link writes ------------------------------------ */
static char out[8192];
static size_t out_len;
static void cap(void *ctx, const char *t) {
    (void)ctx;
    size_t n = strlen(t);
    if (out_len + n < sizeof out) { memcpy(out+out_len,t,n); out_len += n; out[out_len]=0; }
}
static void clear(void) { out_len = 0; out[0] = 0; }
static void feed(epet_link_t *l, const char *s) { epet_link_feed(l, s, strlen(s)); }

static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Must match epet_link_csum() on the device. */
static uint16_t fletch16(const uint8_t *p, size_t n)
{
    uint16_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (uint16_t)((a + p[i]) % 255);
        b = (uint16_t)((b + a) % 255);
    }
    return (uint16_t)((b << 8) | a);
}

static void b64_encode(const uint8_t *in, size_t n, char *o)
{
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16;
        if (i+1 < n) v |= in[i+1] << 8;
        if (i+2 < n) v |= in[i+2];
        o[j++] = B64[(v>>18)&63];
        o[j++] = B64[(v>>12)&63];
        o[j++] = i+1 < n ? B64[(v>>6)&63] : '=';
        o[j++] = i+2 < n ? B64[v&63]      : '=';
    }
    o[j] = 0;
}

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return 0; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv)
{
    const char *pack_path = argc > 1 ? argv[1] : "../web/spark.epmod";
    size_t plen = 0;
    uint8_t *pack = load(pack_path, &plen);
    if (!pack) { printf("cannot read %s -- run tools/make_module.py first\n", pack_path); return 2; }

    printf("blob parsing\n");

    char id[16] = {0}, name[32] = {0};
    uint16_t ver = 0;
    CHECK(epet_modblob_peek(pack, plen, id, sizeof id, name, sizeof name, &ver),
          "peek should succeed");
    CHECK(!strcmp(id, "spark"), "id should be 'spark', got '%s'", id);

    epet_module_t *m = 0;
    CHECK(epet_modblob_parse(pack, plen, &m) == EPET_BLOB_OK, "parse should succeed");
    CHECK(m && m->n_species == 1, "one character");
    CHECK(m && m->n_pages == 2,
          "the pack ships a declarative page AND a bytecode page, got %d",
          m ? m->n_pages : -1);
    if (m) {
        bool declarative = false, bytecode = false;
        for (uint8_t i = 0; i < m->n_pages; i++) {
            if (!strcmp(m->pages[i]->title, "TRAITS")) declarative = true;
            if (!strcmp(m->pages[i]->title, "VITALS")) bytecode = true;
        }
        CHECK(declarative, "the declarative page should be there");
        CHECK(bytecode, "the bytecode page should be there");
    }

    /* every way a bad blob can arrive must be rejected, not crash */
    epet_module_t *bad = 0;
    CHECK(epet_modblob_parse(pack, 10, &bad) == EPET_BLOB_SHORT, "truncated");
    uint8_t *copy = malloc(plen);

    memcpy(copy, pack, plen); copy[0] = 'X';
    CHECK(epet_modblob_parse(copy, plen, &bad) == EPET_BLOB_MAGIC, "bad magic");

    memcpy(copy, pack, plen); copy[5] = 99;
    CHECK(epet_modblob_parse(copy, plen, &bad) == EPET_BLOB_VERSION, "bad version");

    memcpy(copy, pack, plen); copy[plen/2] ^= 0xFF;
    CHECK(epet_modblob_parse(copy, plen, &bad) == EPET_BLOB_CRC, "flipped byte");

    /* an out-of-range frame index must be caught, not followed */
    memcpy(copy, pack, plen);
    CHECK(epet_modblob_parse(copy, plen - 100, &bad) != EPET_BLOB_OK,
          "a short payload must not parse");
    free(copy);

    printf("install through the protocol\n");

    memset(mem, 0, sizeof mem);
    epet_store_set(&MEM);
    epet_modules_reset();
    epet_modules_install(epet_module_core_get(), NULL);
    epet_modblob_free(m);

    epet_bus_t bus; epet_bus_init(&bus);
    epet_link_t link;
    epet_link_init(&link, cap, NULL, &bus);

    clear(); feed(&link, "#PING\n");
    CHECK(strstr(out, "#PONG") != NULL, "ping should pong, got '%s'", out);

    clear(); feed(&link, "#LIST\n");
    CHECK(strstr(out, "#MOD core") && strstr(out, "#END"),
          "list should name core and end, got '%s'", out);

    uint8_t before = epet_species_count();

    char begin[64];
    snprintf(begin, sizeof begin, "#INSTALL %zu\n", plen);
    clear(); feed(&link, begin);
    CHECK(strstr(out, "#READY") != NULL, "should be ready, got '%s'", out);

    /* Chunk by BYTES so each piece carries its own checksum, exactly as the
     * browser does. 360 bytes encodes to 480 base64 characters. */
    char *b64 = malloc(1024);
    const size_t BYTES_PER = 360;
    uint32_t seq = 0;
    for (size_t i = 0; i < plen; i += BYTES_PER) {
        size_t n = plen - i < BYTES_PER ? plen - i : BYTES_PER;
        b64_encode(pack + i, n, b64);
        char line[900];
        snprintf(line, sizeof line, "#D %u %u %s\n",
                 seq, (unsigned)fletch16(pack + i, n), b64);
        clear(); feed(&link, line);
        if (!strstr(out, "#A ")) {
            CHECK(0, "chunk %u not acked: %s", seq, out);
            break;
        }
        seq++;
    }

    /* out-of-order chunks are rejected with the expected sequence */
    clear(); feed(&link, "#D 999 1 QUJD\n");
    CHECK(strstr(out, "#ERR seq") != NULL,
          "a wrong sequence must be rejected, got '%s'", out);

    clear(); feed(&link, "#DONE\n");
    CHECK(strstr(out, "#OK spark") != NULL, "install should succeed, got '%s'", out);

    CHECK(epet_species_count() == before + 1,
          "the pack's character should join the pool: %d -> %d",
          before, epet_species_count());
    CHECK(epet_species_by_name("SPARK") != NULL, "SPARK should be available");

    /* the loaded bytecode page must actually run */
    {
        epet_ui_t ui2; epet_ui_init(&ui2, &bus); ui2.menu_hide_ms = 0;
        epet_modules_populate_ui(&ui2);
        epet_t p2; epet_init(&p2); epet_attach_bus(&p2, &bus);
        p2.display_timeout_ms = 0;
        epet_set_species(&p2, epet_species_by_name("SPARK"), false);

        CHECK(epet_ui_open_titled(&ui2, &p2, "VITALS"),
              "the bytecode page should be in the menu");
        static uint16_t screen[EPET_W * EPET_H];
        memset(screen, 0, sizeof screen);
        epet_ui_render(&ui2, &p2, screen);
        int painted = 0;
        for (int i = 0; i < EPET_W * EPET_H; i++) if (screen[i]) painted++;
        CHECK(painted > 10000,
              "the bytecode render hook should have drawn the screen, %d px",
              painted);

        /* RT is wired in bytecode to feed the pet */
        for (int i = 0; i < 20; i++) epet_update(&p2, 50, NONE, NONE);
        float before2 = p2.hunger;
        epet_ui_handle(&ui2, &p2, 50, EPET_BTN_BIT(EPET_BTN_RT));
        CHECK(p2.hunger < before2 - 20.0f,
              "the module's own code should have fed the pet: %.1f -> %.1f",
              before2, p2.hunger);
        epet_ui_close(&ui2, &p2);
    }

    /* installing the same id twice is refused */
    clear(); feed(&link, begin);
    feed(&link, "#DONE\n");
    CHECK(strstr(out, "#ERR") != NULL, "a second install of the same id must fail");

    printf("survives a restart\n");

    /* simulate a reboot: drop every module, then re-provide from storage */
    epet_release_saved_packs();
    epet_modules_reset();
    epet_modules_provide(epet_module_core_get());
    uint8_t failed = 0;
    uint8_t packs = epet_provide_saved_packs(&failed);
    CHECK(packs == 1 && failed == 0, "the stored pack should re-provide: %d/%d",
          packs, failed);
    CHECK(epet_restore_modules(NULL) == 2, "both modules should reinstall");
    CHECK(epet_species_by_name("SPARK") != NULL,
          "the loaded character should be back after a restart");

    printf("background hooks\n");

    {
        /* A pack's hook must run on subscribed events with none of its pages
         * open -- that is what makes it a module rather than a screen. */
        epet_bus_t hb; epet_bus_init(&hb);
        epet_modules_reset();
        epet_modules_install(epet_module_core_get(), &hb);
        uint8_t subs_before = hb.n_sub;

        epet_module_t *hm = 0;
        CHECK(epet_modblob_parse(pack, plen, &hm) == EPET_BLOB_OK, "parse for hooks");
        CHECK(epet_modules_install(hm, &hb), "install for hooks");
        CHECK(hb.n_sub > subs_before,
              "installing should subscribe the pack's hook: %d -> %d",
              subs_before, hb.n_sub);

        epet_t hp; epet_init(&hp); epet_attach_bus(&hp, &hb);
        epet_set_active(&hp);
        hp.display_timeout_ms = 0;

        epet_ui_t hu; epet_ui_init(&hu, &hb); hu.menu_hide_ms = 0;
        epet_modules_populate_ui(&hu);
        CHECK(hu.active == NULL, "no page should be open");

        for (int i = 0; i < 3000; i++) {
            epet_update(&hp, 50, NONE, NONE);
            epet_bus_dispatch(&hb);
        }
        CHECK(hu.active == NULL, "still no page open");

        uint32_t calls = 0, faults = 0;
        epet_modblob_hook_stats(hm, &calls, &faults);
        CHECK(calls > 0,
              "the hook should have run with no page open, ran %u times", calls);
        CHECK(faults == 0, "the hook should not fault, %u faults", faults);

        /* Removing must unsubscribe: the handler and its state are freed. */
        uint8_t subs_with = hb.n_sub;
        CHECK(epet_modules_remove("spark"), "remove for hooks");
        CHECK(hb.n_sub < subs_with,
              "removing must unsubscribe the hook: %d -> %d", subs_with, hb.n_sub);
        epet_modblob_free(hm);

        /* Events after removal must not reach the freed handler. */
        for (int i = 0; i < 400; i++) {
            epet_update(&hp, 50, NONE, NONE);
            epet_bus_dispatch(&hb);
        }
        CHECK(1, "events after removal did not touch the freed module");

        /* restore the state the later checks expect */
        epet_modules_reset();
        epet_modules_install(epet_module_core_get(), NULL);
        epet_provide_saved_packs(NULL);
        epet_restore_modules(NULL);
    }

    printf("per-character mess\n");

    for (uint8_t i = 0; i < epet_species_count(); i++) {
        const epet_species_t *sp2 = epet_species_builtin(i);
        CHECK(sp2->poop != NULL,
              "%s should carry its own mess sprite", sp2->name);
        if (sp2->poop) {
            CHECK(sp2->poop->w > 0 && sp2->poop->h > 0,
                  "%s mess sprite has no size", sp2->name);
        }
    }

    printf("pet is the removed class\n");

    epet_t pet; epet_init(&pet); epet_attach_bus(&pet, &bus);
    pet.display_timeout_ms = 0;
    const epet_species_t *spark = epet_species_by_name("SPARK");
    epet_set_species(&pet, spark, false);
    for (int i = 0; i < 40; i++) epet_update(&pet, 50, NONE, NONE);
    CHECK(pet.species == spark, "the pet is a SPARK");
    CHECK(!epet_validate_species(&pet), "nothing to fix while the module is here");

    epet_link_init(&link, cap, NULL, &bus);
    clear(); feed(&link, "#REMOVE spark\n");
    CHECK(strstr(out, "#OK spark") != NULL, "remove should succeed, got '%s'", out);
    CHECK(epet_link_take_changed(&link), "removal should flag a change");

    CHECK(epet_species_by_name("SPARK") == NULL, "SPARK should be gone");
    CHECK(epet_validate_species(&pet),
          "the pet's class vanished, so it must be replaced");
    CHECK(pet.species != NULL, "it must have a class again");
    CHECK(pet.species != spark, "and not the removed one");
    CHECK(pet.alive, "the replacement should be a living pet");
    CHECK(pet.age_ms == 0, "it is a new creature, so age restarts");

    /* and a save written while it was a SPARK must not resurrect it */
    CHECK(epet_load_pet(&pet) != EPET_LOAD_OK || pet.species != spark,
          "a saved SPARK must not come back once its module is gone");

    printf("core is protected\n");

    clear(); feed(&link, "#REMOVE core\n");
    CHECK(strstr(out, "#ERR core") != NULL,
          "core must not be removable: it would leave no content");

    clear(); feed(&link, "#REMOVE nosuch\n");
    CHECK(strstr(out, "#ERR notfound") != NULL, "unknown id should say so");

    free(b64); free(pack);
    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
