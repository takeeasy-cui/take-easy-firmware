/*
 * bbk_present.c - FMJ 画布差分输出层实现
 *
 * 设计（用户拍板 2026-10-28：原版点阵 + 申报制差分）：
 *  - FMJ 逻辑层全量重画 160x96 画布（零改动）→ 与 shadow 逐像素比较；
 *  - 脏 bbox 扩展到 16px tile 对齐（墨水屏友好）；
 *  - 只 6x 放大 blit 变化 fb 区域（内核 render 前会清白 fb，区域外不推屏）；
 *  - 跳屏/首帧走 reset 全量 + GC16；屏内变化只推 GL16 局部（横屏坐标 area）。
 */
#include "bbk_present.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "fmj_screen.h"

static uint8_t *s_shadow = NULL;   /* 上次已输出画布快照（PSRAM 15360B） */
static bool     s_force_full = true;

int bbk_present_init(void) {
    if (s_shadow) return 0;
    s_shadow = (uint8_t *)heap_caps_malloc(FMJ_SCREEN_W * FMJ_SCREEN_H,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_shadow) return -1;
    bbk_present_reset();
    return 0;
}

void bbk_present_deinit(void) {
    if (s_shadow) heap_caps_free(s_shadow);
    s_shadow = NULL;
}

void bbk_present_reset(void) { s_force_full = true; }

/* 差分后 6x blit 画布矩形（含端点）到 fb + 对角抗锯齿。
 * 画布墨量 0=白 255=黑 → fb 0xFF=白 0x00=黑。alpha 通道原版路线恒 1，忽略。
 * 注：外层 paint_plugin 已设 EPD_ROT_LANDSCAPE，横屏坐标直写。 */
static void blit_rect(const fmj_canvas_t *c, uint8_t *fb, int x0, int y0, int x1, int y1) {
    const int S = FMJ_HD_SCALE;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FMJ_SCREEN_W - 1) x1 = FMJ_SCREEN_W - 1;
    if (y1 > FMJ_SCREEN_H - 1) y1 = FMJ_SCREEN_H - 1;

    for (int y = y0; y <= y1; ++y) {
        const uint8_t *row = c->px + y * FMJ_SCREEN_W;
        const int by0 = FMJ_GAME_Y + y * S;
        for (int x = x0; x <= x1; ++x) {
            const uint8_t fbv = (uint8_t)(255 - row[x]);
            const int bx0 = FMJ_GAME_X + x * S;
            for (int dy = 0; dy < S; ++dy) {
                const int by = by0 + dy;
                for (int dx = 0; dx < S; ++dx)
                    fmj_screen_pixel(bx0 + dx, by, fbv, fb);
            }
        }
    }

    /* 对角边缘抗锯齿：源 2x2 窗口 a,b/c,d，a/d 反色且 b/c 同色且 a/b 异色时，
     * 在放大块十字点插中值灰（对齐 fmj_present 原版算法 2）。 */
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const uint8_t a = c->px[y * FMJ_SCREEN_W + x];
            const uint8_t b = c->px[y * FMJ_SCREEN_W + (x + 1)];
            const uint8_t cc = c->px[(y + 1) * FMJ_SCREEN_W + x];
            const uint8_t d = c->px[(y + 1) * FMJ_SCREEN_W + (x + 1)];
            const bool a_dark = a > 128, d_dark = d > 128;
            const bool b_dark = b > 128, c_dark = cc > 128;
            if (a_dark == d_dark) continue;   /* 非对角边 */
            if (b_dark != c_dark) continue;   /* 十字噪声，不动 */
            if (a_dark == b_dark) continue;   /* 与 A 同侧无过渡 */
            const uint8_t mid = (uint8_t)((((255 - a) + (255 - d)) / 2) & 0xF0);
            const int ax = FMJ_GAME_X + x * S;
            const int ay = FMJ_GAME_Y + y * S;
            if (a_dark) {
                fmj_screen_pixel(ax + S, ay + S - 1, mid, fb);
                fmj_screen_pixel(ax + S - 1, ay + S, mid, fb);
            } else {
                fmj_screen_pixel(ax + S - 1, ay + S - 1, mid, fb);
                fmj_screen_pixel(ax + S, ay + S, mid, fb);
            }
        }
    }
}

bool bbk_present(const fmj_canvas_t *c, uint8_t *fb,
                 EpdRect *out_area, bool *out_full) {
    if (!out_area || !out_full) return false;
    if (!s_shadow) { *out_full = true; return false; }

    if (s_force_full) {
        blit_rect(c, fb, 0, 0, FMJ_SCREEN_W - 1, FMJ_SCREEN_H - 1);
        memcpy(s_shadow, c->px, (size_t)FMJ_SCREEN_W * FMJ_SCREEN_H);
        s_force_full = false;
        *out_full = true;
        *out_area = (EpdRect){FMJ_GAME_X, FMJ_GAME_Y,
                              FMJ_SCREEN_W * FMJ_HD_SCALE, FMJ_SCREEN_H * FMJ_HD_SCALE};
        return true;
    }

    /* 差分：扫全画布找脏 bbox */
    int x0 = FMJ_SCREEN_W, y0 = FMJ_SCREEN_H, x1 = -1, y1 = -1;
    for (int y = 0; y < FMJ_SCREEN_H; ++y) {
        const uint8_t *n = c->px + y * FMJ_SCREEN_W;
        const uint8_t *o = s_shadow + y * FMJ_SCREEN_W;
        for (int x = 0; x < FMJ_SCREEN_W; ++x) {
            if (n[x] != o[x]) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < 0) { *out_full = false; return false; }   /* 零变化：一帧不刷 */

    /* 16px tile 对齐扩展（抗锯齿十字点需要 +1 邻域） */
    x0 = (x0 / 16) * 16;
    y0 = (y0 / 16) * 16;
    x1 = (x1 / 16) * 16 + 15; if (x1 > FMJ_SCREEN_W - 1) x1 = FMJ_SCREEN_W - 1;
    y1 = (y1 / 16) * 16 + 15; if (y1 > FMJ_SCREEN_H - 1) y1 = FMJ_SCREEN_H - 1;
    if (x0 > 0) x0 -= 1;         /* AA 十字点覆盖脏区左/上边缘 */
    if (y0 > 0) y0 -= 1;

    blit_rect(c, fb, x0, y0, x1, y1);

    /* 局部回写 shadow */
    for (int y = y0; y <= y1; ++y)
        memcpy(s_shadow + y * FMJ_SCREEN_W + x0,
               c->px + y * FMJ_SCREEN_W + x0, (size_t)(x1 - x0 + 1));

    *out_full = false;
    *out_area = (EpdRect){FMJ_GAME_X + x0 * FMJ_HD_SCALE,
                          FMJ_GAME_Y + y0 * FMJ_HD_SCALE,
                          (x1 - x0 + 1) * FMJ_HD_SCALE,
                          (y1 - y0 + 1) * FMJ_HD_SCALE};
    return true;
}
