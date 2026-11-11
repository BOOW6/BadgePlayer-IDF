/**
 * @file voice.c
 * @brief 语音提示播放任务
 *
 * 设计取舍：
 *   1) 单独一个低优先级任务做播放，调用方（net_task / 主循环）只入队。
 *      WiFi 重连是阻塞 20 秒的活儿，不能让它顺带卡住 I2S 写入。
 *   2) 队列深度 4，满了直接丢。断网时反复触发提示音是常态，
 *      宁可少说一句，也不能让队列堵住。
 */
#include "voice.h"

#include <string.h>

#include "audio_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "voice";

static QueueHandle_t  s_q;
static volatile bool  s_busy;

static void voice_task(void *arg)
{
    (void)arg;
    uint8_t id;
    while (true) {
        if (xQueueReceive(s_q, &id, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (id >= (uint8_t)VOICE_MAX) {
            continue;
        }
        const voice_clip_t *c = &g_voice_clips[id];
        if (c->pcm == NULL || c->samples == 0) {
            continue;
        }
        s_busy = true;
        ESP_LOGI(TAG, "play #%u (%.1fs)", (unsigned)id,
                 c->samples / (float)VOICE_SAMPLE_RATE);
        esp_err_t err = audio_player_play_pcm(c->pcm, c->samples,
                                              VOICE_SAMPLE_RATE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "play #%u failed: %s", (unsigned)id,
                     esp_err_to_name(err));
        }
        s_busy = false;
        // 句尾留白：连续两句之间不加间隔会粘成一个词
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

esp_err_t voice_init(void)
{
    if (s_q != NULL) {
        return ESP_OK;
    }
    s_q = xQueueCreate(4, sizeof(uint8_t));
    if (s_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // 优先级 4：低于命令 worker(6)，高于 net(3)。
    // 提示音要抢在重连循环之前出声，但不能压住配网命令。
    if (xTaskCreate(voice_task, "voice", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_play(voice_id_t id)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id < 0 || id >= VOICE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t v = (uint8_t)id;
    if (xQueueSend(s_q, &v, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped #%u", (unsigned)id);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool voice_is_playing(void)
{
    return s_busy;
}
