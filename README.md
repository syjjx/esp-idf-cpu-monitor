# ESP-IDF CPU Monitor

一个独立、低开销的 ESP-IDF CPU 负载监控组件。它通过统计 FreeRTOS
Idle Task 的运行机会，估算每个活动 CPU Core 的负载，并提供总负载、原始
负载和 EMA 平滑后的负载。

组件不依赖 Wi-Fi、BLE、ESP-NOW、NVS、TWAI/CAN 或 HTTP Server，不改变
宿主项目的任务调度策略；移除组件及其引用后不会留下 Hook、任务或其他全局
副作用。

## 特性

- 自动适配 ESP-IDF 支持的单核和双核目标。
- 双核芯片启用 `CONFIG_FREERTOS_UNICORE` 时只监控 Core 0。
- 每个 Core 独立学习 Idle 基线，避免写死芯片频率或计数阈值。
- 默认每秒采样一次，前 5 次采样为校准阶段。
- 同时提供 raw load 与 EMA 平滑 load，默认 EMA 系数为 30%。
- 线程安全的状态快照 API。
- 幂等初始化和完整 `deinit`：会注销 Idle Hook 并删除采样任务。
- 提供单核/双核通用的 basic 示例。

## 目录结构

```text
cpu_monitor/
├── CMakeLists.txt
├── Kconfig
├── include/cpu_monitor.h
├── src/cpu_monitor.c
└── examples/basic/
```

## 接入项目

将整个 `cpu_monitor` 目录复制到项目的 `components/` 下：

```text
your_project/
├── main/
└── components/
    └── cpu_monitor/
```

在使用它的组件 CMakeLists.txt 中声明依赖：

```cmake
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES cpu_monitor)
```

然后在启动阶段初始化一次：

```c
#include "cpu_monitor.h"

void app_main(void)
{
    ESP_ERROR_CHECK(cpu_monitor_init());

    // 初始化其他业务模块……
}
```

## 读取负载

最简单的用法是读取总负载：

```c
if (cpu_monitor_is_ready()) {
    float total_percent = cpu_monitor_get_total_load();
}
```

读取每核负载时，不要假设 CPU1 一定存在：

```c
for (uint32_t core = 0; core < cpu_monitor_get_core_count(); ++core) {
    float load_percent = cpu_monitor_get_core_load(core);
}
```

需要一次性读取完整快照时：

```c
cpu_monitor_info_t info;
ESP_ERROR_CHECK(cpu_monitor_get_info(&info));

// info.ready
// info.core_count
// info.core_load[core]       // EMA 平滑负载，百分比
// info.core_load_raw[core]   // 当前窗口原始负载，百分比
// info.total_load            // 所有活动 Core 的平均平滑负载
// info.idle_count / idle_max // 当前统计计数与学习到的基线
```

初始化后，`cpu_monitor_is_ready()` 会在完成默认 5 次采样后返回 `true`。
在校准期内读取 API 是安全的，但结果不应视为稳定负载。可通过
`cpu_monitor_reset_calibration()` 重新开始学习基线；动态模块或测试程序可在
不再需要组件时调用 `cpu_monitor_deinit()`。

## Kconfig 配置

在 `idf.py menuconfig` 的 `CPU Monitor` 菜单中可配置：

- 是否启用组件；默认启用。
- 采样周期：250–5000 ms，默认 1000 ms。
- 校准采样次数：1–60，默认 5。
- EMA 平滑比例：1–100%，默认 30%。
- 采样任务栈大小和优先级。
- 可选的每次采样调试日志；生产固件建议关闭。

## Basic 示例

示例在 `examples/basic`，无需因目标芯片改变业务代码。它会每秒输出总负载与
所有可用 Core 的平滑/原始负载。

```sh
cd examples/basic
source /path/to/esp-idf/export.sh
idf.py set-target esp32c6   # 也可改为 esp32s3、esp32、esp32s2 等
idf.py build flash monitor
```

在双核目标上创建分别固定到 Core 0、Core 1 的 CPU-bound 测试任务，可观察到
对应 Core 的负载上升；单核目标只会输出 CPU0。

## 原理与限制

FreeRTOS Idle Task 越有机会运行，Idle Hook 的计数越高，因此估算负载为：

```text
load = 100 × (1 - current_idle_count / learned_max_idle_count)
```

这是“Idle Task 可用性”估算值，最适合 CPU 频率固定、持续运行的设备。启用
Dynamic Frequency Scaling、tickless idle 或自动 light sleep 时，Idle Hook 计数
与硬件实际 busy time 的关系会变化；此时结果应作为趋势监控，而不是严格的
硬件占用率。

## 资源占用

- Idle Hook：每次仅执行一次原子计数加一。
- 采样任务：默认 2048 bytes 栈、优先级 2、每秒运行一次。
- 不创建定时器、队列、网络连接或持久化数据。

## License

请在发布仓库前按你的项目许可策略补充 `LICENSE` 文件。
