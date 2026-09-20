#pragma once
/* Send an all-zero WS2812 frame to the onboard LED so it goes dark, then
 * release the pin. Safe to call when no LED is fitted. */
void led_off(void);
