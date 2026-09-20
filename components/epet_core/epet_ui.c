#include "epet_ui.h"
#include "epet_draw.h"
#include <string.h>

void epet_ui_init(epet_ui_t *ui, epet_bus_t *bus)
{
    memset(ui, 0, sizeof(*ui));
    ui->bus = bus;
    ui->menu_hide_ms = EPET_MENU_HIDE_MS_DEFAULT;
    ui->menu_visible = true;      /* show it once at boot so it is discoverable */
}

void epet_ui_show_menu(epet_ui_t *ui)
{
    if (!ui->menu_visible) ui->menu_fade_ms = 0;
    ui->menu_visible = true;
    ui->menu_idle_ms = 0;
}

bool epet_ui_register(epet_ui_t *ui, epet_page_t *page)
{
    if (!ui || !page || ui->n_page >= EPET_MAX_PAGES) return false;
    ui->page[ui->n_page++] = page;
    return true;
}

/* Keep the cursor inside the visible window, scrolling the minimum amount. */
static void follow_cursor(epet_ui_t *ui)
{
    if (ui->n_page <= EPET_MENU_VISIBLE) { ui->scroll = 0; return; }
    if (ui->selected < ui->scroll) {
        ui->scroll = ui->selected;
    } else if (ui->selected >= ui->scroll + EPET_MENU_VISIBLE) {
        ui->scroll = (uint8_t)(ui->selected - EPET_MENU_VISIBLE + 1);
    }
    uint8_t max_scroll = (uint8_t)(ui->n_page - EPET_MENU_VISIBLE);
    if (ui->scroll > max_scroll) ui->scroll = max_scroll;
}

static void move_cursor(epet_ui_t *ui, int delta)
{
    if (ui->n_page == 0) return;
    int n = ui->n_page;
    int next = (int)ui->selected + delta;
    /* Wrap both ways: past the last item lands on the first. */
    next = ((next % n) + n) % n;
    ui->selected = (uint8_t)next;
    follow_cursor(ui);
}

bool epet_ui_open(epet_ui_t *ui, epet_t *pet, epet_page_t *page)
{
    if (!page) return false;
    if (ui->active) epet_ui_close(ui, pet);
    ui->active = page;
    if (page->enter) page->enter(page, ui, pet);
    return true;
}

bool epet_ui_open_titled(epet_ui_t *ui, epet_t *pet, const char *title)
{
    for (uint8_t i = 0; i < ui->n_page; i++) {
        if (strcmp(ui->page[i]->title, title) == 0) {
            ui->selected = i;
            follow_cursor(ui);
            return epet_ui_open(ui, pet, ui->page[i]);
        }
    }
    return false;
}

void epet_ui_close(epet_ui_t *ui, epet_t *pet)
{
    if (!ui->active) return;
    epet_page_t *p = ui->active;
    ui->active = NULL;              /* clear first: leave() may reopen */
    if (p->leave) p->leave(p, ui, pet);
    epet_ui_show_menu(ui);          /* back on the main screen, show where we are */
}

void epet_ui_handle(epet_ui_t *ui, epet_t *pet, uint32_t dt_ms, uint32_t edge_mask)
{
    ui->anim_ms += dt_ms;

    bool edge[EPET_BTN_COUNT];
    for (int i = 0; i < EPET_BTN_COUNT; i++) {
        edge[i] = (edge_mask & EPET_BTN_BIT(i)) != 0;
    }

    if (ui->active) {
        /* BACK always closes, whatever the page does with the other three. */
        if (edge[EPET_BTN_RB]) {
            epet_ui_close(ui, pet);
            return;
        }
        epet_page_t *p = ui->active;
        if (p->update && !p->update(p, ui, pet, dt_ms, edge)) {
            epet_ui_close(ui, pet);
        }
        return;
    }

    /* Main screen. The menu hides itself when unused; the press that brings
     * it back is consumed, so revealing never also moves the cursor or
     * launches a page -- the same rule as the display wake press. */
    bool any = edge[EPET_BTN_LT] || edge[EPET_BTN_LB] || edge[EPET_BTN_RT];

    if (!ui->menu_visible) {
        if (any) epet_ui_show_menu(ui);
        return;
    }

    if (any) ui->menu_idle_ms = 0;
    ui->menu_fade_ms += dt_ms;

    if (edge[EPET_BTN_LT]) move_cursor(ui, -1);
    if (edge[EPET_BTN_LB]) move_cursor(ui, +1);
    if (edge[EPET_BTN_RT] && ui->n_page > 0) {
        epet_ui_open(ui, pet, ui->page[ui->selected]);
        return;
    }

    ui->menu_idle_ms += dt_ms;
    if (ui->menu_hide_ms && ui->menu_idle_ms >= ui->menu_hide_ms) {
        ui->menu_visible = false;
    }
}

void epet_ui_render_menu(const epet_ui_t *ui, uint16_t *fb)
{
    if (!ui->menu_visible || ui->n_page == 0) return;

    /* Fade in over ~180 ms so the reveal does not snap. */
    uint8_t appear = 255;
    if (ui->menu_fade_ms < 180) {
        appear = (uint8_t)(ui->menu_fade_ms * 255u / 180u);
    }
    const uint8_t panel_a = (uint8_t)(150 * appear / 255);
    const uint8_t sel_a   = (uint8_t)(210 * appear / 255);

    epet_shade(fb, EPET_MENU_X, EPET_MENU_Y, EPET_MENU_W,
               EPET_H - EPET_MENU_Y, EPET_C_PANEL, panel_a);

    for (int row = 0; row < EPET_MENU_VISIBLE; row++) {
        int idx = ui->scroll + row;
        if (idx >= ui->n_page) break;

        int y = EPET_MENU_Y + row * EPET_MENU_ROW_H;
        int cx = EPET_MENU_X + EPET_MENU_W / 2;
        int cy = y + EPET_MENU_ROW_H / 2 - 1;
        bool sel = (idx == ui->selected);

        if (sel) {
            epet_shade(fb, EPET_MENU_X + 2, y + 2, EPET_MENU_W - 4,
                       EPET_MENU_ROW_H - 6, EPET_C_MENUSEL, sel_a);
            /* bright rail marks the current item even at a glance */
            epet_shade(fb, EPET_MENU_X, y + 2, 3, EPET_MENU_ROW_H - 6,
                       EPET_C_WHITE, appear);
        }

        uint16_t tint = sel ? EPET_C_WHITE : EPET_C_DIM;
        epet_page_t *pg = ui->page[idx];
        if (pg->icon) {
            pg->icon(pg, fb, cx, cy, tint, sel);
        } else {
            char letter[2] = { pg->title[0], 0 };
            epet_text(fb, cx - 6, cy - 7, letter, tint, 2);
        }
    }

}

void epet_ui_render(epet_ui_t *ui, const epet_t *pet, uint16_t *fb)
{
    if (ui->active && ui->active->render) {
        ui->active->render(ui->active, pet, fb);
        return;
    }
    epet_render_main(ui, pet, fb);
}
