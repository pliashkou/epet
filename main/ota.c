#include "ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "epet-ota";

static esp_ota_handle_t g_handle;
static const esp_partition_t *g_part;
static bool     g_active;
static uint32_t g_expect, g_written, g_seq;

/* base64 state must survive between chunks only in whole groups; each chunk
 * is a multiple of 4 characters, so nothing is carried across. */
static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

static int b64_decode(const char *s, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *s; s++) {
        int v = b64val(*s);
        if (v == -1) {
            if (*s == ' ' || *s == '\r' || *s == '\n') continue;
            return -1;
        }
        if (v == -2) break;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (int)n;
}

void ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running from %s", run ? run->label : "?");

    /* If we just booted a freshly flashed image, accept it. Without this the
     * bootloader rolls back to the previous slot on the next reset. */
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "new firmware booted; marking it valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool ota_in_progress(void) { return g_active; }

int ota_progress(void)
{
    if (!g_active || !g_expect) return 0;
    uint32_t p = (uint32_t)((uint64_t)g_written * 100u / g_expect);
    return p > 100 ? 100 : (int)p;
}

const char *ota_running_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

static void abort_ota(void)
{
    if (g_active) {
        esp_ota_abort(g_handle);
        g_active = false;
    }
    g_expect = g_written = g_seq = 0;
}

bool ota_command(void *ctx, const char *cmd, char *arg)
{
    epet_link_t *l = ctx;
    char reply[96];

    if (!strcmp(cmd, "#FWINFO")) {
        const esp_app_desc_t *d = esp_app_get_description();
        snprintf(reply, sizeof reply, "#FWINFO %s %s %s\n",
                 d->version, d->date, ota_running_slot());
        epet_link_say(l, reply);
        return true;
    }

    if (!strcmp(cmd, "#FW")) {
        abort_ota();
        unsigned long want = strtoul(arg, 0, 10);
        g_part = esp_ota_get_next_update_partition(NULL);
        if (!g_part) { epet_link_say(l, "#ERR noslot\n"); return true; }
        if (want == 0 || want > g_part->size) {
            snprintf(reply, sizeof reply, "#ERR size %u\n", (unsigned)g_part->size);
            epet_link_say(l, reply);
            return true;
        }
        esp_err_t e = esp_ota_begin(g_part, want, &g_handle);
        if (e != ESP_OK) {
            snprintf(reply, sizeof reply, "#ERR begin %s\n", esp_err_to_name(e));
            epet_link_say(l, reply);
            return true;
        }
        g_active = true;
        g_expect = (uint32_t)want;
        g_written = 0;
        g_seq = 0;
        ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)want, g_part->label);
        snprintf(reply, sizeof reply, "#FREADY %s\n", g_part->label);
        epet_link_say(l, reply);
        return true;
    }

    if (!strcmp(cmd, "#FD")) {
        if (!g_active) { epet_link_say(l, "#ERR nofw\n"); return true; }
        char *sp = strchr(arg, ' ');
        if (!sp) { epet_link_say(l, "#ERR nofdseq\n"); return true; }
        *sp = 0;
        unsigned long seq = strtoul(arg, 0, 10);
        char *sp2 = strchr(sp + 1, ' ');
        if (!sp2) { epet_link_say(l, "#ERR nofdcsum\n"); return true; }
        *sp2 = 0;
        unsigned long want_csum = strtoul(sp + 1, 0, 10);

        if (seq != g_seq) {
            snprintf(reply, sizeof reply, "#ERR seq %u\n", (unsigned)g_seq);
            epet_link_say(l, reply);
            return true;
        }
        static uint8_t buf[512];
        int n = b64_decode(sp2 + 1, buf, sizeof buf);
        if (n < 0) { epet_link_say(l, "#ERR b64\n"); return true; }

        /* Verify BEFORE writing: esp_ota_write is append-only, so a bad
         * chunk written here cannot be taken back and the whole image would
         * fail verification minutes later. */
        if (epet_link_csum(buf, (uint32_t)n) != (uint16_t)want_csum) {
            snprintf(reply, sizeof reply, "#ERR csum %u\n", (unsigned)g_seq);
            epet_link_say(l, reply);
            return true;
        }

        esp_err_t e = esp_ota_write(g_handle, buf, (size_t)n);
        if (e != ESP_OK) {
            snprintf(reply, sizeof reply, "#ERR write %s\n", esp_err_to_name(e));
            epet_link_say(l, reply);
            abort_ota();
            return true;
        }
        g_written += (uint32_t)n;
        g_seq++;
        snprintf(reply, sizeof reply, "#FA %u %u\n",
                 (unsigned)g_seq, (unsigned)g_written);
        epet_link_say(l, reply);
        return true;
    }

    if (!strcmp(cmd, "#FDONE")) {
        if (!g_active) { epet_link_say(l, "#ERR nofw\n"); return true; }
        esp_err_t e = esp_ota_end(g_handle);
        g_active = false;
        if (e != ESP_OK) {
            /* A bad image is rejected here rather than at boot. */
            snprintf(reply, sizeof reply, "#ERR image %s\n", esp_err_to_name(e));
            epet_link_say(l, reply);
            return true;
        }
        e = esp_ota_set_boot_partition(g_part);
        if (e != ESP_OK) {
            snprintf(reply, sizeof reply, "#ERR setboot %s\n", esp_err_to_name(e));
            epet_link_say(l, reply);
            return true;
        }
        snprintf(reply, sizeof reply, "#FOK %s %u\n", g_part->label,
                 (unsigned)g_written);
        epet_link_say(l, reply);
        ESP_LOGW(TAG, "firmware written; rebooting");
        vTaskDelay(pdMS_TO_TICKS(400));    /* let the reply drain */
        esp_restart();
        return true;
    }

    if (!strcmp(cmd, "#FABORT")) {
        abort_ota();
        epet_link_say(l, "#OK aborted\n");
        return true;
    }

    return false;
}
