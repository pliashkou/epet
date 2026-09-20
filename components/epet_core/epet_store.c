#include "epet_store.h"

static const epet_store_t *g_store;

void epet_store_set(const epet_store_t *store) { g_store = store; }
const epet_store_t *epet_store_get(void) { return g_store; }

bool epet_store_available(void)
{
    return g_store && g_store->read && g_store->write;
}
