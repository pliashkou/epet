/* The module registry: installing, removing, and what that does to the
 * character pool and the menu. Includes a second module built entirely
 * outside the core, which is the real test of the abstraction. */
#include <stdio.h>
#include <string.h>
#include "epet.h"
#include "epet_module.h"
#include "epet_pages.h"
#include "epet_draw.h"

static int failures = 0;
#define CHECK(cond, ...) do {                           \
    if (!(cond)) {                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__); printf("\n");              \
        failures++;                                     \
    }                                                   \
} while (0)

/* ---- a module defined entirely here ---------------------------------- */

/* one tiny 4x4 character: enough to prove data-only content works */
static const uint8_t PX_IDLE[16] = { 0,1,1,0,  1,2,2,1,  1,2,2,1,  0,1,1,0 };
static const epet_frame_t F_IDLE = { 4, 4, PX_IDLE };
static const epet_key_t   K_IDLE[] = { { &F_IDLE, 500 } };
static const epet_pose_t  P_GUEST[] = {
    { EPET_POSE_IDLE, K_IDLE, 1, true },
};
static const uint8_t PX_BG[64] = {
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
    3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3, 3,3,3,3,3,3,3,3,
};
static const epet_frame_t   F_BG = { 8, 8, PX_BG };
static const epet_palette_t BG_PAL = { { 0, 0x1234, 0x5678, 0x9ABC } };
static const epet_backdrop_t BD_GUEST[] = {
    { "VOID", &F_BG, &BG_PAL, 30, 0, 0, 198, 0x18CE, 150 },
};
static const epet_species_t S_GUEST = {
    .name = "GUEST", .blurb = "FROM A MODULE.",
    .palette = { { 0, 0x0000, 0xF800, 0x07E0 } },
    .poses = P_GUEST, .n_poses = 1, .scale = 4,
    .backdrops = BD_GUEST, .n_backdrops = 1,
    .temper = { 1.0f, 1.0f, 1.0f, 1.0f },
};
static const epet_species_t *const GUEST_SPECIES[] = { &S_GUEST };

static int guest_renders;
static void guest_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    (void)self; (void)pet;
    epet_fill(fb, EPET_C_PANEL);
    guest_renders++;
}
static epet_page_t guest_page = { .title = "GUEST", .render = guest_render };
static epet_page_t *const GUEST_PAGES[] = { &guest_page };

static int installs, removes;
static void guest_installed(const epet_module_t *m, epet_bus_t *bus)
{ (void)m; (void)bus; installs++; }
static void guest_removed(const epet_module_t *m)
{ (void)m; removes++; }

static const epet_module_t guest_module = {
    .id = "guest", .name = "GUEST PACK", .version = 3,
    .species = GUEST_SPECIES, .n_species = 1,
    .pages = GUEST_PAGES, .n_pages = 1,
    .on_install = guest_installed, .on_remove = guest_removed,
};

/* a characters-only module: the shape a downloaded pack would take */
static const epet_species_t *const DATA_SPECIES[] = { &S_GUEST };
static const epet_module_t data_module = {
    .id = "data", .name = "DATA PACK", .version = 1,
    .species = DATA_SPECIES, .n_species = 1,
    .pages = 0, .n_pages = 0,
};

static uint16_t fb[EPET_W * EPET_H];

int main(void)
{
    printf("registry\n");

    epet_modules_reset();
    CHECK(epet_modules_count() == 0, "should start empty");
    CHECK(epet_species_count() == 0, "no modules means no characters");

    CHECK(epet_modules_install(epet_module_core_get(), NULL), "core installs");
    CHECK(epet_modules_count() == 1, "one module");
    const epet_module_t *core = epet_modules_find("core");
    CHECK(core != NULL, "core should be findable by id");
    CHECK(core->n_species >= 2, "core should carry its characters, got %d",
          core->n_species);
    CHECK(core->n_pages >= 8, "core should carry its pages, got %d", core->n_pages);

    uint8_t core_chars = epet_species_count();
    CHECK(core_chars == core->n_species,
          "the pool should be the core's characters: %d vs %d",
          core_chars, core->n_species);

    /* ids are unique */
    CHECK(!epet_modules_install(epet_module_core_get(), NULL),
          "installing the same id twice must fail");

    printf("installing a module\n");

    installs = removes = 0;
    CHECK(epet_modules_install(&guest_module, NULL), "guest installs");
    CHECK(installs == 1, "on_install should fire once, got %d", installs);
    CHECK(epet_modules_count() == 2, "two modules");

    /* its character joins the pool */
    CHECK(epet_species_count() == core_chars + 1,
          "the module's character should join the pool: %d", epet_species_count());
    CHECK(epet_species_by_name("GUEST") == &S_GUEST,
          "the module's character should be findable by name");
    CHECK(epet_modules_owner_of(&S_GUEST) == &guest_module,
          "ownership should be reported");

    /* and births can roll it */
    bool rolled_guest = false;
    epet_seed_random(99);
    for (int i = 0; i < 300 && !rolled_guest; i++) {
        epet_t p; epet_init(&p);
        if (p.species == &S_GUEST) rolled_guest = true;
    }
    CHECK(rolled_guest, "a module's character should be reachable by birth");

    /* its page joins the menu */
    epet_bus_t bus; epet_bus_init(&bus);
    epet_ui_t ui; epet_ui_init(&ui, &bus);
    epet_modules_populate_ui(&ui);
    CHECK(ui.n_page == core->n_pages + 1,
          "the module's page should join the menu: %d", ui.n_page);

    bool found = false;
    for (uint8_t i = 0; i < ui.n_page; i++) {
        if (strcmp(ui.page[i]->title, "GUEST") == 0) found = true;
    }
    CHECK(found, "the module's page should be in the menu");

    /* core pages come first: install order decides menu order */
    CHECK(strcmp(ui.page[0]->title, "FEED") == 0,
          "core pages should come first, got '%s'", ui.page[0]->title);
    CHECK(strcmp(ui.page[ui.n_page - 1]->title, "GUEST") == 0,
          "a later module's pages should come last, got '%s'",
          ui.page[ui.n_page - 1]->title);

    /* the page actually runs */
    epet_t pet; epet_init(&pet);
    pet.display_timeout_ms = 0;
    guest_renders = 0;
    CHECK(epet_ui_open_titled(&ui, &pet, "GUEST"), "should open the module's page");
    epet_ui_render(&ui, &pet, fb);
    CHECK(guest_renders == 1, "the module's page should have rendered");
    epet_ui_close(&ui, &pet);

    printf("removing a module\n");

    CHECK(epet_modules_remove("guest"), "guest removes");
    CHECK(removes == 1, "on_remove should fire once, got %d", removes);
    CHECK(epet_modules_count() == 1, "back to one module");
    CHECK(epet_species_count() == core_chars,
          "its character should leave the pool, got %d", epet_species_count());
    CHECK(epet_species_by_name("GUEST") == NULL,
          "its character should no longer resolve");
    CHECK(!epet_modules_remove("guest"), "removing twice should fail");
    CHECK(!epet_modules_remove("nope"), "removing an unknown id should fail");

    printf("characters-only module\n");

    /* This is the shape a module downloaded over USB/BLE would take: data
     * only, no function pointers, so it could be parsed from a blob. */
    CHECK(data_module.pages == NULL && data_module.n_pages == 0,
          "a data-only module carries no code");
    CHECK(epet_modules_install(&data_module, NULL), "data pack installs");
    CHECK(epet_species_count() == core_chars + 1,
          "its character should join the pool");

    epet_bus_init(&bus);
    epet_ui_init(&ui, &bus);
    epet_modules_populate_ui(&ui);
    CHECK(ui.n_page == core->n_pages,
          "a characters-only module adds no menu items, got %d", ui.n_page);
    epet_modules_remove("data");

    printf("limits\n");

    epet_modules_reset();
    CHECK(epet_modules_count() == 0, "reset clears everything");

    static epet_module_t filler[EPET_MAX_MODULES];
    static char ids[EPET_MAX_MODULES][8];
    for (int i = 0; i < EPET_MAX_MODULES; i++) {
        snprintf(ids[i], sizeof ids[i], "m%d", i);
        filler[i].id = ids[i];
        filler[i].name = ids[i];
        CHECK(epet_modules_install(&filler[i], NULL), "install %d", i);
    }
    static const epet_module_t overflow = { .id = "over", .name = "OVER" };
    CHECK(!epet_modules_install(&overflow, NULL),
          "installing past EPET_MAX_MODULES must fail, not overflow");

    /* leave the registry in a sane state for anything that follows */
    epet_modules_reset();
    epet_modules_install(epet_module_core_get(), NULL);

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
