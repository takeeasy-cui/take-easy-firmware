/* SPDX-License-Identifier: Apache-2.0
 * 中文：计算器锁屏会话入口（sleep.c 的 style 3 分支调用）。
 * English: Calculator lock session entry, called by sleep.c for lock style 3.
 * 返回 true = 密码验证成功（调用方走解锁尾部）；false = 空闲或电源键，回浅睡。
 * True means verified (caller runs the unlock tail); false means idle or power key.
 */
#pragma once
#include <stdbool.h>
#include "epdiy.h"
#include "sleep.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lock_calc_session(EpdiyHighlevelState* hl, cst836u_handle_t touch);

#ifdef __cplusplus
}
#endif
