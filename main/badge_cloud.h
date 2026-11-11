/**
 * @file badge_cloud.h
 * @brief 设备侧云端调用（激活换 token、贴卡解析播放地址）
 *
 * 播放链路：贴卡 -> device_resolve 按 rfid 要地址 -> 直接拉流。
 * 地址由手机在配歌时签好写进服务器，设备不存本地、不做签名。
 * 断网 / token 过期 / 云端挂了时，贴卡无法解析，播放失败（这是在线流的取舍）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 用 sn + secret 换一枚设备 token。
 *
 * @param sn         序列号
 * @param secret     绑定时下发的密钥
 * @param hw_uid     硬件指纹（WiFi STA MAC 的十六进制），首次激活时被云端认领
 * @param out_token  输出缓冲，建议 80 字节以上
 * @param token_size 缓冲大小
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 云端拒绝（密钥不对/未下发）；
 *         ESP_ERR_INVALID_STATE 已激活过（already_active）；其他为网络错误
 */
esp_err_t badge_cloud_activate(const char *sn, const char *secret,
                               const char *hw_uid,
                               char *out_token, size_t token_size);

/**
 * 贴卡解析：用设备 token + 卡号向云端要这张卡对应的播放地址。
 *
 * @param token      设备会话 token（activate 换来的）
 * @param rfid       卡号，形如 "53:25:4E:F7:32:00:01"
 * @param out_url    输出缓冲，建议 768 字节以上
 * @param out_size   缓冲大小
 * @return ESP_OK 拿到地址；ESP_ERR_NOT_FOUND 这张卡没配对（no_badge，可当作新卡）；
 *         ESP_ERR_INVALID_STATE 配了但没音源 / 地址过期（no_song/no_url/url_expired）；
 *         ESP_ERR_INVALID_RESPONSE 云端拒绝（token 失效等）；其他为网络错误
 */
esp_err_t badge_cloud_resolve(const char *token, const char *rfid,
                              char *out_url, size_t out_size);

/** 本机硬件指纹（WiFi STA MAC），失败返回空字符串。 */
void badge_cloud_hw_uid(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
