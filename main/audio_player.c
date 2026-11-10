#include "audio_player.h"

#include <stdbool.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_gmf_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_I2S_BCLK_GPIO  GPIO_NUM_10
#define AUDIO_I2S_LRCK_GPIO  GPIO_NUM_11
#define AUDIO_I2S_DOUT_GPIO  GPIO_NUM_9

static const char *TAG = "audio_player";
static i2s_chan_handle_t s_i2s_tx;
static esp_asp_handle_t s_player;
static volatile bool s_playing;
// 当前正在播的 URL，用于"同一首歌不重复起播"的幂等判断
static char s_current_url[256];
// 输出闸门：停播时立刻关掉，即使 pipeline 没能马上停下，
// 也保证不再有残留 PCM 被推到功放上
static volatile bool s_output_muted = true;
// 当前 I2S 时钟配置的采样率。只有它变了才需要重配时钟，
// 每次切歌都 disable/enable 会让 BCLK 停摆再恢复，甩出一声爆音。
static int s_current_rate = 44100;
// 解码输出的位深，MUSIC_INFO 里更新。只有 16bit 才能按 int16 做音量缩放。
static volatile int s_pcm_bits = 16;

// 软件音量。对齐 Arduino 版 Audio::setVolume(15)：
//   volumetable[15] = 34，增益 = 34/64 ≈ 0.53（-5.5dB）
// IDF 版原来没有任何音量控制，PCM 满幅直出，输出功率比 Arduino 版高
// 3 倍以上。MAX98357A 是 filterless D 类功放，输出幅度越大、喇叭线与
// PCB 走线耦合回输入的能量越强，停播后就越容易越过自激阈值开始啸叫。
#define AUDIO_VOL_SHIFT  6
#define AUDIO_VOL_LEVEL  34

static int write_pcm(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (s_output_muted) {
        // 闸门关闭时把数据"吃掉"，声称已消费，让 pipeline 正常推进直到 stop 生效。
        // 注意必须返回 size 而不是 0：返回 0 会被上层当成写入失败。
        return size;
    }

    // 16bit PCM 数字衰减，和 Arduino 版 playChunk() 完全一样的算法。
    // 位深不是 16bit 时不能按 int16 处理，否则会把采样打散成噪声。
    if (s_pcm_bits == 16) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            ESP_LOGI(TAG, "pcm chunk: %d bytes, volume %d/64", size,
                     AUDIO_VOL_LEVEL);
        }
        int16_t *pcm = (int16_t *)(void *)data;
        int samples = size / (int)sizeof(int16_t);
        for (int i = 0; i < samples; ++i) {
            pcm[i] = (int16_t)((pcm[i] * AUDIO_VOL_LEVEL) >> AUDIO_VOL_SHIFT);
        }
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "pcm is %d-bit, volume scaling skipped", s_pcm_bits);
        }
    }

    size_t written = 0;
    return i2s_channel_write(s_i2s_tx, data, size, &written, portMAX_DELAY);
}

// 向 I2S 灌一段数字静音。
//
// 这是参照 Arduino 版 ESP32-audioI2S 的做法：它的 stopSong() 只做
//   memset(m_outBuff, 0, ...); i2s_zero_dma_buffer(port);
// 也就是把 DMA 里残留的 PCM 全清零，但**不关 I2S**。
//
// 板上 H3.4(98357_GAIN) 和 H3.5(98357_SD) 是悬空的，功放关断状态不受 MCU
// 控制。一旦把 BCLK 停掉，MAX98357A 就进了一个不确定状态，很容易自激啸叫。
// 反过来，只要保持 I2S 在跑、DMA 里全是 0，功放拿到的是持续且确定的静音，
// 这才是 Arduino 版不叫的真正原因。
//
// 灌满一轮 DMA（dma_desc_num * dma_frame_num * 4 字节）大约 30ms，
// 这里按 60ms 灌，保证冲掉队列里还没播完的旧 PCM。
static void i2s_write_silence(int ms)
{
    static const uint8_t zeros[1024] = {0};
    // 按 48kHz / 16bit / 立体声估算，多灌一点无所谓
    int remain = (48000 * 2 * 2 / 1000) * ms;
    size_t written = 0;
    while (remain > 0) {
        size_t chunk = (remain > (int)sizeof(zeros)) ? sizeof(zeros)
                                                     : (size_t)remain;
        // 超时给短一点：这个函数会在 stop 的等待循环里反复调用，
        // 单次阻塞太久会把等待时间撑爆
        if (i2s_channel_write(s_i2s_tx, zeros, chunk, &written,
                              pdMS_TO_TICKS(20)) != ESP_OK) {
            break;
        }
        remain -= (int)chunk;
    }
}

// 静音守护任务：只要处于 muted 状态，就持续往 I2S 喂 0。
//
// 这是最后一道软件保险。板上 98357_SD / 98357_GAIN 悬空，功放的关断与
// 增益 MCU 完全管不了，唯一能做的是保证送进去的永远是确定的 0。
// 只靠 stop 里灌一次静音不够：pipeline 内部线程晚一点收尾、DMA 指针回卷，
// 都可能把旧数据重新顶出来。让一个常驻任务持续喂 0，无论 pipeline 处于
// 什么状态，I2S 上跑的都是静音。
static void silence_keeper_task(void *arg)
{
    (void)arg;
    static const uint8_t zeros[1024] = {0};
    size_t written = 0;
    while (true) {
        if (s_output_muted) {
            // 1024 字节 ≈ 5.3ms@48k/16bit/立体声。
            // i2s_channel_write 正常会阻塞到 DMA 有空位，但如果 I2S 已停或
            // 缓冲满，它会立刻返回 —— 那样本任务就成了忙等死循环，
            // task_wdt 会报 "silence_keeper did not reset the watchdog"。
            // 所以一律再让出 4ms，保证一个 WDT 周期内必然休眠。
            i2s_channel_write(s_i2s_tx, zeros, sizeof(zeros), &written,
                              pdMS_TO_TICKS(20));
            vTaskDelay(pdMS_TO_TICKS(4));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
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
            if (info->bits > 0) {
                s_pcm_bits = info->bits;
            }
            if (info->sample_rate > 0 && info->sample_rate != s_current_rate) {
                i2s_std_clk_config_t clk_cfg =
                    I2S_STD_CLK_DEFAULT_CONFIG(info->sample_rate);
                i2s_channel_disable(s_i2s_tx);
                if (i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg) !=
                    ESP_OK) {
                    ESP_LOGE(TAG, "failed to configure I2S sample rate");
                }
                i2s_channel_enable(s_i2s_tx);
                // 重配期间 DMA 被清空，先补一段静音，避免功放收到空转的噪声
                i2s_write_silence(20);
                s_current_rate = info->sample_rate;
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
    // DMA underrun 时自动把发送缓冲清零。
    //
    // 这个字段默认是 false，含义是：缓冲发空之后，硬件会把 DMA 里最后
    // 一段数据无限重发。停播后听到的"一直啸叫"，本质上就是最后那一小段
    // 音乐在 DMA 里循环播放。打开它，underrun 时补的是 0。
    chan_cfg.auto_clear = true;
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

    // 刚分配的 DMA 缓冲区是未初始化内存，如果就这么让 I2S 空转，
    // 硬件会把随机字节当 PCM 反复推给功放 —— 上电就响的根因之一。
    // 灌满 0 之后**保持 I2S 运行、不要 disable**：DMA 里全是 0 时即使欠载，
    // 硬件重发的也是静音，效果等同 Arduino 版的 i2s_zero_dma_buffer。
    i2s_write_silence(60);

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

    // 静音守护常驻任务。此时 s_output_muted 为 true，它会立即开始灌 0。
    // 优先级与主任务同级（1），靠 i2s_channel_write 的阻塞让出 CPU。
    if (xTaskCreate(silence_keeper_task, "silence_keeper", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to create silence keeper task");
    }

    return ESP_OK;
}

esp_err_t audio_player_play(const char *url)
{
    if (s_player == NULL || url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    // 同一首歌已经在播就直接返回。
    // 少了这一层，只要卡片状态抖一下就会重新 run 一次 pipeline，
    // 每次 run 都会在 MUSIC_INFO 里 disable/enable 一次 I2S 时钟，
    // MAX98357A 在 BCLK 停摆再恢复的瞬间会甩出满幅高频噪声 —— 这就是啸叫。
    if (strcmp(s_current_url, url) == 0) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        if (esp_audio_simple_player_get_state(s_player, &state) == ESP_GMF_ERR_OK &&
            (state == ESP_ASP_STATE_RUNNING || state == ESP_ASP_STATE_PAUSED)) {
            s_output_muted = false;
            return ESP_OK;
        }
    }

    if (audio_player_stop() != ESP_OK) {
        // 上一首没停干净也继续尝试，run 自己会再判断一次状态
        ESP_LOGW(TAG, "previous stop incomplete, trying to run anyway");
    }

    ESP_LOGI(TAG, "playing %s", url);
    if (esp_audio_simple_player_run(s_player, url, NULL) != ESP_GMF_ERR_OK) {
        s_current_url[0] = '\0';
        ESP_LOGE(TAG, "run rejected, player state may be stuck");
        return ESP_FAIL;
    }
    strlcpy(s_current_url, url, sizeof(s_current_url));
    s_output_muted = false;
    return ESP_OK;
}

esp_err_t audio_player_stop(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_asp_state_t state = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_player, &state);

    // 先止血：不管后面 pipeline 能不能干净停下，
    // 这里一关就不会再有音频数据被推到 I2S
    s_output_muted = true;

    if (state != ESP_ASP_STATE_RUNNING && state != ESP_ASP_STATE_PAUSED) {
        ESP_LOGI(TAG, "stop: already idle (state=%d)", (int)state);
        i2s_write_silence(60);
        s_playing = false;
        s_current_url[0] = '\0';
        return ESP_OK;
    }

    ESP_LOGI(TAG, "stop: requesting pipeline stop (state=%d)", (int)state);
    esp_gmf_err_t stop_ret = esp_audio_simple_player_stop(s_player);
    if (stop_ret != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "stop: pipeline stop returned %d", stop_ret);
    }

    // esp_audio_simple_player_stop 走的是 esp_gmf_pipeline_stop，是异步的：
    // 它发出停止请求就返回了，pipeline 真正落地 STOPPED 要等下游任务跑完。
    // 这段时间里 player->state 还是 RUNNING，紧接着的 run 会被
    // "The player still running, call stop first" 直接拒掉，状态机就卡死。
    //
    // 等待期间必须一直给 I2S 喂静音。如果只是 vTaskDelay 干等，
    // DMA 会 underrun，ESP32 的 I2S 硬件会把缓冲区里最后一段音乐 PCM
    // 反复重发 —— 表现就是"卡片拿走之后还要再叫一声"。
    TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(600);
    while ((xTaskGetTickCount() - start) < timeout) {
        i2s_write_silence(10);
        if (esp_audio_simple_player_get_state(s_player, &state) == ESP_GMF_ERR_OK &&
            state != ESP_ASP_STATE_RUNNING && state != ESP_ASP_STATE_PAUSED) {
            break;
        }
    }
    int waited = (int)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);

    if (waited >= 600) {
        ESP_LOGE(TAG, "stop: pipeline still not stopped after %dms, state=%d",
                 waited, (int)state);
    } else {
        ESP_LOGI(TAG, "stop: pipeline stopped in %dms, state=%d", waited,
                 (int)state);
    }

    // 和 Arduino 版 stopSong() 的做法一致：把 DMA 里的残留 PCM 全部冲成 0，
    // 但**保持 I2S 时钟运行**。功放始终有时钟、有确定的 0 输入，才不会自激。
    i2s_write_silence(60);

    s_playing = false;
    s_current_url[0] = '\0';
    return (waited >= 600) ? ESP_ERR_TIMEOUT : ESP_OK;
}

bool audio_player_is_playing(void)
{
    return s_playing;
}
