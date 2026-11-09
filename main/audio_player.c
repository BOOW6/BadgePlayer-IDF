#include "audio_player.h"

#include <stdbool.h>

#include "driver/i2s_std.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_gmf_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define AUDIO_I2S_BCLK_GPIO  GPIO_NUM_10
#define AUDIO_I2S_LRCK_GPIO  GPIO_NUM_11
#define AUDIO_I2S_DOUT_GPIO  GPIO_NUM_9

static const char *TAG = "audio_player";
static i2s_chan_handle_t s_i2s_tx;
static esp_asp_handle_t s_player;
static volatile bool s_playing;

static int write_pcm(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    size_t written = 0;
    return i2s_channel_write(s_i2s_tx, data, size, &written, portMAX_DELAY);
}

static int player_event_callback(esp_asp_event_pkt_t *event, void *ctx)
{
    (void)ctx;

    if (event == NULL) {
        return -1;
    }

    switch (event->type) {
    case ESP_ASP_EVENT_TYPE_MUSIC_INFO:
        if (event->payload != NULL &&
            event->payload_size >= sizeof(esp_asp_music_info_t)) {
            const esp_asp_music_info_t *info = event->payload;
            ESP_LOGI(TAG, "audio format: %d Hz, %u channels, %u bits",
                     info->sample_rate, info->channels, info->bits);
            if (info->sample_rate > 0) {
                i2s_std_clk_config_t clk_cfg =
                    I2S_STD_CLK_DEFAULT_CONFIG(info->sample_rate);
                if (i2s_channel_disable(s_i2s_tx) == ESP_OK) {
                    if (i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg) !=
                        ESP_OK) {
                        ESP_LOGE(TAG, "failed to configure I2S sample rate");
                    }
                    i2s_channel_enable(s_i2s_tx);
                } else {
                    ESP_LOGE(TAG, "failed to disable I2S for clock reconfig");
                }
            }
        }
        break;
    case ESP_ASP_EVENT_TYPE_STATE:
        if (event->payload != NULL &&
            event->payload_size >= sizeof(esp_asp_state_t)) {
            s_playing = *(esp_asp_state_t *)event->payload == ESP_ASP_STATE_RUNNING;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t audio_player_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG,
                       "create I2S channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws = AUDIO_I2S_LRCK_GPIO,
            .dout = AUDIO_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG,
                       "initialize I2S failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG,
                       "enable I2S failed");

    esp_asp_cfg_t config = {
        .out = {
            .cb = write_pcm,
            .user_ctx = NULL,
        },
        .task_prio = 5,
        .task_stack = 8192,
        .task_core = 1,
        .task_stack_in_ext = false,
    };
    if (esp_audio_simple_player_new(&config, &s_player) != ESP_GMF_ERR_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (esp_audio_simple_player_set_event(s_player, player_event_callback, NULL) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t audio_player_play(const char *url)
{
    if (s_player == NULL || url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_playing) {
        if (audio_player_stop() != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_LOGI(TAG, "playing %s", url);
    return esp_audio_simple_player_run(s_player, url, NULL) == ESP_GMF_ERR_OK
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t audio_player_stop(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_playing = false;
    return esp_audio_simple_player_stop(s_player) == ESP_GMF_ERR_OK
               ? ESP_OK
               : ESP_FAIL;
}

bool audio_player_is_playing(void)
{
    return s_playing;
}
