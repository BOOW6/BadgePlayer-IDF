/**
 * @file badge_cloud.c
 * @brief 设备侧云端调用实现
 *
 * 云端约定（和 App 用的是同一套）：
 *   POST https://<endpoint>/.cloud/database/rest/rpc/device_activate
 *   header  x-wb-webapp-access-key: <应用 key>
 *           Origin: <endpoint>
 *   body    {"p_sn":"...","p_secret":"...","p_hw_uid":"..."}
 *   响应    {"data":{"ok":true,"token":"...","expires_in":2592000,...}}
 *
 * 匿名可调用（设备的正确身份是 secret，不是登录账号），所以不带 Authorization。
 */
#include "badge_cloud.h"

#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "badge_cloud";

#ifndef CONFIG_BADGE_CLOUD_ENDPOINT
#define CONFIG_BADGE_CLOUD_ENDPOINT "https://aibaji.app.workbuddy.host"
#endif
#ifndef CONFIG_BADGE_CLOUD_KEY
#define CONFIG_BADGE_CLOUD_KEY \
    "wbpk_HICdTD2EmCudhhwCc5Evnn_ASEVc0iHhE3eg55Pb1t7HJ2bb3vQvwJE"
#endif

/* 签名 URL 可能到几百字节，带上 JSON 包装后 1024 有点紧，放宽到 2KB */
#define RESP_BUF_SIZE 2048

static char s_resp[RESP_BUF_SIZE];

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t used = strlen(s_resp);
        size_t room = sizeof(s_resp) - 1 - used;
        if (room > 0) {
            size_t n = ((size_t)evt->data_len < room) ? (size_t)evt->data_len : room;
            memcpy(s_resp + used, evt->data, n);
            s_resp[used + n] = '\0';
        }
    }
    return ESP_OK;
}

void badge_cloud_hw_uid(char *out, size_t out_size)
{
    if (out == NULL || out_size < 13) {
        if (out && out_size) {
            out[0] = '\0';
        }
        return;
    }
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t badge_cloud_activate(const char *sn, const char *secret,
                               const char *hw_uid,
                               char *out_token, size_t token_size)
{
    if (sn == NULL || sn[0] == '\0' || secret == NULL || secret[0] == '\0' ||
        out_token == NULL || token_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_token[0] = '\0';
    s_resp[0] = '\0';

    char body[320];
    snprintf(body, sizeof(body),
             "{\"p_sn\":\"%s\",\"p_secret\":\"%s\",\"p_hw_uid\":\"%s\"}",
             sn, secret, (hw_uid && hw_uid[0]) ? hw_uid : "");

    char url[128];
    snprintf(url, sizeof(url), "%s/.cloud/database/rest/rpc/device_activate",
             CONFIG_BADGE_CLOUD_ENDPOINT);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .event_handler = http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-wb-webapp-access-key", CONFIG_BADGE_CLOUD_KEY);
    esp_http_client_set_header(client, "Origin", CONFIG_BADGE_CLOUD_ENDPOINT);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "activate request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "activate http %d: %s", status, s_resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(s_resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "activate bad json: %s", s_resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 云端把 RPC 结果包了一层 data
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL) {
        data = root;
    }

    esp_err_t result = ESP_FAIL;
    cJSON *ok = cJSON_GetObjectItem(data, "ok");
    if (cJSON_IsTrue(ok)) {
        cJSON *token = cJSON_GetObjectItem(data, "token");
        if (cJSON_IsString(token) && token->valuestring[0]) {
            strlcpy(out_token, token->valuestring, token_size);
            cJSON *exp = cJSON_GetObjectItem(data, "expires_in");
            ESP_LOGI(TAG, "activated, token expires in %d s",
                     cJSON_IsNumber(exp) ? exp->valueint : 0);
            result = ESP_OK;
        } else {
            // already_active 也走这里：云端认为这机器早配好了，没给新 token
            result = ESP_ERR_INVALID_STATE;
            ESP_LOGW(TAG, "no token (already activated?)");
        }
    } else {
        cJSON *e = cJSON_GetObjectItem(data, "error");
        const char *code = cJSON_IsString(e) ? e->valuestring : "unknown";
        ESP_LOGE(TAG, "activate rejected: %s", code);
        // 密钥不对 / 没下发：需要手机重新配网
        result = ESP_ERR_NOT_FOUND;
    }

    cJSON_Delete(root);
    return result;
}

esp_err_t badge_cloud_resolve(const char *token, const char *rfid,
                              char *out_url, size_t out_size)
{
    if (token == NULL || token[0] == '\0' || rfid == NULL || rfid[0] == '\0' ||
        out_url == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_url[0] = '\0';
    s_resp[0] = '\0';

    char body[512];
    snprintf(body, sizeof(body),
             "{\"p_token\":\"%s\",\"p_rfid\":\"%s\"}", token, rfid);

    char url[128];
    snprintf(url, sizeof(url), "%s/.cloud/database/rest/rpc/device_resolve",
             CONFIG_BADGE_CLOUD_ENDPOINT);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .event_handler = http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-wb-webapp-access-key", CONFIG_BADGE_CLOUD_KEY);
    esp_http_client_set_header(client, "Origin", CONFIG_BADGE_CLOUD_ENDPOINT);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resolve request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "resolve http %d: %s", status, s_resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(s_resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "resolve bad json: %s", s_resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL) {
        data = root;
    }

    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    cJSON *ok = cJSON_GetObjectItem(data, "ok");
    if (cJSON_IsTrue(ok)) {
        cJSON *u = cJSON_GetObjectItem(data, "url");
        if (cJSON_IsString(u) && u->valuestring[0]) {
            strlcpy(out_url, u->valuestring, out_size);
            result = ESP_OK;
        } else {
            result = ESP_ERR_INVALID_STATE;
        }
    } else {
        cJSON *e = cJSON_GetObjectItem(data, "error");
        const char *code = cJSON_IsString(e) ? e->valuestring : "unknown";
        ESP_LOGW(TAG, "resolve rejected: %s", code);
        if (strcmp(code, "no_badge") == 0) {
            result = ESP_ERR_NOT_FOUND;      // 没配对 → 新卡
        } else if (strcmp(code, "no_song") == 0 ||
                   strcmp(code, "no_url") == 0 ||
                   strcmp(code, "url_expired") == 0) {
            result = ESP_ERR_INVALID_STATE;  // 配了但没音源 / 地址过期
        } else {
            result = ESP_ERR_INVALID_RESPONSE; // token 失效等
        }
    }

    cJSON_Delete(root);
    return result;
}
