#include "cpu_monitor.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"

static const char *TAG = "cpu_monitor";

typedef struct {
    volatile uint32_t idle_count[CPU_MONITOR_CORE_COUNT];
    uint32_t idle_max[CPU_MONITOR_CORE_COUNT];
    float load_raw[CPU_MONITOR_CORE_COUNT];
    float load_smooth[CPU_MONITOR_CORE_COUNT];
    float total_load;
    uint32_t sample_count;
    bool ready;
    bool hook_registered[CPU_MONITOR_CORE_COUNT];
    TaskHandle_t task_handle;
} cpu_monitor_ctx_t;

typedef enum {
    CPU_MONITOR_STOPPED,
    CPU_MONITOR_STARTING,
    CPU_MONITOR_RUNNING,
    CPU_MONITOR_STOPPING,
} cpu_monitor_lifecycle_t;

static cpu_monitor_ctx_t s_ctx;
static cpu_monitor_lifecycle_t s_lifecycle = CPU_MONITOR_STOPPED;
static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;

static float clamp_percent(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 100.0f) {
        return 100.0f;
    }
    return value;
}

/* Idle callbacks must remain lock-free and non-blocking. */
static bool cpu_monitor_idle_hook_core0(void)
{
    __atomic_fetch_add(&s_ctx.idle_count[0], 1U, __ATOMIC_RELAXED);
    return false;
}

#if CPU_MONITOR_CORE_COUNT > 1
static bool cpu_monitor_idle_hook_core1(void)
{
    __atomic_fetch_add(&s_ctx.idle_count[1], 1U, __ATOMIC_RELAXED);
    return false;
}
#endif

static esp_freertos_idle_cb_t cpu_monitor_hook_for_core(uint32_t core_id)
{
    if (core_id == 0U) {
        return cpu_monitor_idle_hook_core0;
    }
#if CPU_MONITOR_CORE_COUNT > 1
    if (core_id == 1U) {
        return cpu_monitor_idle_hook_core1;
    }
#endif
    return NULL;
}

static void cpu_monitor_clear_calibration_locked(void)
{
    for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
        __atomic_store_n(&s_ctx.idle_count[core], 0U, __ATOMIC_RELAXED);
        s_ctx.idle_max[core] = 0U;
        s_ctx.load_raw[core] = 0.0f;
        s_ctx.load_smooth[core] = 0.0f;
    }
    s_ctx.total_load = 0.0f;
    s_ctx.sample_count = 0U;
    s_ctx.ready = false;
}

static void cpu_monitor_sample(void)
{
    uint32_t idle_count[CPU_MONITOR_CORE_COUNT];
    float raw_load[CPU_MONITOR_CORE_COUNT];
    float smooth_load[CPU_MONITOR_CORE_COUNT];
    float total = 0.0f;

    for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
        idle_count[core] = __atomic_exchange_n(&s_ctx.idle_count[core], 0U,
                                                __ATOMIC_RELAXED);
    }

    portENTER_CRITICAL(&s_data_lock);
    for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
        if (idle_count[core] > s_ctx.idle_max[core]) {
            s_ctx.idle_max[core] = idle_count[core];
        }

        if (s_ctx.idle_max[core] == 0U) {
            raw_load[core] = 0.0f;
        } else {
            raw_load[core] = clamp_percent(
                100.0f * (1.0f - ((float)idle_count[core] /
                                   (float)s_ctx.idle_max[core])));
        }

        if (s_ctx.sample_count == 0U) {
            smooth_load[core] = raw_load[core];
        } else {
            const float alpha = (float)CONFIG_CPU_MONITOR_EMA_PERCENT / 100.0f;
            smooth_load[core] = alpha * raw_load[core] +
                                (1.0f - alpha) * s_ctx.load_smooth[core];
        }

        s_ctx.load_raw[core] = raw_load[core];
        s_ctx.load_smooth[core] = smooth_load[core];
        total += smooth_load[core];
    }

    s_ctx.total_load = total / (float)CPU_MONITOR_CORE_COUNT;
    ++s_ctx.sample_count;
    if (s_ctx.sample_count >= CONFIG_CPU_MONITOR_WARMUP_SAMPLES) {
        s_ctx.ready = true;
    }
    portEXIT_CRITICAL(&s_data_lock);

#if CONFIG_CPU_MONITOR_DEBUG
    for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
        ESP_LOGI(TAG, "core=%lu raw=%.1f smooth=%.1f idle=%lu max=%lu",
                 (unsigned long)core, (double)raw_load[core],
                 (double)smooth_load[core], (unsigned long)idle_count[core],
                 (unsigned long)s_ctx.idle_max[core]);
    }
    ESP_LOGI(TAG, "total=%.1f", (double)total / (double)CPU_MONITOR_CORE_COUNT);
#endif
}

static void cpu_monitor_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_CPU_MONITOR_SAMPLE_INTERVAL_MS);

    for (;;) {
        vTaskDelay(interval);
        cpu_monitor_sample();
    }
}

esp_err_t cpu_monitor_init(void)
{
#if !CONFIG_CPU_MONITOR_ENABLE
    return ESP_OK;
#else
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_lifecycle == CPU_MONITOR_RUNNING) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_OK;
    }
    if (s_lifecycle != CPU_MONITOR_STOPPED) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle = CPU_MONITOR_STARTING;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    portENTER_CRITICAL(&s_data_lock);
    memset(&s_ctx, 0, sizeof(s_ctx));
    portEXIT_CRITICAL(&s_data_lock);
    esp_err_t result = ESP_OK;
    for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
        esp_freertos_idle_cb_t hook = cpu_monitor_hook_for_core(core);
        result = esp_register_freertos_idle_hook_for_cpu(hook, core);
        if (result != ESP_OK) {
            break;
        }
        s_ctx.hook_registered[core] = true;
    }

    if (result == ESP_OK) {
        const BaseType_t created = xTaskCreate(cpu_monitor_task, "cpu_monitor",
                                                CONFIG_CPU_MONITOR_TASK_STACK_SIZE,
                                                NULL, CONFIG_CPU_MONITOR_TASK_PRIORITY,
                                                &s_ctx.task_handle);
        result = created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    }

    if (result != ESP_OK) {
        for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
            if (s_ctx.hook_registered[core]) {
                esp_deregister_freertos_idle_hook_for_cpu(
                    cpu_monitor_hook_for_core(core), core);
            }
        }
        portENTER_CRITICAL(&s_data_lock);
        memset(&s_ctx, 0, sizeof(s_ctx));
        portEXIT_CRITICAL(&s_data_lock);
        portENTER_CRITICAL(&s_lifecycle_lock);
        s_lifecycle = CPU_MONITOR_STOPPED;
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return result;
    }

    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle = CPU_MONITOR_RUNNING;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    ESP_LOGI(TAG, "started for %u FreeRTOS core(s), %d ms sample interval",
             (unsigned)CPU_MONITOR_CORE_COUNT, CONFIG_CPU_MONITOR_SAMPLE_INTERVAL_MS);
    return ESP_OK;
#endif
}

esp_err_t cpu_monitor_deinit(void)
{
#if !CONFIG_CPU_MONITOR_ENABLE
    return ESP_OK;
#else
    portENTER_CRITICAL(&s_lifecycle_lock);
    if (s_lifecycle == CPU_MONITOR_STOPPED) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_OK;
    }
    if (s_lifecycle != CPU_MONITOR_RUNNING) {
        portEXIT_CRITICAL(&s_lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t task_handle = s_ctx.task_handle;
    s_lifecycle = CPU_MONITOR_STOPPING;
    portEXIT_CRITICAL(&s_lifecycle_lock);

    if (task_handle != NULL) {
        vTaskDelete(task_handle);
    }
    for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
        if (s_ctx.hook_registered[core]) {
            esp_deregister_freertos_idle_hook_for_cpu(cpu_monitor_hook_for_core(core), core);
        }
    }
    portENTER_CRITICAL(&s_data_lock);
    memset(&s_ctx, 0, sizeof(s_ctx));
    portEXIT_CRITICAL(&s_data_lock);
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_lifecycle = CPU_MONITOR_STOPPED;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return ESP_OK;
#endif
}

bool cpu_monitor_is_ready(void)
{
#if !CONFIG_CPU_MONITOR_ENABLE
    return false;
#else
    portENTER_CRITICAL(&s_data_lock);
    const bool ready = s_ctx.ready;
    portEXIT_CRITICAL(&s_data_lock);
    return ready;
#endif
}

uint32_t cpu_monitor_get_core_count(void)
{
    return CPU_MONITOR_CORE_COUNT;
}

float cpu_monitor_get_core_load(uint32_t core_id)
{
    if (core_id >= CPU_MONITOR_CORE_COUNT) {
        return 0.0f;
    }
#if !CONFIG_CPU_MONITOR_ENABLE
    return 0.0f;
#else
    portENTER_CRITICAL(&s_data_lock);
    const float load = s_ctx.load_smooth[core_id];
    portEXIT_CRITICAL(&s_data_lock);
    return load;
#endif
}

float cpu_monitor_get_total_load(void)
{
#if !CONFIG_CPU_MONITOR_ENABLE
    return 0.0f;
#else
    portENTER_CRITICAL(&s_data_lock);
    const float load = s_ctx.total_load;
    portEXIT_CRITICAL(&s_data_lock);
    return load;
#endif
}

esp_err_t cpu_monitor_get_info(cpu_monitor_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    info->core_count = CPU_MONITOR_CORE_COUNT;
#if CONFIG_CPU_MONITOR_ENABLE
    portENTER_CRITICAL(&s_data_lock);
    for (uint32_t core = 0; core < CPU_MONITOR_CORE_COUNT; ++core) {
        info->core_load[core] = s_ctx.load_smooth[core];
        info->core_load_raw[core] = s_ctx.load_raw[core];
        info->idle_count[core] = __atomic_load_n(&s_ctx.idle_count[core],
                                                  __ATOMIC_RELAXED);
        info->idle_max[core] = s_ctx.idle_max[core];
    }
    info->total_load = s_ctx.total_load;
    info->ready = s_ctx.ready;
    portEXIT_CRITICAL(&s_data_lock);
#endif
    return ESP_OK;
}

esp_err_t cpu_monitor_reset_calibration(void)
{
#if !CONFIG_CPU_MONITOR_ENABLE
    return ESP_OK;
#else
    portENTER_CRITICAL(&s_data_lock);
    cpu_monitor_clear_calibration_locked();
    portEXIT_CRITICAL(&s_data_lock);
    return ESP_OK;
#endif
}
