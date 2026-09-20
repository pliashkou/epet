/* Turn the onboard addressable LED off.
 *
 * An I2C pin scan drove GPIO 15 with garbage while hunting for the IMU, and
 * a WS2812 latches whatever it decodes -- so the LED came on and stayed on.
 * It keeps its colour until new data arrives, so floating the pin is not
 * enough: it has to be sent an explicit all-zero frame. */
#include "led.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

#define LED_GPIO   15
#define LED_COUNT  8        /* generous: covers a single LED or a short strip */

static const char *TAG = "epet-led";

void led_off(void)
{
    /* The copy encoder is enough here: build the symbol list ourselves and
     * hand it over verbatim, rather than writing a streaming encoder for a
     * one-shot "everything off". */
    rmt_channel_handle_t chan = NULL;
    rmt_tx_channel_config_t cfg = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,      /* 100 ns per tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&cfg, &chan) != ESP_OK) {
        /* No RMT available: at least stop driving the pin. */
        gpio_reset_pin(LED_GPIO);
        return;
    }

    rmt_encoder_handle_t copy = NULL;
    rmt_copy_encoder_config_t copy_cfg = {};
    if (rmt_new_copy_encoder(&copy_cfg, &copy) != ESP_OK) {
        rmt_del_channel(chan);
        gpio_reset_pin(LED_GPIO);
        return;
    }

    /* A WS2812 "0" bit: ~300ns high then ~900ns low, at 100ns per tick.
     * Name it carefully -- BIT0 is an ESP-IDF macro. */
    static rmt_symbol_word_t syms[LED_COUNT * 24];
    const rmt_symbol_word_t zero_bit = {
        .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9,
    };
    for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) syms[i] = zero_bit;

    rmt_transmit_config_t tx = { .loop_count = 0 };
    if (rmt_enable(chan) == ESP_OK) {
        rmt_transmit(chan, copy, syms, sizeof syms, &tx);
        rmt_tx_wait_all_done(chan, 200);
        rmt_disable(chan);
        ESP_LOGI(TAG, "onboard LED cleared");
    }

    rmt_del_encoder(copy);
    rmt_del_channel(chan);
    gpio_reset_pin(LED_GPIO);
}
