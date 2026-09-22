/**
 * @file cpu_monitor.h
 * @brief Portable ESP-IDF CPU load estimator based on FreeRTOS idle activity.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_FREERTOS_UNICORE
#define CPU_MONITOR_CORE_COUNT 1U
#else
#define CPU_MONITOR_CORE_COUNT SOC_CPU_CORES_NUM
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot of the current CPU load estimate. */
typedef struct {
    uint32_t core_count;
    float core_load[CPU_MONITOR_CORE_COUNT];
    float core_load_raw[CPU_MONITOR_CORE_COUNT];
    uint32_t idle_count[CPU_MONITOR_CORE_COUNT];
    uint32_t idle_max[CPU_MONITOR_CORE_COUNT];
    float total_load;
    bool ready;
} cpu_monitor_info_t;

/**
 * @brief Start CPU load monitoring. Safe to call more than once.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM when the task or an idle hook
 *         cannot be allocated, or an ESP-IDF idle-hook registration error.
 */
esp_err_t cpu_monitor_init(void);

/** Stop monitoring and release its task and FreeRTOS idle hooks. */
esp_err_t cpu_monitor_deinit(void);

/** @brief Whether the configured warmup sample count has completed. */
bool cpu_monitor_is_ready(void);

/** @brief Number of FreeRTOS cores monitored by this build. */
uint32_t cpu_monitor_get_core_count(void);

/** @brief Get smoothed CPU load for one core, in percent. */
float cpu_monitor_get_core_load(uint32_t core_id);

/** @brief Get the mean of the smoothed enabled-core loads, in percent. */
float cpu_monitor_get_total_load(void);

/** @brief Copy a consistent state snapshot into @p info. */
esp_err_t cpu_monitor_get_info(cpu_monitor_info_t *info);

/** @brief Clear learned idle baselines and restart the warmup phase. */
esp_err_t cpu_monitor_reset_calibration(void);

#ifdef __cplusplus
}
#endif
