#pragma once
#include "epet_event.h"

/* Care advisor: watches the pet's need events and decides when the user
 * should actually be bothered.
 *
 * This is the template for extending the system. A module owns its state,
 * subscribes to what it cares about in its _init, and publishes its own
 * events. Nothing in the simulation loop knows it exists. */

typedef struct {
    epet_bus_t *bus;
    uint32_t    reasons;          /* masked need events currently outstanding */
    uint32_t    last_alert_ms;
    uint32_t    min_interval_ms;  /* rate limit between ATTENTION events */
    uint32_t    alerts;           /* lifetime count, handy in tests */
    bool        armed;            /* false until the first alert may be sent */
} epet_care_t;

/* Subscribes itself to bus. min_interval_ms rate-limits ATTENTION. */
void epet_care_init(epet_care_t *care, epet_bus_t *bus, uint32_t min_interval_ms);

/* Human-readable reason mask, e.g. "HUNGRY|DIRTY". Returns a static buffer. */
const char *epet_care_reasons(const epet_care_t *care);
