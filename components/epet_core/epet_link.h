#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "epet_module.h"

/* Line protocol for installing and removing modules from a browser.
 *
 * Text, so it can share the serial port with log output: every request and
 * reply starts with '#', and anything else on the wire is ignored. Payloads
 * are base64 in fixed-size chunks.
 *
 *   host: #LIST
 *   dev : #MOD <id> <version> <n_species> <n_pages> <builtin|loaded>
 *         ... one per installed module ...
 *         #END
 *
 *   host: #INSTALL <length>
 *   dev : #READY
 *   host: #D <seq> <csum> <base64>  (repeated, seq from 0)
 *   dev : #A <next-seq> <bytes so far>
 *         or #ERR seq <expected>   if a chunk was missed
 *   host: #DONE
 *   dev : #OK <id> | #ERR <reason>
 *
 * Chunks carry a sequence number and a Fletcher-16 of their decoded bytes.
 * The sequence number catches a dropped line; the checksum catches bytes
 * dropped INSIDE a line, which otherwise decode to plausible garbage and
 * corrupt the payload silently.
 *
 *   host: #REMOVE <id>
 *   dev : #OK <id> | #ERR <reason>
 *
 *   host: #PING          dev: #PONG <protocol version>
 *   host: #NEWPET        dev: #OK <class>   discard the pet, hatch a new one
 *
 * Feed received bytes to epet_link_feed(). Replies go out through the writer
 * the platform installed. */

#define EPET_LINK_PROTOCOL 1
#define EPET_LINK_LINE_MAX 560

typedef void (*epet_link_write_fn)(void *ctx, const char *text);
/* Handle a command the core link does not know. Return true if handled.
 * Used by the firmware for OTA, which is inherently platform specific. */
typedef bool (*epet_link_extra_fn)(void *ctx, const char *cmd, char *arg);

typedef struct {
    epet_link_write_fn write;
    void              *ctx;
    epet_bus_t        *bus;

    epet_link_extra_fn extra;
    void              *extra_ctx;

    char     line[EPET_LINK_LINE_MAX];
    uint16_t line_len;

    uint8_t *rx;            /* install buffer, allocated while receiving */
    uint32_t rx_cap;
    uint32_t rx_len;
    uint32_t rx_seq;        /* next chunk number expected */
    bool     receiving;

    /* set when the installed/removed set changed, so the app can re-check
     * the pet's class and persist the new module list */
    bool     changed;
} epet_link_t;

void epet_link_init(epet_link_t *l, epet_link_write_fn write, void *ctx,
                    epet_bus_t *bus);
void epet_link_set_extra(epet_link_t *l, epet_link_extra_fn fn, void *ctx);
void epet_link_feed(epet_link_t *l, const char *data, size_t len);
/* Reply helper, so an extension can answer in the same style. */
void epet_link_say(epet_link_t *l, const char *text);
uint16_t epet_link_csum(const uint8_t *p, uint32_t n);
/* True once, clearing the flag: something was installed or removed. */
bool epet_link_take_changed(epet_link_t *l);
