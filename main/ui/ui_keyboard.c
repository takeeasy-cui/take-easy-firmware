/* SPDX-License-Identifier: Apache-2.0
 * 中文：共用墨水屏键盘，一次只有当前页面使用，退出清除输入状态。
 * English: Shared e-paper keyboard; only the active page owns it, state cleared on exit.
 *
 * 21g（用户拍板「输入法直接移植，不再保留咱们的 UI」）：键盘壳整体换成 YanReader
 * editor_vk 版式（源 main/editor_vk.cpp，仓库 autumnc/YanReader-pico），逐段移植：
 *   - 三面板：字母页 / 符号面板（10×3 标点表）/ 数字面板（四行数字盘，左右侧列），
 *     功能行最左两键绕圈切换（字母↔符↔123）——数字在任何模式下都可达（根治
 *     「Key 数字输入不可达」）。
 *   - 四键位布局：26 全拼 / 14 / 18 / 9 九宫格（一键多字母组取自引擎 IME::ambig*
 *     表，万象拼音分组），布局键循环切换并持久化（NVS kb_lay）。
 *   - 候选带两行：上行编码（点它开关 T9 候选宫格），下行候选（左右 ‹› 翻页）。
 *   - 九宫格：左列标点/分音节双身份 + 中间 3×3（1 分词 + 2~9 组）+ 右列 ⌫/重输/0。
 *   - 额外行：[布局] [Ctl] [◀▲▼▶] [Shift] [⌫]；Ctl 组合键宿主暂未接入（占位）。
 *   - 命中判定为「圆角内切圆」（YanReader evkInKey：四角死区防误触）。
 * 输入核心仍为第十八批移植的 YanReader IME 引擎（main/ime，经 ime_glue C 胶水）。
 * 保留本固件调优：局部 DU 差分刷新、按压反馈、长按连删、词栏灰阶整理。
 */
#include "ui_keyboard.h"
#include <ui_kit.h>
#include "ime_glue.h"
#include "settings.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static ui_text_edit_t *s_edit;
static char s_notice[80];
static bool s_ascii, s_chinese;        /* 会话：密码=纯英文；中文=引擎拼音 */
static bool s_engine_ok;               /* 引擎载入成功；失败回退英文直输 */
static int  s_layout;                  /* 0=26 1=14 2=18 3=九宫格（IME::AmbigLayout） */
static int  s_page;                    /* 0=字母 1=符号 2=数字（三面板） */
static bool s_shift, s_ctrl;           /* 待发修饰键 */
static bool s_t9;                      /* T9 候选宫格展开（点编码行开关） */
static bool s_pressed, s_probe, s_keys_only;
static int s_probe_x, s_probe_y;
static EpdRect s_pressed_area, s_feedback_area, s_key_clip;
enum { DIRTY_FIELD = 1, DIRTY_WORDS = 2, DIRTY_SYLLABLES = 4, DIRTY_SPACE = 8,
       DIRTY_LAYOUT = 16, DIRTY_FEEDBACK = 32, DIRTY_WORDS_SETTLE = 64 };
static unsigned s_dirty;
enum { WORDS_SETTLE_CHANGES = 6, WORDS_SETTLE_IDLE_MS = 700 };
static unsigned s_word_updates;
static int64_t s_words_idle_since = -1;
static int64_t s_last_key_ms = -1;     /* 用户词典延迟落盘的空闲起点 */
static bool s_delete_hold;
static EpdRect s_delete_area;
static int s_delete_top;
static int64_t s_delete_next_ms, s_delete_last_ms;

/* 引擎侧快照（主线程独占；键盘退出即弃）：当前页候选 + 组合显示串。 */
#define KB_CAND_MAX 9                    /* T9 宫格一页 9 个（引擎 GRID_PAGE_SIZE） */
#define KB_CAND_TEXT 48
#define KB_COMP_MAX 337                  /* 引擎 displayCode 上限（63 码 + 分隔符）余量 */
static char s_cand[KB_CAND_MAX][KB_CAND_TEXT];
static int s_cand_count;
static char s_comp[KB_COMP_MAX];
static int s_syllable_offset;

static uint32_t stamp(const char *text) {
    uint32_t value = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) value = (value ^ *p) * 16777619u;
    return value;
}
static bool commit(int idx);            /* 前向声明 */
static EpdRect rect(int x, int y, int w, int h, int top) { return (EpdRect){x, y + top, w, h}; }

/* ============ 版式几何（636×510 面板内；rect() 统一加 top 偏移） ============
 * 候选带两行：编码 y0..56 / 候选 y56..128；额外行 y131..187（h56）；
 * 字母行带 y187..439（3 行 × 84）；功能行 y439..495（h56）。 */
#define KBD_X0   24
#define KBD_W    636
#define KEY_W    63                      /* 10 格 = 630，居中偏 3px */
static int row_h(int row) { return (row >= 1 && row <= 3) ? 84 : 56; }
static int row_y(int row) {
    int y = 131;
    for (int r = 0; r < row; ++r) y += row_h(r);
    return y;
}
static int grid_x0(void) { return KBD_X0 + (KBD_W - 10 * KEY_W) / 2; }

/* 额外行横向布局：[布局 2 格] [Ctl 1.5 格] [◀▲▼▶ 各 1 格] [Shift 1.25 格] [⌫]。
 * 九宫格时 ⌫ 在九宫右列，Shift 吃满剩余宽度（YanReader evkNineExtraRowRects）。 */
static void extra_row_rects(int x[8], int w[8]) {
    const int q = KEY_W / 4;
    x[0] = KBD_X0;          w[0] = 8 * q;
    x[1] = x[0] + w[0];     w[1] = 6 * q;
    for (int d = 0; d < 4; ++d) { x[2 + d] = KBD_X0 + (14 + 4 * d) * q; w[2 + d] = 4 * q; }
    x[6] = KBD_X0 + 30 * q; w[6] = 5 * q;
    x[7] = KBD_X0 + 35 * q; w[7] = KBD_W - 35 * q;
}
static void nine_extra_row_rects(int x[8], int w[8]) {
    const int q = KEY_W / 4;
    x[0] = KBD_X0;          w[0] = 8 * q;
    x[1] = x[0] + w[0];     w[1] = 6 * q;
    for (int d = 0; d < 4; ++d) { x[2 + d] = KBD_X0 + (14 + 4 * d) * q; w[2 + d] = 4 * q; }
    x[6] = KBD_X0 + 30 * q; w[6] = KBD_W - 30 * q;
    x[7] = KBD_X0 + KBD_W;  w[7] = 0;
}
/* 功能行五段：①面板 | ②面板 | 空格（吃剩余） | 中/英 | 确定（2 格）。 */
static void func_row_rects(int x[5], int w[5]) {
    w[0] = KEY_W; w[1] = KEY_W; w[2] = KBD_W - 5 * KEY_W; w[3] = KEY_W; w[4] = 2 * KEY_W;
    if (w[2] < KEY_W) w[2] = KEY_W;
    x[0] = KBD_X0; x[1] = x[0] + w[0]; x[2] = x[1] + w[1];
    x[3] = x[2] + w[2]; x[4] = KBD_X0 + KBD_W - w[4];
}
/* 一键多字母布局（14/18）某行横向：整行撑满 10 格、组等宽；九宫格式行（n<cols）
 * 按列定宽整行居中（YanReader evkAmbigRowRects）。 */
static void ambig_row_rects(int r, int xs[8], int ws[8]) {
    const int n = ime_glue_ambig_row_keys(r);
    const int cols = ime_glue_ambig_row_cols(r);
    const int span = 10 * KEY_W;
    if (n <= 0 || cols <= 0) return;
    if (n == cols) {
        const int x0 = grid_x0();
        for (int k = 0; k < n; ++k) {
            xs[k] = x0 + span * k / n;
            ws[k] = x0 + span * (k + 1) / n - xs[k];
        }
        return;
    }
    const int cell = span / cols;
    const int x0 = KBD_X0 + (KBD_W - cell * n) / 2;
    for (int k = 0; k < n; ++k) { xs[k] = x0 + k * cell; ws[k] = cell; }
}
/* 九宫格三段列：左列（标点/分音节双身份，宽按最长音节定）| 中 3×3 | 右列 1.6 格。 */
static int nine_left_w(void) {
    const int lo = KBD_W / 10;
    const int syl = ui_text_fixed_width_px(21, "zhuang") + 20;
    return syl > lo ? syl : lo;
}
static void nine_cols(int *pw, int *cx, int *cw, int *rx, int *rw) {
    const int p = nine_left_w();
    *pw = p;
    *rw = KBD_W * 16 / 100;
    *cw = (KBD_W - p - *rw) / 3;
    *cx = KBD_X0 + p;
    *rx = *cx + 3 * *cw;
}
/* 数字面板：四行数字盘占字母行带（252px 均分四行）；左右侧列 15%。 */
static int num_row_y(int r) { return row_y(1) + 252 * r / 4; }
static int num_row_h(int r) { return row_y(1) + 252 * (r + 1) / 4 - num_row_y(r); }
static int num_side_w(void) { return KBD_W * 15 / 100; }
/* T9 宫格区：候选带之下整块（y131..495）；有歧义读音时左列分音节、右侧 3×3 宫格。 */
static int t9_top(void) { return row_y(0); }
static int t9_h(void) { return 495 - t9_top(); }

static bool cn(void) { return s_engine_ok && s_chinese && !s_ascii; }
static bool nine_active(void) { return cn() && s_layout == 3 && !s_page; }
static bool ambig_active_layout(void) { return cn() && s_layout != 0 && !s_page; }

/* ============ 引擎快照与会话 ============ */
static unsigned s_engine_revision;
static void engine_pull(void) {
    s_comp[0] = 0;
    s_cand_count = 0;
    if (cn()) {
        (void)ime_glue_composition(s_comp, sizeof(s_comp));
        int n = ime_glue_candidate_count();
        if (n > KB_CAND_MAX) n = KB_CAND_MAX;
        for (int i = 0; i < n; ++i) {
            if (ime_glue_candidate(i, s_cand[i], sizeof(s_cand[i])) < 0) { n = i; break; }
        }
        s_cand_count = n;
        if (!ime_glue_ambig_active()) s_syllable_offset = 0;
        /* 组合结束自动收起 T9 宫格（YanReader：组合一收起宫格跟着收，不用再点一次）。 */
        if (s_t9 && !ime_glue_composing()) { s_t9 = false; ime_glue_grid_paging(false); }
    } else if (s_t9) { s_t9 = false; ime_glue_grid_paging(false); }
    const char *status = ime_glue_status();
    if (status[0]) snprintf(s_notice, sizeof(s_notice), "%s", status);
    ++s_engine_revision;
}
void ui_keyboard_end(void) {
    ui_keyboard_hold_cancel();
    if (s_t9) { s_t9 = false; if (s_engine_ok) ime_glue_grid_paging(false); }
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
    if (cn() && s_comp[0]) commit(0);
}
void ui_keyboard_begin(ui_text_edit_t *edit, bool ascii_only) {
    ui_keyboard_end(); s_edit = edit; s_ascii = ascii_only;
    s_chinese = !ascii_only;
    s_page = 0; s_shift = false; s_ctrl = false; s_t9 = false;
    s_engine_ok = false;
    s_layout = ascii_only ? 0 : app_settings_kb_layout();
    if (!ascii_only && edit) {
        if (ime_glue_begin()) {
            s_engine_ok = true;
            ime_glue_set_layout(s_layout);
            /* 正文上下文喂引擎：已出现过的词候选加分（编辑器打开时喂一次）。 */
            char before[129];
            size_t n = edit->cursor;
            if (n >= sizeof(before)) n = sizeof(before) - 1;
            memcpy(before, edit->text, n); before[n] = 0;
            ime_glue_doc_context(before);
        } else {
            s_chinese = false;
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
static void commit_insert(const char *text) {
    if (!insert(text)) return;
    if (s_engine_ok) ime_glue_predict_begin(text);
}
static bool commit(int idx) {
    if (!cn() || idx < 0 || idx >= s_cand_count) return false;
    char out[96];
    if (!ime_glue_select(idx, out, sizeof(out))) return false;
    if (out[0]) commit_insert(out);
    engine_pull();
    return true;
}
static void backspace(void) {
    if (cn() && (s_comp[0] || ime_glue_predicting())) {
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
/* 普通字符键（字母/符号/数字统一入口）：中文态交引擎（句读转全角、组合中数字选词、
 * 拼音组合都由引擎解释），英文/密码态直接落串（Shift 吃字母）。YanReader 同路。 */
static void key_char(char c) {
    if (s_ctrl) { s_ctrl = false; snprintf(s_notice, sizeof(s_notice), "组合键暂未开放"); s_shift = false; return; }
    if (!cn()) {
        char t[2] = {c, 0};
        if (s_shift && c >= 'a' && c <= 'z') { t[0] = (char)(c - 32); s_shift = false; }
        insert(t);
        return;
    }
    char out[96];
    if (ime_glue_key(c, out, sizeof(out)) && out[0]) insert(out);
    s_shift = false;
    engine_pull();
}
/* 一键多字母组键（14/18/九宫格 2..9）：中文态交引擎歧义；英文/密码态退化为组首字母。 */
static void ambig_tap(int group) {
    char letters[16];
    ime_glue_ambig_group_letters(group, letters, sizeof(letters));
    if (!cn()) { key_char(letters[0]); return; }
    char out[96];
    if (ime_glue_ambig(group, out, sizeof(out)) && out[0]) insert(out);
    engine_pull();
}
static bool same_rect(EpdRect a, EpdRect b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }
static bool intersects(EpdRect a, EpdRect b) { return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height; }

/* ============ 键帽绘制（保留按压反馈/探针裁剪机制） ============ */
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
/* 方向键三角形：dir 0=◀ 1=▲ 2=▼ 3=▶。 */
static void arrow_key(uint8_t *fb, EpdRect r, int dir, bool dark) {
    if (s_probe) { if (ui_rect_hit(r, s_probe_x, s_probe_y)) s_key_clip = r; return; }
    if (s_keys_only && !intersects(r, s_key_clip)) return;
    bool pressed = s_pressed && same_rect(r, s_pressed_area);
    ui_fill_round_rect(fb, r, 12, pressed ? 0xb0 : dark ? UI_GRAY_BLACK : UI_GRAY_WHITE);
    ui_draw_round_rect(fb, r, 12, pressed ? 0x20 : 0x60);
    int cx = r.x + r.width / 2, cy = r.y + r.height / 2 + (pressed ? 2 : 0);
    int tipx = cx, tipy = cy, l1x = cx, l1y = cy, l2x = cx, l2y = cy;
    switch (dir) {
        case 0: tipx = cx - 13; l1x = cx + 9; l1y = cy - 9; l2x = cx + 9; l2y = cy + 9; break;
        case 1: tipy = cy - 13; l1x = cx - 9; l1y = cy + 9; l2x = cx + 9; l2y = cy + 9; break;
        case 2: tipy = cy + 13; l1x = cx - 9; l1y = cy - 9; l2x = cx + 9; l2y = cy - 9; break;
        default: tipx = cx + 13; l1x = cx - 9; l1y = cy - 9; l2x = cx - 9; l2y = cy + 9; break;
    }
    for (int d = 0; d <= 9; ++d) {        /* 逐行扫描填充三角形（墨水屏无多边形填充原语） */
        int ax = tipx + (l1x - tipx) * d / 9, ay = tipy + (l1y - tipy) * d / 9;
        int bx = tipx + (l2x - tipx) * d / 9, by = tipy + (l2y - tipy) * d / 9;
        epd_draw_line(ax, ay, bx, by, UI_GRAY_BLACK, fb);
    }
}
/* Shift 图形键帽：字体子集无 ⇧ 码点，沿用旧版 a/A 字母标记（待发反色）。 */
static void shift_key(uint8_t *fb, EpdRect r, bool on) {
    key_paint(fb, r, on ? "A" : "a", 28, on, true);
}

/* ============ 候选带两行 ============ */
/* 候选行：当前页按文字宽度排格（78..606，避开 ‹› 翻页键）。 */
static size_t visible(EpdRect boxes[KB_CAND_MAX], int top) {
    if (!s_cand_count) return 0;
    int left = 78, limit = 606; size_t count = 0;
    while ((int)count < s_cand_count && count < KB_CAND_MAX) {
        int width = ui_text_fixed_width_px(25, s_cand[count]) + 24;
        if (width < 78) width = 78;
        if (width > limit - 78) width = limit - 78;
        if (left + width > limit) break;
        boxes[count] = rect(left, 60, width - 6, 64, top);
        left += width; ++count;
    }
    return count;
}
bool ui_keyboard_page(int direction) {
    if (!cn()) return false;
    bool moved = direction > 0 ? ime_glue_page_next() : ime_glue_page_prev();
    if (!moved) return false;
    engine_pull();
    s_dirty |= DIRTY_WORDS;
    if (s_t9) s_dirty |= DIRTY_SYLLABLES;
    return true;
}
/* 分音节列表（九宫格左列 / T9 宫格左列共用）：返回 3 行窗口 + 是否还有更多。 */
static size_t syllables(const char *out[4], bool *more) {
    static char bufs[3][16];
    if (more) *more = false;
    if (!cn() || !ime_glue_ambig_active()) return 0;
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
    if (!s_probe && !s_keys_only) {
        if (s_notice[0]) ui_text_fixed_vc(fb, 28, top + 28, 21, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        else if (s_comp[0]) ui_text_input_composition(fb, s_comp, rect(28, 10, 500, 36, top), 25);
        else {
            const char *hint = s_ascii ? "密码模式：英文、数字与符号"
                : cn() ? (s_layout == 3 ? "九宫格拼音，点上行编码看候选宫格" : "连续输入拼音，点编码行开宫格")
                       : "轻点输入，Shift 切换大小写";
            ui_text_fixed_vc(fb, 28, top + 28, 21, hint, EPD_DRAW_ALIGN_LEFT, false);
        }
        if (cn() && s_cand_count) {
            char pg[16];
            snprintf(pg, sizeof(pg), "%d/%d", ime_glue_page_number(), ime_glue_page_total());
            ui_text_fixed_vc(fb, 606, top + 28, 18, pg, EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    key_paint(fb, rect(24, 60, 44, 64, top), "‹", 30, false, true);
    key_paint(fb, rect(616, 60, 44, 64, top), "›", 30, false, true);
    EpdRect boxes[KB_CAND_MAX]; size_t count = visible(boxes, top);
    for (size_t i = 0; i < count; ++i) {
        char text[KB_CAND_TEXT]; snprintf(text, sizeof(text), "%.47s", s_cand[i]);
        while (strlen(text) && ui_text_fixed_width_px(25, text) > boxes[i].width - 12) {
            size_t n = strlen(text) - 1; while (n && ((unsigned char)text[n] & 192) == 128) --n; text[n] = 0;
        }
        key_paint(fb, boxes[i], text, 25, false, false);
    }
}

/* ============ 绘制（YanReader 三面板版式） ============ */
static const char *EVK_LETTER_ROWS[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char *EVK_ROW3_MID = "zxcvbnm";
static const char *EVK_SYM_PANEL[3] = {"-/:;()$&@\"", ".,?!'\"#%*+", "~_^<>=[]{}|"};
static const char EVK_NUM_LEFT[4] = {'#', '*', '-', '+'};
static const char EVK_NUM_RIGHT[4] = {0, '.', '@', '/'};
static const char *const NINE_PUNCT_LAB[4] = {"，", "。", "？", "！"};
static const char NINE_PUNCT_KEY[4] = {',', '.', '?', '!'};
static const char *layout_label(int layout) {
    static const char *const k[4] = {"26键", "14键", "18键", "9键"};
    return layout >= 0 && layout < 4 ? k[layout] : k[0];
}

static void draw_t9_panel(uint8_t *fb, int top) {
    /* 左列分音节（歧义组合中才有）；其余 3×3 候选宫格（引擎宫格分页一页 9 个）。 */
    int lw = 0, gx = KBD_X0, gw = KBD_W;
    if (cn() && ime_glue_ambig_active()) {
        lw = nine_left_w();
        gx = KBD_X0 + lw;
        gw = KBD_W - lw;
        const char *syl[4]; bool more;
        size_t n = syllables(syl, &more);
        const int cell_h = t9_h() / 4;
        for (int i = 0; i < 4; ++i) {
            const char *lab = i < (int)n ? syl[i] : (i == 3 && (more || s_syllable_offset)) ? "更多" : "";
            key_paint(fb, rect(KBD_X0, t9_top() + i * cell_h, lw - 4, cell_h - 4, top), lab, 21, false, true);
        }
    }
    const int cw = gw / 3, ch = t9_h() / 3;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        int idx = r * 3 + c;
        char text[KB_CAND_TEXT]; text[0] = 0;
        if (idx < s_cand_count) {
            snprintf(text, sizeof(text), "%s", s_cand[idx]);
            while (strlen(text) && ui_text_fixed_width_px(25, text) > cw - 14) {
                size_t n = strlen(text) - 1; while (n && ((unsigned char)text[n] & 192) == 128) --n; text[n] = 0;
            }
        }
        key_paint(fb, rect(gx + c * cw, t9_top() + r * ch, cw - 4, ch - 4, top), text, 25, false, false);
    }
}
static void draw_nine_left(uint8_t *fb, int top) {
    const int band_y = row_y(1), band_h = 252;
    if (cn() && ime_glue_ambig_active()) {
        const char *syl[4]; bool more;
        size_t n = syllables(syl, &more);
        for (int i = 0; i < 4; ++i) {
            const int cy = band_y + band_h * i / 4;
            const int ch = band_y + band_h * (i + 1) / 4 - cy;
            const char *lab = i < (int)n ? syl[i] : (i == 3 && (more || s_syllable_offset)) ? "更多" : "";
            key_paint(fb, rect(KBD_X0, cy, nine_left_w() - 4, ch - 4, top), lab, 21, false, true);
        }
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const int cy = band_y + band_h * i / 4;
        const int ch = band_y + band_h * (i + 1) / 4 - cy;
        key_paint(fb, rect(KBD_X0, cy, nine_left_w() - 4, ch - 4, top), NINE_PUNCT_LAB[i], 25, false, true);
    }
}
void ui_keyboard_draw(uint8_t *fb, int top) {
    if (!s_probe && !s_keys_only) {
        ui_fill_round_rect(fb, rect(16, -12, 652, 534, top), 26, 0xe8);
        ui_draw_round_rect(fb, rect(16, -12, 652, 534, top), 26, 0x60);
    }
    paint_words(fb, top);
    const bool nine = nine_active();
    /* T9 宫格展开时：候选带之下整块归面板，键区不画。 */
    if (s_t9 && s_comp[0]) { draw_t9_panel(fb, top); return; }
    /* 额外行：[布局] [Ctl] [◀▲▼▶] [Shift] [⌫]（九宫格无 ⌫，Shift 吃满剩余）。 */
    {
        int ex[8], ew[8];
        if (nine) nine_extra_row_rects(ex, ew); else extra_row_rects(ex, ew);
        const int ry = row_y(0), rh = row_h(0);
        key_paint(fb, rect(ex[0], ry, ew[0], rh, top), cn() ? layout_label(s_layout) : "26键", 21,
                  ambig_active_layout(), true);
        key_paint(fb, rect(ex[1], ry, ew[1], rh, top), "Ctl", 22, s_ctrl, true);
        for (int d = 0; d < 4; ++d) arrow_key(fb, rect(ex[2 + d], ry, ew[2 + d], rh, top), d, false);
        shift_key(fb, rect(ex[6], ry, ew[6], rh, top), s_shift);
        if (!nine && ew[7]) key_paint(fb, rect(ex[7], ry, ew[7], rh, top), "⌫", 28, false, true);
    }
    /* 键区三张面（字母行带 187..439）。 */
    if (s_page == 1) {                    /* 符号面板：10 列 × 3 行（数字已独立成面） */
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 10; ++c) {
                char t[2] = {EVK_SYM_PANEL[r][c], 0};
                key_paint(fb, rect(grid_x0() + c * KEY_W, row_y(1 + r), KEY_W - 4, row_h(1 + r) - 4, top), t, 26, false, false);
            }
    } else if (s_page == 2) {             /* 数字面板：四行数字盘，末行 0 横跨中间三列 */
        const int sw = num_side_w();
        const int mw = KBD_W - 2 * sw;
        for (int r = 0; r < 4; ++r) {
            const int ry = num_row_y(r), rh = num_row_h(r);
            char t[2] = {EVK_NUM_LEFT[r], 0};
            key_paint(fb, rect(KBD_X0, ry, sw - 4, rh - 4, top), t, 26, false, true);
            if (r < 3) {
                for (int c = 0; c < 3; ++c) {
                    char d[2] = {(char)('1' + r * 3 + c), 0};
                    key_paint(fb, rect(KBD_X0 + sw + c * (mw / 3), ry, mw / 3 - 4, rh - 4, top), d, 28, false, false);
                }
            } else {
                key_paint(fb, rect(KBD_X0 + sw, ry, mw - 4, rh - 4, top), "0", 28, false, false);
            }
            if (r == 0) key_paint(fb, rect(KBD_X0 + KBD_W - sw, ry, sw - 4, rh - 4, top), "⌫", 28, false, true);
            else { char t2[2] = {EVK_NUM_RIGHT[r], 0}; key_paint(fb, rect(KBD_X0 + KBD_W - sw, ry, sw - 4, rh - 4, top), t2, 26, false, true); }
        }
    } else if (nine) {                    /* 九宫格：左列 + 3×3 + 右列 */
        int pw, cx, cw, rx, rw;
        nine_cols(&pw, &cx, &cw, &rx, &rw);
        draw_nine_left(fb, top);
        for (int r = 0; r < 3; ++r) {
            const int ky = row_y(1) + r * 84;
            for (int c = 0; c < 3; ++c) {
                const int idx = r * 3 + c;
                char lab[24];
                if (idx == 0) snprintf(lab, sizeof(lab), "1分词");
                else ime_glue_ambig_group_label(idx - 1, lab, sizeof(lab));
                key_paint(fb, rect(cx + c * cw, ky, cw - 4, 80, top), lab, 21, false, false);
            }
            if (r == 0) key_paint(fb, rect(rx, ky, rw - 4, 80, top), "⌫", 28, false, true);
            else key_paint(fb, rect(rx, ky, rw - 4, 80, top), r == 1 ? "重输" : "0", r == 1 ? 22 : 28, false, true);
        }
    } else if (ambig_active_layout()) {   /* 14 / 18 键：一行一组、组等宽撑满 */
        for (int r = 0; r < 3; ++r) {
            int xs[8], ws[8];
            ambig_row_rects(r, xs, ws);
            const int n = ime_glue_ambig_row_keys(r);
            for (int k = 0; k < n && k < 8; ++k) {
                char lab[24];
                int g = ime_glue_ambig_row_group(r, k);
                ime_glue_ambig_group_label(g, lab, sizeof(lab));
                key_paint(fb, rect(xs[k], row_y(1 + r), ws[k] - 4, 80, top), lab, 21, false, false);
            }
        }
    } else {                              /* 26 全拼：三行，第三行 9 格（7 字母 + ，。） */
        for (int r = 0; r < 3; ++r) {
            const int ky = row_y(1 + r), kh = row_h(1 + r);
            if (r < 2) {
                const char *row = EVK_LETTER_ROWS[r];
                const int n = (int)strlen(row);
                const int kx = KBD_X0 + (KBD_W - n * KEY_W) / 2;
                for (int k = 0; k < n; ++k) {
                    char t[2] = {s_shift ? (char)(row[k] - 32) : row[k], 0};
                    key_paint(fb, rect(kx + k * KEY_W, ky, KEY_W - 4, kh - 4, top), t, 28, false, false);
                }
            } else {
                const int kx = KBD_X0 + (KBD_W - 9 * KEY_W) / 2;
                for (int k = 0; k < 9; ++k) {
                    char t[8] = {0};
                    if (k == 7) snprintf(t, sizeof(t), "%s", cn() ? "，" : ",");
                    else if (k == 8) snprintf(t, sizeof(t), "%s", cn() ? "。" : ".");
                    else { t[0] = s_shift ? (char)(EVK_ROW3_MID[k] - 32) : EVK_ROW3_MID[k]; t[1] = 0; }
                    key_paint(fb, rect(kx + k * KEY_W, ky, KEY_W - 4, kh - 4, top), t, 28, false, false);
                }
            }
        }
    }
    /* 功能行：①面板 | ②面板 | 空格 | 中/英 | 确定。 */
    {
        int fx[5], fw[5];
        func_row_rects(fx, fw);
        const int by = row_y(4), bh = row_h(4);
        key_paint(fb, rect(fx[0], by, fw[0], bh, top), s_page == 1 ? "ABC" : "符", 22, false, true);
        key_paint(fb, rect(fx[1], by, fw[1], bh, top), s_page == 2 ? "ABC" : "123", 22, false, true);
        key_paint(fb, rect(fx[2], by, fw[2], bh, top), s_comp[0] ? "选词" : "空格", 26, false, false);
        key_paint(fb, rect(fx[3], by, fw[3], bh, top), s_ascii ? "英" : s_chinese ? "中" : "英", 24,
                  s_ascii, true);
        key_paint(fb, rect(fx[4], by, fw[4], bh, top), "确定", 28, true, false);
    }
}

/* ============ 命中测试（绘制同款几何；副作用键在命中时完成） ============ */
static bool in_key(int x, int y, int kx, int ky, int kw, int kh) {
    if (kw <= 0 || kh <= 0) return false;
    if (x < kx || x >= kx + kw || y < ky || y >= ky + kh) return false;
    const int r = (kw < kh ? kw : kh) / 5;   /* YanReader evkInKey：圆角内切圆，四角死区 */
    if (r <= 0) return true;
    const int cx = (x < kx + r) ? kx + r : (x >= kx + kw - r ? kx + kw - r - 1 : x);
    const int cy = (y < ky + r) ? ky + r : (y >= ky + kh - r ? ky + kh - r - 1 : y);
    if (cx == x && cy == y) return true;
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}
static ui_keyboard_result_t t9_hit(int x, int y, int top) {
    int lw = 0, gx = KBD_X0;
    if (cn() && ime_glue_ambig_active()) {
        lw = nine_left_w(); gx = KBD_X0 + lw;
        const int cell_h = t9_h() / 4;
        const char *syl[4]; bool more;
        size_t n = syllables(syl, &more);
        for (int i = 0; i < 4; ++i) {
            const int cy = t9_top() + i * cell_h;
            if (!in_key(x, y, KBD_X0, cy, lw - 4, cell_h - 4)) continue;
            if (i == 3 && (int)n <= 3 && (more || s_syllable_offset)) { s_syllable_offset += 3; return UI_KEYBOARD_CHANGED; }
            if (i < (int)n && ime_glue_ambig_select(s_syllable_offset + i)) { engine_pull(); return UI_KEYBOARD_CHANGED; }
            return UI_KEYBOARD_CHANGED;
        }
    }
    const int cw = (KBD_W - lw) / 3, ch = t9_h() / 3;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        int idx = r * 3 + c;
        if (in_key(x, y, gx + c * cw, t9_top() + r * ch, cw - 4, ch - 4)) {
            if (idx < s_cand_count && commit(idx)) return UI_KEYBOARD_CHANGED;
            return UI_KEYBOARD_CHANGED;
        }
    }
    return UI_KEYBOARD_NONE;
}
static ui_keyboard_result_t tap(int x, int y, int top) {
    if (!s_edit || y < top || y >= top + 502 || x < KBD_X0 || x >= KBD_X0 + KBD_W) return UI_KEYBOARD_NONE;
    /* 21h 修复（真机：键盘正常弹出但所有点击无反应）：绘制走 rect(...,top) 全是屏幕坐标，
     * 而分派里 in_key 的键位 y（row_y/num_row_y/cy/by）一直是面板坐标——触摸 y 差一个
     * top，导致编码行/额外行/字母区/功能行全部判 miss（只有候选行 visible() 的盒子和
     * 数字面板行带判断恰好一致才活）。入口统一换成面板内 y，in_key 调用保持面板坐标。 */
    y -= top;
    const bool nine = nine_active();
    /* 候选带两行。 */
    if (y < 56) {                         /* 编码行：组合中点它开关 T9 宫格 */
        if (s_comp[0]) {
            s_t9 = !s_t9;
            ime_glue_grid_paging(s_t9);
            engine_pull();
            return UI_KEYBOARD_CHANGED;
        }
        return UI_KEYBOARD_NONE;
    }
    if (y < 128) {                        /* 候选行：‹› 翻页 + 候选块 */
        if (x < 68) return ui_keyboard_page(-1) ? UI_KEYBOARD_CHANGED : UI_KEYBOARD_NONE;
        if (x >= 616) return ui_keyboard_page(1) ? UI_KEYBOARD_CHANGED : UI_KEYBOARD_NONE;
        EpdRect boxes[KB_CAND_MAX]; size_t count = visible(boxes, 0);   /* 面板坐标（y 已归一） */
        for (size_t i = 0; i < count; ++i)
            if (in_key(x, y, boxes[i].x, boxes[i].y, boxes[i].width, boxes[i].height)) {
                if (commit((int)i)) return UI_KEYBOARD_CHANGED;
                return UI_KEYBOARD_CHANGED;
            }
        return UI_KEYBOARD_NONE;
    }
    if (s_t9 && s_comp[0]) return t9_hit(x, y, top);   /* 宫格展开：键区整块归面板 */
    /* 额外行。 */
    {
        const int ry = row_y(0), rh = row_h(0);
        if (y >= ry && y < ry + rh) {
            int ex[8], ew[8];
            if (nine) nine_extra_row_rects(ex, ew); else extra_row_rects(ex, ew);
            if (in_key(x, y, ex[0], ry, ew[0], rh)) {          /* 布局循环（仅中文态） */
                if (cn()) {
                    s_layout = (s_layout + 1) % (ime_glue_ambig_layout_count() > 0 ? ime_glue_ambig_layout_count() : 4);
                    app_settings_set_kb_layout((uint8_t)s_layout);
                    ime_glue_set_layout(s_layout);
                    ime_glue_reset();
                    engine_pull();
                } else snprintf(s_notice, sizeof(s_notice), s_ascii ? "密码模式固定 26 键" : "英文态固定 26 键");
                return UI_KEYBOARD_CHANGED;
            }
            if (in_key(x, y, ex[1], ry, ew[1], rh)) { s_ctrl = !s_ctrl; return UI_KEYBOARD_CHANGED; }
            for (int d = 0; d < 4; ++d)
                if (in_key(x, y, ex[2 + d], ry, ew[2 + d], rh)) {
                    if (s_comp[0] || ime_glue_predicting()) {  /* 组合中：翻候选页 */
                        return ui_keyboard_page(d == 0 || d == 1 ? -1 : 1) ? UI_KEYBOARD_CHANGED : UI_KEYBOARD_CHANGED;
                    }
                    if (d == 0 || d == 3) {                    /* 无组合：移动编辑光标 */
                        ui_text_edit_move(s_edit, d == 0 ? -1 : 1);
                        return UI_KEYBOARD_CHANGED;
                    }
                    return UI_KEYBOARD_CHANGED;
                }
            if (in_key(x, y, ex[6], ry, ew[6], rh)) { s_shift = !s_shift; return UI_KEYBOARD_CHANGED; }
            if (!nine && ew[7] && in_key(x, y, ex[7], ry, ew[7], rh)) { backspace(); return UI_KEYBOARD_CHANGED; }
            return UI_KEYBOARD_NONE;
        }
    }
    /* 字母行带（187..439）：按当前面板/布局分派。 */
    {
        const int band_y = row_y(1), band_h = 252;
        if (y >= band_y && y < band_y + band_h) {
            if (s_page == 1) {                                /* 符号面板 */
                for (int r = 0; r < 3; ++r) {
                    const int ky = row_y(1 + r), kh = row_h(1 + r);
                    if (y < ky || y >= ky + kh) continue;
                    for (int c = 0; c < 10; ++c)
                        if (in_key(x, y, grid_x0() + c * KEY_W, ky, KEY_W - 4, kh - 4)) {
                            key_char(EVK_SYM_PANEL[r][c]);
                            return UI_KEYBOARD_CHANGED;
                        }
                    return UI_KEYBOARD_NONE;
                }
            } else if (s_page == 2) {                         /* 数字面板：末行整行宽 0 */
                const int sw = num_side_w();
                const int mw = KBD_W - 2 * sw;
                for (int r = 0; r < 4; ++r) {
                    const int ry = num_row_y(r), rh = num_row_h(r);
                    if (y < ry || y >= ry + rh) continue;
                    if (x < KBD_X0 + sw) { key_char(EVK_NUM_LEFT[r]); return UI_KEYBOARD_CHANGED; }
                    if (x >= KBD_X0 + KBD_W - sw) {
                        if (r == 0) { backspace(); return UI_KEYBOARD_CHANGED; }
                        key_char(EVK_NUM_RIGHT[r]); return UI_KEYBOARD_CHANGED;
                    }
                    if (x >= KBD_X0 + sw && x < KBD_X0 + sw + mw) {
                        if (r < 3) key_char((char)('1' + r * 3 + (x - KBD_X0 - sw) / (mw / 3)));
                        else key_char('0');
                        return UI_KEYBOARD_CHANGED;
                    }
                    return UI_KEYBOARD_NONE;
                }
            } else if (nine) {                                /* 九宫格 */
                int pw, cx, cw, rx, rw;
                nine_cols(&pw, &cx, &cw, &rx, &rw);
                if (x < KBD_X0 + pw) {                        /* 左列：分音节 / 标点 */
                    const char *syl[4]; bool more;
                    size_t n = syllables(syl, &more);
                    if (cn() && ime_glue_ambig_active()) {
                        for (int i = 0; i < 4; ++i) {
                            const int cy = band_y + band_h * i / 4;
                            const int ch = band_y + band_h * (i + 1) / 4 - cy;
                            if (!in_key(x, y, KBD_X0, cy, pw - 4, ch - 4)) continue;
                            if (i == 3 && (int)n <= 3 && (more || s_syllable_offset)) { s_syllable_offset += 3; return UI_KEYBOARD_CHANGED; }
                            if (i < (int)n && ime_glue_ambig_select(s_syllable_offset + i)) { engine_pull(); return UI_KEYBOARD_CHANGED; }
                            return UI_KEYBOARD_CHANGED;
                        }
                        return UI_KEYBOARD_NONE;
                    }
                    for (int i = 0; i < 4; ++i) {
                        const int cy = band_y + band_h * i / 4;
                        const int ch = band_y + band_h * (i + 1) / 4 - cy;
                        if (in_key(x, y, KBD_X0, cy, pw - 4, ch - 4)) { key_char(NINE_PUNCT_KEY[i]); return UI_KEYBOARD_CHANGED; }
                    }
                    return UI_KEYBOARD_NONE;
                }
                if (x >= rx) {                                /* 右列：⌫ / 重输 / 0 */
                    const int ky0 = band_y;
                    if (in_key(x, y, rx, ky0, rw - 4, 80)) { backspace(); return UI_KEYBOARD_CHANGED; }
                    if (in_key(x, y, rx, ky0 + 84, rw - 4, 80)) {
                        if (s_comp[0]) { ime_glue_cancel_composition(); engine_pull(); }
                        return UI_KEYBOARD_CHANGED;
                    }
                    if (in_key(x, y, rx, ky0 + 168, rw - 4, 80)) {
                        if (!s_comp[0]) key_char('0');         /* 组合中 0 静默（YanReader EVK_NOOP） */
                        return UI_KEYBOARD_CHANGED;
                    }
                    return UI_KEYBOARD_NONE;
                }
                for (int r = 0; r < 3; ++r) {                 /* 中间 3×3：1分词 + 组 0..7 */
                    const int ky = band_y + r * 84;
                    if (y < ky || y >= ky + 84) continue;
                    for (int c = 0; c < 3; ++c) {
                        const int idx = r * 3 + c;
                        if (!in_key(x, y, cx + c * cw, ky, cw - 4, 80)) continue;
                        if (idx == 0) { if (s_comp[0]) { ime_glue_ambig_commit_syllable(); engine_pull(); } return UI_KEYBOARD_CHANGED; }
                        ambig_tap(idx - 1);
                        return UI_KEYBOARD_CHANGED;
                    }
                    return UI_KEYBOARD_NONE;
                }
            } else if (ambig_active_layout()) {               /* 14 / 18 键 */
                for (int r = 0; r < 3; ++r) {
                    const int ky = row_y(1 + r);
                    if (y < ky || y >= ky + 84) continue;
                    int xs[8], ws[8];
                    ambig_row_rects(r, xs, ws);
                    const int n = ime_glue_ambig_row_keys(r);
                    for (int k = 0; k < n && k < 8; ++k)
                        if (in_key(x, y, xs[k], ky, ws[k] - 4, 80)) {
                            ambig_tap(ime_glue_ambig_row_group(r, k));
                            return UI_KEYBOARD_CHANGED;
                        }
                    return UI_KEYBOARD_NONE;
                }
            } else {                                          /* 26 全拼 */
                for (int r = 0; r < 3; ++r) {
                    const int ky = row_y(1 + r), kh = row_h(1 + r);
                    if (y < ky || y >= ky + kh) continue;
                    if (r < 2) {
                        const char *row = EVK_LETTER_ROWS[r];
                        const int n = (int)strlen(row);
                        const int kx = KBD_X0 + (KBD_W - n * KEY_W) / 2;
                        for (int k = 0; k < n; ++k)
                            if (in_key(x, y, kx + k * KEY_W, ky, KEY_W - 4, kh - 4)) { key_char(row[k]); return UI_KEYBOARD_CHANGED; }
                        return UI_KEYBOARD_NONE;
                    }
                    const int kx = KBD_X0 + (KBD_W - 9 * KEY_W) / 2;
                    for (int k = 0; k < 9; ++k)
                        if (in_key(x, y, kx + k * KEY_W, ky, KEY_W - 4, kh - 4)) {
                            if (k == 7) key_char(',');
                            else if (k == 8) key_char('.');
                            else key_char(EVK_ROW3_MID[k]);
                            return UI_KEYBOARD_CHANGED;
                        }
                    return UI_KEYBOARD_NONE;
                }
            }
            return UI_KEYBOARD_NONE;
        }
    }
    /* 功能行：①②面板 | 空格 | 中/英 | 确定。 */
    {
        const int by = row_y(4), bh = row_h(4);
        if (y >= by && y < by + bh) {
            int fx[5], fw[5];
            func_row_rects(fx, fw);
            if (in_key(x, y, fx[0], by, fw[0], bh)) {          /* ①：字母→符 符→ABC 数→符 */
                s_page = s_page == 2 ? 1 : (s_page == 1 ? 0 : 1);
                return UI_KEYBOARD_CHANGED;
            }
            if (in_key(x, y, fx[1], by, fw[1], bh)) {          /* ②：数→ABC 其余→数字 */
                s_page = s_page == 2 ? 0 : 2;
                return UI_KEYBOARD_CHANGED;
            }
            if (in_key(x, y, fx[2], by, fw[2], bh)) {          /* 空格：组合中选首选，否则空格 */
                if (s_comp[0] || ime_glue_predicting()) { if (!commit(0)) insert(" "); }
                else insert(" ");
                return UI_KEYBOARD_CHANGED;
            }
            if (in_key(x, y, fx[3], by, fw[3], bh)) {          /* 中/英 */
                if (s_ascii) snprintf(s_notice, sizeof(s_notice), "密码使用英文、数字与符号");
                else if (s_comp[0]) snprintf(s_notice, sizeof(s_notice), "请先选词，再切换语言");
                else { s_chinese = !s_chinese; engine_pull(); }
                return UI_KEYBOARD_CHANGED;
            }
            if (in_key(x, y, fx[4], by, fw[4], bh)) {          /* 确定：组合先上屏（v6）再保存 */
                if (!s_comp[0]) return UI_KEYBOARD_DONE;
                if (commit(0)) return UI_KEYBOARD_DONE;
                snprintf(s_notice, sizeof(s_notice), "请先选择词语，再保存");
                return UI_KEYBOARD_CHANGED;
            }
            return UI_KEYBOARD_NONE;
        }
    }
    return UI_KEYBOARD_NONE;
}

/* ============ 按压反馈 / 长按连删 / 空闲整理（保留原版调优） ============ */
static void feedback_dirty(EpdRect area) {
    s_feedback_area = s_feedback_area.width ? ui_rect_union(s_feedback_area, area) : area;
    s_dirty |= DIRTY_FEEDBACK;
}
bool ui_keyboard_release(void) {
    ui_keyboard_hold_cancel();
    if (!s_pressed) return false;
    feedback_dirty(s_pressed_area); s_pressed = false; return true;
}
/* 当前退格键位置（长按连删起点）：面板/布局不同位置不同。 */
static EpdRect backspace_rect(int top) {
    if (s_t9 && s_comp[0]) return (EpdRect){0};
    if (s_page == 2) return rect(KBD_X0 + KBD_W - num_side_w(), num_row_y(0), num_side_w(), num_row_h(0), top);
    if (nine_active()) {
        int pw, cx, cw, rx, rw;
        nine_cols(&pw, &cx, &cw, &rx, &rw);
        return rect(rx, row_y(1), rw, 84, top);
    }
    return rect(KBD_X0 + 35 * (KEY_W / 4), row_y(0), KBD_W - 35 * (KEY_W / 4), row_h(0), top);
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
    EpdRect area = backspace_rect(top);
    if (!area.width || !ui_rect_hit(area, x, y)) return;
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
    int page = s_page, layout = s_layout, syl_off = s_syllable_offset;
    bool shift = s_shift, ctrl = s_ctrl, t9 = s_t9, chinese = s_chinese;
    bool pending = s_comp[0] != 0;
    ui_keyboard_result_t result = tap(x, y, top);
    if (result != UI_KEYBOARD_CHANGED) return result == UI_KEYBOARD_NONE && s_dirty ? UI_KEYBOARD_CHANGED : result;
    if (text != stamp(s_edit->text) || cursor != s_edit->cursor) s_dirty |= DIRTY_FIELD;
    if (page != s_page || layout != s_layout || shift != s_shift || ctrl != s_ctrl ||
        t9 != s_t9 || chinese != s_chinese) s_dirty |= DIRTY_LAYOUT;
    else {
        uint32_t cand2 = 0;
        for (int i = 0; i < s_cand_count; ++i) cand2 = cand2 * 31u + stamp(s_cand[i]);
        if (comp != stamp(s_comp) || cand != cand2 || notice != stamp(s_notice) || syl_off != s_syllable_offset) {
            s_dirty |= DIRTY_WORDS;
            if (s_t9) s_dirty |= DIRTY_SYLLABLES;            /* 宫格内容随候选页变 */
        }
        if (nine_active() && comp != stamp(s_comp)) s_dirty |= DIRTY_SYLLABLES;
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
            EpdRect area = rect(24, 0, 612, 128, top);
            epd_fill_rect(area, 0xe8, fb); paint_words(fb, top); include(&update, area);
            if (update.settle) s_word_updates = 0;
            else if ((s_dirty & DIRTY_WORDS) && s_word_updates < WORDS_SETTLE_CHANGES) ++s_word_updates;
            s_words_idle_since = -1;
        }
        if (s_dirty & DIRTY_SYLLABLES) {
            if (s_t9 && s_comp[0]) {                          /* T9 宫格整块（候选+读音列） */
                EpdRect area = rect(24, t9_top(), 612, t9_h(), top);
                epd_fill_rect(area, 0xe8, fb); draw_t9_panel(fb, top); include(&update, area);
            } else if (nine_active()) {                       /* 九宫格左列 */
                int pw, cx, cw, rx, rw;
                nine_cols(&pw, &cx, &cw, &rx, &rw);
                EpdRect area = rect(KBD_X0, row_y(1), pw, 252, top);
                epd_fill_rect(area, 0xe8, fb); draw_nine_left(fb, top); include(&update, area);
            }
        }
        if (s_dirty & DIRTY_SPACE) {
            int fx[5], fw[5];
            func_row_rects(fx, fw);
            EpdRect area = rect(fx[2], row_y(4), fw[2], row_h(4), top);
            key_paint(fb, area, s_comp[0] ? "选词" : "空格", 26, false, false); include(&update, area);
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
