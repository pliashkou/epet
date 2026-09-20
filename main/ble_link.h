#pragma once
#include <stdbool.h>
#include "epet_event.h"
#include "epet_link.h"

/* Module link over BLE, as a Nordic UART Service (the de-facto serial-over-
 * BLE profile, and what Web Bluetooth can talk to without a shim).
 *
 * The protocol is identical to the one on the wire -- epet_link_t does not
 * know or care which pipe it is on. */

/* The radio is OFF until the user asks for it from the UPDATE page: it costs
 * power and exposes a write interface, neither of which a pet needs while it
 * is just being a pet. */
bool ble_link_start(epet_bus_t *bus);
void ble_link_stop(void);
bool ble_link_active(void);      /* radio on (advertising or connected) */
/* True while a central is connected; the app uses this to stay awake. */
bool ble_link_connected(void);
/* The link instance fed by BLE, so the app can check its `changed` flag. */
epet_link_t *ble_link_get(void);

typedef enum {
    BLE_STATE_OFF = 0,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_RECEIVING,
} ble_state_t;

ble_state_t ble_link_state(void);
/* 0..100 while receiving, else 0. */
uint8_t     ble_link_progress(void);
