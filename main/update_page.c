/* UPDATE: the only place the radio is switched on.
 *
 * Shipped as its own module, so it shows up in MODS alongside core and
 * anything installed. Living in main/ rather than epet_core keeps the
 * platform-specific radio out of the portable code. */
#include "epet_module.h"
#include "epet_draw.h"
#include "epet_ui.h"
#include "ble_link.h"
#include "esp_log.h"
#include "sdkconfig.h"

static epet_bus_t *g_bus;

static void update_enter(epet_page_t *self, epet_ui_t *ui, epet_t *pet)
{
    (void)self; (void)ui; (void)pet;
    /* Opening the page IS the request to be updatable. */
    ble_link_start(g_bus);
}

static bool update_tick(epet_page_t *self, epet_ui_t *ui, epet_t *pet,
                        uint32_t dt_ms, const bool edge[EPET_BTN_COUNT])
{
    (void)self; (void)ui; (void)pet; (void)dt_ms;
    if (edge[EPET_BTN_RT]) {
        if (ble_link_active()) ble_link_stop();
        else                   ble_link_start(g_bus);
    }
    return true;
}

static void update_leave(epet_page_t *self, epet_ui_t *ui, epet_t *pet)
{
    (void)self; (void)ui; (void)pet;
    /* Leaving turns it off again -- ble_link_stop() declines while a
     * transfer is in flight, so backing out mid-install cannot corrupt it. */
    ble_link_stop();
}

static void update_render(epet_page_t *self, const epet_t *pet, uint16_t *fb)
{
    (void)pet;
    ble_state_t st = ble_link_state();

    epet_fill(fb, EPET_C_PANEL);
    uint16_t accent = (st == BLE_STATE_OFF)      ? EPET_RGB565(70, 78, 92)
                    : (st == BLE_STATE_RECEIVING)? EPET_RGB565(90, 190, 120)
                                                 : EPET_RGB565(70, 130, 210);
    epet_rect(fb, 0, 0, EPET_W, 28, accent);
    int tw = epet_text_width(self->title, 2);
    epet_text(fb, (EPET_W - tw) / 2, 7, self->title, EPET_C_WHITE, 2);

    /* a simple radio glyph that fills in as the state improves */
    int cx = 120, cy = 92;
    epet_disc(fb, cx, cy + 22, 6, st == BLE_STATE_OFF ? EPET_C_DIM : EPET_C_WHITE);
    for (int r = 14; r <= 38; r += 12) {
        uint16_t c = EPET_C_DIM;
        if (st == BLE_STATE_ADVERTISING && r <= 26) c = EPET_C_WHITE;
        if (st == BLE_STATE_CONNECTED || st == BLE_STATE_RECEIVING) c = EPET_C_WHITE;
        for (int a = -40; a <= 40; a += 2) {
            int x = cx + (a * r) / 60;
            int y = cy + 22 - (r * 7) / 10 + (a * a) / (r * 2);
            epet_rect(fb, x, y, 2, 2, c);
        }
    }

    const char *label, *hint;
    uint16_t colour = EPET_C_WHITE;
    switch (st) {
    case BLE_STATE_OFF:
        label = "RADIO OFF"; hint = "RT TURN ON"; colour = EPET_C_DIM; break;
    case BLE_STATE_ADVERTISING:
        label = "READY TO PAIR"; hint = "RT TURN OFF"; break;
    case BLE_STATE_CONNECTED:
        label = "CONNECTED"; hint = "RT TURN OFF"; colour = EPET_C_GOOD; break;
    default:
        label = "RECEIVING"; hint = "DO NOT UNPLUG"; colour = EPET_C_GOOD; break;
    }
    tw = epet_text_width(label, 2);
    epet_text(fb, (EPET_W - tw) / 2, 140, label, colour, 2);

    if (st != BLE_STATE_OFF) {
        epet_text(fb, 10, 166, "NAME", EPET_C_WHITE, 1);
        epet_text(fb, 60, 166, "EPET", EPET_C_GOOD, 1);
    }
    if (st == BLE_STATE_RECEIVING) {
        uint8_t pct = ble_link_progress();
        epet_bar(fb, 20, 182, 200, 11, (float)pct, EPET_C_GOOD);
        epet_number(fb, 108, 198, pct, EPET_C_WHITE, 1);
        epet_text(fb, 126, 198, "%", EPET_C_WHITE, 1);
    } else {
        epet_text(fb, 10, 184, "SERIAL ALWAYS ON", EPET_C_DIM, 1);
        epet_text(fb, 10, 198, "RADIO ONLY WHILE HERE", EPET_C_DIM, 1);
    }

    epet_rect(fb, 0, EPET_H - 16, EPET_W, 16, EPET_C_MENU);
    epet_text(fb, 6, EPET_H - 12, hint, EPET_C_DIM, 1);
    epet_text(fb, 170, EPET_H - 12, "RB BACK", EPET_C_DIM, 1);
}

static void update_icon(epet_page_t *self, uint16_t *fb, int cx, int cy,
                        uint16_t tint, bool sel)
{
    (void)self; (void)sel;
    epet_disc(fb, cx, cy + 7, 3, tint);
    for (int r = 5; r <= 11; r += 3) {
        for (int a = -30; a <= 30; a += 3) {
            int x = cx + (a * r) / 40;
            int y = cy + 7 - (r * 8) / 10 + (a * a) / (r * 3);
            epet_rect(fb, x, y, 2, 2, tint);
        }
    }
}

static epet_page_t page_update = {
    .title = "UPDATE", .enter = update_enter, .update = update_tick,
    .render = update_render, .leave = update_leave, .icon = update_icon,
};

static epet_page_t *const UPDATE_PAGES[] = { &page_update };

static void on_install(const epet_module_t *self, epet_bus_t *bus)
{
    (void)self;
    g_bus = bus;
}

const epet_module_t epet_module_update = {
    .id = "update", .name = "UPDATE MODE", .version = 1,
    .species = 0, .n_species = 0,
    .pages = UPDATE_PAGES, .n_pages = 1,
    .on_install = on_install,
};
