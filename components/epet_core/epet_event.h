#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Allocation-free publish/subscribe bus.
 *
 * Modules subscribe to a bitmask of event types and keep their own state in
 * their own ctx, so new behaviour is added by writing a module and
 * subscribing it -- not by editing the simulation loop.
 *
 * Events are queued rather than dispatched inline, so a handler may publish
 * further events without recursion. Drain with epet_bus_dispatch(). */

typedef enum {
    EPET_EV_MINUTE = 0,   /* pet age crossed a whole minute; a=minute index  */
    EPET_EV_FED,
    EPET_EV_PLAYED,
    EPET_EV_CLEANED,
    EPET_EV_POOPED,       /* a = pile count after the event                  */
    EPET_EV_HUNGRY,       /* need thresholds, with hysteresis so they do not */
    EPET_EV_SAD,          /* chatter around the boundary                     */
    EPET_EV_DIRTY,
    EPET_EV_SICK,
    EPET_EV_RECOVERED,
    EPET_EV_FELL_ASLEEP,
    EPET_EV_WOKE,
    EPET_EV_DIED,
    EPET_EV_REBORN,
    EPET_EV_DISPLAY_ON,
    EPET_EV_DISPLAY_OFF,
    EPET_EV_ATTENTION,    /* raised by epet_care; a = reason mask            */
    EPET_EV_BUTTON,       /* a = epet_btn_t, b = epet_action_t taken          */
    EPET_EV_COUNT
} epet_event_type_t;

_Static_assert(EPET_EV_COUNT <= 32, "event mask is 32 bits wide");

#define EPET_EV_MASK(t) (1u << (t))
#define EPET_EV_ALL     0xFFFFFFFFu

typedef struct {
    epet_event_type_t type;
    uint32_t          age_ms;   /* pet age when raised */
    int32_t           a;        /* type-specific payload */
    int32_t           b;
} epet_event_t;

typedef void (*epet_handler_fn)(const epet_event_t *ev, void *ctx);

#define EPET_MAX_SUBSCRIBERS  12
#define EPET_EVENT_QUEUE_LEN  32
/* Safety cap: a handler that republishes forever cannot wedge dispatch. */
#define EPET_DISPATCH_BUDGET  (EPET_EVENT_QUEUE_LEN * 4)

typedef struct {
    uint32_t        mask;
    epet_handler_fn fn;
    void           *ctx;
    const char     *name;
} epet_sub_t;

typedef struct {
    epet_sub_t   sub[EPET_MAX_SUBSCRIBERS];
    uint8_t      n_sub;

    epet_event_t q[EPET_EVENT_QUEUE_LEN];
    uint8_t      head;        /* next slot to read  */
    uint8_t      count;       /* events queued      */

    uint16_t     dropped;     /* queue overflows since init */
    uint32_t     published;
    uint32_t     delivered;
} epet_bus_t;

void        epet_bus_init(epet_bus_t *bus);
/* Returns false if the subscriber table is full. */
bool        epet_bus_subscribe(epet_bus_t *bus, uint32_t mask,
                               epet_handler_fn fn, void *ctx, const char *name);
/* Remove every subscription registered with this ctx. Required when a
 * module is uninstalled: its handler and state are about to be freed. */
void        epet_bus_unsubscribe(epet_bus_t *bus, void *ctx);
void        epet_bus_publish(epet_bus_t *bus, epet_event_type_t type,
                             uint32_t age_ms, int32_t a, int32_t b);
void        epet_bus_dispatch(epet_bus_t *bus);
bool        epet_bus_pending(const epet_bus_t *bus);
const char *epet_event_name(epet_event_type_t type);
