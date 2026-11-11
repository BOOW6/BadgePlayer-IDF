/**
 * @file badge_cfg.c
 * @brief NVS 配置读写实现
 */
#include "badge_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "badge_cfg";

#define NS_CFG  "bcfg"
#define NS_MAP  "bmap"

/* 卡映射的值上限：url 最大 768，rfid 前缀最长 ~30，再加分隔符，1024 足够 */
#define MAP_VALUE_MAX 1024

static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    esp_err_t err = nvs_open(ns, mode, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", ns, esp_err_to_name(err));
    }
    return err;
}

/** 读字符串字段，读不到就留空，不当作错误 */
static void read_str(nvs_handle_t h, const char *key, char *dst, size_t dst_size)
{
    size_t len = dst_size;
    dst[0] = '\0';
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read %s failed: %s", key, esp_err_to_name(err));
        dst[0] = '\0';
    }
}

static esp_err_t write_str(nvs_handle_t h, const char *key, const char *val)
{
    if (val == NULL || val[0] == '\0') {
        // 空值就删掉这个键，避免下次读出一个空串还要再判一次
        esp_err_t err = nvs_erase_key(h, key);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    return nvs_set_str(h, key, val);
}

bool badge_cfg_load(badge_cfg_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    if (open_ns(NS_CFG, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    read_str(h, "sn", out->sn, sizeof(out->sn));
    read_str(h, "ssid", out->ssid, sizeof(out->ssid));
    read_str(h, "pass", out->pass, sizeof(out->pass));
    read_str(h, "secret", out->secret, sizeof(out->secret));
    read_str(h, "token", out->token, sizeof(out->token));

    int64_t exp = 0;
    if (nvs_get_i64(h, "texp", &exp) == ESP_OK) {
        out->token_exp = exp;
    }
    nvs_close(h);

    /* 有 ssid 就算「配过网」。
     *
     * 早先要求 secret 也齐备，结果 secret 一旦缺失（清过 NVS、云端判定已激活
     * 就不再下发 secret），net_task 会永远卡在初始的 while(!configured) 里：
     * 表现为「配网当时能连上，一断电重启就再也连不上」，而且不补激活。
     * secret 只影响 device_activate，不影响连 WiFi 和拉流播放。 */
    out->configured = (out->ssid[0] != '\0');
    out->activated = (out->token[0] != '\0');

    ESP_LOGI(TAG, "loaded: sn=%s wifi=%s secret=%s token=%s",
             out->sn[0] ? out->sn : "(none)",
             out->ssid[0] ? "yes" : "no",
             out->secret[0] ? "yes" : "no",
             out->token[0] ? "yes" : "no");
    return out->sn[0] != '\0' || out->configured;
}

esp_err_t badge_cfg_save(const badge_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = write_str(h, "sn", cfg->sn);
    if (err == ESP_OK) err = write_str(h, "ssid", cfg->ssid);
    if (err == ESP_OK) err = write_str(h, "pass", cfg->pass);
    if (err == ESP_OK) err = write_str(h, "secret", cfg->secret);
    if (err == ESP_OK) err = write_str(h, "token", cfg->token);
    if (err == ESP_OK) err = nvs_set_i64(h, "texp", cfg->token_exp);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t badge_cfg_save_wifi(const char *ssid, const char *pass, const char *secret)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = write_str(h, "ssid", ssid);
    if (err == ESP_OK) err = write_str(h, "pass", pass);
    if (err == ESP_OK && secret != NULL) err = write_str(h, "secret", secret);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t badge_cfg_save_sn(const char *sn)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = write_str(h, "sn", sn);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t badge_cfg_save_token(const char *token, int64_t exp_epoch)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NS_CFG, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = write_str(h, "token", token);
    if (err == ESP_OK) err = nvs_set_i64(h, "texp", exp_epoch);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t badge_cfg_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_CFG, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    err = nvs_open(NS_MAP, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

/*
 * 卡号（rfid uid，形如 "53:25:4E:F7:32:00:01"）有 20 个字符，直接当 NVS key
 * 会超 NVS 的 15 字符上限，报 ESP_ERR_NVS_KEY_TOO_LONG，URL 根本写不进去，
 * 表现就是「card in 了却不去拉流」。这里用 FNV-1a 哈希压成 8 位十六进制短 key。
 */
static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void rfid_key(const char *rfid, char *key, size_t key_size)
{
    snprintf(key, key_size, "%08lx", (unsigned long)fnv1a32(rfid));
}

esp_err_t badge_map_set(const char *rfid, const char *url)
{
    if (rfid == NULL || rfid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // 传空 URL 等于删掉这张卡
    if (url == NULL || url[0] == '\0') {
        return badge_map_remove(rfid);
    }

    char key[9];
    rfid_key(rfid, key, sizeof(key));

    // value 存成 "rfid|url"，读的时候校验前缀，挡住（极低概率的）哈希碰撞。
    char val[MAP_VALUE_MAX];
    snprintf(val, sizeof(val), "%s|%s", rfid, url);

    nvs_handle_t h;
    esp_err_t err = open_ns(NS_MAP, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "map set %s failed: %s", rfid, esp_err_to_name(err));
    }
    return err;
}

bool badge_map_get(const char *rfid, char *out_url, size_t out_size)
{
    if (rfid == NULL || out_url == NULL || out_size == 0) {
        return false;
    }
    out_url[0] = '\0';

    char key[9];
    rfid_key(rfid, key, sizeof(key));

    nvs_handle_t h;
    if (open_ns(NS_MAP, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char buf[MAP_VALUE_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK || buf[0] == '\0') {
        return false;
    }

    // 拆 "rfid|url"，前缀必须和这张卡一致，不一致就是哈希碰撞，当作没找到
    char *sep = strchr(buf, '|');
    if (sep == NULL) {
        return false;
    }
    *sep = '\0';
    if (strcmp(buf, rfid) != 0) {
        return false;
    }
    strlcpy(out_url, sep + 1, out_size);
    return out_url[0] != '\0';
}

esp_err_t badge_map_remove(const char *rfid)
{
    if (rfid == NULL || rfid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char key[9];
    rfid_key(rfid, key, sizeof(key));
    nvs_handle_t h;
    esp_err_t err = open_ns(NS_MAP, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

int badge_map_count(void)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NS_MAP, NVS_TYPE_STR, &it);
    int n = 0;
    while (err == ESP_OK && it != NULL) {
        n++;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

esp_err_t badge_map_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_MAP, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}
