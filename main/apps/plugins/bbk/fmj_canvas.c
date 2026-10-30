/*
 * fmj_canvas.c - 160x96 灰度画布实现
 */
#include "fmj_canvas.h"

#include <string.h>

void fmj_canvas_clear(fmj_canvas_t *c, int color) {
    memset(c->px, (uint8_t)(color & 0xFF), sizeof(c->px));
    memset(c->alpha, 1, sizeof(c->alpha));
}

void fmj_canvas_clear_transparent(fmj_canvas_t *c) {
    memset(c->px, 0, sizeof(c->px));
    memset(c->alpha, 0, sizeof(c->alpha));
}

void fmj_canvas_set_px(fmj_canvas_t *c, int x, int y, int color) {
    if (x < 0 || x >= FMJ_SCREEN_W || y < 0 || y >= FMJ_SCREEN_H) return;
    c->px[y * FMJ_SCREEN_W + x] = (uint8_t)color;
    c->alpha[y * FMJ_SCREEN_W + x] = 1;
}

void fmj_canvas_clear_alpha_rect(fmj_canvas_t *c, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= FMJ_SCREEN_H) continue;
        int sx = x0 < 0 ? 0 : x0;
        int ex = x1 >= FMJ_SCREEN_W ? FMJ_SCREEN_W - 1 : x1;
        for (int x = sx; x <= ex; ++x) c->alpha[y * FMJ_SCREEN_W + x] = 0;
    }
}

int fmj_canvas_get_px(const fmj_canvas_t *c, int x, int y) {
    if (x < 0 || x >= FMJ_SCREEN_W || y < 0 || y >= FMJ_SCREEN_H) return 0;
    return c->px[y * FMJ_SCREEN_W + x];
}

void fmj_canvas_draw_rect_fill(fmj_canvas_t *c, int x0, int y0, int x1, int y1, int color) {
    for (int y = y0; y <= y1; ++y) {
        if (y < 0 || y >= FMJ_SCREEN_H) continue;
        int sx = x0 < 0 ? 0 : x0;
        int ex = x1 >= FMJ_SCREEN_W ? FMJ_SCREEN_W - 1 : x1;
        for (int x = sx; x <= ex; ++x) {
            c->px[y * FMJ_SCREEN_W + x] = (uint8_t)color;
            c->alpha[y * FMJ_SCREEN_W + x] = 1;
        }
    }
}

void fmj_canvas_draw_rect_stroke(fmj_canvas_t *c, int x0, int y0, int x1, int y1, int color) {
    for (int x = x0; x <= x1; ++x) {
        fmj_canvas_set_px(c, x, y0, color);
        fmj_canvas_set_px(c, x, y1, color);
    }
    for (int y = y0; y <= y1; ++y) {
        fmj_canvas_set_px(c, x0, y, color);
        fmj_canvas_set_px(c, x1, y, color);
    }
}

/* 分层边框：外圈浅灰 + 内圈深墨，输出后呈现立体层次（优化版对话框样式） */
void fmj_canvas_draw_rect_layered(fmj_canvas_t *c, int x0, int y0, int x1, int y1) {
    for (int x = x0 - 1; x <= x1 + 1; ++x) {
        fmj_canvas_set_px(c, x, y0 - 1, FMJ_GRAY_FRAME);
        fmj_canvas_set_px(c, x, y1 + 1, FMJ_GRAY_FRAME);
    }
    for (int y = y0 - 1; y <= y1 + 1; ++y) {
        fmj_canvas_set_px(c, x0 - 1, y, FMJ_GRAY_FRAME);
        fmj_canvas_set_px(c, x1 + 1, y, FMJ_GRAY_FRAME);
    }
    fmj_canvas_draw_rect_stroke(c, x0, y0, x1, y1, FMJ_BLACK);
}

void fmj_canvas_draw_line(fmj_canvas_t *c, int x0, int y0, int x1, int y1, int color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        fmj_canvas_set_px(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

size_t fmj_image_store_size(int width, int height, int number) {
    size_t bpr = (size_t)((width * 2 + 7) / 8);
    return bpr * (size_t)height * (size_t)number;
}

void fmj_canvas_draw_image(fmj_canvas_t *c, const fmj_image_t *img, int num,
                           int x, int y, bool invert) {
    if (!img || !img->px) return;
    if (num < 1) num = 1;
    if (num > img->number) {
        if (img->number > 0) num = 1;      /* 原版：越界画第 1 帧 */
        else return;
    }
    int frame = num - 1;
    for (int py = 0; py < img->height; ++py) {
        int cy = y + py;
        if (cy < 0 || cy >= FMJ_SCREEN_H) continue;
        for (int px = 0; px < img->width; ++px) {
            int cx = x + px;
            if (cx < 0 || cx >= FMJ_SCREEN_W) continue;
            int v = fmj_image_px(img, frame, px, py);
            if (v == 2) continue; /* 透明 */
            int color = invert ? (v == 0 ? FMJ_BLACK : FMJ_WHITE)
                              : (v == 0 ? FMJ_WHITE : FMJ_BLACK);
            c->px[cy * FMJ_SCREEN_W + cx] = (uint8_t)color;
            c->alpha[cy * FMJ_SCREEN_W + cx] = 1;
        }
    }
}
