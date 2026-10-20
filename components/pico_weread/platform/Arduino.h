/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 协议所需的最小时间与随机数适配，不引入 Arduino 运行时。
 * Minimal protocol clock/random adapter without an Arduino runtime.
 * 冻结：不访问显示或硬件设置。/ Frozen: no display or hardware settings access.
 */
#pragma once
#include <string>
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
using String = std::string;
inline unsigned long millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
inline void delay(unsigned long ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
inline long random(long low, long high) { return high <= low ? low : low + esp_random() % (high - low); }
struct PicoHeapInfo {
    size_t getFreeHeap() const { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
    size_t getMaxAllocHeap() const { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
};
inline PicoHeapInfo ESP;
