#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Persistence.
 *
 * The core does not know what a filesystem or NVS is. A platform supplies
 * three blob operations keyed by a short string; the core builds the save
 * format on top so device and simulator persist identically. */

typedef struct {
    /* On entry *len is the buffer size; on success it is the bytes read. */
    bool (*read) (void *ctx, const char *key, void *buf, size_t *len);
    bool (*write)(void *ctx, const char *key, const void *buf, size_t len);
    bool (*erase)(void *ctx, const char *key);
    void *ctx;
} epet_store_t;

/* Pass NULL to disable persistence (the default). */
void                epet_store_set(const epet_store_t *store);
const epet_store_t *epet_store_get(void);
bool                epet_store_available(void);
