/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/// 空闲时启动有界后台对时；主循环独占 PMU 与 NVS 的交接。
/// Start bounded background sync while idle; the main loop alone hands time to PMU/NVS.
void app_time_service_poll(bool idle);

/// 启动运行期 RC 慢时钟周期重校准任务（2026-10-28：离线走时治漂，
/// 用主晶振纠 RC 漂移，不依赖网络/PMU；与 NTP 互补）。
/// Start the periodic RC slow-clock recalibration task (offline drift fix).
void app_clock_trim_start(void);
