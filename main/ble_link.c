#include "ble_link.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_EPET_BLE

#include <string.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "epet-ble";

/* Nordic UART Service. RX is written by the central, TX is notified to it. */
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static bool        g_started;
static uint16_t    g_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t    g_tx_handle;
static uint8_t     g_addr_type;
static epet_link_t g_link;
static epet_bus_t *g_bus;

epet_link_t *ble_link_get(void) { return &g_link; }
bool ble_link_connected(void) { return g_conn != BLE_HS_CONN_HANDLE_NONE; }
bool ble_link_active(void) { return g_started; }

ble_state_t ble_link_state(void)
{
    if (!g_started) return BLE_STATE_OFF;
    if (g_link.receiving) return BLE_STATE_RECEIVING;
    if (g_conn != BLE_HS_CONN_HANDLE_NONE) return BLE_STATE_CONNECTED;
    return BLE_STATE_ADVERTISING;
}

uint8_t ble_link_progress(void)
{
    if (!g_link.receiving || g_link.rx_cap == 0) return 0;
    uint32_t pct = (uint32_t)((uint64_t)g_link.rx_len * 100u / g_link.rx_cap);
    return pct > 100 ? 100 : (uint8_t)pct;
}

/* Notifications are capped by the negotiated MTU, so a protocol line may
 * need several. The host reassembles on newlines exactly as over serial. */
static void ble_write(void *ctx, const char *text)
{
    (void)ctx;
    if (g_conn == BLE_HS_CONN_HANDLE_NONE || !g_tx_handle) return;

    uint16_t mtu = ble_att_mtu(g_conn);
    uint16_t max = (mtu > 3) ? (uint16_t)(mtu - 3) : 20;
    size_t len = strlen(text);

    for (size_t off = 0; off < len; ) {
        uint16_t n = (uint16_t)((len - off) < max ? (len - off) : max);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(text + off, n);
        if (!om) return;
        if (ble_gatts_notify_custom(g_conn, g_tx_handle, om) != 0) return;
        off += n;
    }
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                     void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    char buf[512];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    epet_link_feed(&g_link, buf, got);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                     void *arg)
{
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0;   /* notify only */
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &RX_UUID.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &TX_UUID.u,
                .access_cb = tx_access,
                .val_handle = &g_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    /* The service UUID must be advertised or Web Bluetooth cannot filter. */
    fields.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) { ESP_LOGW(TAG, "adv fields: %d", rc); }

    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof adv);
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_addr_type, NULL, BLE_HS_FOREVER, &adv,
                           gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "adv start: %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &g_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising as '%s'", ble_svc_gap_device_name());
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            g_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "central connected");
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected");
        g_conn = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %d", ev->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_link_start(epet_bus_t *bus)
{
    if (g_started) return true;
    g_bus = bus;
    epet_link_init(&g_link, ble_write, NULL, bus);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = NULL;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(SERVICES);
    if (rc) { ESP_LOGE(TAG, "count_cfg: %d", rc); return false; }
    rc = ble_gatts_add_svcs(SERVICES);
    if (rc) { ESP_LOGE(TAG, "add_svcs: %d", rc); return false; }

    ble_svc_gap_device_name_set("epet");
    nimble_port_freertos_init(host_task);
    g_started = true;
    return true;
}

void ble_link_stop(void)
{
    if (!g_started) return;
    /* Never yank the radio out from under a transfer in progress. */
    if (g_link.receiving) return;

    if (g_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(g_conn, BLE_ERR_REM_USER_CONN_TERM);
        g_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
    g_started = false;
    ESP_LOGI(TAG, "radio off");
}

#else  /* CONFIG_EPET_BLE disabled */

static epet_link_t g_link;
bool ble_link_start(epet_bus_t *bus) { (void)bus; return false; }
void ble_link_stop(void) { }
bool ble_link_active(void) { return false; }
bool ble_link_connected(void) { return false; }
epet_link_t *ble_link_get(void) { return &g_link; }
ble_state_t ble_link_state(void) { return BLE_STATE_OFF; }
uint8_t ble_link_progress(void) { return 0; }

#endif
