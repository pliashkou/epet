#include "epet_event.h"

static const char *const NAMES[EPET_EV_COUNT] = {
    [EPET_EV_MINUTE]      = "MINUTE",
    [EPET_EV_FED]         = "FED",
    [EPET_EV_PLAYED]      = "PLAYED",
    [EPET_EV_CLEANED]     = "CLEANED",
    [EPET_EV_POOPED]      = "POOPED",
    [EPET_EV_HUNGRY]      = "HUNGRY",
    [EPET_EV_SAD]         = "SAD",
    [EPET_EV_DIRTY]       = "DIRTY",
    [EPET_EV_SICK]        = "SICK",
    [EPET_EV_RECOVERED]   = "RECOVERED",
    [EPET_EV_FELL_ASLEEP] = "FELL_ASLEEP",
    [EPET_EV_WOKE]        = "WOKE",
    [EPET_EV_DIED]        = "DIED",
    [EPET_EV_REBORN]      = "REBORN",
    [EPET_EV_DISPLAY_ON]  = "DISPLAY_ON",
    [EPET_EV_DISPLAY_OFF] = "DISPLAY_OFF",
    [EPET_EV_ATTENTION]   = "ATTENTION",
    [EPET_EV_BUTTON]      = "BUTTON",
};

const char *epet_event_name(epet_event_type_t type)
{
    if (type < 0 || type >= EPET_EV_COUNT || !NAMES[type]) return "?";
    return NAMES[type];
}

void epet_bus_init(epet_bus_t *bus)
{
    bus->n_sub = 0;
    bus->head = 0;
    bus->count = 0;
    bus->dropped = 0;
    bus->published = 0;
    bus->delivered = 0;
}

bool epet_bus_subscribe(epet_bus_t *bus, uint32_t mask,
                        epet_handler_fn fn, void *ctx, const char *name)
{
    if (!bus || !fn || bus->n_sub >= EPET_MAX_SUBSCRIBERS) return false;
    bus->sub[bus->n_sub++] = (epet_sub_t){
        .mask = mask, .fn = fn, .ctx = ctx, .name = name ? name : "?",
    };
    return true;
}

void epet_bus_unsubscribe(epet_bus_t *bus, void *ctx)
{
    if (!bus) return;
    uint8_t w = 0;
    for (uint8_t i = 0; i < bus->n_sub; i++) {
        if (bus->sub[i].ctx == ctx) continue;
        if (w != i) bus->sub[w] = bus->sub[i];
        w++;
    }
    bus->n_sub = w;
}

void epet_bus_publish(epet_bus_t *bus, epet_event_type_t type,
                      uint32_t age_ms, int32_t a, int32_t b)
{
    if (!bus) return;               /* publishing with no bus attached is fine */
    bus->published++;

    if (bus->count >= EPET_EVENT_QUEUE_LEN) {
        /* Drop the newest: losing a fresh event is preferable to shifting the
         * queue and dropping one already waiting to be handled. */
        if (bus->dropped < UINT16_MAX) bus->dropped++;
        return;
    }
    uint8_t slot = (uint8_t)((bus->head + bus->count) % EPET_EVENT_QUEUE_LEN);
    bus->q[slot] = (epet_event_t){ .type = type, .age_ms = age_ms, .a = a, .b = b };
    bus->count++;
}

bool epet_bus_pending(const epet_bus_t *bus)
{
    return bus && bus->count > 0;
}

void epet_bus_dispatch(epet_bus_t *bus)
{
    if (!bus) return;

    int budget = EPET_DISPATCH_BUDGET;
    while (bus->count > 0 && budget-- > 0) {
        epet_event_t ev = bus->q[bus->head];
        bus->head = (uint8_t)((bus->head + 1) % EPET_EVENT_QUEUE_LEN);
        bus->count--;

        for (uint8_t i = 0; i < bus->n_sub; i++) {
            if (bus->sub[i].mask & EPET_EV_MASK(ev.type)) {
                bus->sub[i].fn(&ev, bus->sub[i].ctx);
                bus->delivered++;
            }
        }
    }
}
