# CPU Monitor

`cpu_monitor` is a self-contained ESP-IDF component that estimates active CPU
load from the number of times each FreeRTOS idle task can run. It automatically
uses Core 0 only on single-core and `CONFIG_FREERTOS_UNICORE` builds, and uses
both active cores otherwise.

```c
ESP_ERROR_CHECK(cpu_monitor_init());

if (cpu_monitor_is_ready()) {
    float total = cpu_monitor_get_total_load();
}
```

The sampling task runs once per second by default. Each core learns its own
maximum idle count, then reports both raw and EMA-smoothed load. Total load is
the average of the active-core smoothed loads. `cpu_monitor_get_info()` returns
a consistent full snapshot, including the current and learned idle counts.

The component owns only its idle hooks and its sampling task;
`cpu_monitor_deinit()` removes both. It does not initialize networking, storage,
CAN, power management, or HTTP services.

The estimate is most meaningful on fixed-frequency systems. Dynamic frequency
scaling, tickless idle, and automatic light sleep can change the relationship
between idle-hook activity and actual hardware busy time.
