/* SPDX-License-Identifier: Apache-2.0
 * 中文：计算器共享内核：四则状态机与全点阵绘制；应用页与计算器锁屏复用。
 * English: Shared calculator core: four-operation state machine and dot-matrix painting,
 * reused by the app page and the calculator lock.
 * 冻结（用户 2026-10-19）：锁屏模式下"＝"恰为四位数字才尝试验证 lock_pin；
 * 其余一切按键（含验证失败的＝）永远走正常计算器语义。
 * Frozen: in lock mode only an exactly-four-digit equals attempts lock_pin verification;
 * every other key (and a failed equals) always keeps normal calculator semantics.
 * 绘制全走 nothing 点阵（0-9 + - = . % × ÷ ± ⌫ C），无字体依赖：
 * 应用页与锁屏态（ttf 未就绪/被锁字体替换）都能画，显示区可安全走 DU 局部刷新。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "epdiy.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 锁屏模式：＝ 恰四位数字时验证 lock_pin 并置解锁标志。/ Lock mode: verify on a four-digit equals and set the unlock flag.
void calc_core_set_unlock_mode(bool on);
/// 上次"＝"验证成功（仅锁屏模式）。/ True after a successful equals verification (lock mode only).
bool calc_core_unlocked(void);
/// 重置为初始状态（含解锁标志与解锁模式复位）。/ Reset to the initial state, including the unlock flag.
void calc_core_reset(void);
/// 当前输入串（只读，验证方使用）。/ Current entry string (read-only).
const char* calc_core_current(void);
/// 按一个键：数字/运算/C/⌫/±/./%/＝。/ Press one key.
void calc_core_press(const char* key);
/// 命中返回键名（静态串），未命中 NULL。/ Key name at a point, or NULL.
const char* calc_core_key_at(uint16_t x, uint16_t y);
/// 显示区（含边框外扩 2px，DU 局部刷新用）。/ Display area incl. a 2px frame margin for partial DU refresh.
EpdRect calc_core_display_area(void);
/// 键区+显示框（进页/整页时一次）。/ Keypad and display frame (once per full paint).
void calc_core_paint_static(uint8_t* fb);
/// 显示区内容（每次按键后）。/ Display content (after each press).
void calc_core_paint_display(uint8_t* fb);
/// 整页：清屏+键区+显示。/ Full page: clear, keypad and display.
void calc_core_paint_page(uint8_t* fb);

#ifdef __cplusplus
}
#endif
