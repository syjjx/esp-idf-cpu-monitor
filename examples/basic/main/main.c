#include <inttypes.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cpu_monitor.h"

static const char *TAG = "cpu_monitor_example";

void app_main(void)
{
    ESP_ERROR_CHECK(cpu_monitor_init());

    for (;;) {
        cpu_monitor_info_t info;
        ESP_ERROR_CHECK(cpu_monitor_get_info(&info));

        if (!info.ready) {
            ESP_LOGI(TAG, "calibrating (%" PRIu32 " core%s)", info.core_count,
                     info.core_count == 1U ? "" : "s");
        } else {
            for (uint32_t core = 0; core < info.core_count; ++core) {
                ESP_LOGI(TAG, "CPU%" PRIu32 ": %.1f%% (raw %.1f%%)", core,
                         (double)info.core_load[core],
                         (double)info.core_load_raw[core]);
            }
            ESP_LOGI(TAG, "TOTAL: %.1f%%", (double)info.total_load);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
