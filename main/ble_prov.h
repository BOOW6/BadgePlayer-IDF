/**
 * @file ble_prov.h
 * @brief 蓝牙配网通道：手机 App 与吧唧机之间唯一的双向链路
 *
 * 为什么要蓝牙而不是 SoftAP：
 *   SoftAP 要手机切走 WiFi，iOS/Android 都要额外处理网络切换，
 *   而且切走之后就看不到"配网是否成功"的实时回执。蓝牙全双工，
 *   手机保持联网的同时还能收设备回调，体验干净得多。
 *
 * 协议：自定义 GATT 服务，一条 write（App→设备）+ 一条 notify（设备→App），
 *       上面跑「分帧 + UTF-8 JSON」。帧格式见 ble_prov.c 顶部注释。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 收到一条完整 JSON 时的回调。
 *
 * 注意：回调运行在 NimBLE host 任务上下文里，**不要在里面做阻塞操作**
 * （连 WiFi、下载文件、等 HTTP）。需要耗时的活儿请丢到自己的任务队列。
 * json 指针只在回调期间有效。
 */
typedef void (*ble_cmd_cb_t)(const char *json);

/**
 * 初始化 BLE 并开始广播。
 * @param device_name 广播名，建议 "AIBAJI-XXXX"（SN 后四位）
 * @param cb          收到 JSON 命令的回调
 */
esp_err_t ble_prov_init(const char *device_name, ble_cmd_cb_t cb);

/** 往手机推一条 JSON（内部自动分帧）。未连接时直接丢弃并返回 false。 */
bool ble_prov_send(const char *json);

/** 当前是否有手机连着。 */
bool ble_prov_connected(void);

/** 主动断开当前连接（配网完成后调用，省电）。 */
void ble_prov_disconnect(void);

#ifdef __cplusplus
}
#endif
