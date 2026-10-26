/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Pico 高电量读数稳定器；不改 PMU 曲线、保护阈值或充电参数。
 * English: Stabilize Pico upper-SOC readings without changing PMU curves, safety limits or charging.
 * 移植自上游 wegooo-cell/read-pico-reader rc98（c543e2b，2026-10-25 吸收；许可证条款随文件保留）。 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint16_t samples[5], shown;
    uint32_t sampled_ms, changed_ms, budget_ms;
    uint8_t count, next;
    bool valid, powered;
} pico_soc_filter_t;
/// 输入有效的原始千分比；插拔后短暂观察，高段按时间收敛，低压/低电量立即采用原值。
/// Feed valid raw permille; briefly observe cable transitions, converge upper SOC by time and accept low readings immediately.
uint16_t pico_soc_filter_update(pico_soc_filter_t *state, uint16_t raw, uint16_t mv,
                               bool powered, bool full, bool low, uint32_t now_ms);
