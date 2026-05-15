#include "pnky_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "pnky_config";

static nvs_handle_t pnky_handle;

static const char *pnky_key_names[PNKY_KEY_COUNT] = {
    "solana_wallet",
    "api_key",
    "challenge_nonce",
    "device_id",
    "server_url",
    "ping_interval"
};

static const char *pnky_defaults[PNKY_KEY_COUNT] = {
    "",
    "",
    "",
    "",
    "https://norugcoin.punkyshungry.com",
    "60"
};

void pnky_config_init(void)
{
    esp_err_t err = nvs_open(PNKY_NVS_NAMESPACE, NVS_READWRITE, &pnky_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", PNKY_NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < PNKY_KEY_COUNT; i++) {
        size_t len = 0;
        err = nvs_get_str(pnky_handle, pnky_key_names[i], NULL, &len);
        if (err != ESP_OK || len <= 1) {
            nvs_set_str(pnky_handle, pnky_key_names[i], pnky_defaults[i]);
        }
    }
    nvs_commit(pnky_handle);

    char *device_id = pnky_config_get_string(PNKY_KEY_DEVICE_ID);
    if (!device_id || strlen(device_id) < 8) {
        pnky_config_generate_device_id();
    }
    free(device_id);

    ESP_LOGI(TAG, "PNKY config initialized");
}

char *pnky_config_get_string(pnky_config_key_t key)
{
    if (key < 0 || key >= PNKY_KEY_COUNT) return NULL;

    size_t len = 0;
    esp_err_t err = nvs_get_str(pnky_handle, pnky_key_names[key], NULL, &len);
    if (err != ESP_OK || len == 0) {
        return strdup(pnky_defaults[key]);
    }

    char *buf = malloc(len);
    if (!buf) return NULL;

    err = nvs_get_str(pnky_handle, pnky_key_names[key], buf, &len);
    if (err != ESP_OK) {
        free(buf);
        return strdup(pnky_defaults[key]);
    }

    return buf;
}

void pnky_config_set_string(pnky_config_key_t key, const char *value)
{
    if (key < 0 || key >= PNKY_KEY_COUNT) return;
    if (!value) value = "";

    nvs_set_str(pnky_handle, pnky_key_names[key], value);
    nvs_commit(pnky_handle);
}

int pnky_config_get_int(pnky_config_key_t key)
{
    char *val = pnky_config_get_string(key);
    if (!val) return 0;
    int result = atoi(val);
    free(val);
    return result;
}

void pnky_config_set_int(pnky_config_key_t key, int value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    pnky_config_set_string(key, buf);
}

void pnky_config_generate_device_id(void)
{
    char id[PNKY_DEVICE_ID_LEN + 1] = {0};
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < PNKY_DEVICE_ID_LEN; i++) {
        id[i] = hex[rand() & 0x0F];
    }
    id[PNKY_DEVICE_ID_LEN] = '\0';

    nvs_set_str(pnky_handle, pnky_key_names[PNKY_KEY_DEVICE_ID], id);
    nvs_commit(pnky_handle);
    ESP_LOGI(TAG, "Generated device ID: %s", id);
}

const char *pnky_config_get_device_id(void)
{
    static char cached_id[PNKY_DEVICE_ID_LEN + 1] = {0};

    if (cached_id[0] != '\0') return cached_id;

    size_t len = sizeof(cached_id);
    esp_err_t err = nvs_get_str(pnky_handle, pnky_key_names[PNKY_KEY_DEVICE_ID], cached_id, &len);
    if (err != ESP_OK || strlen(cached_id) < 8) {
        pnky_config_generate_device_id();
        len = sizeof(cached_id);
        nvs_get_str(pnky_handle, pnky_key_names[PNKY_KEY_DEVICE_ID], cached_id, &len);
    }

    return cached_id;
}
