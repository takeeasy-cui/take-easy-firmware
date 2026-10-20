/* SPDX-License-Identifier: Apache-2.0
 * 中文：YanReader IME 引擎（C++ 单例）的 C 胶水层，供 ui_keyboard.c（C 文件）调用。
 * English: C glue for the C++ IME singleton so the C keyboard module can drive it.
 * 移植说明（2026-10-19 方案 A）：键盘壳（绘制/局部刷/按压反馈）仍是本固件原版，
 * 输入核心换成引擎：拼音/两分/词组/用户词典/预测全部来自 ime 分区码表。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 引擎初始化（幂等）：挂载 ime 分区、载入拼音表、激活。失败返回 false。
bool ime_glue_begin(void);
/// 会话结束：清组合 + 用户词典整表落盘（退出键盘时调）。
void ime_glue_end(void);
/// 只清当前组合（不落盘），键盘内部换语言/换布局用。
void ime_glue_reset(void);

/// 布局：0=26 全拼 1=14 2=18 3=九宫格（IME::AmbigLayout）。
void ime_glue_set_layout(int layout);
/// 英文模式（词库预测）开关。
void ime_glue_set_english(bool on);
bool ime_glue_english(void);

/// 通用按键（字母/数字选字/退格'\b'等，语义同 IME::handleKey）。
/// 返回 true=引擎吃掉；out 非空=有文字上屏（调用方负责插入编辑缓冲）。
bool ime_glue_key(int key, char *out, size_t cap);
/// 歧义键（九宫格组号 0..7）。
bool ime_glue_ambig(int group, char *out, size_t cap);
/// 选当前页第 idx 个候选（0 基）。
bool ime_glue_select(int idx, char *out, size_t cap);
/// 组合退格；out 非空=歧义续接残段上屏。
bool ime_glue_backspace(char *out, size_t cap);
/// 候选翻页；返回是否翻了。
bool ime_glue_page_next(void);
bool ime_glue_page_prev(void);
/// 上屏后开联想（喂刚上屏文本）。
void ime_glue_predict_begin(const char *text);
/// 喂正文尾部给引擎（已出现词加分）。
void ime_glue_doc_context(const char *text);
/// 主循环空闲调用：用户词典延迟落盘（idle_us=距最后按键微秒）。
void ime_glue_tick_idle(long idle_us);

/// 当前是否有未完成组合（含歧义键/联想/英文组合）。
bool ime_glue_composing(void);
bool ime_glue_predicting(void);
/// 组合显示串（拼音按音节分好，如 "fen'xi"）。
size_t ime_glue_composition(char *buf, size_t cap);
/// 当前页候选：返回数量；逐条取（idx 越界返回 -1）。
int ime_glue_candidate_count(void);
int ime_glue_candidate(int idx, char *buf, size_t cap);
/// 页码（1 基）与总页数。
int ime_glue_page_number(void);
int ime_glue_page_total(void);

/// 九宫格分音节选择（左列）：返回可选音节数量；选中第 idx 个。
int ime_glue_ambig_syllable(int idx, char *buf, size_t cap);
bool ime_glue_ambig_select(int idx);
bool ime_glue_ambig_active(void);
/// 引擎状态消息（一次性取出，空串=无）。
const char *ime_glue_status(void);

#ifdef __cplusplus
}
#endif
