/*
 * fmj_font.c - HZK16/ASC16 点阵字库实现（与原版 TextRender 逻辑一致）
 */
#include "fmj_font.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "fmj_datlib.h"
#include "fmj_screen.h"

static uint8_t *s_hz = NULL;   /* HZK16，区位码偏移 = (94*(hi-0xA1)+(lo-0xA1))*32 */
static uint8_t *s_asc = NULL; /* ASC16，偏移 = code*16 */

int fmj_font_init(const char *path_hz, const char *path_asc) {
    FILE *f = fopen(path_hz, "rb");
    if (!f) return -1;
    s_hz = heap_caps_malloc(262144, MALLOC_CAP_SPIRAM); /* HZK16 标准 262144 字节 */
    if (!s_hz) { fclose(f); return -2; }
    size_t got = fread(s_hz, 1, 262144, f);
    fclose(f);
    if (got < 262144) return -3;

    f = fopen(path_asc, "rb");
    if (!f) return -4;
    s_asc = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM); /* ASC16 128 字符 * 16 字节 */
    if (!s_asc) { fclose(f); return -5; }
    got = fread(s_asc, 1, 4096, f);
    fclose(f);
    if (got < 4096) return -6;
    return 0;
}

void fmj_font_deinit(void) {
    if (s_hz) { heap_caps_free(s_hz); s_hz = NULL; }
    if (s_asc) { heap_caps_free(s_asc); s_asc = NULL; }
}

bool fmj_font_ready(void) { return s_hz && s_asc; }



/* 取 GBK 双字节序列的字形；返回 false 表示空字形 */
static bool glyph_hz(int hi, int lo, const uint8_t **out) {
    if (hi < 0xA1 || lo < 0xA1) return false;
    int off = (94 * (hi - 0xA1) + (lo - 0xA1)) * 32;
    *out = s_hz + off;
    return true;
}

const uint8_t *fmj_font_hz_glyph(int hi, int lo) {
    const uint8_t *g = NULL;
    if (!s_hz) return NULL;
    if (!glyph_hz(hi, lo, &g)) return NULL;
    return g;
}

const uint8_t *fmj_font_asc_glyph(int code) {
    if (!s_asc || code < 0 || code >= 128) return NULL;
    return s_asc + code * 16;
}

int fmj_text_width(const uint8_t *gbk) {
    int w = 0, i = 0;
    if (!gbk) return 0;
    while (gbk[i] != 0) {
        if ((gbk[i] & 0xFF) >= 0xA1) { w += 16; i += 2; }
        else { w += 8; i += 1; }
    }
    return w;
}

/* ---------- 16x16 / 8x16 点阵字形绘制（逐像素写画布，与原版 TextRender 一致） */
static void draw_hz(fmj_canvas_t *c, const uint8_t *g, int x, int y, int fg, int bg) {
    for (int row = 0; row < 16; ++row)
        for (int col = 0; col < 16; ++col) {
            int bit = (g[row * 2 + (col >> 3)] >> (7 - (col & 7))) & 1;
            fmj_canvas_set_px(c, x + col, y + row, bit ? fg : bg);
        }
}

static void draw_asc(fmj_canvas_t *c, const uint8_t *g, int x, int y, int fg, int bg) {
    for (int row = 0; row < 16; ++row)
        for (int col = 0; col < 8; ++col) {
            int bit = (g[row] >> (7 - col)) & 1;
            fmj_canvas_set_px(c, x + col, y + row, bit ? fg : bg);
        }
}

void fmj_draw_text(fmj_canvas_t *c, const uint8_t *gbk, int x, int y, int fg, int bg) {
    if (!gbk || !s_hz || !s_asc) return;
    int cx = x, i = 0;
    while (gbk[i] != 0) {
        int t = gbk[i] & 0xFF;
        if (t >= 0xA1) {
            const uint8_t *g;
            if (glyph_hz(t, gbk[i + 1] & 0xFF, &g)) draw_hz(c, g, cx, y, fg, bg);
            cx += 16;
            i += 2;
        } else if (t < 128) {
            draw_asc(c, s_asc + t * 16, cx, y, fg, bg);
            cx += 8;
            i += 1;
        } else {
            cx += 8;
            i += 1;
        }
    }
}

int fmj_draw_text_scroll(fmj_canvas_t *c, const uint8_t *gbk, int y_start,
                         int y_top, int y_bottom, int fg, int bg) {
    if (!gbk || !s_hz || !s_asc) return y_start;
    int cx = 0, cy = y_start, i = 0;
    while (gbk[i] != 0) {
        int t = gbk[i] & 0xFF;
        int adv = (t >= 0xA1) ? 16 : 8;
        if (t >= 0xA1) {
            if (cx + 16 > FMJ_SCREEN_W) { cx = 0; cy += 16; }
        } else {
            if (cx + 8 > FMJ_SCREEN_W) { cx = 0; cy += 16; }
        }
        if (cy >= y_bottom) break;
        /* 逐行绘制：只在 [y_top, y_bottom) 内的行画 */
        if (cy >= y_top) {
            if (t >= 0xA1) {
                const uint8_t *g;
                if (glyph_hz(t, gbk[i + 1] & 0xFF, &g)) draw_hz(c, g, cx, cy, fg, bg);
                cx += 16; i += 2;
            } else if (t < 128) {
                draw_asc(c, s_asc + t * 16, cx, cy, fg, bg);
                cx += 8; i += 1;
            } else {
                cx += 8; i += 1;
            }
        } else {
            cx += adv; i += (t >= 0xA1) ? 2 : 1;
        }
    }
    return cy;
}

/* 居中黑框提示（原版 bmpInformationBg 样式，优化为灰度分层边框） */
void fmj_draw_msg_box(fmj_canvas_t *c, const uint8_t *text) {
    if (!text) return;
    int len = fmj_cstr_len(text, 0);
    int line_num = len / 16;
    if (line_num >= 5) line_num = 4;
    int text_y = 39 - line_num * 8;
    int bottom = text_y - 2 + 21 + 16 * line_num;
    /* 黑底 */
    fmj_canvas_draw_rect_fill(c, 11, text_y - 2, 148, bottom, FMJ_BLACK);
    /* 内白 */
    fmj_canvas_draw_rect_fill(c, 12, text_y - 1, 135, bottom - 1, FMJ_WHITE);
    /* 右侧豁口与左下阴影线（原版装饰，灰度柔和化） */
    fmj_canvas_draw_rect_fill(c, 147, text_y - 2, 148, text_y + 1, FMJ_WHITE);
    fmj_canvas_draw_line(c, 11, text_y + 19 + 16 * line_num, 13, text_y + 19 + 16 * line_num, FMJ_GRAY_AA);
    fmj_canvas_draw_line(c, 11, text_y + 20 + 16 * line_num, 13, text_y + 20 + 16 * line_num, FMJ_GRAY_AA);
    fmj_draw_text_rect(c, text, 0, 16, text_y, 16 + 16 * 8, text_y + 16 * line_num + 16,
                       FMJ_BLACK, FMJ_WHITE);
}

int fmj_draw_text_scaled(fmj_canvas_t *c, const uint8_t *gbk, int x, int y,
                         int scale, int fg, int bg) {
    if (!gbk || !s_hz || !s_asc || scale < 1) return 0;
    int cx = x, i = 0;
    while (gbk[i] != 0) {
        int t = gbk[i] & 0xFF;
        if (t >= 0xA1) {
            const uint8_t *g;
            if (glyph_hz(t, gbk[i + 1] & 0xFF, &g)) {
                for (int row = 0; row < 16; ++row)
                    for (int col = 0; col < 16; ++col) {
                        int bit = (g[row * 2 + (col >> 3)] >> (7 - (col & 7))) & 1;
                        int color = bit ? fg : bg;
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx)
                                fmj_canvas_set_px(c, cx + col * scale + sx, y + row * scale + sy, color);
                    }
            }
            cx += 16 * scale;
            i += 2;
        } else if (t < 128) {
            const uint8_t *g = s_asc + t * 16;
            for (int row = 0; row < 16; ++row)
                for (int col = 0; col < 8; ++col) {
                    int bit = (g[row] >> (7 - col)) & 1;
                    int color = bit ? fg : bg;
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx)
                            fmj_canvas_set_px(c, cx + col * scale + sx, y + row * scale + sy, color);
                }
            cx += 8 * scale;
            i += 1;
        } else {
            cx += 8 * scale;
            i += 1;
        }
    }
    return cx - x;
}

int fmj_draw_text_rect(fmj_canvas_t *c, const uint8_t *gbk, int start,
                       int x0, int y0, int x1, int y1, int fg, int bg) {
    if (!gbk || !s_hz || !s_asc) return start;
    int cx = x0, cy = y0, i = start;
    while (gbk[i] != 0 && cy <= y1 - 16) {
        int t = gbk[i] & 0xFF;
        if (t >= 0xA1) {
            if (cx + 16 > x1) { cx = x0; cy += 16; if (cy > y1 - 16) break; }
            const uint8_t *g;
            if (glyph_hz(t, gbk[i + 1] & 0xFF, &g)) draw_hz(c, g, cx, cy, fg, bg);
            cx += 16;
            i += 2;
        } else if (t < 128) {
            if (cx + 8 > x1) { cx = x0; cy += 16; if (cy > y1 - 16) break; }
            draw_asc(c, s_asc + t * 16, cx, cy, fg, bg);
            cx += 8;
            i += 1;
        } else {
            cx += 8;
            i += 1;
        }
    }
    return i;
}
