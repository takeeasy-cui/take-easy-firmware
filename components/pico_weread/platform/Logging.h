/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 保留错误与阶段日志，关闭可能含请求参数的调试日志。
 * Keep error/stage logs; suppress request-parameter debug logs.
 * 冻结：不得输出账号会话。/ Frozen: never log account sessions.
 */
#pragma once
#include "esp_log.h"
#define LOG_ERR(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define LOG_INF(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define LOG_DBG(tag, ...) do {} while (0)
#define LOG_LEVEL 1
