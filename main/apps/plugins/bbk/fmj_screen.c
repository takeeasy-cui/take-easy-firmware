/*
 * fmj_screen.c - 横屏绘制原语（take-easy BBK 插件）
 *
 * 依赖外层旋转窗口：GameCore paint_plugin 对 landscape 插件包
 * epd_set_rotation(EPD_ROT_LANDSCAPE)，本层直接用横屏坐标写 fb。
 */
#include "fmj_screen.h"

#include <string.h>

#include "epdiy.h"
#include "fmj_font.h"

void fmj_screen_pixel(int x, int y, uint8_t v, uint8_t *fb) {
    if (x < 0 || x >= FMJ_LW || y < 0 || y >= FMJ_LH) return;
    epd_draw_pixel(x, y, v, fb);   /* 旋转由外层 paint_plugin 统一设置 */
}

void fmj_screen_fill_rect(int x0, int y0, int w, int h, uint8_t v, uint8_t *fb) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) fmj_screen_pixel(x, y, v, fb);
    }
}

void fmj_screen_frame_rect(int x0, int y0, int w, int h, uint8_t v, int lw, uint8_t *fb) {
    for (int k = 0; k < lw; ++k) {
        int x1 = x0 + w - 1 - k;
        int y1 = y0 + h - 1 - k;
        for (int x = x0 + k; x <= x1; ++x) {
            fmj_screen_pixel(x, y0 + k, v, fb);
            fmj_screen_pixel(x, y1, v, fb);
        }
        for (int y = y0 + k; y <= y1; ++y) {
            fmj_screen_pixel(x0 + k, y, v, fb);
            fmj_screen_pixel(x1, y, v, fb);
        }
    }
}

void fmj_screen_text(const uint8_t *gbk, int x, int y, int scale, uint8_t fg, uint8_t *fb) {
    if (!gbk || scale <= 0) return;
    int cx = x, i = 0;
    while (gbk[i] != 0) {
        int t = gbk[i] & 0xFF;
        const uint8_t *glyph = NULL;
        int gw = 0;
        if (t >= 0xA1) {
            glyph = fmj_font_hz_glyph(t, gbk[i + 1] & 0xFF);
            gw = 16;
            i += 2;
        } else if (t < 128) {
            glyph = fmj_font_asc_glyph(t);
            gw = 8;
            i += 1;
        } else {
            i += 1;
            cx += 8 * scale;
            continue;
        }
        if (glyph) {
            for (int row = 0; row < 16; ++row) {
                const uint8_t *line = glyph + row * (gw == 16 ? 2 : 1);
                int bits = gw == 16 ? ((line[0] << 8) | line[1]) : (line[0] << 8);
                for (int col = 0; col < gw; ++col) {
                    if ((bits >> (15 - col)) & 1) {
                        for (int dy = 0; dy < scale; ++dy)
                            for (int dx = 0; dx < scale; ++dx)
                                fmj_screen_pixel(cx + col * scale + dx,
                                                 y + row * scale + dy, fg, fb);
                    }
                }
            }
        }
        cx += gw * scale;
    }
}

int fmj_screen_text_width(const uint8_t *gbk, int scale) {
    int w = 0, i = 0;
    if (!gbk) return 0;
    while (gbk[i] != 0) {
        if ((gbk[i] & 0xFF) >= 0xA1) { w += 16; i += 2; }
        else { w += 8; i += 1; }
    }
    return w * scale;
}
