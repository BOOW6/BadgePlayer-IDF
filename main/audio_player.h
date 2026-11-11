#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_player_init(void);
esp_err_t audio_player_play(const char *url);
esp_err_t audio_player_stop(void);
bool audio_player_is_playing(void);

/**
 * @brief 直接推一段单声道 16bit PCM 到 I2S（用于内置语音提示）
 *
 * 不走解码流水线：提示音必须断网也能响，不能依赖网络或文件系统。
 * 内部会先把正在播的音乐停掉，播完恢复静音闸门。
 *
 * @param pcm         采样数据，单声道
 * @param samples     采样点数（不是字节数）
 * @param sample_rate 采样率，<=0 时按 16000 处理
 */
esp_err_t audio_player_play_pcm(const int16_t *pcm, size_t samples,
                                int sample_rate);
