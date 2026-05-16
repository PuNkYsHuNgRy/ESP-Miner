#include "pnky_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"

static const char *TAG = "pnky_config";

static nvs_handle_t pnky_handle;

static const char *pnky_key_names[PNKY_KEY_COUNT] = {
    "wallet",
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
    "http://norugcoin.punkyshungry.com",
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

void pnky_config_erase_key(pnky_config_key_t key)
{
    if (key < 0 || key >= PNKY_KEY_COUNT) return;
    nvs_erase_key(pnky_handle, pnky_key_names[key]);
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

static char s_device_id[PNKY_DEVICE_ID_LEN + 1] = {0};

const char *pnky_config_get_device_id(void)
{
    if (s_device_id[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_device_id, sizeof(s_device_id),
                 "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return s_device_id;
}

void pnky_config_generate_device_id(void)
{
    s_device_id[0] = '\0';
}
