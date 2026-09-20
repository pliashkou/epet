#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "epet.h"
#include "epet_event.h"

/* Menu + pages (sub-programs).
 *
 * The main screen shows a scrolling menu down the left edge and the pet on
 * the right. Selecting a menu item runs that page, which then owns the whole
 * screen and the buttons until it closes.
 *
 * Controls (fixed):
 *   LT  previous item / page-defined
 *   LB  next item     / page-defined   -- wraps last -> first
 *   RT  SELECT: run the highlighted item / page-defined confirm
 *   RB  BACK:   close the running page and return to the menu
 *
 * Adding a sub-program means writing an epet_page_t and registering it. No
 * change to the menu, the simulation, or the input handling is needed. */

#define EPET_MENU_VISIBLE 4       /* items on screen at once */
#define EPET_MAX_PAGES    24
/* The menu is a translucent overlay that fades out when unused. */
#define EPET_MENU_HIDE_MS_DEFAULT 5000

struct epet_ui;
typedef struct epet_ui epet_ui_t;

typedef struct epet_page {
    const char *title;            /* used by open_titled, and as icon fallback */

    /* All hooks are optional. `self` lets a page keep private state in
     * its own struct by embedding epet_page_t first, or via ctx. */
    void (*enter) (struct epet_page *self, epet_ui_t *ui, epet_t *pet);
    /* Return false to close the page (same as BACK). edge[] is already
     * filtered: a press that only woke the display never reaches here. */
    bool (*update)(struct epet_page *self, epet_ui_t *ui, epet_t *pet,
                   uint32_t dt_ms, const bool edge[EPET_BTN_COUNT]);
    /* Draw the whole screen. Called only while this page is active. */
    void (*render)(struct epet_page *self, const epet_t *pet, uint16_t *fb);
    /* Draw this page's menu icon, centred on (cx, cy), roughly 26px across.
     * `tint` is already chosen for the selected/unselected state -- use it so
     * the menu stays legible. NULL falls back to the title's first letter. */
    void (*icon)  (struct epet_page *self, uint16_t *fb, int cx, int cy,
                   uint16_t tint, bool selected);
    void (*leave) (struct epet_page *self, epet_ui_t *ui, epet_t *pet);

    void *ctx;
} epet_page_t;

struct epet_ui {
    epet_page_t *page[EPET_MAX_PAGES];
    uint8_t      n_page;

    uint8_t      selected;        /* cursor within the full list */
    uint8_t      scroll;          /* index of the first visible row */
    epet_page_t *active;          /* NULL while on the main screen */

    epet_bus_t  *bus;
    uint32_t     anim_ms;         /* free-running, for page animations */

    /* Overlay visibility. The menu reveals on LT/LB and fades after
     * menu_hide_ms of no input. 0 disables hiding. */
    bool         menu_visible;
    uint32_t     menu_idle_ms;
    uint32_t     menu_hide_ms;
    uint32_t     menu_fade_ms;    /* counts up while appearing, for the fade */
};

void epet_ui_init(epet_ui_t *ui, epet_bus_t *bus);
bool epet_ui_register(epet_ui_t *ui, epet_page_t *page);

/* Feed it the surviving-edge mask from epet_update(). */
void epet_ui_handle(epet_ui_t *ui, epet_t *pet, uint32_t dt_ms,
                    uint32_t edge_mask);
void epet_ui_render(epet_ui_t *ui, const epet_t *pet, uint16_t *fb);

/* A page may close itself, or another module may force a page open. */
void epet_ui_close(epet_ui_t *ui, epet_t *pet);
bool epet_ui_open(epet_ui_t *ui, epet_t *pet, epet_page_t *page);
bool epet_ui_open_titled(epet_ui_t *ui, epet_t *pet, const char *title);

/* Menu geometry. The menu floats over the scene, so the content area is the
 * whole screen and the pet stays centred whether the menu is up or not. */
#define EPET_MENU_X     0
#define EPET_MENU_W     44
#define EPET_MENU_Y     48
#define EPET_MENU_ROW_H 46
#define EPET_CONTENT_X  0
#define EPET_CONTENT_W  EPET_W

/* Draws nothing when the menu is hidden. */
void epet_ui_render_menu(const epet_ui_t *ui, uint16_t *fb);
/* Reveal the menu (and restart its hide timer) from anywhere. */
void epet_ui_show_menu(epet_ui_t *ui);
