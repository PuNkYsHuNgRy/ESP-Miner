#include "pnky_ping.h"
#include "pnky_config.h"
#include "pnky_ws_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/md.h"
#include "driver/temperature_sensor.h"

static const char *TAG = "pnky_ping";

static int pnky_409_count = 0;
bool pnky_first_ping_done = false;
bool pnky_license_valid = false;

static temperature_sensor_handle_t s_temp_sensor = NULL;
static bool s_temp_sensor_enabled = false;

static void compute_challenge_response(const char *nonce, const char *key, char *out, size_t out_size)
{
    size_t data_len = strlen(nonce) + strlen(key);
    char *data = malloc(data_len + 1);
    if (!data) { out[0] = '\0'; return; }
    snprintf(data, data_len + 1, "%s%s", nonce, key);

    uint8_t hash[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char *)data, data_len);
    mbedtls_md_finish(&ctx, hash);
    mbedtls_md_free(&ctx);
    free(data);

    for (int i = 0; i < 32 && i * 2 + 2 <= (int)out_size; i++)
        snprintf(out + i * 2, out_size - i * 2, "%02x", hash[i]);
}

static bool pnky_send_ping_internal(int depth, GlobalState *GLOBAL_STATE)
{
    if (depth > 2) {
        ESP_LOGE(TAG, "Max ping auth retries exceeded");
        return false;
    }

    char *server_url = pnky_config_get_string(PNKY_KEY_SERVER_URL);
    if (!server_url) return false;

    if (!GLOBAL_STATE->SYSTEM_MODULE.is_connected) {
        ESP_LOGW(TAG, "Ping skipped: WiFi not connected");
        free(server_url);
        return false;
    }

    char *api_key = pnky_config_get_string(PNKY_KEY_API_KEY);
    char *challenge_nonce = pnky_config_get_string(PNKY_KEY_CHALLENGE_NONCE);
    char *solana_wallet = pnky_config_get_string(PNKY_KEY_SOLANA_WALLET);
    const char *device_id = pnky_config_get_device_id();

    if (!solana_wallet || strlen(solana_wallet) < 10) {
        ESP_LOGW(TAG, "No Solana wallet configured, skipping ping");
        free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
        return false;
    }

    SystemModule *mod = &GLOBAL_STATE->SYSTEM_MODULE;

    uint64_t uptime_us = esp_timer_get_time();
    unsigned long uptime_sec = (unsigned long)(uptime_us / 1000000ULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "public_key", solana_wallet);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "uptime_seconds", uptime_sec);
    cJSON_AddStringToObject(root, "firmware_version", mod->version ? mod->version : "unknown");

    bool btc_connected = false;
    taskENTER_CRITICAL(&GLOBAL_STATE->stratum_mux);
    if (GLOBAL_STATE->ws_ctx && pnky_ws_is_connected(GLOBAL_STATE->ws_ctx) && GLOBAL_STATE->ws_subscribed)
        btc_connected = true;
    else if (GLOBAL_STATE->transport)
        btc_connected = true;
    taskEXIT_CRITICAL(&GLOBAL_STATE->stratum_mux);
    cJSON_AddBoolToObject(root, "btc_connected", btc_connected);
    cJSON_AddNumberToObject(root, "btc_hashrate", (double)mod->current_hashrate);
    cJSON_AddNumberToObject(root, "btc_diff", GLOBAL_STATE->pool_difficulty);
    cJSON_AddNumberToObject(root, "btc_shares", (double)mod->shares_accepted);

    cJSON_AddNumberToObject(root, "btc_hashes", (double)mod->total_hashes);

    if (s_temp_sensor_enabled) {
        float temp;
        if (temperature_sensor_get_celsius(s_temp_sensor, &temp) == ESP_OK) {
            cJSON_AddNumberToObject(root, "temperature", temp);
        }
    }

    if (api_key && strlen(api_key) > 0) {
        cJSON_AddStringToObject(root, "api_key", api_key);
    }

    char cr_buf[65] = {0};
    if (challenge_nonce && strlen(challenge_nonce) > 0 && api_key && strlen(api_key) > 0) {
        compute_challenge_response(challenge_nonce, api_key, cr_buf, sizeof(cr_buf));
        cJSON_AddStringToObject(root, "challenge_response", cr_buf);
    }

    if (pnky_409_count >= 5) {
        cJSON_AddBoolToObject(root, "reregister", true);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/ping", server_url);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(json);
        goto cleanup;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(json);
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    char resp_buf[512] = {0};
    int resp_len = esp_http_client_read(client, resp_buf, sizeof(resp_buf) - 1);
    if (resp_len > 0) resp_buf[resp_len] = '\0';
    esp_http_client_cleanup(client);
    free(json);

    ESP_LOGI(TAG, "Ping responded: %d", status);

    if (status == 200) {
        pnky_409_count = 0;
        cJSON *resp = cJSON_Parse(resp_buf);
        if (resp) {
            bool save_nonce = false;
            bool save_key = false;
            cJSON *nonce_item = cJSON_GetObjectItem(resp, "challenge_nonce");
            if (nonce_item && cJSON_IsString(nonce_item) && strlen(nonce_item->valuestring) > 0) {
                pnky_config_set_string(PNKY_KEY_CHALLENGE_NONCE, nonce_item->valuestring);
                save_nonce = true;
            }
            cJSON *key_item = cJSON_GetObjectItem(resp, "api_key");
            if (key_item && cJSON_IsString(key_item) && strlen(key_item->valuestring) > 0) {
                pnky_config_set_string(PNKY_KEY_API_KEY, key_item->valuestring);
                save_key = true;
            }
            cJSON *msg_item = cJSON_GetObjectItem(resp, "message");
            if (msg_item && cJSON_IsString(msg_item)) {
                pnky_license_valid = (strstr(msg_item->valuestring, "awaiting license") == NULL &&
                                      strstr(msg_item->valuestring, "License revoked") == NULL &&
                                      strstr(msg_item->valuestring, "Not yet approved") == NULL);
            }
            ESP_LOGI(TAG, "Ping OK (nonce:%s key:%s license:%s)",
                     save_nonce ? "saved" : "unchanged",
                     save_key ? "saved" : "unchanged",
                     pnky_license_valid ? "valid" : "invalid");
            cJSON_Delete(resp);
        }
        pnky_first_ping_done = true;
        free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
        return true;

    } else if (status == 401) {
        cJSON *resp = cJSON_Parse(resp_buf);
        if (resp) {
            cJSON *detail = cJSON_GetObjectItem(resp, "detail");
            if (detail && cJSON_IsObject(detail)) {
                cJSON *k = cJSON_GetObjectItem(detail, "api_key");
                if (k && cJSON_IsString(k) && strlen(k->valuestring) > 0) {
                    pnky_config_set_string(PNKY_KEY_API_KEY, k->valuestring);
                    cJSON *n = cJSON_GetObjectItem(detail, "challenge_nonce");
                    if (n && cJSON_IsString(n) && strlen(n->valuestring) > 0) {
                        pnky_config_set_string(PNKY_KEY_CHALLENGE_NONCE, n->valuestring);
                    }
                    ESP_LOGI(TAG, "Got API key from 401, retrying");
                    cJSON_Delete(resp);
                    pnky_first_ping_done = true;
                    free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    return pnky_send_ping_internal(depth + 1, GLOBAL_STATE);
                }
            }
            cJSON_Delete(resp);
        }
        ESP_LOGW(TAG, "Auth failed (401) without new key, clearing credentials");
        pnky_config_erase_key(PNKY_KEY_API_KEY);
        pnky_config_erase_key(PNKY_KEY_CHALLENGE_NONCE);
        pnky_first_ping_done = true;
        free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
        return false;

    } else if (status == 409) {
        pnky_409_count++;
        ESP_LOGW(TAG, "Device conflict (409), count=%d", pnky_409_count);
        cJSON *resp = cJSON_Parse(resp_buf);
        if (resp) {
            cJSON *r = cJSON_GetObjectItem(resp, "reregister");
            if (cJSON_IsTrue(r)) {
                ESP_LOGW(TAG, "Server asked to re-register, generating new device ID");
                pnky_config_generate_device_id();
                pnky_409_count = 0;
            }
            cJSON_Delete(resp);
        }
        pnky_first_ping_done = true;
        free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
        return false;

    } else if (status == 429) {
        ESP_LOGW(TAG, "Rate limited (429), retrying after 60s");
        pnky_first_ping_done = true;
        free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        return pnky_send_ping_internal(depth + 1, GLOBAL_STATE);
    }

    ESP_LOGW(TAG, "Ping http_status=%d", status);
    pnky_first_ping_done = true;
    free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
    return false;

cleanup:
    free(server_url), free(api_key), free(challenge_nonce), free(solana_wallet);
    return false;
}

bool isPoolConnected(GlobalState *GLOBAL_STATE)
{
    if (GLOBAL_STATE->ws_ctx && pnky_ws_is_connected(GLOBAL_STATE->ws_ctx))
        return true;
    if (GLOBAL_STATE->transport)
        return true;
    return false;
}

bool pnky_send_ping(GlobalState *GLOBAL_STATE)
{
    return pnky_send_ping_internal(0, GLOBAL_STATE);
}

void pnky_ping_init(GlobalState *GLOBAL_STATE)
{
    const char *device_id = pnky_config_get_device_id();
    char *api_key = pnky_config_get_string(PNKY_KEY_API_KEY);
    ESP_LOGI(TAG, "PNKY ping initialized (device: %s, has_key: %s)",
             device_id, api_key && strlen(api_key) > 0 ? "yes" : "no");
    free(api_key);

    temperature_sensor_config_t temp_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&temp_config, &s_temp_sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_temp_sensor);
        if (err == ESP_OK) {
            s_temp_sensor_enabled = true;
            ESP_LOGI(TAG, "Temperature sensor enabled");
        }
    }
    if (!s_temp_sensor_enabled) {
        ESP_LOGW(TAG, "Temperature sensor not available (err: %s)", esp_err_to_name(err));
    }
}

void pnky_ping_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    vTaskDelay(10000 / portTICK_PERIOD_MS);

    while (1) {
        pnky_send_ping(GLOBAL_STATE);

        int interval = pnky_config_get_int(PNKY_KEY_PING_INTERVAL);
        if (interval < 10) interval = 60;
        vTaskDelay(interval * 1000 / portTICK_PERIOD_MS);
    }
}
