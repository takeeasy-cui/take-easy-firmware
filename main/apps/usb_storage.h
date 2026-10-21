/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：在 TF 卡和电脑之间独占切换 USB 磁盘访问。
 * English: Switch exclusive SD access between firmware and USB host.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t usb_storage_start(void);
esp_err_t usb_storage_stop(void);
bool usb_storage_active(void);
bool usb_storage_connected(void);
/// 上次 stop 失败是否发生在 TF 卡重新挂载阶段（供读卡页区分提示文案）。/ True when the last stop failed at local remount.
bool usb_storage_stop_mount_failed(void);

/// 开机回收串口/JTAG 所需的共享内部 PHY。
/// / Reclaim the shared internal PHY for Serial/JTAG at boot.
void usb_storage_phy_init(void);
