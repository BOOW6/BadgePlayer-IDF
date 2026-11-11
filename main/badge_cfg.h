/**
 * @file badge_cfg.h
 * @brief 设备本地配置：SN、WiFi、密钥、token 全部落在 NVS
 *
 * 为什么不用分区或者文件：
 *   NVS 有磨损均衡和掉电保护，写坏的概率远低于自己撸一块裸 flash。
 *   这些字段都是几十字节，NVS 最合适。
 *
 * 安全边界（当前阶段故意不做）：
 *   NVS 默认是明文。拆机接夹具能读出 secret。但因为 secret 一机一密、
 *   绑定时才生成、且设备端 token 只能只读拉歌，泄露一台只损失一台。
 *   量产前再叠加 NVS 加密（CONFIG_NVS_ENCRYPTION）即可。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_SN_LEN      24
#define BADGE_SSID_LEN    33
#define BADGE_PASS_LEN    65
#define BADGE_SECRET_LEN  65
#define BADGE_TOKEN_LEN   65
#define BADGE_FW_VERSION  "1.0.0"

typedef struct {
    char    sn[BADGE_SN_LEN];          // 出厂序列号，产线烧固件后由 App 下发
    char    ssid[BADGE_SSID_LEN];      // 已配的 WiFi
    char    pass[BADGE_PASS_LEN];
    char    secret[BADGE_SECRET_LEN];  // 绑定时云端下发的一机一密密钥
    char    token[BADGE_TOKEN_LEN];    // secret 换来的会话 token
    int64_t token_exp;                 // token 到期时间（epoch 秒，0 表示未知）
    bool    configured;                // WiFi + secret 是否齐备
    bool    activated;                 // 是否拿到过有效 token
} badge_cfg_t;

/** 从 NVS 读出配置。返回 false 表示设备还是全新未配置状态。 */
bool badge_cfg_load(badge_cfg_t *out);

/** 把内存里的配置整体写回 NVS。 */
esp_err_t badge_cfg_save(const badge_cfg_t *cfg);

/** 只更新 WiFi 三个字段（配网成功时用）。 */
esp_err_t badge_cfg_save_wifi(const char *ssid, const char *pass, const char *secret);

/** 只更新 SN。 */
esp_err_t badge_cfg_save_sn(const char *sn);

/** 只更新 token 与到期时间。 */
esp_err_t badge_cfg_save_token(const char *token, int64_t exp_epoch);

/** 擦掉整块配置（恢复出厂 / 解绑）。 */
esp_err_t badge_cfg_erase(void);

/** rfid -> 音频 URL 映射：设备靠它把读到的卡号翻成要拉哪条流。 */
esp_err_t badge_map_set(const char *rfid, const char *url);
bool      badge_map_get(const char *rfid, char *out_url, size_t out_size);
/** 删掉一张卡的映射（传空 URL 等价调用）。 */
esp_err_t badge_map_remove(const char *rfid);
/** 清空全部映射（重新同步歌单前调用）。 */
esp_err_t badge_map_clear(void);
/** 当前已配对的卡片数量。 */
int       badge_map_count(void);

#ifdef __cplusplus
}
#endif
