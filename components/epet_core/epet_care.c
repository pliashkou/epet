#include "epet_care.h"
#include <string.h>
#include <stdio.h>

/* Needs that justify interrupting the user. */
#define CARE_WATCHED (EPET_EV_MASK(EPET_EV_HUNGRY) | \
                      EPET_EV_MASK(EPET_EV_DIRTY)  | \
                      EPET_EV_MASK(EPET_EV_SICK)   | \
                      EPET_EV_MASK(EPET_EV_SAD))

/* Events that clear an outstanding need. */
#define CARE_CLEARED (EPET_EV_MASK(EPET_EV_FED)       | \
                      EPET_EV_MASK(EPET_EV_PLAYED)    | \
                      EPET_EV_MASK(EPET_EV_CLEANED)   | \
                      EPET_EV_MASK(EPET_EV_RECOVERED) | \
                      EPET_EV_MASK(EPET_EV_REBORN))

#define CARE_SUBSCRIBED (CARE_WATCHED | CARE_CLEARED | \
                         EPET_EV_MASK(EPET_EV_MINUTE) | \
                         EPET_EV_MASK(EPET_EV_DIED))

static void maybe_alert(epet_care_t *c, uint32_t age_ms)
{
    if (!c->reasons) return;

    bool due = !c->armed || (age_ms - c->last_alert_ms) >= c->min_interval_ms;
    if (!due) return;

    c->last_alert_ms = age_ms;
    c->armed = true;
    c->alerts++;
    epet_bus_publish(c->bus, EPET_EV_ATTENTION, age_ms, (int32_t)c->reasons, 0);
}

static void on_event(const epet_event_t *ev, void *ctx)
{
    epet_care_t *c = (epet_care_t *)ctx;

    switch (ev->type) {
    case EPET_EV_HUNGRY:
    case EPET_EV_DIRTY:
    case EPET_EV_SICK:
    case EPET_EV_SAD:
        c->reasons |= EPET_EV_MASK(ev->type);
        maybe_alert(c, ev->age_ms);       /* a fresh need alerts immediately */
        break;

    case EPET_EV_FED:       c->reasons &= ~EPET_EV_MASK(EPET_EV_HUNGRY); break;
    case EPET_EV_PLAYED:    c->reasons &= ~EPET_EV_MASK(EPET_EV_SAD);    break;
    case EPET_EV_CLEANED:   c->reasons &= ~EPET_EV_MASK(EPET_EV_DIRTY);  break;
    case EPET_EV_RECOVERED: c->reasons &= ~EPET_EV_MASK(EPET_EV_SICK);   break;

    case EPET_EV_REBORN:
        c->reasons = 0;
        c->armed = false;
        break;

    case EPET_EV_MINUTE:
        /* The periodic nudge: if a need is still outstanding, remind. */
        maybe_alert(c, ev->age_ms);
        break;

    case EPET_EV_DIED:
        c->reasons = 0;   /* nothing left to ask for */
        break;

    default:
        break;
    }
}

void epet_care_init(epet_care_t *care, epet_bus_t *bus, uint32_t min_interval_ms)
{
    care->bus = bus;
    care->reasons = 0;
    care->last_alert_ms = 0;
    care->min_interval_ms = min_interval_ms;
    care->alerts = 0;
    care->armed = false;
    epet_bus_subscribe(bus, CARE_SUBSCRIBED, on_event, care, "care");
}

const char *epet_care_reasons(const epet_care_t *care)
{
    static char buf[64];
    buf[0] = '\0';
    static const epet_event_type_t order[] = {
        EPET_EV_SICK, EPET_EV_HUNGRY, EPET_EV_DIRTY, EPET_EV_SAD,
    };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (!(care->reasons & EPET_EV_MASK(order[i]))) continue;
        if (buf[0]) strncat(buf, "|", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, epet_event_name(order[i]), sizeof(buf) - strlen(buf) - 1);
    }
    return buf[0] ? buf : "none";
}
