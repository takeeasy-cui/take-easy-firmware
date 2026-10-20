/* SPDX-License-Identifier: Apache-2.0
 * 中文：共用手机式墨水屏键盘，一次只有当前页面使用，退出清除输入状态。
 * English: Shared smartphone-style e-paper keyboard; only the active page owns it, with state cleared on exit.
 * 2026-10-19 方案 A：输入核心替换为 YanReader IME 引擎（main/ime，经 ime_glue C 胶水）。
 * 拼音全拼/九宫格/词组/用户词典/联想预测全部来自 ime 分区码表（3.9MB）；
 * 键盘壳（绘制、局部 DU 刷新、按压反馈、长按连删、词栏灰阶整理）保持本固件原版调优。
 * 用户修订：普通输入只画变化区域，布局切换才重画键盘，不累计整页清屏次数。
 * 用户修订：删除键长按连续删除；抬手、移开、多点或输入会话结束立即取消，仅局部快刷。
 * 用户修订：候选高频变化后在无触摸停顿时仅整理词栏，不打断连打、不全屏黑闪。
 */
#include "ui_keyboard.h"
#include <ui_kit.h>
#include "ime_glue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static ui_text_edit_t *s_edit;
static char s_notice[80];
static bool s_ascii, s_chinese, s_nine, s_upper;
static bool s_engine_ok;              /* 引擎载入成功（ime 分区在位）；失败则回退英文直输。 */
static unsigned s_panel;
static bool s_pressed, s_probe, s_keys_only;
static int s_probe_x, s_probe_y;
static EpdRect s_pressed_area, s_feedback_area, s_key_clip;
enum { DIRTY_FIELD = 1, DIRTY_WORDS = 2, DIRTY_SYLLABLES = 4, DIRTY_SPACE = 8, DIRTY_LAYOUT = 16, DIRTY_FEEDBACK = 32, DIRTY_WORDS_SETTLE = 64 };
static unsigned s_dirty;
enum { WORDS_SETTLE_CHANGES = 6, WORDS_SETTLE_IDLE_MS = 700 };
static unsigned s_word_updates;
static int64_t s_words_idle_since = -1;
static int64_t s_last_key_ms = -1;    /* 用户词典延迟落盘的空闲起点 */
static bool s_delete_hold;
static EpdRect s_delete_area;
static int s_delete_top;
static int64_t s_delete_next_ms, s_delete_last_ms;

/* 引擎侧快照（主线程独占；键盘退出即弃）：当前页候选 + 组合显示串。 */
#define KB_CAND_MAX 5
#define KB_CAND_TEXT 48
#define KB_COMP_MAX 337               /* 引擎 displayCode 上限（63 码 + 分隔符）余量 */
static char s_cand[KB_CAND_MAX][KB_CAND_TEXT];
static int s_cand_count;
static char s_comp[KB_COMP_MAX];
static int s_syllable_offset;

static uint32_t stamp(const char *text) {
    uint32_t value = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) value = (value ^ *p) * 16777619u;
    return value;
}
static bool commit(int idx);   /* 前向声明：保存前冲刷与确定键共用 */
static const char *const groups[] = {"", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
static const char *const symbols[] = {"，","。","？","！","：","；","“","”","（","）","、","…","—","·","-","_","/","@","#","&","+","=","%","\\","*","$","<",">","[","]"};
static const char *const ascii_symbols[] = {"!","@","#","$","%","^","&","*","(",")","-","_","+","=","/","\\",":",";","\"","'",".",",","?","<",">","[","]","{","}","|","`","~"};
static EpdRect rect(int x, int y, int w, int h, int top) { return (EpdRect){x, y + top, w, h}; }

/* 引擎状态拉取：组合串 + 当前页候选 + 状态消息 → 本地快照（含脏区判定用的 revision）。 */
static unsigned s_engine_revision;
static void engine_pull(void) {
    s_comp[0] = 0;
    s_cand_count = 0;
    if (s_engine_ok && s_chinese && !s_ascii) {
        (void)ime_glue_composition(s_comp, sizeof(s_comp));
        int n = ime_glue_candidate_count();
        if (n > KB_CAND_MAX) n = KB_CAND_MAX;
        for (int i = 0; i < n; ++i) {
            if (ime_glue_candidate(i, s_cand[i], sizeof(s_cand[i])) < 0) { n = i; break; }
        }
        s_cand_count = n;
        if (!ime_glue_ambig_active()) s_syllable_offset = 0;
    }
    const char *status = ime_glue_status();
    if (status[0]) snprintf(s_notice, sizeof(s_notice), "%s", status);
    ++s_engine_revision;
}
void ui_keyboard_end(void) {
    ui_keyboard_hold_cancel();
    s_pressed = s_probe = s_keys_only = false; s_feedback_area = (EpdRect){0};
    if (s_engine_ok) ime_glue_end();      /* 清组合 + 用户词典整表落盘 */
    s_engine_ok = false; s_edit = NULL;
    s_dirty = 0;
    s_word_updates = 0; s_words_idle_since = -1; s_last_key_ms = -1;
    s_cand_count = 0; s_comp[0] = 0; s_syllable_offset = 0;
    memset(s_notice, 0, sizeof(s_notice));
}
/* 保存前冲刷：组合未上屏时选首选上屏（用户 2026-10-19：点确定/物理键保存应带上未选字）。 */
void ui_keyboard_commit_pending(void) {
    if (s_engine_ok && s_chinese && s_comp[0]) commit(0);
}
void ui_keyboard_begin(ui_text_edit_t *edit, bool ascii_only) {
    ui_keyboard_end(); s_edit = edit; s_ascii = ascii_only;
    s_chinese = !ascii_only; s_nine = !ascii_only; s_upper = false; s_panel = 0;
    s_engine_ok = false;
    if (!ascii_only && edit) {
        if (ime_glue_begin()) {
            s_engine_ok = true;
            ime_glue_set_layout(s_nine ? 3 : 0);
            /* 正文上下文喂引擎：已出现过的词候选加分（编辑器打开时喂一次）。 */
            char before[129];
            size_t n = edit->cursor;
            if (n >= sizeof(before)) n = sizeof(before) - 1;
            memcpy(before, edit->text, n); before[n] = 0;
            ime_glue_doc_context(before);
        } else {
            s_chinese = s_nine = false;
            snprintf(s_notice, sizeof(s_notice), "输入引擎加载失败，暂用英文键盘");
        }
    }
    engine_pull();
}
bool ui_keyboard_pending(void) { return s_comp[0] != 0; }
static bool insert(const char *text) {
    if (s_edit && ui_text_edit_insert(s_edit, text)) { s_notice[0] = 0; return true; }
    snprintf(s_notice, sizeof(s_notice), "文字已达到长度上限"); return false;
}
/* 上屏 + 联想：提交文本落进编辑缓冲后开预测（引擎吃上下文尾部出下一批候选）。 */
static void commit_insert(const char *text) {
    if (!insert(text)) return;
    if (s_engine_ok) ime_glue_predict_begin(text);
}
/* 选当前页第 idx 个候选（0 基）；引擎负责用户词典学习与续接。 */
static bool commit(int idx) {
    if (!s_engine_ok || !s_chinese || idx < 0 || idx >= s_cand_count) return false;
    char out[96];
    if (!ime_glue_select(idx, out, sizeof(out))) return false;
    if (out[0]) commit_insert(out);
    engine_pull();
    return true;
}
static void backspace(void) {
    if (s_engine_ok && s_chinese && (s_comp[0] || ime_glue_predicting())) {
        char out[96];
        if (ime_glue_backspace(out, sizeof(out))) {
            if (out[0]) insert(out);      /* 歧义续接残段：退格收尾上屏 */
            engine_pull();
            s_notice[0] = 0;
            return;
        }
    }
    if (s_edit) ui_text_edit_backspace(s_edit);
    engine_pull();
    s_notice[0] = 0;
}
static void letter_key(char letter) {
    if (!s_engine_ok || !s_chinese) { char text[2] = {s_upper ? letter - 32 : letter, 0}; insert(text); return; }
    char out[96];
    if (ime_glue_key(letter, out, sizeof(out)) && out[0]) insert(out);
    engine_pull();
}
static bool same_rect(EpdRect a, EpdRect b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }
static bool intersects(EpdRect a, EpdRect b) { return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height; }
static void key_paint(uint8_t *fb, EpdRect r, const char *text, int px, bool dark, bool utility) {
    if (s_probe) { if (ui_rect_hit(r, s_probe_x, s_probe_y)) s_key_clip = r; return; }
    if (s_keys_only && !intersects(r, s_key_clip)) return;
    bool pressed = s_pressed && same_rect(r, s_pressed_area);
    ui_fill_round_rect(fb, r, 12, dark ? (pressed ? 0x50 : UI_GRAY_BLACK) : pressed ? 0xb0 : utility ? 0xd8 : UI_GRAY_WHITE);
    ui_draw_round_rect(fb, r, 12, pressed ? 0x20 : 0x60);
    if (!strcmp(text, "⌫")) {
        int cx = r.x + r.width / 2, cy = r.y + r.height / 2 + (pressed ? 2 : 0);
        for (int d = 0; d < 2; ++d) {
            epd_draw_line(cx - 17, cy, cx - 7, cy - 11 + d, UI_GRAY_BLACK, fb);
            epd_draw_line(cx - 17, cy, cx - 7, cy + 11 - d, UI_GRAY_BLACK, fb);
            epd_draw_line(cx - 7, cy - 11 + d, cx + 17, cy - 11 + d, UI_GRAY_BLACK, fb);
            epd_draw_line(cx - 7, cy + 11 - d, cx + 17, cy + 11 - d, UI_GRAY_BLACK, fb);
            epd_draw_line(cx + 17 - d, cy - 11, cx + 17 - d, cy + 11, UI_GRAY_BLACK, fb);
            epd_draw_line(cx - 2, cy - 5 + d, cx + 8, cy + 5 + d, UI_GRAY_BLACK, fb);
            epd_draw_line(cx - 2, cy + 5 + d, cx + 8, cy - 5 + d, UI_GRAY_BLACK, fb);
        }
    } else ui_text_fixed_vc(fb, r.x + r.width / 2, r.y + r.height / 2 + (pressed ? 2 : 0), px, text, EPD_DRAW_ALIGN_CENTER, dark);
}
/* 候选行：当前页（引擎每页 5）按文字宽度排格，右缘 606 截断。 */
static size_t visible(EpdRect boxes[KB_CAND_MAX], int top) {
    if (!s_cand_count) return 0;
    int left = 78, limit = 606; size_t count = 0;
    while ((int)count < s_cand_count && count < KB_CAND_MAX) {
        int width = ui_text_fixed_width_px(25, s_cand[count]) + 24;
        if (width < 78) width = 78;
        if (width > limit - 78) width = limit - 78;
        if (left + width > limit) break;
        boxes[count] = rect(left, 104, width - 6, 56, top);
        left += width; ++count;
    }
    return count;
}
bool ui_keyboard_page(int direction) {
    if (!s_engine_ok || !s_chinese) return false;
    bool moved = direction > 0 ? ime_glue_page_next() : ime_glue_page_prev();
    if (!moved) return false;
    engine_pull();
    s_dirty |= DIRTY_WORDS;
    return true;
}
/* 九宫格左列 = 引擎"下一个音节"表（ambigSyllables），3 行 + 更多翻页。 */
static size_t syllables(const char *out[4], bool *more) {
    static char bufs[3][16];
    if (more) *more = false;
    if (!s_engine_ok || !s_chinese || !s_nine || !ime_glue_ambig_active()) return 0;
    int total = 0;
    char probe[16];
    while (ime_glue_ambig_syllable(total, probe, sizeof(probe)) >= 0) ++total;
    if (total <= s_syllable_offset) return 0;
    size_t count = 0;
    for (int i = s_syllable_offset; i < total && count < 3; ++i) {
        ime_glue_ambig_syllable(i, bufs[count], sizeof(bufs[0]));
        out[count] = bufs[count]; ++count;
    }
    if (more && s_syllable_offset + 3 < total) *more = true;
    return count;
}
static void paint_words(uint8_t *fb, int top) {
    if (!s_probe && !s_keys_only && s_notice[0]) ui_text_fixed_vc(fb, 28, top + 75, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    else if (!s_probe && !s_keys_only && s_comp[0])
        ui_text_input_composition(fb, s_comp, rect(28, 57, 552, 35, top), 25);
    else if (!s_probe && !s_keys_only) ui_text_fixed_vc(fb, 28, top + 75, 22, s_chinese ? "连续输入拼音，选择词语" : "轻点输入，可切换大小写", EPD_DRAW_ALIGN_LEFT, false);
    EpdRect boxes[KB_CAND_MAX]; size_t count = visible(boxes, top);
    for (size_t i = 0; i < count; ++i) {
        char text[KB_CAND_TEXT]; snprintf(text, sizeof(text), "%.47s", s_cand[i]);
        while (strlen(text) && ui_text_fixed_width_px(25, text) > boxes[i].width - 12) {
            size_t n = strlen(text) - 1; while (n && ((unsigned char)text[n] & 192) == 128) --n; text[n] = 0;
        }
        key_paint(fb, boxes[i], text, 25, false, i == 0);
    }
    key_paint(fb, rect(24, 104, 44, 56, top), "‹", 30, false, true);
    key_paint(fb, rect(616, 104, 44, 56, top), "›", 30, false, true);
}
static void paint_syllables(uint8_t *fb, int top) {
    const char *choices[4]; bool more; size_t n = syllables(choices, &more);
    for (int i = 0; i < 4; ++i) key_paint(fb, rect(24, 180 + i * 58, 82, 50, top),
        i < (int)n ? choices[i] : i == 3 && (more || s_syllable_offset) ? "更多" : "", 22, false, true);
}
void ui_keyboard_draw(uint8_t *fb, int top) {
    if (!s_probe && !s_keys_only) {
    ui_fill_round_rect(fb, rect(16, -12, 652, 534, top), 26, 0xe8);
    ui_draw_round_rect(fb, rect(16, -12, 652, 534, top), 26, 0x60);
    }
    key_paint(fb, rect(24, 0, 144, 44, top), "九宫格", 24, s_nine, s_ascii || !s_engine_ok);
    key_paint(fb, rect(176, 0, 144, 44, top), "全键盘", 24, !s_nine, false);
    if (!s_probe && !s_keys_only) ui_text_fixed_vc(fb, 651, top + 22, 24, s_ascii ? "英文密码" : s_chinese ? "中文拼音" : s_upper ? "英文大写" : "英文小写", EPD_DRAW_ALIGN_RIGHT, false);
    paint_words(fb, top);
    if (s_panel || !s_nine) {
        static const char *const rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        for (int row = 0; row < 3; ++row) {
            int len = s_panel ? 10 : (int)strlen(rows[row]);
            int left = s_panel || !row ? 24 : row == 1 ? 55 : 117;
            for (int col = 0; col < len; ++col) {
                char letter[2] = {0}; const char *label;
                if (s_panel == 1) { letter[0] = row == 0 ? "1234567890"[col] : ascii_symbols[(row - 1) * 10 + col][0]; label = letter; }
                else if (s_panel >= 2) label = (s_chinese && !s_ascii && s_panel != 3 ? symbols : ascii_symbols)[s_panel == 3 && row == 0 && col < 6 ? (const int[]){30,31,28,29,26,27}[col] : row * 10 + col];
                else { letter[0] = s_upper ? rows[row][col] - 32 : rows[row][col]; label = letter; }
                key_paint(fb, rect(left + col * 64, 180 + row * 78, 56, 68, top), label, 28, false, false);
            }
        }
        if (s_panel) {
            key_paint(fb, rect(536, 336, 56, 68, top), "重输", 21, false, true);
            key_paint(fb, rect(600, 336, 56, 68, top), "⌫", 28, false, true);
        } else {
            key_paint(fb, rect(24, 336, 80, 68, top), s_upper ? "A" : "a", 30, s_upper, true);
            key_paint(fb, rect(578, 336, 82, 68, top), "⌫", 31, false, true);
        }
    } else {
        paint_syllables(fb, top);
        for (int i = 0; i < 9; ++i) {
            char upper[6]; snprintf(upper, sizeof(upper), "%s", groups[i]);
            for (char *c = upper; *c; ++c) *c -= 32;
            EpdRect r = rect(116 + (i % 3) * 150, 180 + (i / 3) * 78, 140, 68, top);
            key_paint(fb, r, i == 0 ? "选词" : upper, 30, false, false);
        }
        key_paint(fb, rect(568, 180, 92, 68, top), "⌫", 32, false, true);
        key_paint(fb, rect(568, 258, 92, 68, top), "重输", 25, false, true);
        key_paint(fb, rect(568, 336, 92, 68, top), "清空", 25, false, true);
    }
    key_paint(fb, rect(24, 430, 86, 72, top), s_panel ? "ABC" : "123", 25, false, true);
    key_paint(fb, rect(118, 430, 80, 72, top), s_panel == 2 ? "更多" : "符号", 24, false, true);
    key_paint(fb, rect(206, 430, 220, 72, top), s_comp[0] ? "选择" : "空格", 26, false, false);
    key_paint(fb, rect(434, 430, 92, 72, top), s_nine ? "中文" : s_chinese ? "中 / En" : "En / 中", 21, false, true);
    key_paint(fb, rect(534, 430, 126, 72, top), "确定", 28, true, false);
}
static ui_keyboard_result_t tap(int x, int y, int top) {
    if (!s_edit || y < top || y >= top + 510 || x < 24 || x >= 660) return UI_KEYBOARD_NONE;
    if (ui_rect_hit(rect(24, 0, 144, 44, top), x, y) || ui_rect_hit(rect(176, 0, 144, 44, top), x, y)) {
        bool nine = x < 168;
        if ((nine && (s_ascii || !s_engine_ok)) || (nine == s_nine && !s_panel)) return UI_KEYBOARD_NONE;
        /* 引擎按布局解释键序列：切换即弃当前组合（组号含义随布局变），不做码型转换。 */
        if (s_engine_ok && ime_glue_composing() && nine != s_nine) ime_glue_reset();
        s_nine = nine; s_panel = 0;
        if (nine) { s_chinese = true; s_upper = false; }
        if (s_engine_ok) ime_glue_set_layout(nine ? 3 : 0);
        engine_pull(); return UI_KEYBOARD_CHANGED;
    }
    if (y >= top + 104 && y < top + 160) {
        if (x < 68) return ui_keyboard_page(-1) ? UI_KEYBOARD_CHANGED : UI_KEYBOARD_NONE;
        if (x >= 616) return ui_keyboard_page(1) ? UI_KEYBOARD_CHANGED : UI_KEYBOARD_NONE;
        EpdRect boxes[KB_CAND_MAX]; size_t count = visible(boxes, top);
        for (size_t i = 0; i < count; ++i) if (ui_rect_hit(boxes[i], x, y)) { commit((int)i); return UI_KEYBOARD_CHANGED; }
        return UI_KEYBOARD_NONE;
    }
    if (y >= top + 430 && y < top + 502) {
        if (x >= 534) {
            /* v6（用户 2026-10-19“输入信息不会保存”）：组合未上屏时确定=先选首选上屏再保存。 */
            if (!s_comp[0]) return UI_KEYBOARD_DONE;
            if (commit(0)) return UI_KEYBOARD_DONE;
            snprintf(s_notice, sizeof(s_notice), "请先选择词语，再保存");
        } else if (x >= 434) {
            if (s_nine) return UI_KEYBOARD_NONE;
            if (s_comp[0]) snprintf(s_notice, sizeof(s_notice), "请先选词，再切换语言");
            else if (!s_ascii && s_engine_ok) { s_chinese = !s_chinese; engine_pull(); }
            else snprintf(s_notice, sizeof(s_notice), "密码使用英文、数字与符号");
        } else if (x >= 206) {
            /* 空格 = 组合中选首选（经典拼音习惯）；联想态选首联想；否则输入空格。 */
            if (s_comp[0] || ime_glue_predicting()) { if (!commit(0)) insert(" "); }
            else insert(" ");
        } else if (s_comp[0]) snprintf(s_notice, sizeof(s_notice), "请先选词，再输入数字符号");
        else if (x < 110) s_panel = s_panel ? 0 : 1;
        else s_panel = s_panel == 2 ? 3 : 2;
        return UI_KEYBOARD_CHANGED;
    }
    if (s_panel || !s_nine) {
        static const char *const rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
        for (int row = 0; row < 3; ++row) {
            if (s_panel && row == 2 && ui_rect_hit(rect(600, 336, 56, 68, top), x, y)) { backspace(); return UI_KEYBOARD_CHANGED; }
            if (s_panel && row == 2 && ui_rect_hit(rect(536, 336, 56, 68, top), x, y)) {
                if (s_engine_ok) ime_glue_reset();
                s_edit->text[0] = 0; ui_text_edit_place(s_edit, 0); engine_pull(); return UI_KEYBOARD_CHANGED;
            }
            if (!s_panel && row == 2 && ui_rect_hit(rect(24, 336, 80, 68, top), x, y)) { s_upper = !s_upper; return UI_KEYBOARD_CHANGED; }
            if (!s_panel && row == 2 && ui_rect_hit(rect(578, 336, 82, 68, top), x, y)) { backspace(); return UI_KEYBOARD_CHANGED; }
            int left = s_panel || !row ? 24 : row == 1 ? 55 : 117;
            int len = s_panel ? 10 : (int)strlen(rows[row]);
            for (int col = 0; col < len; ++col) if (ui_rect_hit(rect(left + col * 64, 180 + row * 78, 56, 68, top), x, y)) {
                if (!s_panel) letter_key(rows[row][col]);
                else if (s_panel == 1) { char value[2] = {row == 0 ? "1234567890"[col] : ascii_symbols[(row - 1) * 10 + col][0], 0}; insert(value); }
                else insert((s_chinese && !s_ascii && s_panel != 3 ? symbols : ascii_symbols)[s_panel == 3 && row == 0 && col < 6 ? (const int[]){30,31,28,29,26,27}[col] : row * 10 + col]);
                return UI_KEYBOARD_CHANGED;
            }
        }
    } else {
        const char *choices[4]; bool more; size_t n = syllables(choices, &more);
        if (ui_rect_hit(rect(24, 354, 82, 50, top), x, y) && (more || s_syllable_offset)) {
            s_syllable_offset = more ? s_syllable_offset + 3 : 0;
            s_dirty |= DIRTY_SYLLABLES; return UI_KEYBOARD_CHANGED;
        }
        for (size_t i = 0; i < n; ++i) if (ui_rect_hit(rect(24, 180 + (int)i * 58, 82, 50, top), x, y)) {
            /* 分音节选择：把该音节接到已选前缀并重算候选（引擎记偏好表）。 */
            if (ime_glue_ambig_select((int)(s_syllable_offset + i))) engine_pull();
            return UI_KEYBOARD_CHANGED;
        }
        if (ui_rect_hit(rect(568, 180, 92, 68, top), x, y)) { backspace(); return UI_KEYBOARD_CHANGED; }
        if (ui_rect_hit(rect(568, 258, 92, 68, top), x, y)) {
            if (s_engine_ok) ime_glue_reset();
            s_notice[0] = 0; engine_pull(); return UI_KEYBOARD_CHANGED;
        }
        if (ui_rect_hit(rect(568, 336, 92, 68, top), x, y)) {
            if (s_engine_ok) ime_glue_reset();
            s_notice[0] = 0;
            s_edit->text[0] = 0; ui_text_edit_place(s_edit, 0); engine_pull(); return UI_KEYBOARD_CHANGED;
        }
        for (int i = 0; i < 9; ++i) if (ui_rect_hit(rect(116 + (i % 3) * 150, 180 + (i / 3) * 78, 140, 68, top), x, y)) {
            if (!i) {
                if (s_comp[0] || ime_glue_predicting()) { if (!commit(0)) snprintf(s_notice, sizeof(s_notice), "请先输入拼音并选择词语"); }
                return UI_KEYBOARD_CHANGED;
            }
            /* 九宫格组键（2..9 → 引擎组 0..7）：一键多字母歧义输入。 */
            char out[96];
            if (s_engine_ok && ime_glue_ambig(i - 1, out, sizeof(out)) && out[0]) insert(out);
            engine_pull();
            return UI_KEYBOARD_CHANGED;
        }
    }
    return UI_KEYBOARD_NONE;
}

static void feedback_dirty(EpdRect area) {
    s_feedback_area = s_feedback_area.width ? ui_rect_union(s_feedback_area, area) : area;
    s_dirty |= DIRTY_FEEDBACK;
}
bool ui_keyboard_release(void) {
    ui_keyboard_hold_cancel();
    if (!s_pressed) return false;
    feedback_dirty(s_pressed_area); s_pressed = false; return true;
}
bool ui_keyboard_press(int x, int y, int top, int64_t now_ms) {
    s_words_idle_since = -1;
    ui_keyboard_release();
    ui_keyboard_hold_start(x, y, top, now_ms);
    if (!s_edit || x < 0 || y < 0) return false;
    s_probe = true; s_probe_x = x; s_probe_y = y; s_key_clip = (EpdRect){0};
    ui_keyboard_draw(NULL, top); s_probe = false;
    if (!s_key_clip.width) return false;
    s_pressed_area = s_key_clip; s_pressed = true;
    feedback_dirty(s_pressed_area); return true;
}
void ui_keyboard_hold_cancel(void) { s_delete_hold = false; }
void ui_keyboard_hold_start(int x, int y, int top, int64_t now_ms) {
    ui_keyboard_hold_cancel();
    if (!s_edit || now_ms < 0) return;
    EpdRect area = s_panel ? rect(600, 336, 56, 68, top)
        : s_nine ? rect(568, 180, 92, 68, top) : rect(578, 336, 82, 68, top);
    if (!ui_rect_hit(area, x, y)) return;
    s_delete_area = area; s_delete_top = top;
    s_delete_hold = true; s_delete_last_ms = now_ms; s_delete_next_ms = now_ms + UI_LONG_PRESS_MS;
}
ui_keyboard_result_t ui_keyboard_hold_tick(bool held, int x, int y, int64_t now_ms) {
    if (!s_delete_hold) return UI_KEYBOARD_NONE;
    if (!s_edit || !held || now_ms < s_delete_last_ms || !ui_rect_hit(s_delete_area, x, y)) {
        ui_keyboard_hold_cancel(); return UI_KEYBOARD_NONE;
    }
    s_delete_last_ms = now_ms;
    if (now_ms < s_delete_next_ms) return UI_KEYBOARD_NONE;
    if (!s_comp[0] && !s_edit->cursor) { ui_keyboard_hold_cancel(); return UI_KEYBOARD_NONE; }
    // 推屏耗时不补发连删，保持可控；每次复用普通删除的脏区标记。
    // Never catch up after display delays; reuse ordinary backspace dirty-region tracking.
    s_delete_next_ms = now_ms + 120;
    ui_keyboard_result_t result = ui_keyboard_tap(s_delete_area.x + s_delete_area.width / 2,
        s_delete_area.y + s_delete_area.height / 2, s_delete_top, now_ms);
    if (result == UI_KEYBOARD_NONE) ui_keyboard_hold_cancel();
    return result;
}

ui_keyboard_result_t ui_keyboard_idle_tick(bool touching, int64_t now_ms) {
    /* 用户词典延迟整表落盘：引擎内部 400ms 空闲闸门 + 2s 延迟，打字期间不写卡。 */
    if (s_engine_ok && s_edit && now_ms >= 0)
        ime_glue_tick_idle(touching || s_last_key_ms < 0 || now_ms <= s_last_key_ms
                           ? 0 : (long)(now_ms - s_last_key_ms) * 1000);
    // 连打、按住或多指时不整理；一次停顿只推一次，不追补累计的候选变化。
    // Skip typing, held keys and multitouch; settle once per pause without replaying accumulated changes.
    if (!s_edit || s_dirty || touching || s_pressed || s_delete_hold ||
        now_ms < 0 || s_word_updates < WORDS_SETTLE_CHANGES) {
        s_words_idle_since = -1; return UI_KEYBOARD_NONE;
    }
    if (s_words_idle_since < 0 || now_ms < s_words_idle_since) {
        s_words_idle_since = now_ms; return UI_KEYBOARD_NONE;
    }
    if (now_ms - s_words_idle_since < WORDS_SETTLE_IDLE_MS) return UI_KEYBOARD_NONE;
    s_dirty |= DIRTY_WORDS_SETTLE;
    return UI_KEYBOARD_CHANGED;
}

ui_keyboard_result_t ui_keyboard_tap(int x, int y, int top, int64_t now_ms) {
    if (!s_edit) return UI_KEYBOARD_NONE;
    s_last_key_ms = now_ms;
    uint32_t text = stamp(s_edit->text), comp = stamp(s_comp), notice = stamp(s_notice);
    uint32_t cand = 0;
    for (int i = 0; i < s_cand_count; ++i) cand = cand * 31u + stamp(s_cand[i]);
    size_t cursor = s_edit->cursor;
    bool nine = s_nine, chinese = s_chinese, upper = s_upper, pending = s_comp[0] != 0;
    unsigned panel = s_panel;
    ui_keyboard_result_t result = tap(x, y, top);
    if (result != UI_KEYBOARD_CHANGED) return result == UI_KEYBOARD_NONE && s_dirty ? UI_KEYBOARD_CHANGED : result;
    if (text != stamp(s_edit->text) || cursor != s_edit->cursor) s_dirty |= DIRTY_FIELD;
    if (nine != s_nine || chinese != s_chinese || upper != s_upper || panel != s_panel) s_dirty |= DIRTY_LAYOUT;
    else {
        uint32_t cand2 = 0;
        for (int i = 0; i < s_cand_count; ++i) cand2 = cand2 * 31u + stamp(s_cand[i]);
        if (comp != stamp(s_comp) || cand != cand2 || notice != stamp(s_notice)) s_dirty |= DIRTY_WORDS;
        if (s_nine && s_chinese && comp != stamp(s_comp)) s_dirty |= DIRTY_SYLLABLES;
        if (pending != (s_comp[0] != 0)) s_dirty |= DIRTY_SPACE;
    }
    return s_dirty ? result : UI_KEYBOARD_NONE;
}
static void include(ui_keyboard_update_t *update, EpdRect area) {
    update->area = update->area.width ? ui_rect_union(update->area, area) : area;
}
ui_keyboard_update_t ui_keyboard_update(uint8_t *fb, int top, EpdRect field, int px,
                                        bool masked, const char *suffix, bool force_field) {
    ui_keyboard_update_t update = {.layout = (s_dirty & DIRTY_LAYOUT) != 0,
        .settle = s_dirty == DIRTY_WORDS_SETTLE && !force_field};
    if (!s_edit) return update;
    if (force_field || (s_dirty & DIRTY_FIELD)) {
        ui_clear_rect_fast(fb, field);
        ui_text_input_draw(fb, s_edit, field, px, masked, suffix);
        include(&update, field); update.field = true;
    }
    if (update.layout) {
        ui_keyboard_draw(fb, top);
        include(&update, rect(16, -12, 652, 534, top));
        s_word_updates = 0; s_words_idle_since = -1;
    } else {
        if (s_dirty & (DIRTY_WORDS | DIRTY_WORDS_SETTLE)) {
            EpdRect area = rect(24, 52, 636, 114, top);
            epd_fill_rect(area, 0xe8, fb); paint_words(fb, top); include(&update, area);
            if (update.settle) s_word_updates = 0;
            else if ((s_dirty & DIRTY_WORDS) && s_word_updates < WORDS_SETTLE_CHANGES) ++s_word_updates;
            s_words_idle_since = -1;
        }
        if (s_dirty & DIRTY_SYLLABLES) {
            EpdRect area = rect(24, 180, 82, 224, top);
            epd_fill_rect(area, 0xe8, fb); paint_syllables(fb, top); include(&update, area);
        }
        if (s_dirty & DIRTY_SPACE) {
            EpdRect area = rect(206, 430, 220, 72, top);
            key_paint(fb, area, s_comp[0] ? "选择" : "空格", 26, false, false); include(&update, area);
        }
    }
    if (!update.layout && (s_dirty & DIRTY_FEEDBACK)) {
        s_keys_only = true; s_key_clip = s_feedback_area;
        ui_keyboard_draw(fb, top); s_keys_only = false;
        include(&update, s_feedback_area);
    }
    s_feedback_area = (EpdRect){0}; s_dirty = 0;
    return update;
}
