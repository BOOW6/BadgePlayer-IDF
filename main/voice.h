/**
 * @file voice.h
 * @brief 语音提示播放（非阻塞）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "voice_data.h"

/** 初始化：建队列和播放任务。必须在 audio_player_init() 之后调用。 */
esp_err_t voice_init(void);

/**
 * 排队播一段提示音，立即返回。
 *
 * 队列满了就丢弃 —— 提示音是「锦上添花」，不值得为它阻塞主循环。
 * 断网重试那种场景本来就可能连续触发，丢一两句比卡住业务好。
 */
esp_err_t voice_play(voice_id_t id);

/** 当前是否有提示音在播 */
bool voice_is_playing(void);
