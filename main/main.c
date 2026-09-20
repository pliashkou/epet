/* ESP32-S3 platform layer for epet: ST7789 panel, buttons, power.
 * Everything else -- simulation, menu, pages, events -- lives in
 * components/epet_core and is shared verbatim with the macOS simulator. */
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_rom_uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "epet.h"
#include "epet_ui.h"
#include "epet_pages.h"
#include "epet_care.h"
#include "epet_draw.h"
#include "epet_module.h"
#include "epet_save.h"
#include "epet_store.h"
#include "epet_link.h"
#include "epet_modblob.h"
#include "driver/uart.h"
#include "ble_link.h"
#include "ota.h"
#include "imu.h"
#include "led.h"

extern const epet_module_t epet_module_update;

static const char *TAG = "epet";

/* Panel pins, from the board's factory program. */
#define LCD_HOST     SPI2_HOST
#define PIN_MOSI     41
#define PIN_CLK      40
#define PIN_CS       39
#define PIN_DC       38
#define PIN_RST      42
#define PIN_BL       4
#define LCD_Y_GAP    80          /* 240x240 window sits 80 rows into the GRAM */
#define LCD_CLOCK_HZ (40 * 1000 * 1000)

#define PIN_BOOT     0

/* Positional buttons: two down each side of the screen. */
static const int btn_pins[EPET_BTN_COUNT] = {
    [EPET_BTN_LT] = CONFIG_EPET_BTN_LT_GPIO,
    [EPET_BTN_LB] = CONFIG_EPET_BTN_LB_GPIO,
    [EPET_BTN_RT] = CONFIG_EPET_BTN_RT_GPIO,
    [EPET_BTN_RB] = CONFIG_EPET_BTN_RB_GPIO,
};

static esp_lcd_panel_handle_t panel;
/* One buffer, not two: epet_core writes panel-ready RGB565, so the render
 * target IS the DMA source. That removed a 9.5 ms per-frame byte-swap and
 * 115 KB of RAM. */
static uint16_t *core_fb;

static void lcd_flush(void);

static epet_t      pet;
static epet_bus_t  bus;
static epet_ui_t   ui;
static epet_care_t care;
static epet_autosave_t autosave;
static epet_link_t     link;

/* ---- panel ----------------------------------------------------------- */

static void lcd_init(void)
{
    gpio_config_t bl = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << PIN_BL };
    ESP_ERROR_CHECK(gpio_config(&bl));

    /* GPIO4 is the backlight in the vendor's own factory program, and on
     * this board driving it changes nothing: a sweep of GPIO4 plus 17 other
     * unassigned pins, both polarities, with the pin number shown on the
     * panel, never dimmed it. The backlight is wired to the rail and is not
     * switchable in software here. The pin is still driven in case a board
     * revision does wire it, but do not expect it to darken anything. */
    ESP_ERROR_CHECK(gpio_sleep_sel_dis(PIN_BL));

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = EPET_W * EPET_H * sizeof(uint16_t) + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t iocfg = {
        .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_CLOCK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &iocfg, &io));

    esp_lcd_panel_dev_config_t cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    /* Factory program uses MADCTL 0xC0 (MX+MY) with inversion on. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    gpio_set_level(PIN_BL, 1);
}

static void lcd_set_power(bool on)
{
    if (on) {
        ESP_ERROR_CHECK(esp_lcd_panel_disp_sleep(panel, false));
        vTaskDelay(pdMS_TO_TICKS(120));   /* ST7789 needs 120 ms after SLPOUT */
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        gpio_set_level(PIN_BL, 1);
    } else {
        /* Paint the panel black before switching it off.
         *
         * The backlight cannot be turned off on this board, so whatever the
         * controller still holds keeps being lit. Blanking first is the
         * difference between a glowing picture of a pet and a dark
         * rectangle -- the only dimming available here. */
        for (int i = 0; i < EPET_W * EPET_H; i++) core_fb[i] = 0;
        lcd_flush();

        gpio_set_level(PIN_BL, 0);
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_sleep(panel, true));
    }
}

static void lcd_flush(void)
{
    esp_lcd_panel_draw_bitmap(panel, 0, 0, EPET_W, EPET_H, core_fb);
}

/* ---- buttons --------------------------------------------------------- */

static void buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < EPET_BTN_COUNT; i++) {
        if (btn_pins[i] >= 0) mask |= 1ULL << btn_pins[i];
    }
#if CONFIG_EPET_BOOT_AS_LT
    mask |= 1ULL << PIN_BOOT;
#endif
    if (!mask) return;
    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT, .pin_bit_mask = mask,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static bool button_down(epet_btn_t b)
{
    bool hit = false;
    if (btn_pins[b] >= 0) hit = (gpio_get_level(btn_pins[b]) == 0);
#if CONFIG_EPET_BOOT_AS_LT
    if (b == EPET_BTN_LT && gpio_get_level(PIN_BOOT) == 0) hit = true;
#endif
    return hit;
}

/* ---- sleep ----------------------------------------------------------- */

#if CONFIG_EPET_LIGHT_SLEEP
static void sleep_init(void)
{
    for (int i = 0; i < EPET_BTN_COUNT; i++) {
        if (btn_pins[i] < 0) continue;
        ESP_ERROR_CHECK(gpio_wakeup_enable(btn_pins[i], GPIO_INTR_LOW_LEVEL));
    }
#if CONFIG_EPET_BOOT_AS_LT
    ESP_ERROR_CHECK(gpio_wakeup_enable(PIN_BOOT, GPIO_INTR_LOW_LEVEL));
#endif
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    /* Wake when the host talks to us. Without this the module link is dead
     * whenever the screen is off: the CPU is halted and never reads the
     * UART, so a browser sees no reply until a button or the timer wake. */
    ESP_ERROR_CHECK(uart_set_wakeup_threshold(CONFIG_ESP_CONSOLE_UART_NUM, 3));
    ESP_ERROR_CHECK(esp_sleep_enable_uart_wakeup(CONFIG_ESP_CONSOLE_UART_NUM));

#if CONFIG_EPET_WAKE_INTERVAL_MS > 0
    /* Also wake periodically so the simulation advances and modules get a
     * chance to react -- poops appear, needs cross, care can ask for you.
     * A timer wake does NOT light the screen; only a press or an alert does. */
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
        (uint64_t)CONFIG_EPET_WAKE_INTERVAL_MS * 1000ULL));
#endif
}

static void idle_sleep(uint32_t max_ms)
{
    /* Light sleep stops the UART clock mid-character otherwise. */
    esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);
    if (max_ms) {
        esp_sleep_enable_timer_wakeup((uint64_t)max_ms * 1000ULL);
    }
    esp_light_sleep_start();
}
#endif

/* ---- entropy ---------------------------------------------------------
 * esp_random() only yields true entropy once the RF subsystem is running.
 * With WiFi and BT off it can return the SAME value after every reset, which
 * made every boot hatch the same class. So we roll a seed forward in NVS:
 * each boot consumes the stored value and writes the next one. */
static uint32_t boot_seed(void)
{
    uint32_t seed = esp_random() ^ (uint32_t)esp_timer_get_time();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs unavailable (%s); class may repeat across boots",
                 esp_err_to_name(err));
        return seed;
    }

    nvs_handle_t h;
    if (nvs_open("epet", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(h, "seed", &stored) == ESP_OK) seed ^= stored;
        nvs_set_u32(h, "seed", seed * 1664525u + 1013904223u);
        nvs_commit(h);
        nvs_close(h);
    }
    return seed;
}

/* ---- persistence backend (NVS) ---------------------------------------
 * The core owns the save format; this only moves blobs. */
static bool nvs_read(void *ctx, const char *key, void *buf, size_t *len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open("epet", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return e == ESP_OK;
}

static bool nvs_write(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open("epet", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, key, buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static bool nvs_erase(void *ctx, const char *key)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open("epet", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
}

static const epet_store_t NVS_STORE = {
    .read = nvs_read, .write = nvs_write, .erase = nvs_erase, .ctx = NULL,
};

/* ---- module link over the console UART -------------------------------
 * Requests and replies are '#'-prefixed lines, so they coexist with logs on
 * the same port the browser is already connected to. */
static void link_write(void *ctx, const char *text)
{
    (void)ctx;
    uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, text, strlen(text));
}

static void link_poll(void)
{
    /* Drain fully: one small read per frame is far below the line rate. */
    char buf[512];
    for (int guard = 0; guard < 64; guard++) {
        int n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, (uint8_t *)buf,
                                sizeof buf, 0);
        if (n <= 0) break;
        epet_link_feed(&link, buf, (size_t)n);
    }
}

#if CONFIG_EPET_SHAKE_WAKE
/* Shake handling, shared by the awake and sleeping paths.
 *
 * A dead pet needs polling while the screen is ON as well: the wake hold
 * lights the display, and if credit only accrued during light sleep it would
 * freeze there and the revive hold could never be reached. */
static void shake_update(epet_t *pet, uint32_t elapsed_ms, uint32_t *credit)
{
    if (!imu_present()) return;

    /* The accelerometer stops updating across light sleep. Rewriting CTRL2
     * restarts the sampling pipeline, so do it only after an actual sleep --
     * doing it before every burst made the samples read almost identical. */
    if (elapsed_ms >= CONFIG_EPET_SHAKE_POLL_MS) imu_resume();

    if (imu_shaken(CONFIG_EPET_SHAKE_THRESHOLD_MG)) {
        *credit += elapsed_ms ? elapsed_ms : CONFIG_EPET_SHAKE_CONFIRM_MS;
    } else {
        uint32_t drain = (elapsed_ms ? elapsed_ms : CONFIG_EPET_SHAKE_CONFIRM_MS) / 2;
        *credit = *credit > drain ? *credit - drain : 0;
    }

    if (!pet->alive) {
        /* Light the screen at the normal hold so it is obvious the shaking
         * is registering, then keep counting toward the revive hold. */
        if (*credit >= CONFIG_EPET_SHAKE_HOLD_MS && !pet->display_on) {
            epet_display_nudge(pet, CONFIG_EPET_DISPLAY_TIMEOUT_MS);
        }
        if (*credit >= CONFIG_EPET_REVIVE_HOLD_MS) {
            *credit = 0;
            epet_display_nudge(pet, CONFIG_EPET_DISPLAY_TIMEOUT_MS);
            epet_apply_action(pet, EPET_ACT_REVIVE);
            ESP_LOGI(TAG, "shaken back to life: a %s hatched",
                     pet->species->name);
            epet_save_pet(pet);
        }
        return;
    }

    if (*credit >= CONFIG_EPET_SHAKE_HOLD_MS) {
        *credit = 0;
        ESP_LOGI(TAG, "shaken; waking the screen");
        epet_display_nudge(pet, CONFIG_EPET_DISPLAY_TIMEOUT_MS);
        epet_pet_emote(pet, EPET_POSE_HAPPY);
    }
}
#endif

/* ---- event subscribers ----------------------------------------------- */

static void on_attention(const epet_event_t *ev, void *ctx)
{
    epet_t *p = ctx;
    ESP_LOGW(TAG, "attention needed (%s) -- lighting the screen",
             epet_care_reasons(&care));
    (void)ev;
    epet_display_nudge(p, CONFIG_EPET_ALERT_SHOW_MS);
    /* Hook for a future LED or buzzer: no such part on this board. */
}

static void on_event(const epet_event_t *ev, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "ev %-12s t=%" PRIu32 "s a=%" PRId32,
             epet_event_name(ev->type), ev->age_ms / 1000u, ev->a);
}

/* ---- main ------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "epet booting");

    /* DMA-capable: the panel transfers straight out of it. */
    core_fb = heap_caps_malloc(EPET_W * EPET_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!core_fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return;
    }

    led_off();      /* an earlier pin scan latched the onboard LED on */
    lcd_init();
    buttons_init();
#if CONFIG_EPET_SHAKE_WAKE
    imu_init();
#endif
#if CONFIG_EPET_LIGHT_SLEEP
    sleep_init();
#endif

    epet_seed_random(boot_seed());
    epet_bus_init(&bus);

    epet_store_set(&NVS_STORE);   /* boot_seed() already brought NVS up */

    /* The console UART is also the module link, so give it a driver we can
     * read from. */
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                        4096, 0, 0, NULL, 0));

    /* Announce what this build can offer, re-parse any packs installed from
     * a browser, then restore what was installed last time. Core is always
     * installed: without it there is no content. */
    epet_modules_provide(epet_module_core_get());
#if CONFIG_EPET_BLE
    epet_modules_provide(&epet_module_update);
#endif
    uint8_t pack_bad = 0;
    epet_packs_result_t pack_why = EPET_PACKS_OK;
    uint8_t packs = epet_provide_saved_packs_ex(&pack_bad, &pack_why);
    ESP_LOGI(TAG, "packs: %d restored, %d unreadable (%s), free heap %u",
             packs, pack_bad, epet_packs_result_name(pack_why),
             (unsigned)esp_get_free_heap_size());
    uint8_t restored = epet_restore_modules(&bus);
    if (!epet_modules_find("core")) {
        epet_modules_install(epet_module_core_get(), &bus);
    }
#if CONFIG_EPET_BLE
    if (!epet_modules_find("update")) {
        epet_modules_install(&epet_module_update, &bus);
    }
#endif
    epet_save_modules();
    ESP_LOGI(TAG, "modules: %d installed (%d restored), %d characters",
             epet_modules_count(), restored, epet_species_count());

    epet_init(&pet);
    epet_attach_bus(&pet, &bus);

    ota_init();
    epet_link_init(&link, link_write, NULL, &bus);
    epet_link_set_extra(&link, ota_command, &link);
    epet_link_set_extra(ble_link_get(), ota_command, ble_link_get());

    epet_load_result_t lr = epet_load_pet(&pet);
    if (lr == EPET_LOAD_OK) {
        ESP_LOGI(TAG, "resumed a %s, age %" PRIu32 "s, hp %.0f",
                 pet.species->name, pet.age_ms / 1000u, pet.health);
    } else {
        ESP_LOGI(TAG, "no saved pet (%s); hatching a new one",
                 epet_load_result_name(lr));
    }
    pet.display_timeout_ms = CONFIG_EPET_DISPLAY_TIMEOUT_MS;
    pet.revive_hold_ms     = CONFIG_EPET_REVIVE_HOLD_MS;
    /* Loaded modules get events without a pet pointer of their own. */
    epet_set_active(&pet);

    epet_ui_init(&ui, &bus);
    ui.menu_hide_ms = CONFIG_EPET_MENU_HIDE_MS;
    epet_modules_populate_ui(&ui);

    epet_care_init(&care, &bus, CONFIG_EPET_CARE_INTERVAL_MS);
    epet_autosave_init(&autosave, &bus, CONFIG_EPET_AUTOSAVE_MS);
    epet_bus_subscribe(&bus, EPET_EV_MASK(EPET_EV_ATTENTION),
                       on_attention, &pet, "alert");
    epet_bus_subscribe(&bus, EPET_EV_ALL & ~EPET_EV_MASK(EPET_EV_BUTTON),
                       on_event, NULL, "log");

    if (lr != EPET_LOAD_OK) {
        ESP_LOGI(TAG, "hatched a %s -- %s", pet.species->name, pet.species->blurb);
    }
    ESP_LOGI(TAG, "buttons LT=%d LB=%d RT=%d RB=%d%s",
             btn_pins[EPET_BTN_LT], btn_pins[EPET_BTN_LB],
             btn_pins[EPET_BTN_RT], btn_pins[EPET_BTN_RB],
#if CONFIG_EPET_BOOT_AS_LT
             " (+BOOT as LT)");
#else
             "");
#endif
#if CONFIG_EPET_LIGHT_SLEEP
    ESP_LOGI(TAG, "blank after %d ms; light sleep, wake on GPIO or every %d ms",
             CONFIG_EPET_DISPLAY_TIMEOUT_MS, CONFIG_EPET_WAKE_INTERVAL_MS);
#else
    ESP_LOGI(TAG, "blank after %d ms; polling every %d ms",
             CONFIG_EPET_DISPLAY_TIMEOUT_MS, CONFIG_EPET_IDLE_POLL_MS);
#endif

    bool down[EPET_BTN_COUNT] = {false};
    bool panel_on = true;
    uint32_t shake_credit_ms = 0;
    int64_t prev = esp_timer_get_time();

    while (1) {
        bool edge[EPET_BTN_COUNT] = {false};
        for (int i = 0; i < EPET_BTN_COUNT; i++) {
            bool now = button_down(i);
            if (now && !down[i]) edge[i] = true;
            down[i] = now;
        }

        int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - prev) / 1000);
        prev = now_us;

        /* dt_ms may span a long light sleep, so step it in bounded chunks. */
        uint32_t mask = epet_advance(&pet, dt_ms, down, edge);
        epet_ui_handle(&ui, &pet, dt_ms, mask);
        epet_bus_dispatch(&bus);        /* handlers may nudge the display on */

        if (pet.display_on != panel_on) {
            panel_on = pet.display_on;
            ESP_LOGI(TAG, "display %s", panel_on ? "wake" : "sleep");
            lcd_set_power(panel_on);
            prev = esp_timer_get_time();   /* don't bill the 120 ms to dt */
        }

#if CONFIG_EPET_SHAKE_WAKE
        imu_tick(dt_ms);
#endif
        link_poll();
        if (epet_link_take_changed(&link) || epet_link_take_changed(ble_link_get())) {
            /* A module came or went: the pet's class may have gone with it,
             * and the menu must be rebuilt. */
            if (epet_validate_species(&pet)) {
                ESP_LOGW(TAG, "class was removed with its module; "
                              "started a %s", pet.species->name);
            }
            epet_ui_init(&ui, &bus);
            ui.menu_hide_ms = CONFIG_EPET_MENU_HIDE_MS;
            epet_modules_populate_ui(&ui);
            epet_save_pet(&pet);
            ESP_LOGI(TAG, "modules now: %d, characters: %d",
                     epet_modules_count(), epet_species_count());
        }

        epet_autosave_tick(&autosave, &pet, dt_ms);

        /* Stay awake while a central is connected or a transfer is running:
         * light sleep would stop us servicing either link. */
        if (link.receiving || ble_link_get()->receiving ||
            ble_link_connected() || ota_in_progress()) {
            /* A module is arriving: service the port, skip the redraw. */
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (pet.display_on) {
#if CONFIG_EPET_SHAKE_WAKE
            /* Only while dead: the sample burst blocks ~75 ms, which would
             * halve the frame rate if it ran every frame for no reason. */
            if (!pet.alive) {
                static uint32_t since;
                since += 33;
                if (since >= CONFIG_EPET_SHAKE_CONFIRM_MS) {
                    shake_update(&pet, since, &shake_credit_ms);
                    since = 0;
                }
            }
#endif
            epet_ui_render(&ui, &pet, core_fb);
            lcd_flush();
            vTaskDelay(pdMS_TO_TICKS(33));
        } else {
#if CONFIG_EPET_LIGHT_SLEEP
            /* Flush before sleeping: the next wake could be minutes away. */
            if (autosave.dirty) epet_save_pet(&pet);
            autosave.dirty = false;

            /* With no IMU interrupt line the accelerometer can only be read
             * when we are awake, so sleep in short hops while shake-to-wake
             * is on. Each hop is a few ms of CPU and one I2C read.
             * While a shake may be building, sample faster: at the idle rate
             * it would take seconds to gather enough samples to judge. */
            uint32_t nap = 0;
#if CONFIG_EPET_SHAKE_WAKE && CONFIG_EPET_IMU_INT_GPIO < 0
            if (imu_present()) {
                nap = shake_credit_ms > 0 ? CONFIG_EPET_SHAKE_CONFIRM_MS
                                          : CONFIG_EPET_SHAKE_POLL_MS;
            }
#endif
            int64_t nap_from = esp_timer_get_time();
            idle_sleep(nap);
            uint32_t slept_ms =
                (uint32_t)((esp_timer_get_time() - nap_from) / 1000);

            /* A UART wake means the host is mid-sentence; the bytes that woke
             * us are lost, so give it a moment and let the loop drain the
             * rest. The host retries its first command for this reason. */
            link_poll();

#if CONFIG_EPET_SHAKE_WAKE
            shake_update(&pet, slept_ms ? slept_ms : nap, &shake_credit_ms);
#endif
#else
            vTaskDelay(pdMS_TO_TICKS(CONFIG_EPET_IDLE_POLL_MS));
#endif
        }
    }
}
