#include <lwip/tcpip.h>

#include "system.h"
#include "work_queue.h"
#include "serial.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_config.h"
#include "utils.h"
#include "stratum_task.h"
#include "hashrate_monitor_task.h"
#include "asic.h"
#include "freertos/task.h"
#include "scoreboard.h"
#include "pnky/pnky_ws_client.h"
#include "pnky/pnky_config.h"
#include "stratum_api.h"

static const char *TAG = "asic_result";

void ASIC_result_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    while (1)
    {
        // Check if ASIC is initialized before trying to process work
        if (!GLOBAL_STATE->ASIC_initalized) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
        
        //task_result *asic_result = (*GLOBAL_STATE->ASIC_functions.receive_result_fn)(GLOBAL_STATE);
        task_result *asic_result = ASIC_process_work(GLOBAL_STATE);

        if (asic_result == NULL)
        {
            continue;
        }

        if (asic_result->register_type != REGISTER_INVALID) {
            hashrate_monitor_register_read(GLOBAL_STATE, asic_result->register_type, asic_result->asic_nr, asic_result->value, asic_result->timestamp_us);
            continue;
        }

        uint8_t job_id = asic_result->job_id;

        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
        bool valid = (GLOBAL_STATE->valid_jobs[job_id] != 0);
        bm_job *active_job = valid ? GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id] : NULL;
        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

        if (!valid || active_job == NULL)
        {
            ESP_LOGW(TAG, "Invalid job nonce found, 0x%02X", job_id);
            continue;
        }
        // check the nonce difficulty
        double nonce_diff = test_nonce_value(active_job, asic_result->nonce, asic_result->rolled_version);

        if (GLOBAL_STATE->SELF_TEST_MODULE.is_active) continue;

        uint32_t version_bits = asic_result->rolled_version ^ active_job->version;
        if (nonce_diff >= active_job->pool_diff)
        {
            taskENTER_CRITICAL(&GLOBAL_STATE->stratum_mux);
            esp_transport_handle_t transport = GLOBAL_STATE->transport;
            pnky_ws_ctx_t *ws = GLOBAL_STATE->ws_ctx;
            int uid = GLOBAL_STATE->send_uid++;
            const char *user = GLOBAL_STATE->SYSTEM_MODULE.pool_user ? GLOBAL_STATE->SYSTEM_MODULE.pool_user : PNKY_BTC_WALLET;
            taskEXIT_CRITICAL(&GLOBAL_STATE->stratum_mux);

            if (ws && pnky_ws_is_connected(ws)) {
                const char *device_id = pnky_config_get_device_id();
                char ws_user[128];
                snprintf(ws_user, sizeof(ws_user), "%s.%s", PNKY_BTC_WALLET, device_id);
                char submit_msg[512];
                int n = snprintf(submit_msg, sizeof(submit_msg),
                    "{\"id\":%d,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%08lx\",\"%08lx\",\"%08lx\"]}\n",
                    uid, ws_user,
                    active_job->jobid ? active_job->jobid : "",
                    active_job->extranonce2 ? active_job->extranonce2 : "",
                    (unsigned long)active_job->ntime,
                    (unsigned long)asic_result->nonce,
                    (unsigned long)version_bits);

                uint64_t sent_time_us = esp_timer_get_time();
                STRATUM_V1_stamp_tx(uid, sent_time_us);
                bool ok = pnky_ws_send(ws, submit_msg, n);
                if (!ok) {
                    ESP_LOGW(TAG, "Unable to write share to WS socket");
                }

                float process_time = (sent_time_us - asic_result->timestamp_us) / 1000.0f;
                GLOBAL_STATE->SYSTEM_MODULE.process_time = process_time;
                ESP_LOGI(TAG, "Processing time: %0.1f ms", process_time);

            } else if (transport) {
                uint64_t sent_time_us = 0;
                int ret = STRATUM_V1_submit_share(
                    transport,
                    uid,
                    user,
                    active_job->jobid,
                    active_job->extranonce2,
                    active_job->ntime,
                    asic_result->nonce,
                    version_bits,
                    &sent_time_us);

                if (ret < 0) {
                    ESP_LOGW(TAG, "Unable to write share to socket (ret: %d, errno %d: %s)", ret, errno, strerror(errno));
                }

                float process_time = (sent_time_us - asic_result->timestamp_us) / 1000.0f;
                GLOBAL_STATE->SYSTEM_MODULE.process_time = process_time;
                ESP_LOGI(TAG, "Processing time: %0.1f ms", process_time);
            } else {
                ESP_LOGW(TAG, "No stratum connection, dropping share (job 0x%02X)", job_id);
            }
        }

        //log the ASIC response
        ESP_LOGI(TAG, "ID: %s, ASIC nr: %d, Core: %d/%d, ver: %08" PRIX32 " Nonce %08" PRIX32 " diff %.1f of %g.", active_job->jobid, asic_result->asic_nr, asic_result->core_id, asic_result->small_core_id, asic_result->rolled_version, asic_result->nonce, nonce_diff, active_job->pool_diff);

        SYSTEM_notify_found_nonce(GLOBAL_STATE, nonce_diff, job_id);

        scoreboard_add(&GLOBAL_STATE->SYSTEM_MODULE.scoreboard, nonce_diff, active_job->jobid, active_job->extranonce2, active_job->ntime, asic_result->nonce, version_bits);
    }
}
