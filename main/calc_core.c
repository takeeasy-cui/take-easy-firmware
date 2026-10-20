/* SPDX-License-Identifier: Apache-2.0
 * 中文：计算器共享内核实现（2026-10-19 从 apps/app_calc.c 抽取）。
 * English: Shared calculator core implementation, extracted from apps/app_calc.c.
 * 状态机逐字保留原语义；表达式与主值全部点阵化（原主值已是点阵，原表达式 ui_text
 * 灰阶 TTF 改为小点阵），使显示区成为纯黑白目标——DU 阈值刷新无损且无字体依赖。
 * 边框统一 INK：原 NX8_DIM 灰边框在 DU 阈值（>=0x80→白）下会消失。
 */
#include "calc_core.h"
#include "lock_pin.h"
#include "ui_kit.h"
#include "ui_nothing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALC_ROWS 5
#define CALC_COLS 4
#define CALC_KEY_H 140
#define CALC_KEYS_Y 424
#define CALC_DISPLAY_Y 116
#define CALC_DISPLAY_H 232

static const char* const k_keys[CALC_ROWS][CALC_COLS] = {
    {"C", "⌫", "%", "÷"},
    {"7", "8", "9", "×"},
    {"4", "5", "6", "−"},
    {"1", "2", "3", "+"},
    {"±", "0", ".", "="},
};

static char s_expr[80];
static char s_cur[32] = "0";
static double s_acc;
static char s_op;          /* 0 = 无操作挂起 / no pending operation */
static int s_fresh = 1;    /* 刚按过运算符/等号 / fresh after operator or equals */
static bool s_unlock_mode;
static bool s_unlocked;

static EpdRect calc_key_rect(int r, int c) {
    return (EpdRect){NX_MARGIN + (684 - 2 * NX_MARGIN + 14) / 4 * c,
                     CALC_KEYS_Y + (CALC_KEY_H + 14) * r,
                     (684 - 2 * NX_MARGIN - 42) / 4, CALC_KEY_H};
}

EpdRect calc_core_display_area(void) {
    return (EpdRect){NX_MARGIN - 2, CALC_DISPLAY_Y - 2,
                     684 - 2 * NX_MARGIN + 4, CALC_DISPLAY_H + 4};
}

const char* calc_core_key_at(uint16_t x, uint16_t y) {
    for (int r = 0; r < CALC_ROWS; ++r)
        for (int c = 0; c < CALC_COLS; ++c)
            if (ui_rect_hit(calc_key_rect(r, c), x, y)) return k_keys[r][c];
    return NULL;
}

void calc_core_set_unlock_mode(bool on) { s_unlock_mode = on; }
bool calc_core_unlocked(void) { return s_unlocked; }
const char* calc_core_current(void) { return s_cur; }

void calc_core_reset(void) {
    s_cur[0] = '0'; s_cur[1] = 0;
    s_expr[0] = 0;
    s_acc = 0; s_op = 0; s_fresh = 1;
    s_unlocked = false;
}

static void fmt_value(double v, char* out, size_t cap) {
    if (v == (long long)v && v > -1e15 && v < 1e15) {
        snprintf(out, cap, "%lld", (long long)v);
    } else {
        snprintf(out, cap, "%.8g", v);
    }
}

/* 显示符到内部 ASCII 运算符映射：×→* −→- ÷→/ */
static char op_internal(const char* key) {
    if (!strcmp(key, "×")) return '*';
    if (!strcmp(key, "−")) return '-';
    if (!strcmp(key, "÷")) return '/';
    return key[0];
}

static double apply(double a, char op, double b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b == 0 ? 0 : a / b;
        default: return b;
    }
}

void calc_core_press(const char* key) {
    if (!strcmp(key, "C")) {
        s_cur[0] = '0'; s_cur[1] = 0;
        s_acc = 0; s_op = 0; s_fresh = 1;
        s_expr[0] = 0;
        return;
    }
    if (!strcmp(key, "⌫")) {
        size_t n = strlen(s_cur);
        if (n) {
            --n;
            while (n && ((unsigned char)s_cur[n] & 0xc0) == 0x80) --n;
            s_cur[n] = 0;
        }
        if (!s_cur[0]) { s_cur[0] = '0'; s_cur[1] = 0; }
        return;
    }
    if (!strcmp(key, "±")) {
        if (s_cur[0] == '-') memmove(s_cur, s_cur + 1, strlen(s_cur));
        else if (strcmp(s_cur, "0")) {
            char tmp[44];
            snprintf(tmp, sizeof(tmp), "-%.30s", s_cur);
            snprintf(s_cur, sizeof(s_cur), "%.31s", tmp);
        }
        return;
    }
    if (!strcmp(key, ".")) {
        if (!strchr(s_cur, '.')) strncat(s_cur, ".", sizeof(s_cur) - strlen(s_cur) - 1);
        return;
    }
    if (!strcmp(key, "%")) {
        double v = strtod(s_cur, NULL) / 100.0;
        fmt_value(v, s_cur, sizeof(s_cur));
        s_fresh = 1;
        return;
    }
    if (!strcmp(key, "+") || !strcmp(key, "−") || !strcmp(key, "×") || !strcmp(key, "÷")) {
        double cur = strtod(s_cur, NULL);
        if (s_op && !s_fresh) {
            s_acc = apply(s_acc, s_op, cur);
            fmt_value(s_acc, s_cur, sizeof(s_cur));
        } else {
            s_acc = cur;
        }
        s_op = op_internal(key);
        s_fresh = 1;
        size_t len = strlen(s_expr);
        snprintf(s_expr + len, sizeof(s_expr) - len, " %s %c ", s_cur, s_op);
        return;
    }
    if (!strcmp(key, "=")) {
        /* 计算器锁屏解锁闸（用户 2026-10-19）：恰为四位数字才尝试验证；
         * 成功置解锁标志且不改显示（解锁流程接管画面），失败/非四位继续正常计算。 */
        if (s_unlock_mode && strlen(s_cur) == 4 &&
            s_cur[0] >= '0' && s_cur[0] <= '9' && s_cur[1] >= '0' && s_cur[1] <= '9' &&
            s_cur[2] >= '0' && s_cur[2] <= '9' && s_cur[3] >= '0' && s_cur[3] <= '9') {
            char pin[5];
            memcpy(pin, s_cur, 4);
            pin[4] = 0;
            if (lock_pin_verify(pin)) {
                lock_pin_wipe(pin, sizeof(pin));
                s_unlocked = true;
                return;
            }
        }
        if (!s_op) return;
        double cur = strtod(s_cur, NULL);
        s_acc = apply(s_acc, s_op, cur);
        fmt_value(s_acc, s_cur, sizeof(s_cur));
        s_op = 0;
        s_fresh = 1;
        s_expr[0] = 0;
        return;
    }
    /* 数字键 */
    if (s_fresh) { s_cur[0] = 0; s_fresh = 0; }
    if (strlen(s_cur) < sizeof(s_cur) - 2) {
        if (!strcmp(s_cur, "0")) s_cur[0] = 0;
        strncat(s_cur, key, 1);
        if (!s_cur[0]) { s_cur[0] = '0'; s_cur[1] = 0; }
    }
}

/* 表达式点阵化：内部 ASCII 运算符映射回显示符（*→×），其余字符点阵字模直出。
 * 超宽只保留尾部（右端最近一次运算），与右对齐语义一致。 */
static void paint_expr_dots(uint8_t* fb, EpdRect display) {
    if (!s_expr[0]) return;
    char shown[96];
    size_t o = 0;
    for (size_t i = 0; s_expr[i] && o + 4 < sizeof(shown); ++i) {
        char c = s_expr[i];
        if (c == '*') { shown[o++] = (char)0xC3; shown[o++] = (char)0x97; }  /* × */
        else shown[o++] = c;
    }
    shown[o] = 0;
    const int pitch = 4, dot = 2;
    const int limit = display.width - 40;
    const char* from = shown;
    if (nothing_dot_text_w(from, pitch) > limit) {
        int bytes = (int)strlen(shown);
        while (bytes > 1 && nothing_dot_text_w(shown + strlen(shown) - bytes, pitch) > limit)
            --bytes;
        from = shown + strlen(shown) - bytes;
    }
    int w = nothing_dot_text_w(from, pitch);
    nothing_dot_text(fb, display.x + display.width - 20 - w, display.y + 14,
                     from, dot, pitch, NX8_STEEL);
}

void calc_core_paint_display(uint8_t* fb) {
    EpdRect display = {NX_MARGIN, CALC_DISPLAY_Y, 684 - 2 * NX_MARGIN, CALC_DISPLAY_H};
    ui_fill_round_rect(fb, display, NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, display, NX_R_CARD, NX8_INK);
    paint_expr_dots(fb, display);
    /* 主数值：点阵样式，字号对齐首页"继续阅读进度百分比"（dot 3 / pitch 8）。 */
    char shown[40];
    snprintf(shown, sizeof(shown), "%s", s_cur);
    int glen = (int)strlen(shown);
    int pitch = 8;
    int w_max = display.width - 40;
    while (glen > 1 && (6 * glen - 1) * pitch > w_max && pitch > 6) --pitch;
    int dot_r = 3;
    int nx = nothing_dot_text_w(shown, pitch);
    int nx0 = display.x + display.width - 20 - nx;
    nothing_dot_text(fb, nx0, display.y + (display.height - 7 * pitch) / 2,
                     shown, dot_r, pitch, NX8_INK);
}

void calc_core_paint_static(uint8_t* fb) {
    EpdRect display = {NX_MARGIN, CALC_DISPLAY_Y, 684 - 2 * NX_MARGIN, CALC_DISPLAY_H};
    ui_draw_control_frame(fb, display, NX_R_CARD, NX8_INK);
    for (int r = 0; r < CALC_ROWS; ++r) {
        for (int c = 0; c < CALC_COLS; ++c) {
            EpdRect k = calc_key_rect(r, c);
            bool accent = (c == CALC_COLS - 1) || !strcmp(k_keys[r][c], "C");
            bool solid = accent && !strcmp(k_keys[r][c], "=");
            if (solid) ui_fill_round_rect(fb, k, NX_R_SM, NX8_INK);
            else ui_draw_round_rect(fb, k, NX_R_SM, NX8_INK);
            /* 键帽：点阵字形，尺寸对齐首页温度（dot 3 / pitch 6）。 */
            const int kp = 6, kr = 3;
            int kw = nothing_dot_text_w(k_keys[r][c], kp);
            int kx = k.x + (k.width - kw) / 2;
            int ky = k.y + (k.height - 7 * kp) / 2;
            nothing_dot_text(fb, kx, ky, k_keys[r][c], kr, kp, solid ? NX8_PAPER : NX8_INK);
        }
    }
}

void calc_core_paint_page(uint8_t* fb) {
    ui_clear_page(fb);
    calc_core_paint_static(fb);
    calc_core_paint_display(fb);
}
