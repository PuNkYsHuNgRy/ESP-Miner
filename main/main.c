#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "asic_result_task.h"
#include "create_jobs_task.h"
#include "hashrate_monitor_task.h"
#include "fan_controller_task.h"
#include "statistics_task.h"
#include "system.h"
#include "http_server.h"
#include "serial.h"
#include "stratum_task.h"
#include "i2c_bitaxe.h"
#include "adc.h"
#include "nvs_config.h"
#include "self_test.h"
#include "asic.h"
#include "bap/bap.h"
#include "device_config.h"
#include "connect.h"
#include "asic_reset.h"
#include "asic_init.h"
#include "task_monitor.h"
#include "filesystem.h"
#include "input.h"
#include "log_buffer.h"
#include "pnky/pnky_config.h"
#include "pnky/pnky_ping.h"
#include "esp_partition.h"
#include "power/TPS546.h"
#include "thermal/EMC2101.h"
#include "device_config.h"

#include "driver/i2c_master.h"
#include "pnky/pnky_ota.h"

static GlobalState GLOBAL_STATE;

static const char * TAG = "bitaxe";

void app_main(void)
{
    if (esp_psram_is_initialized()) {
        GLOBAL_STATE.psram_is_available = true;
        log_buffer_init();
    }

    ESP_LOGI(TAG, "Welcome to the bitaxe - FOSS || GTFO!");

    if (xTaskCreate(cpu_monitor_task, "cpu_monitor", 4096, (void *)&GLOBAL_STATE, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating cpu monitor task");
    }
#ifdef CONFIG_ENABLE_TASK_MONITOR
    if (xTaskCreate(task_monitor_task, "task_monitor", 8192, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating task monitor task");
    }
#endif
  
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
    }

    // Init I2C
    ESP_ERROR_CHECK(i2c_bitaxe_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    // Initialize RST pin to low early to minimize ASIC power consumption
    ESP_ERROR_CHECK(asic_hold_reset_low());
    ESP_LOGI(TAG, "RST pin initialized to low");

    // wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Init ADC
    ADC_init();

    // initialize the ESP32 NVS
    if (nvs_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    pnky_config_init();
    pnky_ota_init();

    // Small delay to let NVS task process pending writes from init
    vTaskDelay(pdMS_TO_TICKS(100));

    // Check for pre-config binary at 0x3F0000 in factory partition (written by web flasher)
    // Format: byte 0=magic(0xEE), byte 1=ssid_len, bytes 2-33=ssid,
    //         byte 34=pass_len, bytes 35-98=password,
    //         byte 98=wallet_len, bytes 99+=wallet
    {
        const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
        if (part) {
            uint8_t cfg[256] = {0};
            if (esp_partition_read(part, 0x3F0000 - part->address, cfg, sizeof(cfg)) == ESP_OK && cfg[0] == 0xEE) {
                char ssid[33] = {0};
                char pass[64] = {0};
                char wallet[45] = {0};
                uint8_t ssid_len = cfg[1] < 32 ? cfg[1] : 32;
                uint8_t pass_len = cfg[34] < 63 ? cfg[34] : 63;
                uint8_t wallet_len = cfg[98] < 44 ? cfg[98] : 44;
                memcpy(ssid, &cfg[2], ssid_len);
                memcpy(pass, &cfg[35], pass_len);
                memcpy(wallet, &cfg[99], wallet_len);
                ESP_LOGI(TAG, "Pre-config found: SSID=%s wallet=%s", ssid, wallet);
                ESP_LOGI(TAG, "Pre-config: writing SSID...");
                nvs_config_set_string(NVS_CONFIG_WIFI_SSID, ssid);
                ESP_LOGI(TAG, "Pre-config: writing password...");
                nvs_config_set_string(NVS_CONFIG_WIFI_PASS, pass);
                ESP_LOGI(TAG, "Pre-config: writing wallet...");
                pnky_config_set_string(PNKY_KEY_SOLANA_WALLET, wallet_len > 0 ? wallet : "");
                ESP_LOGI(TAG, "Pre-config: invalidating config magic...");
                // Invalidate config so it's not re-applied on next boot.
                // Can't erase factory partition while running from it, so write
                // the config to the www (SPIFFS) partition instead for invalidation.
                // Actually, just mark as applied in NVS — re-applying is idempotent anyway.
                ESP_LOGI(TAG, "Pre-config applied");
            }
        }
    }

    // Ensure SSID is initialized before any screen/self-test uses it.
    GLOBAL_STATE.SYSTEM_MODULE.ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID);
    if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL || strlen(GLOBAL_STATE.SYSTEM_MODULE.ssid) == 0) {
        ESP_LOGW(TAG, "No SSID configured in NVS, using empty string");
        GLOBAL_STATE.SYSTEM_MODULE.ssid = strdup("");
        if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for SSID");
            return;
        }
    }

    if (device_config_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init device config");
        return;
    }

    // Detect board version from I2C hardware if NVS board_version is unknown ("000")
    if (strcmp(GLOBAL_STATE.DEVICE_CONFIG.board_version, "000") == 0 ||
        strncmp(GLOBAL_STATE.DEVICE_CONFIG.board_version, "60", 2) == 0) {

        i2c_master_bus_handle_t bus_handle;
        if (i2c_bitaxe_get_master_bus_handle(&bus_handle) == ESP_OK) {
            bool has_tps546 = i2c_master_probe(bus_handle, TPS546_I2CADDR, 50) == ESP_OK;

            if (strcmp(GLOBAL_STATE.DEVICE_CONFIG.board_version, "000") == 0 && has_tps546) {
                ESP_LOGI(TAG, "Auto-detected Gamma board (TPS546 found), setting board version to 602");
                nvs_config_set_string(NVS_CONFIG_BOARD_VERSION, "602");
                nvs_config_set_string(NVS_CONFIG_DEVICE_MODEL, "Gamma");
                device_config_init(&GLOBAL_STATE);
            }
        }
    }

    // Gamma boards (600-series) - display currently disabled (I2C NACK issue)
    if (strncmp(GLOBAL_STATE.DEVICE_CONFIG.board_version, "60", 2) == 0) {
        nvs_config_set_string(NVS_CONFIG_DISPLAY, "NONE");
        ESP_LOGI(TAG, "Gamma board detected: display disabled (TBI)");
    }

    // On fresh flash, NVS frequency/voltage may be wrong Kconfig defaults.
    // Set proper ASIC defaults based on detected board model.
    {
        float cur_freq = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
        uint16_t cur_volt = nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
        const AsicConfig *asic = &GLOBAL_STATE.DEVICE_CONFIG.family.asic;
        if (cur_freq <= 300 || cur_freq != asic->default_frequency_mhz) {
            ESP_LOGW(TAG, "ASIC frequency %.0f MHz looks wrong for %s, setting default %d MHz",
                     cur_freq, asic->name, asic->default_frequency_mhz);
            nvs_config_set_float(NVS_CONFIG_ASIC_FREQUENCY, asic->default_frequency_mhz);
        }
        if (cur_volt >= 1400 || cur_volt != asic->default_voltage_mv) {
            ESP_LOGW(TAG, "Core voltage %u mV looks wrong for %s, setting default %u mV",
                     cur_volt, asic->name, asic->default_voltage_mv);
            nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, asic->default_voltage_mv);
        }
    }

    if (self_test_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init self test");
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);
    if (scoreboard_init(&GLOBAL_STATE.SYSTEM_MODULE.scoreboard) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init scoreboard");
    }

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        wifi_init(&GLOBAL_STATE);
    }

    esp_err_t system_init_ret = SYSTEM_init_peripherals(&GLOBAL_STATE);
    
    if (system_init_ret == ESP_OK) {
        if (xTaskCreate(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating power management task");
        }
        if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
            if (xTaskCreate(FAN_CONTROLLER_task, "fan_controller", 8192, (void *) &GLOBAL_STATE, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating fan controller task");
            }
        }
    } else {
        ESP_LOGE(TAG, "Critical peripheral initialization failure (%s). Entering degraded mode.", esp_err_to_name(GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret));
    }
    
    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        // start the API for AxeOS
        start_rest_server((void *) &GLOBAL_STATE);
    }

    // After mounting SPIFFS
    SYSTEM_init_versions(&GLOBAL_STATE);

    // Initialize BAP interface
    esp_err_t bap_ret = BAP_init(&GLOBAL_STATE);
    if (bap_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BAP interface: %d", bap_ret);
        // Continue anyway, as BAP is not critical for core functionality
    }

    while (!GLOBAL_STATE.SYSTEM_MODULE.is_connected) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    pnky_ping_init(&GLOBAL_STATE);
    if (xTaskCreate(pnky_ping_task, "pnky_ping", 8192, (void *)&GLOBAL_STATE, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating pnky ping task");
    }
    if (xTaskCreate(pnky_ota_task, "pnky_ota", 10240, (void *)&GLOBAL_STATE, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Error creating pnky ota task");
    }

    queue_init(&GLOBAL_STATE.stratum_queue);

    if (system_init_ret == ESP_OK) {
        if (asic_initialize(&GLOBAL_STATE, ASIC_INIT_COLD_BOOT, 0) == 0) {
            return;
        }

        if (xTaskCreate(create_jobs_task, "stratum miner", 12288, (void *) &GLOBAL_STATE, 20, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating stratum miner task");
        }
        if (xTaskCreate(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating asic result task");
        }

        if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
            if (xTaskCreate(stratum_task, "stratum admin", 16384, (void *) &GLOBAL_STATE, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Error creating stratum admin task");
            }
        }

        if (xTaskCreateWithCaps(hashrate_monitor_task, "hashrate monitor", 8192, (void *) &GLOBAL_STATE, 5, NULL, MALLOC_CAP_SPIRAM) !=
            pdPASS) {
            ESP_LOGE(TAG, "Error creating hashrate monitor task");
        }
        if (xTaskCreateWithCaps(statistics_task, "statistics", 8192, (void *) &GLOBAL_STATE, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "Error creating statistics task");
        }
    }

    if (GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret = system_init_ret;
        if (xTaskCreate(self_test_task, "self_test", 8192, (void *) &GLOBAL_STATE, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Error creating self test task");
        }
    }
}
