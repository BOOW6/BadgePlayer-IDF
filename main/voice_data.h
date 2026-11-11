/**
 * @file voice_data.h
 * @brief 内置语音提示素材的声明
 *
 * 为什么把音频编进固件而不是放文件系统：
 *   这些提示的用途就是「连不上网的时候告诉用户怎么办」，
 *   那种状态下既没有网络也不该依赖外部存储。把 16kHz/16bit/单声道
 *   PCM 直接放进 .rodata，I2S 一推就响，不需要解码器、不需要挂文件系统。
 *
 * 素材由 tools/gen_voice.py 生成，改文案后重跑脚本即可。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define VOICE_SAMPLE_RATE 16000

/** 一段语音：单声道 16bit PCM + 采样点数 */
typedef struct {
    const int16_t *pcm;
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
