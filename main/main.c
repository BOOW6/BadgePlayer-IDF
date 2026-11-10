#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "audio_player.h"
#include "pn532.h"
#include "pn532_driver_spi.h"

// Pin assignments from Netlist_Schematic2_2026-10-28.enet.json.
#define MOTOR_PWMA_GPIO     GPIO_NUM_14
#define MOTOR_AIN1_GPIO     GPIO_NUM_12
#define MOTOR_AIN2_GPIO     GPIO_NUM_13

// CN1: SCK, MOSI, MISO, SS, 3V3, GND, RESET, IRQ.
#define PN532_SCK_GPIO      GPIO_NUM_15
#define PN532_MISO_GPIO     GPIO_NUM_16
#define PN532_MOSI_GPIO     GPIO_NUM_17
#define PN532_SS_GPIO       GPIO_NUM_18
#define PN532_RESET_GPIO    GPIO_NUM_3
#define PN532_IRQ_GPIO      GPIO_NUM_8
#define PN532_SPI_HOST      SPI2_HOST
#define PN532_SPI_CLOCK_HZ  100000
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_MAX_RETRIES    10

// 连续读不到卡片的次数阈值，达到后才判定卡片离开。
// 每次失败要等满 100ms 超时，再加 20ms 循环延时，5 次约 600ms，
// 与 Arduino 版 10 x 50ms 的去抖时间相当。
#define CARD_ABSENCE_THRESHOLD  5

#ifndef CONFIG_BADGE_WIFI_SSID
#define CONFIG_BADGE_WIFI_SSID ""
#endif
#ifndef CONFIG_BADGE_WIFI_PASSWORD
#define CONFIG_BADGE_WIFI_PASSWORD ""
#endif

static const char *TAG = "badge_player";
static EventGroupHandle_t s_wifi_events;
static int s_wifi_retries;

typedef struct {
    const char *uid;
    const char *url;
    const char *name;
} song_entry_t;

static const song_entry_t SONGS[] = {
    {"53:25:4E:F7:32:00:01",
     "http://music.163.com/song/media/outer/url?id=1330348068.mp3", "起风了"},
    {"53:03:5D:10:76:00:01",
     "http://music.163.com/song/media/outer/url?id=1962556941.mp3", "无羁"},
    {"53:09:8B:F7:32:00:01",
     "http://music.163.com/song/media/outer/url?id=569214252.mp3",
     "如果有一天我变得很有钱"},
};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // 回调运行在 wifi 任务上下文里，这里用 ESP_ERROR_CHECK 一旦失败
        // 会直接 abort 整机，所以只记日志
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries++ < WIFI_MAX_RETRIES) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "retry connect failed: %s", esp_err_to_name(err));
            }
        } else {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            ESP_LOGE(TAG, "Wi-Fi connection failed after %d retries",
                     WIFI_MAX_RETRIES);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, got IP");
    }
}

static esp_err_t init_wifi(void)
{
    if (CONFIG_BADGE_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi credentials are not configured");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "network init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG,
                       "event loop creation failed");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), TAG, "Wi-Fi init failed");
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler, NULL),
                       TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &wifi_event_handler, NULL),
                       TAG, "IP event registration failed");

    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)sta_config.sta.ssid, CONFIG_BADGE_WIFI_SSID,
            sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, CONFIG_BADGE_WIFI_PASSWORD,
            sizeof(sta_config.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                       "Wi-Fi mode setup failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG,
                       "Wi-Fi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    return (bits & WIFI_CONNECTED_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void uid_to_string(const uint8_t *uid, uint8_t uid_length,
                          char *buffer, size_t buffer_size)
{
    size_t offset = 0;
    for (uint8_t i = 0; i < uid_length && offset + 3 < buffer_size; ++i) {
        offset += snprintf(buffer + offset, buffer_size - offset,
                           i == 0 ? "%02X" : ":%02X", uid[i]);
    }
}

static const song_entry_t *find_song(const char *uid)
{
    for (size_t i = 0; i < sizeof(SONGS) / sizeof(SONGS[0]); ++i) {
        if (strcmp(uid, SONGS[i].uid) == 0) {
            return &SONGS[i];
        }
    }
    return NULL;
}

static void motor_stop(void)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    ESP_ERROR_CHECK(gpio_set_level(MOTOR_AIN1_GPIO, 0));
    ESP_ERROR_CHECK(gpio_set_level(MOTOR_AIN2_GPIO, 0));
}

static void motor_clockwise(void)
{
    ESP_ERROR_CHECK(gpio_set_level(MOTOR_AIN1_GPIO, 1));
    ESP_ERROR_CHECK(gpio_set_level(MOTOR_AIN2_GPIO, 0));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 31));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static void init_motor(void)
{
    const gpio_config_t direction = {
        .pin_bit_mask = (1ULL << MOTOR_AIN1_GPIO) | (1ULL << MOTOR_AIN2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&direction));

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t channel = {
        .gpio_num = MOTOR_PWMA_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    motor_stop();
}

static esp_err_t init_pn532(pn532_io_t *pn532_io)
{
    ESP_LOGI(TAG, "initializing PN532 SPI driver");
    ESP_RETURN_ON_ERROR(pn532_new_driver_spi(
                            PN532_MISO_GPIO,
                            PN532_MOSI_GPIO,
                            PN532_SCK_GPIO,
                            PN532_SS_GPIO,
                            PN532_RESET_GPIO,
                            PN532_IRQ_GPIO,
                            PN532_SPI_HOST,
                            PN532_SPI_CLOCK_HZ,
                            pn532_io),
                       TAG, "PN532 SPI driver creation failed");

    esp_err_t err = pn532_init(pn532_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PN532 initialization failed: %s", esp_err_to_name(err));
        pn532_delete_driver(pn532_io);
        return err;
    }

    uint32_t version = 0;
    err = pn532_get_firmware_version(pn532_io, &version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PN532 firmware query failed: %s", esp_err_to_name(err));
        pn532_delete_driver(pn532_io);
        return err;
    }

    ESP_LOGI(TAG, "PN532 chip PN5%02X firmware %u.%u",
             (unsigned)((version >> 24) & 0xFF),
             (unsigned)((version >> 16) & 0xFF),
             (unsigned)((version >> 8) & 0xFF));
    return ESP_OK;
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

    // WiFi 只是在线播放的前提，连不上不能让整机 abort。
    // 原代码是 ESP_ERROR_CHECK(init_wifi())：一旦路由器不可达，
    // init_wifi 等满 15s 返回 ESP_ERR_TIMEOUT，ESP_ERROR_CHECK 直接
    // abort() 重启，形成无限重启循环，NFC 永远没机会初始化。
    esp_err_t wifi_err = init_wifi();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi unavailable (%s); NFC/audio will still start",
                 esp_err_to_name(wifi_err));
    }

    ESP_ERROR_CHECK(audio_player_init());
    init_motor();

    pn532_io_t pn532_io = {0};
    while (init_pn532(&pn532_io) != ESP_OK) {
        motor_stop();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "NFC and audio ready; present a card to play a song");
    bool card_present = false;
    int absence_count = 0;
    char card_uid[3 * 10] = {0};

    while (true) {
        uint8_t uid[10] = {0};
        uint8_t uid_length = 0;
        esp_err_t err = pn532_read_passive_target_id(
            &pn532_io,
            PN532_BRTY_ISO14443A_106KBPS,
            uid,
            &uid_length,
            100);
        bool detected = (err == ESP_OK);

        if (detected) {
            absence_count = 0;
            if (!card_present) {
                card_present = true;
                ESP_LOGI(TAG, "NFC card detected, UID length: %u",
                         (unsigned)uid_length);
                ESP_LOG_BUFFER_HEX(TAG, uid, uid_length);
                uid_to_string(uid, uid_length, card_uid, sizeof(card_uid));
                const song_entry_t *song = find_song(card_uid);
                if (song == NULL) {
                    ESP_LOGW(TAG, "no song mapped for UID %s", card_uid);
                } else if (audio_player_play(song->url) == ESP_OK) {
                    ESP_LOGI(TAG, "playing %s", song->name);
                    motor_clockwise();
                } else {
                    ESP_LOGE(TAG, "failed to start %s", song->name);
                    motor_stop();
                }
            }
        } else if (card_present) {
            // 单次读失败不能判定卡片离开。卡片在临界距离或轻微晃动时，
            // INLISTPASSIVETARGET 会时成功时失败，若直接翻转状态就会
            // 反复 play/stop，把 I2S 时钟不停停摆又恢复 → 啸叫。
            // 这里要求连续 CARD_ABSENCE_THRESHOLD 次读不到才算离开。
            if (++absence_count >= CARD_ABSENCE_THRESHOLD) {
                card_present = false;
                absence_count = 0;
                ESP_LOGI(TAG, "NFC card removed");
                esp_err_t stop_err = audio_player_stop();
                if (stop_err != ESP_OK) {
                    // 这里不能用 ESP_ERROR_CHECK：stop 失败会直接 abort，
                    // 而它只是个收尾动作，不值得把整机重启
                    ESP_LOGW(TAG, "audio stop returned %s",
                             esp_err_to_name(stop_err));
                }
                motor_stop();
            }
        }

        // 读卡成功时 pn532_read_passive_target_id 会立刻返回，
        // 没有这个延时主循环会以几百 Hz 空转（Arduino 版有 delay(5)）
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
