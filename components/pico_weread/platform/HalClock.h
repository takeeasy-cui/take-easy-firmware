/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 复用 Pico 的联网校时。/ Reuse Pico's online clock synchronization.
 * 冻结：只在后台调用。/ Frozen: background task only.
 */
#pragma once
extern "C" {
#include "read_pico_transfer.h"
}
enum class ClockSyncState { Failed, Complete };
struct PicoClock {
    bool requestSync() { uint32_t epoch; return read_pico_transfer_sync_time_online(&epoch) == ESP_OK; }
    ClockSyncState syncState() const { return ClockSyncState::Failed; }
};
inline PicoClock halClock;
