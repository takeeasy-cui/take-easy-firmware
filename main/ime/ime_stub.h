/* SPDX-License-Identifier: Apache-2.0
 * take-easy 移植桩（2026-10-19）：YanReader IME 引擎原依赖 pjournal 的
 * settings_manager/g_settings 全局配置与 longop/input_tick 探针；本固件
 * 尚未接入设置页，先以默认值桩替，接口保持原样，后续设置页再补真实绑定。
 */
#pragma once
#include <string>

struct ime_settings_stub_t {
    bool imeDebug() const { return false; }
    bool imeSentence() const { return true; }
    bool imeDocContext() const { return false; }
    bool imeCandidateHighlight() const { return false; }
    bool imeMetaLetterSel() const { return true; }
    std::string imePredictMode() const { return ""; }
    std::string imeFuzzy() const { return ""; }
};
extern ime_settings_stub_t g_settings;

/* 欠载探针与触摸补采在 pjournal 里接长活写循环；本固件用户词典落盘路径
 * 由调用方（键盘层）节流，这里置空。 */
#define longop_begin(path) ((void)(path))
#define longop_end()       ((void)0)
#define input_tick_throttled() ((void)0)

/* 实体键盘 Win 单击 = 候选字母选字（pjournal KEY_META_TAP）。 */
#define KEY_META_TAP 0xB3