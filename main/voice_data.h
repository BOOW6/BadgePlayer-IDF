/**
 * @file voice_data.h
 * @brief 内置语音提示素材的声明
 *
 * 离线语音提示素材
 * 把 8 kHz/8bit μ-law 二进制数据嵌入固件
 * 播放时解码为 16bit PCM，直接通过 I2S 送给 MAX98357A
 *
 * 素材由 tools/gen_voice.py 生成，改文案后重跑脚本即可
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define VOICE_SAMPLE_RATE 8000

/** 一段语音：单声道 8 kHz 8bit μ-law + 采样点数 */
typedef struct {
    const uint8_t *ulaw;
    size_t         samples;
} voice_clip_t;

/** 语音编号。顺序必须和 voice_data.c 里 g_voice_clips 的下标一致 */
typedef enum {
    VOICE_NET_OK = 0,     /* 网络已连接 */
    VOICE_WIFI_FAIL = 1,  /* 网络连接失败，请检查路由器 */
    VOICE_NEED_PROV = 2,  /* 请打开手机应用，重新配置网络 */
    VOICE_NO_NET = 3,     /* 设备未联网，请先配置网络 */
    VOICE_MAX = 4,
} voice_id_t;

extern const voice_clip_t g_voice_clips[VOICE_MAX];
