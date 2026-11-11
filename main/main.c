/**
 * @file main.c
 * @brief AI 吧唧机主程序
 *
 * 播放链路：读卡 → 云端 device_resolve 解析播放地址 → 直接拉流播放
 *
 * 为什么不在本地存 URL：
 *   换歌不该逼着用户去连一次蓝牙。把「卡 → 歌」的权威留在服务器：
 *   手机在配歌/换歌时签好地址写进库，设备贴卡时按卡号现查现播。
 *   代价是断网不能播、贴卡到出声有 1~3 秒 TLS 握手 + 解析 + 缓冲。
 *   换来的是换歌零蓝牙交互、不占 flash、歌单改完立刻生效。
 *
 * 分工：
 *   - BLE 通道只负责「配网 / 绑定 / 学卡 / 贴卡即发现上报」
 *   - 播放地址由手机签好写服务器，设备贴卡时云端解析，本地不存
 *   - 云端负责「配网激活」和「贴卡解析播放地址」
 *
 * 上电流程：
 *   NVS → 音频 → 电机 → PN532 → BLE 广播
 *     → 已有 WiFi 配置就后台连一次并激活（失败不阻塞）
 *     → 进主循环读卡
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_player.h"
#include "badge_cfg.h"
#include "badge_cloud.h"
#include "ble_prov.h"
#include "voice.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pn532.h"
#include "pn532_driver_spi.h"

// ---------------- 硬件引脚（来自 Netlist_Schematic2_2026-10-28） ----------------
#define MOTOR_PWMA_GPIO     GPIO_NUM_14
#define MOTOR_AIN1_GPIO     GPIO_NUM_12
#define MOTOR_AIN2_GPIO     GPIO_NUM_13

// CN1: SCK, MOSI, MISO, SS, 3V3, GND, RESET, IRQ
#define PN532_SCK_GPIO      GPIO_NUM_15
#define PN532_MISO_GPIO     GPIO_NUM_16
#define PN532_MOSI_GPIO     GPIO_NUM_17
#define PN532_SS_GPIO       GPIO_NUM_18
#define PN532_RESET_GPIO    GPIO_NUM_3
#define PN532_IRQ_GPIO      GPIO_NUM_8
#define PN532_SPI_HOST      SPI2_HOST
#define PN532_SPI_CLOCK_HZ  100000

// 连续读不到卡片的次数阈值，达到后才判定卡片离开。
// 单次读失败不能翻转状态，否则临界距离下会反复 play/stop，I2S 时钟停摆 → 啸叫。
#define CARD_ABSENCE_THRESHOLD  5

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_DISC_BIT       BIT2
#define WIFI_MAX_RETRIES    8

/*
 * 断网升级策略。
 *
 * 背景：吧唧机被拿到一个没见过的 WiFi 环境（换了房子、出差、店里），
 * NVS 里存的还是老 SSID，于是永远连不上。原来的逻辑是「每 10 秒重试一次，
 * 无限循环，一声不吭」——用户看到的就是机器彻底傻了。
 *
 * 现在按失败次数分档：先提醒查路由器，再明确要求重新配网，之后只低频重试。
 * 计数单位是「一轮 wifi_connect 失败」，一轮约 20 秒。
 */
#define WIFI_FAIL_HINT_AT      3    /* 约 1 分钟：先提示检查路由器 */
#define WIFI_FAIL_PROV_AT      6    /* 约 3 分钟：提示重新配网 */
#define WIFI_FAIL_REMIND_EVERY 10   /* 之后每 10 轮（约 10 分钟）再提醒一次 */

#define WIFI_RETRY_FAST_MS     10000
#define WIFI_RETRY_MID_MS      30000
#define WIFI_RETRY_SLOW_MS     60000

#define URL_MAX_LEN 768

static const char *TAG = "badge";

static badge_cfg_t       s_cfg;
static EventGroupHandle_t s_wifi_ev;
static int               s_wifi_retries;
static volatile bool     s_wifi_up;
static char              s_ip[16];

/* 主动重配期间置位：此时 STA_DISCONNECTED 不能自动重连，否则会拿着
 * 旧 ssid 一直自愈，新配置永远排不上号。 */
static volatile bool     s_wifi_suppress_reconnect;

/* 连续联网失败次数；连上就清零。用来决定什么时候开口提示用户。 */
static volatile int      s_wifi_fail_cnt;
/* 已经判定「当前环境连不上，需要重新配网」，通过 BLE 的 stat 告诉 App */
static volatile bool     s_need_prov;

// 学习模式：打开后读到卡只上报 UID，不播放
static volatile bool s_learn_mode;
static char          s_learn_last_uid[32];
static int64_t       s_learn_last_ms;

// 贴卡即发现：非学卡模式下读到未配对的卡，主动上报 `newcard`
static char          s_new_last_uid[32];
static int64_t       s_new_last_ms;

// 命令队列：BLE 回调只负责入队，真正的处理在 worker 里做（可以阻塞）
static QueueHandle_t s_cmd_q;

static bool s_motor_on;

// ================= 电机 =================
static void motor_stop(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    gpio_set_level(MOTOR_AIN1_GPIO, 0);
    gpio_set_level(MOTOR_AIN2_GPIO, 0);
    s_motor_on = false;
}

static void motor_run(void)
{
    if (s_motor_on) {
        return;
    }
    gpio_set_level(MOTOR_AIN1_GPIO, 1);
    gpio_set_level(MOTOR_AIN2_GPIO, 0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 31);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_motor_on = true;
}

static void init_motor(void)
{
    const gpio_config_t dir = {
        .pin_bit_mask = (1ULL << MOTOR_AIN1_GPIO) | (1ULL << MOTOR_AIN2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&dir);

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    const ledc_channel_config_t ch = {
        .gpio_num = MOTOR_PWMA_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
    motor_stop();
}

/**
 * 震动节奏提示。
 *
 * 板上没 LED，喇叭也不是随时都在响，电机是唯一「一定存在、一定可用」的
 * 反馈通道。断网这种「用户看不见、听不到」的故障，靠它至少能表达
 * 「我知道你在，但我出问题了」。
 *
 * 注意这是阻塞调用，只能在非实时路径上（net_task / 贴卡瞬间）用。
 */
static void buzz_pattern(int times)
{
    for (int i = 0; i < times; ++i) {
        motor_run();
        vTaskDelay(pdMS_TO_TICKS(120));
        motor_stop();
        if (i + 1 < times) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ================= WiFi =================
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* 不在这里自动 connect。设备是「配置驱动」的：启动时还没有有效
         * ssid，贸然 connect 会把状态机推进 connecting，导致紧随其后的
         * set_config 报 ESP_ERR_WIFI_STATE。由 wifi_connect_raw() 显式发起。 */
        ESP_LOGD(TAG, "sta start, wait for config");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        xEventGroupSetBits(s_wifi_ev, WIFI_DISC_BIT);
        // 正在换配置：不自动重连，等调用方 set_config 后自己 connect
        if (s_wifi_suppress_reconnect) {
            return;
        }
        if (s_wifi_retries++ < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_ev, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_retries = 0;
        s_wifi_up = true;
        xEventGroupSetBits(s_wifi_ev, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "wifi up, ip=%s", s_ip);
    }
}

static bool s_wifi_inited;
static SemaphoreHandle_t s_wifi_mtx;

static esp_err_t wifi_init_once(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    /* 默认 country 是 world-safe(01)，扫描范围只到 1-11 信道。
     * 有些路由器固定在 12/13 信道（部分老设备、进口路由），设备连扫都扫不到，
     * 表现和「密码错」一模一样。设成 CN 才能覆盖国内全部合规信道。 */
    esp_err_t cerr = esp_wifi_set_country_code("CN", true);
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "set country CN failed: %s", esp_err_to_name(cerr));
    }

    s_wifi_ev = xEventGroupCreate();
    if (s_wifi_ev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_inited = true;
    return ESP_OK;
}

/**
 * 用给定账号连 WiFi，阻塞到成功或超时。
 *
 * 重配时的顺序很关键：先 disconnect 再 set_config。否则老连接会一直用旧
 * 配置重连，新配置要等它彻底断掉才生效，表现为「填了新密码还是连老网络」。
 */
static bool wifi_connect_raw(const char *ssid, const char *pass, int timeout_ms)
{
    if (wifi_init_once() != ESP_OK) {
        return false;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    xEventGroupClearBits(s_wifi_ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISC_BIT);
    s_wifi_retries = 0;

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* esp_wifi_disconnect() 是异步的：返回 ESP_OK 只代表请求已下发，
     * 底层状态机可能还停在 connecting。此时紧接着 set_config 会报
     *   E wifi: sta is connecting, cannot set config
     *   E badge: set_config failed: ESP_ERR_WIFI_STATE
     * 所以必须等 STA_DISCONNECTED 事件落地（或本来就未连接），再写配置。
     * 同时压住事件处理器里的自动重连，避免它拿旧 ssid 反复自愈。 */
    s_wifi_suppress_reconnect = true;
    esp_err_t derr = esp_wifi_disconnect();
    if (derr == ESP_OK) {
        EventBits_t dbits = xEventGroupWaitBits(
            s_wifi_ev, WIFI_DISC_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
        if (!(dbits & WIFI_DISC_BIT)) {
            ESP_LOGW(TAG, "disconnect event timeout, force continue");
        }
    } else if (derr != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(derr));
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        // 兜底：极少数情况下状态机切换更慢，退一步再试一次
        ESP_LOGW(TAG, "set_config retry, first err=%s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(400));
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        s_wifi_suppress_reconnect = false;
        return false;
    }
    s_wifi_suppress_reconnect = false;

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect call failed: %s", esp_err_to_name(err));
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }
    ESP_LOGW(TAG, "wifi connect timeout/fail (ssid=%s)", ssid);
    return false;
}

/**
 * 串行化 WiFi 连接：配网（worker 任务）和后台重连（net 任务）会同时想连，
 * esp_wifi_set_config 不是并发安全的，加把锁让它们排队。
 */
static bool wifi_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (s_wifi_mtx == NULL) {
        return wifi_connect_raw(ssid, pass, timeout_ms);
    }
    if (xSemaphoreTake(s_wifi_mtx, pdMS_TO_TICKS(45000)) != pdTRUE) {
        ESP_LOGW(TAG, "wifi busy, skip this round");
        return false;
    }
    bool ok = wifi_connect_raw(ssid, pass, timeout_ms);
    xSemaphoreGive(s_wifi_mtx);
    return ok;
}

// ================= BLE 命令 =================
static void send_json(const char *json)
{
    if (!ble_prov_send(json)) {
        ESP_LOGW(TAG, "send failed (not connected?)");
    }
}

static void reply_cfg(bool ok, const char *err)
{
    char buf[192];
    if (ok) {
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"cfg\",\"ok\":true,\"ip\":\"%s\",\"sn\":\"%s\"}",
                 s_ip, s_cfg.sn);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"cfg\",\"ok\":false,\"err\":\"%s\"}", err ? err : "unknown");
    }
    send_json(buf);
}

static void do_status(void)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"stat\",\"sn\":\"%s\",\"wifi\":%d,\"ip\":\"%s\","
             "\"activated\":%d,\"paired\":%d,"
             "\"learn\":%d,\"playing\":%d,\"need_prov\":%d,\"fw\":\"%s\"}",
             s_cfg.sn,
             s_wifi_up ? 1 : 0,
             s_ip,
             (s_cfg.token[0] ? 1 : 0),
             badge_map_count(),
             s_learn_mode ? 1 : 0,
             audio_player_is_playing() ? 1 : 0,
             s_need_prov ? 1 : 0,
             BADGE_FW_VERSION);
    send_json(buf);
}

/** 配网：存 NVS → 连 WiFi → 激活。整个过程十几秒，只能在 worker 里跑。 */
static void do_cfg(cJSON *root)
{
    const char *sn     = cJSON_GetStringValue(cJSON_GetObjectItem(root, "sn"));
    const char *ssid   = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *pass   = cJSON_GetStringValue(cJSON_GetObjectItem(root, "pass"));
    const char *secret = cJSON_GetStringValue(cJSON_GetObjectItem(root, "secret"));

    if (ssid == NULL || ssid[0] == '\0') {
        reply_cfg(false, "missing_ssid");
        return;
    }

    if (sn && sn[0]) {
        strlcpy(s_cfg.sn, sn, sizeof(s_cfg.sn));
        badge_cfg_save_sn(s_cfg.sn);
    }
    strlcpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid));
    strlcpy(s_cfg.pass, pass ? pass : "", sizeof(s_cfg.pass));
    if (secret && secret[0]) {
        strlcpy(s_cfg.secret, secret, sizeof(s_cfg.secret));
        // 换了密钥，旧 token 一定无效了
        s_cfg.token[0] = '\0';
    }
    badge_cfg_save_wifi(s_cfg.ssid, s_cfg.pass, s_cfg.secret);

    // 先告诉手机「参数收到了」，避免它以为断线。
    // 注意类型必须是 prog 而不是 cfg：手机在等的是 t=='cfg' 的最终结果，
    // 这里如果也发 cfg+ok:true，App 会当成「配网成功」立刻去调绑定，
    // 而设备其实还在连 WiFi —— 结果是绑定失败在 005 的「必须已激活」上。
    send_json("{\"t\":\"prog\",\"step\":\"connecting\"}");

    if (!wifi_connect(s_cfg.ssid, s_cfg.pass, 20000)) {
        reply_cfg(false, "wifi_timeout");
        return;
    }

    // 连上了才谈激活；激活失败也不影响后续播放（播放只要 WiFi + 有效 URL）
    if (s_cfg.secret[0]) {
        char hw[16] = {0};
        badge_cloud_hw_uid(hw, sizeof(hw));
        char token[80] = {0};
        esp_err_t err = badge_cloud_activate(s_cfg.sn, s_cfg.secret, hw,
                                            token, sizeof(token));
        if (err == ESP_OK) {
            strlcpy(s_cfg.token, token, sizeof(s_cfg.token));
            badge_cfg_save_token(s_cfg.token, 0);
            ESP_LOGI(TAG, "device activated");
        } else if (err == ESP_ERR_INVALID_STATE) {
            // 云端说这台机早就激活过 —— 重配网场景，不算错
            ESP_LOGW(TAG, "already activated on cloud");
        } else {
            ESP_LOGW(TAG, "activate failed: %s", esp_err_to_name(err));
        }
    }

    s_cfg.configured = true;
    // 配网成功要复位失败计数，否则 net_task 里那套「连不上」的提示逻辑
    // 会带着旧账继续算，下一次刚失败一次就直接进到「请重新配网」。
    s_wifi_fail_cnt = 0;
    s_need_prov = false;
    voice_play(VOICE_NET_OK);
    reply_cfg(true, NULL);
}

/**
 * 同步歌单：手机把每张卡对应的音频地址推下来，这里只做「存 URL」。
 *
 * 全量覆盖语义：先清空再写，所以手机上删掉的吧唧在设备上也会消失。
 * 地址是手机现签的（30 天有效），过期后重新连一次 App 就会刷新。
 */
static void do_sync(cJSON *root)
{
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        send_json("{\"t\":\"sync\",\"ok\":false,\"err\":\"bad_items\"}");
        return;
    }

    badge_map_clear();

    int paired = 0, no_audio = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        const char *rfid = cJSON_GetStringValue(cJSON_GetObjectItem(it, "rfid"));
        const char *url  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "url"));
        if (!rfid || !rfid[0]) {
            continue;
        }
        if (!url || !url[0]) {
            // 歌还没上传音源，只记个数，不留死映射
            no_audio++;
            continue;
        }
        if (badge_map_set(rfid, url) == ESP_OK) {
            paired++;
        }
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"sync\",\"ok\":true,\"paired\":%d,\"no_audio\":%d}",
             paired, no_audio);
    send_json(buf);
}

static void do_delete(cJSON *root)
{
    const char *rfid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rfid"));
    if (rfid && rfid[0]) {
        badge_map_remove(rfid);
    }
    send_json("{\"t\":\"del\",\"ok\":true}");
}

static void do_reset(void)
{
    send_json("{\"t\":\"reset\",\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));   // 给 notify 一点时间推出去
    badge_cfg_erase();
    esp_restart();
}

static void worker_task(void *arg)
{
    (void)arg;
    char *json = NULL;
    while (true) {
        if (xQueueReceive(s_cmd_q, &json, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        cJSON *root = cJSON_Parse(json);
        free(json);
        if (root == NULL) {
            ESP_LOGW(TAG, "bad json from phone");
            continue;
        }

        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(root, "t"));
        if (t == NULL) {
            cJSON_Delete(root);
            continue;
        }
        // 命令到达日志：排查「手机点了没反应」时，先看这条有没有出现。
        // 没有 → 蓝牙写入没通；有 → 命令通了，问题在别处。
        ESP_LOGI(TAG, "cmd: %s", t);

        if (strcmp(t, "cfg") == 0) {
            do_cfg(root);
        } else if (strcmp(t, "stat") == 0) {
            do_status();
        } else if (strcmp(t, "sync") == 0) {
            do_sync(root);
        } else if (strcmp(t, "del") == 0) {
            do_delete(root);
        } else if (strcmp(t, "reset") == 0) {
            do_reset();
        } else if (strcmp(t, "learn") == 0) {
            cJSON *on = cJSON_GetObjectItem(root, "on");
            s_learn_mode = cJSON_IsTrue(on);
            s_learn_last_uid[0] = '\0';
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"t\":\"learn\",\"on\":%d}",
                     s_learn_mode ? 1 : 0);
            send_json(buf);
            ESP_LOGI(TAG, "learn mode %s", s_learn_mode ? "on" : "off");
        } else if (strcmp(t, "play") == 0) {
            // 调试用：按 rfid 直接播
            const char *rfid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rfid"));
            char url[URL_MAX_LEN];
            if (rfid && badge_map_get(rfid, url, sizeof(url))) {
                if (audio_player_play(url) == ESP_OK) {
                    motor_run();
                }
            }
        } else if (strcmp(t, "voice") == 0) {
            // 调试用：手机上试听内置提示音，确认喇叭和素材都正常。
            // {"t":"voice","id":2}
            cJSON *id = cJSON_GetObjectItem(root, "id");
            if (cJSON_IsNumber(id)) {
                voice_play((voice_id_t)id->valueint);
            }
        } else if (strcmp(t, "stop") == 0) {
            audio_player_stop();
            motor_stop();
        } else {
            ESP_LOGW(TAG, "unknown cmd: %s", t);
        }
        cJSON_Delete(root);
    }
}

/** BLE 回调：只做入队，绝不阻塞 */
static void on_ble_cmd(const char *json)
{
    char *copy = strdup(json);
    if (copy == NULL) {
        return;
    }
    if (xQueueSend(s_cmd_q, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, dropped");
        free(copy);
    }
}

// ================= PN532 =================
static void uid_to_string(const uint8_t *uid, uint8_t len, char *buf, size_t size)
{
    size_t off = 0;
    for (uint8_t i = 0; i < len && off + 3 < size; ++i) {
        off += snprintf(buf + off, size - off, i == 0 ? "%02X" : ":%02X", uid[i]);
    }
}

static esp_err_t init_pn532(pn532_io_t *io)
{
    ESP_LOGI(TAG, "initializing PN532 over SPI");
    ESP_RETURN_ON_ERROR(
        pn532_new_driver_spi(PN532_MISO_GPIO, PN532_MOSI_GPIO, PN532_SCK_GPIO,
                             PN532_SS_GPIO, PN532_RESET_GPIO, PN532_IRQ_GPIO,
                             PN532_SPI_HOST, PN532_SPI_CLOCK_HZ, io),
        TAG, "pn532 driver");
    ESP_RETURN_ON_ERROR(pn532_init(io), TAG, "pn532 init");

    uint32_t version = 0;
    ESP_RETURN_ON_ERROR(pn532_get_firmware_version(io, &version), TAG, "pn532 fw");
    ESP_LOGI(TAG, "PN532 fw %u.%u", (unsigned)((version >> 16) & 0xFF),
             (unsigned)((version >> 8) & 0xFF));
    return ESP_OK;
}

/**
 * 播放某张卡对应的音频：贴卡时经云端 device_resolve 实时要地址，不在本地存 URL。
 *
 * @return ESP_OK 已起播；ESP_ERR_NOT_FOUND 这张卡没配对（可当作新卡）；
 *         ESP_ERR_INVALID_STATE 没 WiFi / 没 token / 地址过期等；
 *         其他为网络或播放错误
 */
static esp_err_t play_card(const char *rfid)
{
    if (!s_wifi_up) {
        ESP_LOGW(TAG, "no wifi, cannot stream");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_cfg.token[0]) {
        ESP_LOGW(TAG, "no token, cannot resolve");
        return ESP_ERR_INVALID_STATE;
    }

    char url[URL_MAX_LEN];
    esp_err_t err = badge_cloud_resolve(s_cfg.token, rfid, url, sizeof(url));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "resolve %s: %s", rfid, esp_err_to_name(err));
        return err; // NOT_FOUND 交给上层判为新卡
    }

    if (audio_player_play(url) != ESP_OK) {
        ESP_LOGE(TAG, "play failed for card %s", rfid);
        return ESP_FAIL;
    }
    motor_run();
    return ESP_OK;
}

static void report_card(const char *uid)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"t\":\"card\",\"uid\":\"%s\"}", uid);
    ESP_LOGI(TAG, "card learned: %s", uid);
    send_json(buf);
}

/** 贴到一张没配过的卡就短震一下，给用户「机器看见你了」的物理反馈。 */
static void buzz_new_card(void)
{
    motor_run();
    vTaskDelay(pdMS_TO_TICKS(150));
    motor_stop();
}

/**
 * 贴卡即发现：把「场里有张没配过的卡」告诉手机。
 *
 * 注意**只能在上报成功之后**才把 uid 记进 s_new_last_uid。
 * 早期版本不管发没发出去都记账，结果设备先读到卡、手机后连上的场景里，
 * 消息丢了却以为报过了，之后再也不补 —— 用户看到的就是「贴了没反应」。
 *
 * @return true 表示已经交给蓝牙发出去（不代表手机一定收到了）
 */
static bool try_report_new_card(const char *uid)
{
    if (!ble_prov_connected()) return false;
    if (strcmp(uid, s_new_last_uid) == 0) return false;

    char buf[96];
    snprintf(buf, sizeof(buf), "{\"t\":\"newcard\",\"uid\":\"%s\"}", uid);
    send_json(buf);
    strlcpy(s_new_last_uid, uid, sizeof(s_new_last_uid));
    s_new_last_ms = esp_log_timestamp();
    ESP_LOGI(TAG, "new card reported: %s", uid);
    return true;
}

// ================= 后台联网（不阻塞主流程） =================
/**
 * 常驻联网任务。
 *
 * 早期版本连不上就自杀，结果路由器重启一次、或者用户第一次配网之后，
 * 机器就一直离线，只能靠重新配网救回来。现在改成常驻守护：
 *   - 没配网 → 每 5 秒看一次有没有被 App 配上
 *   - 配了但离线 → 自己重连，不等用户操作
 *   - 连上了但没 token（重刷固件会这样）→ 补激活，瞬时失败按退避重试
 *
 * 「换个 WiFi 环境」是最高频的现场故障：机器被拿到公司、店里、酒店，
 * NVS 里还是家里那个 SSID。这时候固件必须做两件事 —— 别把重试频率顶那么高
 * （费电、刷日志、可能连上别人的同名热点），并且**开口告诉用户怎么办**。
 * 光靠 App 提示没用：用户这时候根本没打开 App。
 *
 * 激活不能"只试一次"：最常见的一次失败根本不是密钥问题，而是刚拿到 IP
 * 那一刻 DNS 还没就绪（getaddrinfo 返回 202），属于瞬时错误，必须重试。
 * 只有云端明确拒绝（HTTP 非 200 / 参数错）才停止，那才是密钥作废。
 */
static void net_task(void *arg)
{
    (void)arg;
    int act_fail = 0;      // 连续瞬时失败次数
    bool act_given_up = false;

    while (!s_cfg.configured) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "net guardian started (ssid=%s)", s_cfg.ssid);

    while (true) {
        if (!s_wifi_up) {
            if (wifi_connect(s_cfg.ssid, s_cfg.pass, 20000)) {
                bool was_down_long = (s_wifi_fail_cnt >= WIFI_FAIL_HINT_AT);
                s_wifi_fail_cnt = 0;
                if (s_need_prov) {
                    s_need_prov = false;
                }
                ESP_LOGI(TAG, "net up: %s", s_ip);
                if (was_down_long) {
                    // 之前已经明确提示过「连不上」，现在连上了得给个收尾,
                    // 否则用户不知道要不要再操作一次
                    voice_play(VOICE_NET_OK);
                }
                // 刚拿到 IP 时 lwIP 的 DNS 服务器还没从 DHCP 里就绪，
                // 立刻发 HTTP 会 getaddrinfo 202。给它一点时间。
                vTaskDelay(pdMS_TO_TICKS(1500));
            } else {
                if (s_wifi_fail_cnt < 1000) {
                    s_wifi_fail_cnt++;
                }
                int n = s_wifi_fail_cnt;
                ESP_LOGW(TAG, "wifi fail #%d (ssid=%s)", n, s_cfg.ssid);

                /* 分档提示。只在跨过阈值那一次开口，之后低频重复，
                 * 避免每 30 秒念一遍把用户逼疯。 */
                if (n == WIFI_FAIL_HINT_AT) {
                    voice_play(VOICE_WIFI_FAIL);
                    buzz_pattern(2);
                } else if (n == WIFI_FAIL_PROV_AT) {
                    s_need_prov = true;
                    voice_play(VOICE_NEED_PROV);
                    buzz_pattern(3);
                } else if (n > WIFI_FAIL_PROV_AT &&
                           (n % WIFI_FAIL_REMIND_EVERY) == 0) {
                    voice_play(VOICE_NEED_PROV);
                }

                int delay_ms = (n < WIFI_FAIL_HINT_AT) ? WIFI_RETRY_FAST_MS
                             : (n < WIFI_FAIL_PROV_AT) ? WIFI_RETRY_MID_MS
                                                       : WIFI_RETRY_SLOW_MS;
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                continue;
            }
        }

        if (!act_given_up && s_cfg.secret[0] && !s_cfg.token[0]) {
            char hw[16] = {0};
            badge_cloud_hw_uid(hw, sizeof(hw));
            char token[80] = {0};
            esp_err_t err = badge_cloud_activate(s_cfg.sn, s_cfg.secret, hw,
                                                token, sizeof(token));
            if (err == ESP_OK) {
                strlcpy(s_cfg.token, token, sizeof(s_cfg.token));
                badge_cfg_save_token(s_cfg.token, 0);
                ESP_LOGI(TAG, "device activated");
                act_fail = 0;
            } else if (err == ESP_ERR_INVALID_RESPONSE ||
                       err == ESP_ERR_INVALID_ARG) {
                // 云端已明确拒绝（密钥作废等），重试打不开
                ESP_LOGW(TAG, "activate rejected, stop retrying");
                act_given_up = true;
            } else {
                act_fail++;
                int backoff = act_fail * 5;
                if (backoff > 60) {
                    backoff = 60;
                }
                ESP_LOGW(TAG, "activate failed (%d): %s, retry in %ds",
                         act_fail, esp_err_to_name(err), backoff);
                vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    badge_cfg_load(&s_cfg);

    ESP_ERROR_CHECK(audio_player_init());
    init_motor();

    // 语音提示通道复用同一条 I2S，必须排在音频初始化之后
    if (voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "voice init failed, prompts disabled");
    } else if (!s_cfg.configured) {
        // 出厂 / 恢复出厂后的第一次上电：直接说下一步该干什么。
        // 不配网的话 net_task 会一直静默等待，用户对着黑盒无从下手。
        voice_play(VOICE_NO_NET);
    }

    pn532_io_t pn532_io = {0};
    while (init_pn532(&pn532_io) != ESP_OK) {
        motor_stop();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    s_cmd_q = xQueueCreate(4, sizeof(char *));
    if (s_cmd_q == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    s_wifi_mtx = xSemaphoreCreateMutex();
    if (s_wifi_mtx == NULL) {
        ESP_LOGE(TAG, "wifi mutex create failed");
        return;
    }
    xTaskCreate(worker_task, "cmd_worker", 8192, NULL, 6, NULL);

    // 广播名优先用 SN 后四位，没有 SN 就退回 MAC 后四位
    char ble_name[24];
    if (s_cfg.sn[0] != '\0' && strlen(s_cfg.sn) >= 4) {
        snprintf(ble_name, sizeof(ble_name), "AIBAJI-%s", s_cfg.sn + strlen(s_cfg.sn) - 4);
    } else {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(ble_name, sizeof(ble_name), "AIBAJI-%02X%02X", mac[4], mac[5]);
    }
    ESP_ERROR_CHECK(ble_prov_init(ble_name, on_ble_cmd));

    xTaskCreate(net_task, "net", 8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready: sn=%s paired=%d", s_cfg.sn[0] ? s_cfg.sn : "(unset)",
             badge_map_count());

    bool card_present = false;
    bool playing = false;
    bool card_is_new = false; // 当前场里这张卡是不是「没配过」
    int absence = 0;
    char card_uid[32] = {0};

    // 蓝牙连接的上一拍状态，用来抓「刚连上」这个边沿。
    // 见下面 ble up 处：每连上一次就要允许把场里的卡重报一遍。
    bool prev_ble = false;

    // 读卡心跳：没卡时每 5 秒打一行，用来区分「天线没场」「有场但没卡」
    // 和「读到了但逻辑没走」。排完故障可以删掉。
    uint32_t poll_tries = 0;
    int64_t poll_report_ms = esp_log_timestamp();

    while (true) {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;
        esp_err_t err = pn532_read_passive_target_id(
            &pn532_io, PN532_BRTY_ISO14443A_106KBPS, uid, &uid_len, 100);
        bool detected = (err == ESP_OK);
        ++poll_tries;

        int64_t now_ms = esp_log_timestamp();
        bool cur_ble = ble_prov_connected();
        if (now_ms - poll_report_ms >= 5000) {
            ESP_LOGI(TAG, "poll: %u, last=%s, learn=%d, card=%s, new=%d, ble=%d",
                     (unsigned)poll_tries, esp_err_to_name(err),
                     s_learn_mode ? 1 : 0,
                     card_present ? card_uid : "-",
                     card_is_new ? 1 : 0,
                     cur_ble ? 1 : 0);
            poll_tries = 0;
            poll_report_ms = now_ms;
        }

        // 蓝牙从「没连」变成「连上」的这一拍，清掉上报去抖。
        //
        // 原来的去抖只按 uid 判重：报过一次就再也不报。可 App 那边的监听挂在
        // 设备详情页上 —— 用户离开那个页面，或者 App 重启，消息就收不到了，
        // 而固件还以为「报过了」。回来再贴同一张卡，永远没反应。
        // 现在每建立一次新连接都允许重报一次，配合 App 进页面自动连接，
        // 就成了「进设备页 → 贴卡 → 弹窗」。
        if (cur_ble && !prev_ble) {
            s_new_last_uid[0] = '\0';
            ESP_LOGI(TAG, "ble up: 允许重新上报场里的卡");
        }
        prev_ble = cur_ble;

        if (detected) {
            absence = 0;
            if (!card_present) {
                card_present = true;
                uid_to_string(uid, uid_len, card_uid, sizeof(card_uid));
                ESP_LOGI(TAG, "card in: %s", card_uid);

                if (s_learn_mode) {
                    // 同一张卡放着不动，3 秒内不重复上报
                    int64_t now = esp_log_timestamp();
                    if (strcmp(card_uid, s_learn_last_uid) != 0 ||
                        now - s_learn_last_ms > 3000) {
                        strlcpy(s_learn_last_uid, card_uid, sizeof(s_learn_last_uid));
                        s_learn_last_ms = now;
                        report_card(card_uid);
                    }
                } else {
                    esp_err_t r = play_card(card_uid);
                    if (r == ESP_ERR_NOT_FOUND) {
                        // 云端说这张卡没配对 —— 贴卡即发现。先震一下给物理反馈，
                        // 上报交给下面的补报逻辑（蓝牙没连时它下一轮会重试）。
                        card_is_new = true;
                        buzz_new_card();
                    } else if (r == ESP_ERR_INVALID_STATE && !s_wifi_up) {
                        /* 断网时贴卡，以前是彻底静默的：play_card 直接返回，
                         * 主循环 playing=false，用户贴了十次也不知道哪里不对。
                         * 这里必须出声 —— 这是「机器没坏，只是没网」的唯一提示。 */
                        voice_play(VOICE_NO_NET);
                        buzz_pattern(2);
                    }
                    playing = (r == ESP_OK);
                }
            }
        } else if (card_present) {
            if (++absence >= CARD_ABSENCE_THRESHOLD) {
                card_present = false;
                card_is_new = false;
                // 拿开再贴应该重新弹窗（用户可能在 App 里点了取消）。
                // 去抖交给 CARD_ABSENCE_THRESHOLD：连续 5 次读不到才算离开，
                // 临界距离抖动不会误判成「离开」。
                s_new_last_uid[0] = '\0';
                absence = 0;
                if (playing) {
                    playing = false;
                    audio_player_stop();
                    motor_stop();
                }
                ESP_LOGI(TAG, "card out");
            }
        }

        // 补报：卡先放着、App 后打开是很常见的顺序。
        // 主循环只在「卡刚进场」那一刻上报一次，那时候蓝牙多半还没连，
        // 消息就丢了，用户会以为「贴了没反应」。所以蓝牙一连上就补一次。
        // try_report_new_card 内部按 uid 去抖，同一张卡只会弹一次窗。
        if (card_is_new) {
            try_report_new_card(card_uid);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
