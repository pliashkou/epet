#pragma once
#include <stdbool.h>
#include <stdint.h>

/* QMI8658 six-axis IMU, used for shake-to-wake.
 *
 * The board's interrupt line is not identified in any documentation I could
 * verify -- the schematic names IMU_INT1/IMU_INT2 but not which GPIO they
 * reach, and the vendor's header claims SPI for a part that is wired to I2C
 * here. So the default is polling, and CONFIG_EPET_IMU_INT_GPIO switches to
 * a real interrupt once the pin is confirmed. */

bool imu_init(void);
/* The IMU is not readable until about 1.5 s after power-up, so detection is
 * deferred: call this from the main loop until imu_present() is true. */
void imu_tick(uint32_t dt_ms);
/* Restart sampling after a light sleep; see the note in imu.c. */
void imu_resume(void);
bool imu_present(void);
/* Magnitude of acceleration minus gravity, in milli-g. Roughly 0 at rest. */
bool imu_read_motion(int *milli_g);
bool imu_read_raw(int *x, int *y, int *z);
/* True if the board moved appreciably since the last call. Measures the
 * change between samples, not deviation from gravity -- see the note in
 * imu.c for why the obvious version does not work. */
bool imu_shaken(int threshold_mg);
/* The delta computed by the most recent imu_shaken() call, in mg. */
int  imu_last_delta(void);
