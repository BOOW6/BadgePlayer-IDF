/**
 * @file ble_prov.c
 * @brief BLE 配网通道实现（NimBLE）
 *
 * 帧格式（一条 JSON 拆成 N 帧，逐帧 write / notify）：
 *
 *   byte0  0xAB        魔数，用来快速丢掉垃圾数据
 *   byte1  0x01        协议版本
 *   byte2  seq         帧序号，从 0 开始
 *   byte3  total       总帧数（1..255）
 *   byte4..            UTF-8 载荷
 *
 * 为什么不用长度前缀：BLE 每包都是定长 MTU，长度前缀反而要维护状态机。
 * 序号 + 总数足够重组，丢任何一帧就整条丢弃，让手机重发（幂等命令无副作用）。
 *
 * 为什么不做 CRC：BLE 链路层本身有 24 位 CRC，应用层再叠一层收益很小。
 */
#include "ble_prov.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_prov";

// 0000ab10-0000-1000-8000-00805f9b34fb
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x10, 0xab, 0x00, 0x00);
// 0000ab11-...  手机写入
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x11, 0xab, 0x00, 0x00);
// 0000ab12-...  设备通知
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0xab, 0x00, 0x00);

#define FRAME_MAGIC      0xAB
#define FRAME_VER        0x01
#define FRAME_HDR_LEN    4
#define RX_BUF_SIZE      8192   // 歌单 JSON 上限，8KB 足够 8 个吧唧
#define MAX_MTU          517

static ble_cmd_cb_t s_cb;
static char         s_name[24];
static uint16_t     s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t     s_tx_handle;
static uint8_t      s_own_addr_type;

// 接收重组状态
static uint8_t  s_rx_buf[RX_BUF_SIZE];
static uint16_t s_rx_len;
static uint8_t  s_rx_expect_seq;
static uint8_t  s_rx_total;

static void start_advertising(void);

static void rx_reset(void)
{
    s_rx_len = 0;
    s_rx_expect_seq = 0;
    s_rx_total = 0;
}

/** 处理一帧；凑满整条 JSON 就交给上层 */
static void handle_frame(const uint8_t *data, uint16_t len)
{
    if (len < FRAME_HDR_LEN + 1 || data[0] != FRAME_MAGIC || data[1] != FRAME_VER) {
        ESP_LOGW(TAG, "bad frame header (len=%u)", (unsigned)len);
        return;
    }

    const uint8_t seq   = data[2];
    const uint8_t total = data[3];
    const uint16_t dlen = len - FRAME_HDR_LEN;

    if (total == 0) {
        return;
    }
    if (seq == 0) {
        rx_reset();
        s_rx_total = total;
    }
    if (total != s_rx_total || seq != s_rx_expect_seq) {
        // 中途丢帧或乱序：整条作废，等手机重发
        ESP_LOGW(TAG, "frame out of order (seq=%u expect=%u)", seq, s_rx_expect_seq);
        rx_reset();
        return;
    }
    if ((uint32_t)s_rx_len + dlen > RX_BUF_SIZE - 1) {
        ESP_LOGE(TAG, "json too large, dropped");
        rx_reset();
        return;
    }

    memcpy(s_rx_buf + s_rx_len, data + FRAME_HDR_LEN, dlen);
    s_rx_len += dlen;
    s_rx_expect_seq++;

    if ((uint8_t)(seq + 1) == s_rx_total) {
        s_rx_buf[s_rx_len] = '\0';
        ESP_LOGD(TAG, "rx %u bytes", (unsigned)s_rx_len);
        if (s_cb) {
            s_cb((const char *)s_rx_buf);
        }
        rx_reset();
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t buf[MAX_MTU];
    if (len > sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (os_mbuf_copydata(ctxt->om, 0, len, buf) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    handle_frame(buf, len);
    return 0;
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // 手机写入：配网、同步歌单、学习模式、查询状态
                .uuid = &RX_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // 设备通知：回执、卡片 UID、下载进度
                .uuid = &TX_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            rx_reset();
            ESP_LOGI(TAG, "phone connected, handle=%d", s_conn_handle);
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        rx_reset();
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu updated: %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // 广播包里放 128 位服务 UUID，手机按 UUID 过滤最省事
    fields.uuids128 = &SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed rc=%d", rc);
        return;
    }

    // 名字放 scan response，否则 31 字节装不下
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan rsp failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\"", s_name);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_prov_init(const char *device_name, ble_cmd_cb_t cb)
{
    s_cb = cb;
    strlcpy(s_name, device_name ? device_name : "AIBAJI", sizeof(s_name));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_name);

    int rc = ble_gatts_count_cfg(GATT_SVCS);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt count rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt add rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "ble_prov ready");
    return ESP_OK;
}

bool ble_prov_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_prov_send(const char *json)
{
    if (json == NULL || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }

    const uint16_t len = strlen(json);
    if (len == 0) {
        return false;
    }

    // 按当前协商出来的 MTU 算每帧能塞多少：MTU 含 3 字节 ATT 头，
    // 再扣掉我们自己的 4 字节帧头。默认 MTU 23 时每帧 16 字节。
    uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu < 23) {
        mtu = 23;
    }
    uint16_t chunk = mtu - 3 - FRAME_HDR_LEN;
    if (chunk < 8) {
        chunk = 8;
    }

    uint8_t total = (len + chunk - 1) / chunk;
    if (total == 0 || total > 255) {
        ESP_LOGE(TAG, "json too long (%u bytes)", (unsigned)len);
        return false;
    }

    uint8_t frame[MAX_MTU];
    for (uint8_t seq = 0; seq < total; ++seq) {
        uint16_t offset = (uint16_t)seq * chunk;
        uint16_t n = (offset + chunk <= len) ? chunk : (len - offset);

        frame[0] = FRAME_MAGIC;
        frame[1] = FRAME_VER;
        frame[2] = seq;
        frame[3] = total;
        memcpy(frame + FRAME_HDR_LEN, json + offset, n);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, FRAME_HDR_LEN + n);
        if (om == NULL) {
            ESP_LOGE(TAG, "mbuf alloc failed at seq %u", seq);
            return false;
        }
        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify seq %u rc=%d", seq, rc);
            return false;
        }
        // 给协议栈留出把包推出去的时间，否则连续 notify 会抢不到缓冲。
        // 注意 HZ=100 时 pdMS_TO_TICKS(8) = 0 tick，vTaskDelay(0) 只让出 CPU
        // 不真正等待，等于没有间隔 —— 这里必须自己兜到至少 1 tick。
        {
            TickType_t gap = pdMS_TO_TICKS(8);
            vTaskDelay(gap ? gap : 1);
        }
    }
    return true;
}

void ble_prov_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
}
