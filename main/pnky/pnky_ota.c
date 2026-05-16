#include "pnky_ota.h"
#include "pnky_config.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "pnky_ota";

#define PNKY_OTA_CHECK_INTERVAL_US (3600ULL * 1000000ULL)

static void pnky_ota_check(GlobalState *GLOBAL_STATE)
{
    ESP_LOGI(TAG, "OTA check starting...");

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGW(TAG, "No OTA partition available");
        return;
    }

    char *server_url = pnky_config_get_string(PNKY_KEY_SERVER_URL);
    if (!server_url) return;

    if (strncmp(server_url, "https://", 8) == 0) {
        char *http_url = malloc(strlen(server_url));
        if (http_url) {
            snprintf(http_url, strlen(server_url), "http://%s", server_url + 8);
            free(server_url);
            server_url = http_url;
        }
    }

    // Fetch latest version (platform-specific endpoint)
    char ver_url[256];
    snprintf(ver_url, sizeof(ver_url), "%s/api/v1/firmware-version/bitaxe-esp32s3", server_url);

    esp_http_client_config_t ver_config = {
        .url = ver_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t ver_client = esp_http_client_init(&ver_config);
    if (!ver_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for version check");
        free(server_url);
        return;
    }

    esp_err_t err = esp_http_client_open(ver_client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Version check HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(ver_client);
        free(server_url);
        return;
    }

    int content_length = esp_http_client_fetch_headers(ver_client);
    int status = esp_http_client_get_status_code(ver_client);
    if (status != 200) {
        ESP_LOGW(TAG, "Version check returned %d", status);
        esp_http_client_close(ver_client);
        esp_http_client_cleanup(ver_client);
        free(server_url);
        return;
    }

    char latest_ver[64] = {0};
    int ver_len = esp_http_client_read(ver_client, latest_ver, sizeof(latest_ver) - 1);
    esp_http_client_close(ver_client);
    esp_http_client_cleanup(ver_client);
    if (ver_len > 0) latest_ver[ver_len] = '\0';
    // Strip quotes and whitespace
    for (char *p = latest_ver; *p; p++) {
        if (*p == '\"' || *p == '\r' || *p == '\n') {
            memmove(p, p + 1, strlen(p));
            p--;
        }
    }

    if (strlen(latest_ver) == 0) {
        ESP_LOGW(TAG, "Empty version from server");
        free(server_url);
        return;
    }

    const char *current_ver = GLOBAL_STATE->SYSTEM_MODULE.version;
    if (!current_ver) current_ver = "0.0.0";

    // Don't compare if same string
    if (strcmp(current_ver, latest_ver) == 0) {
        ESP_LOGI(TAG, "Up to date (current: %s)", current_ver);
        free(server_url);
        return;
    }

    // Parse versions (strip non-numeric prefix)
    int cur_major = 0, cur_minor = 0, cur_patch = 0;
    int lat_major = 0, lat_minor = 0, lat_patch = 0;
    sscanf(current_ver, "%*[^0-9]%d.%d.%d", &cur_major, &cur_minor, &cur_patch);
    sscanf(latest_ver, "%*[^0-9]%d.%d.%d", &lat_major, &lat_minor, &lat_patch);

    if (lat_major < cur_major ||
        (lat_major == cur_major && lat_minor < cur_minor) ||
        (lat_major == cur_major && lat_minor == cur_minor && lat_patch <= cur_patch)) {
        ESP_LOGI(TAG, "Up to date (current: %s, server: %s)", current_ver, latest_ver);
        free(server_url);
        return;
    }

    ESP_LOGI(TAG, "Update available: %s (current: %s)", latest_ver, current_ver);

    // Pause mining
    GLOBAL_STATE->SYSTEM_MODULE.mining_paused = true;
    ESP_LOGI(TAG, "Pausing mining for OTA...");
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Download firmware
    char fw_url[512];
    snprintf(fw_url, sizeof(fw_url), "%s/firmware/bitaxe-esp32s3/firmware.bin", server_url);
    free(server_url);

    ESP_LOGI(TAG, "Downloading firmware: %s", fw_url);

    esp_http_client_config_t dl_config = {
        .url = fw_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 120000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t dl_client = esp_http_client_init(&dl_config);
    if (!dl_client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for download");
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    esp_err_t dl_err = esp_http_client_open(dl_client, 0);
    if (dl_err != ESP_OK) {
        ESP_LOGE(TAG, "Download HTTP open failed: %s", esp_err_to_name(dl_err));
        esp_http_client_cleanup(dl_client);
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    int content_len = esp_http_client_fetch_headers(dl_client);
    int dl_status = esp_http_client_get_status_code(dl_client);
    if (dl_status != 200) {
        ESP_LOGE(TAG, "Download returned %d", dl_status);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    if (content_len <= 0) {
        ESP_LOGE(TAG, "Invalid firmware size: %d", content_len);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    ESP_LOGI(TAG, "Firmware size: %d bytes (partition: %s @ 0x%x)",
             content_len, update->label, update->address);

    esp_ota_handle_t ota_handle;
    esp_err_t ota_err = esp_ota_begin(update, content_len, &ota_handle);
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ota_err));
        esp_http_client_cleanup(dl_client);
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    char buf[1024];
    int total_read = 0;
    int read_len;
    while ((read_len = esp_http_client_read(dl_client, buf, sizeof(buf))) > 0) {
        ota_err = esp_ota_write(ota_handle, buf, read_len);
        if (ota_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ota_err));
            esp_ota_abort(ota_handle);
esp_http_client_close(dl_client);
    esp_http_client_cleanup(dl_client);
            GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
            return;
        }
        total_read += read_len;
    }

    esp_http_client_cleanup(dl_client);

    ota_err = esp_ota_end(ota_handle);
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ota_err));
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    if (total_read != content_len) {
        ESP_LOGE(TAG, "Download incomplete: %d/%d bytes", total_read, content_len);
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    ota_err = esp_ota_set_boot_partition(update);
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ota_err));
        GLOBAL_STATE->SYSTEM_MODULE.mining_paused = false;
        return;
    }

    ESP_LOGI(TAG, "OTA complete (%d bytes), boot partition: %s. Restarting...", total_read, update->label);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();
}

void pnky_ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "OTA initialized | Partition: %s @ 0x%x",
             running ? running->label : "N/A",
             running ? running->address : 0);
}

void pnky_ota_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    ESP_LOGI(TAG, "OTA task started, waiting 30s...");
    vTaskDelay(30000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "OTA first check...");

    while (1) {
        pnky_ota_check(GLOBAL_STATE);
        vTaskDelay(PNKY_OTA_CHECK_INTERVAL_US / portTICK_PERIOD_MS);
    }
}
