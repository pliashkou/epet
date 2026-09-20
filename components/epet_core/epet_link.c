#include "epet_link.h"
#include "epet_modblob.h"
#include "epet_save.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static void say(epet_link_t *l, const char *s)
{
    if (l->write) l->write(l->ctx, s);
}

static void sayf(epet_link_t *l, const char *fmt, ...)
{
    char buf[EPET_LINK_LINE_MAX];
    va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    say(l, buf);
}

/* ---- base64 ----------------------------------------------------------- */

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

/* Appends decoded bytes to l->rx. Returns false on a bad character or overflow. */
static bool b64_append(epet_link_t *l, const char *s)
{
    uint32_t acc = 0;
    int bits = 0;
    for (; *s; s++) {
        int v = b64val(*s);
        if (v == -1) {
            if (*s == ' ' || *s == '\r' || *s == '\n') continue;
            return false;
        }
        if (v == -2) break;                 /* padding ends the data */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (l->rx_len >= l->rx_cap) return false;
            l->rx[l->rx_len++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return true;
}

/* A dropped byte inside a base64 chunk still decodes to *something*, so
 * without a per-chunk check the corruption is silent and only shows up when
 * the whole image fails to verify -- minutes later. */
uint16_t epet_link_csum(const uint8_t *p, uint32_t n)
{
    uint16_t a = 1, b = 0;                  /* Fletcher-16 */
    for (uint32_t i = 0; i < n; i++) {
        a = (uint16_t)((a + p[i]) % 255);
        b = (uint16_t)((b + a) % 255);
    }
    return (uint16_t)((b << 8) | a);
}

/* ---- commands --------------------------------------------------------- */

static void cmd_list(epet_link_t *l)
{
    for (uint8_t i = 0; i < epet_modules_count(); i++) {
        const epet_module_t *m = epet_modules_get(i);
        if (!m) continue;
        sayf(l, "#MOD %s %u %u %u %s\n", m->id, (unsigned)m->version,
             (unsigned)m->n_species, (unsigned)m->n_pages,
             epet_modblob_is_loaded(m) ? "loaded" : "builtin");
    }
    say(l, "#END\n");
}

static void rx_free(epet_link_t *l)
{
    free(l->rx);
    l->rx = 0;
    l->rx_cap = l->rx_len = 0;
    l->receiving = false;
}

static void cmd_install_begin(epet_link_t *l, const char *arg)
{
    unsigned long want = strtoul(arg, 0, 10);
    if (want == 0 || want > EPET_PACK_MAX_BYTES) {
        sayf(l, "#ERR size\n");
        return;
    }
    rx_free(l);
    l->rx = malloc(want);
    if (!l->rx) { sayf(l, "#ERR memory\n"); return; }
    l->rx_cap = (uint32_t)want;
    l->rx_len = 0;
    l->rx_seq = 0;
    l->receiving = true;
    say(l, "#READY\n");
}

static void cmd_install_done(epet_link_t *l)
{
    if (!l->receiving) { say(l, "#ERR nodata\n"); return; }
    l->receiving = false;

    char id[16] = {0};
    if (!epet_modblob_peek(l->rx, l->rx_len, id, sizeof id, 0, 0, 0)) {
        say(l, "#ERR notapack\n");
        rx_free(l);
        return;
    }
    if (epet_modules_find(id)) {
        sayf(l, "#ERR installed %s\n", id);
        rx_free(l);
        return;
    }

    epet_module_t *m = 0;
    epet_blob_result_t r = epet_modblob_parse(l->rx, l->rx_len, &m);
    if (r != EPET_BLOB_OK || !m) {
        sayf(l, "#ERR parse %s\n", epet_blob_result_name(r));
        rx_free(l);
        return;
    }

    /* Persist the pack BEFORE installing, so a reboot right now still has it. */
    if (!epet_save_pack(id, l->rx, l->rx_len)) {
        say(l, "#ERR storage\n");
        epet_modblob_free(m);
        rx_free(l);
        return;
    }

    epet_modules_provide(m);
    if (!epet_modules_install(m, l->bus)) {
        say(l, "#ERR install\n");
        epet_erase_pack(id);
        epet_modblob_free(m);
        rx_free(l);
        return;
    }
    epet_save_modules();
    l->changed = true;
    sayf(l, "#OK %s\n", id);
    rx_free(l);
}

static void cmd_remove(epet_link_t *l, const char *id)
{
    if (!*id) { say(l, "#ERR noid\n"); return; }
    if (strcmp(id, "core") == 0) {
        say(l, "#ERR core\n");   /* removing all content would brick the UI */
        return;
    }
    const epet_module_t *m = epet_modules_find(id);
    if (!m) { sayf(l, "#ERR notfound %s\n", id); return; }

    bool loaded = epet_modblob_is_loaded(m);
    if (!epet_modules_remove(id)) { say(l, "#ERR remove\n"); return; }
    epet_erase_pack(id);
    epet_save_modules();
    if (loaded) epet_modblob_free((epet_module_t *)m);
    l->changed = true;
    sayf(l, "#OK %s\n", id);
}

/* ---- line handling ---------------------------------------------------- */

static void handle(epet_link_t *l, char *line)
{
    if (line[0] != '#') return;            /* not for us */
    char *sp = strchr(line, ' ');
    const char *arg = sp ? sp + 1 : "";
    if (sp) *sp = 0;

    if      (!strcmp(line, "#PING"))    sayf(l, "#PONG %d\n", EPET_LINK_PROTOCOL);
    else if (!strcmp(line, "#LIST"))    cmd_list(l);
    else if (!strcmp(line, "#INSTALL")) cmd_install_begin(l, arg);
    else if (!strcmp(line, "#D")) {
        if (!l->receiving) { say(l, "#ERR nodata\n"); return; }

        /* "<seq> <data>" -- the sequence number makes a dropped line
         * detectable. Acking every chunk also stops the host outrunning the
         * UART buffer, which silently truncated the pack before. */
        /* "<seq> <csum> <data>" */
        char *sp2 = strchr(arg, ' ');
        if (!sp2) { say(l, "#ERR nodseq\n"); return; }
        *sp2 = 0;
        unsigned long seq = strtoul(arg, 0, 10);
        char *sp3 = strchr(sp2 + 1, ' ');
        if (!sp3) { say(l, "#ERR nocsum\n"); return; }
        *sp3 = 0;
        unsigned long want_csum = strtoul(sp2 + 1, 0, 10);
        const char *data = sp3 + 1;

        if (seq != l->rx_seq) {
            /* Re-sent chunk we already have, or one arrived out of order.
             * Either way, tell the host exactly where to resume. */
            sayf(l, "#ERR seq %lu\n", (unsigned long)l->rx_seq);
            return;
        }
        uint32_t before = l->rx_len;
        if (!b64_append(l, data)) {
            l->rx_len = before;
            say(l, "#ERR b64\n");
            return;
        }
        if (epet_link_csum(l->rx + before, l->rx_len - before) !=
            (uint16_t)want_csum) {
            l->rx_len = before;             /* drop it; the host will resend */
            sayf(l, "#ERR csum %lu\n", (unsigned long)l->rx_seq);
            return;
        }
        l->rx_seq++;
        sayf(l, "#A %lu %lu\n", (unsigned long)l->rx_seq,
             (unsigned long)l->rx_len);
    }
    else if (!strcmp(line, "#DONE"))    cmd_install_done(l);
    else if (!strcmp(line, "#REMOVE"))  cmd_remove(l, arg);
    else if (l->extra && l->extra(l->extra_ctx, line, (char *)arg)) { /* handled */ }
    else                                sayf(l, "#ERR unknown\n");
}

void epet_link_init(epet_link_t *l, epet_link_write_fn write, void *ctx,
                    epet_bus_t *bus)
{
    memset(l, 0, sizeof *l);
    l->write = write;
    l->ctx = ctx;
    l->bus = bus;
}

void epet_link_feed(epet_link_t *l, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            if (l->line_len) {
                l->line[l->line_len] = 0;
                handle(l, l->line);
                l->line_len = 0;
            }
            continue;
        }
        if (l->line_len + 1 < EPET_LINK_LINE_MAX) {
            l->line[l->line_len++] = c;
        } else {
            l->line_len = 0;               /* overlong: drop it */
        }
    }
}

void epet_link_set_extra(epet_link_t *l, epet_link_extra_fn fn, void *ctx)
{
    l->extra = fn;
    l->extra_ctx = ctx;
}

void epet_link_say(epet_link_t *l, const char *text) { say(l, text); }

bool epet_link_take_changed(epet_link_t *l)
{
    bool c = l->changed;
    l->changed = false;
    return c;
}
