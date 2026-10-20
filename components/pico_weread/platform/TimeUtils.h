/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 将协议时间读取映射到已校准的系统时钟。
 * Map protocol time reads to the synchronized system clock.
 * 冻结：不写云端阅读进度。/ Frozen: no cloud reading-progress writes.
 */
#pragma once
#include <ctime>
#include <cstdint>
namespace TimeUtils {
inline bool isClockValid(uint32_t t) { return t >= 1700000000U && t < 4102444800U; }
inline uint32_t getCurrentValidTimestamp() {
    const time_t now = time(nullptr);
    return now >= 0 && isClockValid(static_cast<uint32_t>(now)) ? static_cast<uint32_t>(now) : 0;
}
inline bool isClockValid() { return getCurrentValidTimestamp() != 0; }
}
