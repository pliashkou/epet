/* Menu navigation, scrolling, wrapping, and page lifecycle. */
#include <stdio.h>
#include <string.h>
#include "epet.h"
#include "epet_ui.h"
#include "epet_pages.h"

static int failures = 0;
#define CHECK(cond, ...) do {                           \
    if (!(cond)) {                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__); printf("\n");              \
        failures++;                                     \
    }                                                   \
} while (0)

static uint32_t M(epet_btn_t b) { return EPET_BTN_BIT(b); }

/* a page that records its lifecycle */
typedef struct {
    epet_page_t base;
    int entered, left, updates;
    bool want_close;
} probe_t;

static void probe_enter(epet_page_t *s, epet_ui_t *ui, epet_t *p)
{ (void)ui; (void)p; ((probe_t *)s)->entered++; }
static void probe_leave(epet_page_t *s, epet_ui_t *ui, epet_t *p)
{ (void)ui; (void)p; ((probe_t *)s)->left++; }
static bool probe_update(epet_page_t *s, epet_ui_t *ui, epet_t *p,
                         uint32_t dt, const bool edge[EPET_BTN_COUNT])
{ (void)ui; (void)p; (void)dt; (void)edge;
  probe_t *q = (probe_t *)s; q->updates++; return !q->want_close; }

int main(void)
{
    printf("menu navigation\n");

    epet_bus_t bus; epet_bus_init(&bus);
    epet_t pet; epet_init(&pet); epet_attach_bus(&pet, &bus);
    pet.display_timeout_ms = 0;            /* keep the screen on for the test */

    epet_ui_t ui; epet_ui_init(&ui, &bus);
    ui.menu_hide_ms = 0;                   /* never hide, for the nav tests */
    epet_pages_register_builtin(&ui);

    CHECK(ui.n_page > EPET_MENU_VISIBLE,
          "need more pages than fit on screen to exercise scrolling, got %d",
          ui.n_page);
    CHECK(ui.selected == 0 && ui.scroll == 0, "should start at the top");

    /* moving down inside the visible window does not scroll */
    for (int i = 0; i < EPET_MENU_VISIBLE - 1; i++) {
        epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
    }
    CHECK(ui.selected == EPET_MENU_VISIBLE - 1, "cursor at %d", ui.selected);
    CHECK(ui.scroll == 0, "should not have scrolled yet, scroll=%d", ui.scroll);

    /* the next one scrolls by exactly one row */
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
    CHECK(ui.selected == EPET_MENU_VISIBLE, "cursor at %d", ui.selected);
    CHECK(ui.scroll == 1, "should scroll by 1, got %d", ui.scroll);

    /* the cursor is always within the visible window */
    for (int i = 0; i < 40; i++) {
        epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
        CHECK(ui.selected >= ui.scroll &&
              ui.selected < ui.scroll + EPET_MENU_VISIBLE,
              "cursor %d outside window [%d,%d)",
              ui.selected, ui.scroll, ui.scroll + EPET_MENU_VISIBLE);
        CHECK(ui.scroll + EPET_MENU_VISIBLE <= ui.n_page,
              "window runs past the end: scroll=%d", ui.scroll);
    }

    /* wrap: from the last item, LB lands on the first */
    ui.selected = (uint8_t)(ui.n_page - 1);
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
    CHECK(ui.selected == 0, "LB from last should wrap to first, got %d", ui.selected);
    CHECK(ui.scroll == 0, "wrapping to the top should show the top, got %d", ui.scroll);

    /* wrap the other way too */
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LT));
    CHECK(ui.selected == ui.n_page - 1,
          "LT from first should wrap to last, got %d", ui.selected);
    CHECK(ui.selected < ui.scroll + EPET_MENU_VISIBLE,
          "wrapping to the end should scroll it into view");

    printf("page lifecycle\n");

    epet_ui_init(&ui, &bus);
    ui.menu_hide_ms = 0;
    probe_t probe = { .base = { .title = "PROBE", .enter = probe_enter,
                                .update = probe_update, .leave = probe_leave } };
    epet_ui_register(&ui, &probe.base);

    CHECK(ui.active == NULL, "should start on the main screen");
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_RT));       /* select */
    CHECK(ui.active == &probe.base, "RT should open the page");
    CHECK(probe.entered == 1, "enter should fire once, got %d", probe.entered);

    epet_ui_handle(&ui, &pet, 50, 0);
    CHECK(probe.updates == 1, "page should receive updates, got %d", probe.updates);

    /* RB closes from anywhere */
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_RB));
    CHECK(ui.active == NULL, "RB should close the page");
    CHECK(probe.left == 1, "leave should fire once, got %d", probe.left);

    /* the page's own update can close it */
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_RT));
    CHECK(ui.active == &probe.base, "reopened");
    probe.want_close = true;
    epet_ui_handle(&ui, &pet, 50, 0);
    CHECK(ui.active == NULL, "returning false should close the page");
    CHECK(probe.left == 2, "leave should fire again, got %d", probe.left);

    /* while a page is open, LT/LB must not move the menu underneath */
    probe.want_close = false;
    epet_ui_init(&ui, &bus);
    ui.menu_hide_ms = 0;
    epet_pages_register_builtin(&ui);
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));       /* cursor -> 1 */
    uint8_t before = ui.selected;
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_RT));       /* open it */
    CHECK(ui.active != NULL, "page should be open");
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LT));
    CHECK(ui.selected == before,
          "menu cursor must not move while a page is open: %d -> %d",
          before, ui.selected);

    /* open by title, for modules that want to jump straight to a page */
    epet_ui_close(&ui, &pet);
    CHECK(epet_ui_open_titled(&ui, &pet, "STATS"), "should find STATS");
    CHECK(ui.active != NULL && strcmp(ui.active->title, "STATS") == 0,
          "STATS should be active");
    CHECK(!epet_ui_open_titled(&ui, &pet, "NOPE"), "unknown title should fail");

    printf("menu auto-hide\n");

    epet_ui_init(&ui, &bus);
    epet_pages_register_builtin(&ui);
    ui.menu_hide_ms = 5000;
    CHECK(ui.menu_visible, "menu should be visible at boot so it is discoverable");

    /* it fades out on its own */
    for (int i = 0; i < 120; i++) epet_ui_handle(&ui, &pet, 50, 0);   /* 6 s */
    CHECK(!ui.menu_visible, "menu should hide after 5 s unused");

    /* LB brings it back, and that press is CONSUMED -- the cursor holds */
    uint8_t before_reveal = ui.selected;
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
    CHECK(ui.menu_visible, "LB should reveal the menu");
    CHECK(ui.selected == before_reveal,
          "the revealing press must not also move the cursor: %d -> %d",
          before_reveal, ui.selected);

    /* the NEXT press does move it */
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
    CHECK(ui.selected == (uint8_t)((before_reveal + 1) % ui.n_page),
          "the following press should move the cursor, got %d", ui.selected);

    /* LT reveals too */
    for (int i = 0; i < 120; i++) epet_ui_handle(&ui, &pet, 50, 0);
    CHECK(!ui.menu_visible, "menu should hide again");
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LT));
    CHECK(ui.menu_visible, "LT should reveal the menu");

    /* a reveal press must not launch a page either */
    for (int i = 0; i < 120; i++) epet_ui_handle(&ui, &pet, 50, 0);
    CHECK(!ui.menu_visible, "hidden again");
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_RT));
    CHECK(ui.menu_visible, "RT should reveal rather than surprise-launch");
    CHECK(ui.active == NULL, "the reveal press must NOT open a page");
    epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_RT));
    CHECK(ui.active != NULL, "the following RT should open the page");

    /* closing a page shows the menu again, so you can see where you are */
    epet_ui_close(&ui, &pet);
    CHECK(ui.menu_visible, "closing a page should re-show the menu");

    /* activity keeps it up */
    epet_ui_init(&ui, &bus);
    epet_pages_register_builtin(&ui);
    ui.menu_hide_ms = 5000;
    for (int i = 0; i < 40; i++) {
        epet_ui_handle(&ui, &pet, 50, 0);
        if (i % 10 == 0) epet_ui_handle(&ui, &pet, 50, M(EPET_BTN_LB));
    }
    CHECK(ui.menu_visible, "repeated input should keep the menu up");

    /* hide_ms 0 disables hiding */
    epet_ui_init(&ui, &bus);
    epet_pages_register_builtin(&ui);
    ui.menu_hide_ms = 0;
    for (int i = 0; i < 400; i++) epet_ui_handle(&ui, &pet, 50, 0);   /* 20 s */
    CHECK(ui.menu_visible, "hide_ms 0 should keep the menu up forever");

    /* every page carries an icon, so the menu is never a wall of letters */
    epet_ui_init(&ui, &bus);
    epet_pages_register_builtin(&ui);
    for (int i = 0; i < ui.n_page; i++) {
        CHECK(ui.page[i]->icon != NULL,
              "page '%s' should have an icon", ui.page[i]->title);
    }

    /* registration is bounded */
    epet_ui_init(&ui, &bus);
    static epet_page_t filler[EPET_MAX_PAGES];
    for (int i = 0; i < EPET_MAX_PAGES; i++) {
        filler[i].title = "X";
        CHECK(epet_ui_register(&ui, &filler[i]), "register %d should succeed", i);
    }
    static epet_page_t overflow = { .title = "OVER" };
    CHECK(!epet_ui_register(&ui, &overflow), "registering past the limit must fail");

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
