#include "imu.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "epet-imu";

/* Same bus the factory program uses. */
#define IMU_SDA 47
#define IMU_SCL 48
#define IMU_HZ  100000   /* conservative: the part sulks at 400k right after power-up */

/* i2c_master_transmit/receive/transmit_receive take a timeout in MILLISECONDS,
 * not in ticks. Wrapping it in pdMS_TO_TICKS() was what broke every register
 * read: with CONFIG_FREERTOS_HZ=100 pdMS_TO_TICKS(50) is 5, the driver read
 * that as 5 ms, and its own pdMS_TO_TICKS(5) floors to 0 ticks -- so it polled
 * the completion queue once, without blocking, before the ~650 us transaction
 * could finish, and reported ESP_ERR_INVALID_STATE. i2c_master_probe() was
 * always passed raw ms, which is why probing "worked" while reads never did. */
#define IMU_TMO_MS 100

/* The part answers on one of two addresses depending on its SA0 pin. */
#define ADDR_LOW   0x6A
#define ADDR_HIGH  0x6B

#define REG_WHO_AM_I 0x00
#define REG_REVISION 0x01
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03
#define REG_CTRL7    0x08
#define REG_AX_L     0x35

#define WHO_AM_I_QMI8658 0x05

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;

static bool rd(uint8_t reg, uint8_t *buf, size_t n)
{
    if (!s_dev) return false;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n,
                                       IMU_TMO_MS) == ESP_OK;
}

static bool wr(uint8_t reg, uint8_t val)
{
    if (!s_dev) return false;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, IMU_TMO_MS) == ESP_OK;
}

bool imu_present(void) { return s_present; }

static bool open_bus(int sda, int scl)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus) == ESP_OK;
}

static void imu_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(400));        /* let the rails settle */

    if (!open_bus(IMU_SDA, IMU_SCL)) {
        ESP_LOGW(TAG, "i2c bus unavailable; shake-to-wake off");
        vTaskDelete(NULL);
        return;
    }

    const uint8_t addrs[] = { ADDR_HIGH, ADDR_LOW };
    for (int i = 0; i < 2 && !s_present; i++) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = IMU_HZ,
        };
        if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) continue;
        i2c_master_probe(s_bus, addrs[i], 100);

        for (int tries = 0; tries < 20 && !s_present; tries++) {
            uint8_t who = 0;
            if (rd(REG_WHO_AM_I, &who, 1) && who == WHO_AM_I_QMI8658) {
                uint8_t rev = 0;
                rd(REG_REVISION, &rev, 1);
                ESP_LOGI(TAG, "QMI8658 at 0x%02X WHO_AM_I 0x%02X rev 0x%02X "
                              "(try %d)", addrs[i], who, rev, tries + 1);
                /* CTRL1 auto-increments the register pointer so the six
                 * accelerometer bytes come out of one burst read; bit 5 (BE)
                 * stays clear because imu_read_motion() parses little-endian. */
                wr(REG_CTRL1, 0x40);
                wr(REG_CTRL2, 0x24);   /* accel +/-8g, 500 Hz */
                wr(REG_CTRL7, 0x01);   /* accelerometer on */
                s_present = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!s_present) {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }
    }

    if (s_present) {
        /* CTRL7 has just switched the accelerometer on; the first conversion
         * is not ready for an ODR period or two. */
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t raw[6] = {0};
        if (rd(REG_AX_L, raw, sizeof raw)) {
            ESP_LOGD(TAG, "accel ax=%d ay=%d az=%d (raw counts)",
                     (int)(int16_t)(raw[0] | (raw[1] << 8)),
                     (int)(int16_t)(raw[2] | (raw[3] << 8)),
                     (int)(int16_t)(raw[4] | (raw[5] << 8)));
        }
    } else {
        ESP_LOGW(TAG, "IMU did not answer; shake-to-wake off");
    }
    vTaskDelete(NULL);
}

bool imu_read_raw(int *x, int *y, int *z)
{
    uint8_t raw[6];
    if (!s_present || !rd(REG_AX_L, raw, sizeof raw)) return false;
    *x = (int16_t)(raw[0] | (raw[1] << 8));
    *y = (int16_t)(raw[2] | (raw[3] << 8));
    *z = (int16_t)(raw[4] | (raw[5] << 8));
    return true;
}

bool imu_read_motion(int *milli_g)
{
    uint8_t raw[6];
    if (!s_present || !rd(REG_AX_L, raw, sizeof raw)) return false;

    int16_t ax = (int16_t)(raw[0] | (raw[1] << 8));
    int16_t ay = (int16_t)(raw[2] | (raw[3] << 8));
    int16_t az = (int16_t)(raw[4] | (raw[5] << 8));

    /* At +/-8g full scale a count is 8000/32768 mg. Work in mg to stay in
     * integers -- no FPU cost in an interrupt-ish path. */
    int x = (ax * 8000) / 32768;
    int y = (ay * 8000) / 32768;
    int z = (az * 8000) / 32768;

    /* Magnitude via integer sqrt, then subtract 1g so a resting device
     * reads about zero whatever its orientation. */
    uint32_t sq = (uint32_t)(x * x + y * y + z * z);
    uint32_t r = 0, bit = 1u << 30;
    while (bit > sq) bit >>= 2;
    while (bit) {
        if (sq >= r + bit) { sq -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    int mag = (int)r - 1000;
    *milli_g = mag < 0 ? -mag : mag;
    return true;
}

/* Re-apply the sampling configuration.
 *
 * After a light sleep the part comes back with its accelerometer no longer
 * updating: reads still succeed but return the same sample every time, so a
 * shake measures as 0-4 mg of change while the board is being thrown about.
 * Writing CTRL7 again restarts it. Two register writes per wake is nothing
 * next to waking the screen. */
void imu_resume(void)
{
    if (!s_present) return;
    wr(REG_CTRL1, 0x40);      /* auto-increment reads, little endian */
    wr(REG_CTRL2, 0x24);      /* +/-8g, 250 Hz output */
    wr(REG_CTRL7, 0x01);      /* accel on, gyro off */

    /* Writing CTRL2 restarts the sampling pipeline, and reads taken straight
     * afterwards come back near-identical -- a shaken board measured 4-87 mg
     * instead of thousands. Let it produce fresh samples before anyone looks.
     * This was easy to miss: it only bit on the 400 ms idle polls, which are
     * the ones that resume, while the faster confirm polls read fine. */
    esp_rom_delay_us(25000);
}

static int s_last_delta;

int imu_last_delta(void) { return s_last_delta; }

bool imu_shaken(int threshold_mg)
{
    /* Take a short burst of samples, not one.
     *
     * One reading per wake aliases badly: a shake is periodic at a few Hz,
     * and sampling it once every 400 ms keeps catching it at a similar phase,
     * so a board being thrown about measures 0-4 mg of change -- pure sensor
     * noise. Sampling fast for a moment sees the motion whatever its phase.
     *
     * The burst costs ~75 ms of awake time per poll. That is the price of
     * not having the IMU's interrupt line wired to a known pin. */
    enum { BURST = 6, GAP_MS = 15 };

    int px = 0, py = 0, pz = 0;
    int worst = 0;
    bool have_prev = false;

    for (int i = 0; i < BURST; i++) {
        int x, y, z;
        if (!imu_read_raw(&x, &y, &z)) return false;

        if (have_prev) {
            /* counts -> mg at +/-8g full scale */
            int dx = ((x - px) * 8000) / 32768;
            int dy = ((y - py) * 8000) / 32768;
            int dz = ((z - pz) * 8000) / 32768;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dz < 0) dz = -dz;
            int d = dx + dy + dz;
            if (d > worst) worst = d;
        }
        px = x; py = y; pz = z;
        have_prev = true;

        /* Busy-wait, not vTaskDelay: the FreeRTOS tick here is 10 ms, so
         * pdMS_TO_TICKS(15) is 1 tick and yields only to the next tick edge.
         * The whole burst could then land inside a few milliseconds and read
         * six near-identical samples -- which is exactly what made a shaken
         * board measure 5-60 mg instead of thousands. */
        if (i + 1 < BURST) esp_rom_delay_us(GAP_MS * 1000);
    }

    s_last_delta = worst;
    return worst >= threshold_mg;
}

bool imu_init(void)
{
    xTaskCreate(imu_task, "imu", 4096, NULL, 4, NULL);
    return true;
}

void imu_tick(uint32_t dt_ms) { (void)dt_ms; }
