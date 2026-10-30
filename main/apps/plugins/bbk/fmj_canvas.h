/*
 * fmj_canvas.h - 160x96 单色画布与位图绘制
 */
#ifndef FMJ_FMJ_CANVAS_H
#define FMJ_FMJ_CANVAS_H

#include "fmj.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 画布：160x96 单字节灰度（0=白 .. 255=黑，墨量值）。
 * alpha[]: 1=该像素为 UI 有效层（输出时 4x 合成覆盖），
 *          0=该像素透明（保留下方高清素材层内容）。
 * 场景绘制（地图/角色高清贴图）时清为透明，UI（文字/边框/对话框）绘制置 alpha=1。
 */
typedef struct {
    uint8_t px[FMJ_SCREEN_H * FMJ_SCREEN_W];
    uint8_t alpha[FMJ_SCREEN_H * FMJ_SCREEN_W];
} fmj_canvas_t;

/* 解码后的图像：每像素 2bit
 * 0=白 1=黑 2=透明。行按字节对齐。可含多帧。 */
typedef struct {
    int width;
    int height;
    int number;          /* 帧数 */
    int bytes_per_row;   /* width*2bit 打包后的行字节数 */
    uint8_t *px;         /* number*height*bytes_per_row 字节（PSRAM） */
} fmj_image_t;

void fmj_canvas_clear(fmj_canvas_t *c, int color);          /* 不透明清屏（alpha=1） */
void fmj_canvas_clear_transparent(fmj_canvas_t *c);        /* 透明清屏（alpha=0） */
void fmj_canvas_set_px(fmj_canvas_t *c, int x, int y, int color);
/* 把矩形区域置为透明（alpha=0，露出高清层），用于对话框中的头像/图占位 */
void fmj_canvas_clear_alpha_rect(fmj_canvas_t *c, int x0, int y0, int x1, int y1);
int fmj_canvas_get_px(const fmj_canvas_t *c, int x, int y);

void fmj_canvas_draw_rect_fill(fmj_canvas_t *c, int x0, int y0, int x1, int y1, int color);
void fmj_canvas_draw_rect_stroke(fmj_canvas_t *c, int x0, int y0, int x1, int y1, int color);
/* 优化样式：外圈浅灰 + 内圈深墨的立体边框 */
void fmj_canvas_draw_rect_layered(fmj_canvas_t *c, int x0, int y0, int x1, int y1);
void fmj_canvas_draw_line(fmj_canvas_t *c, int x0, int y0, int x1, int y1, int color);

/* 绘制解码图像的 num 帧（num 从 1 开始，同原版 ResImage.draw）。invert 反白。 */
void fmj_canvas_draw_image(fmj_canvas_t *c, const fmj_image_t *img, int num,
                           int x, int y, bool invert);

/* 计算解码图像存储字节数 */
size_t fmj_image_store_size(int width, int height, int number);

/* 像素读取（解码图像） */
static inline int fmj_image_px(const fmj_image_t *img, int frame, int x, int y) {
    int bpr = img->bytes_per_row;
    int byte = (frame * img->height + y) * bpr + (x >> 2);
    int shift = 6 - ((x & 3) << 1);
    return (img->px[byte] >> shift) & 3;
}

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_CANVAS_H */
