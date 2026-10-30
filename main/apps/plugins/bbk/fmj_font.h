/*
 * fmj_font.h - HZK16/ASC16 点阵字库与 GBK 文本绘制
 */
#ifndef FMJ_FMJ_FONT_H
#define FMJ_FMJ_FONT_H

#include "fmj.h"
#include "fmj_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 加载字库（约 267KB + 4KB，载入 PSRAM）。
 * path_hz 如 "/sdcard/fmj/HZK16"，path_asc 如 "/sdcard/fmj/ASC16"。
 */
int fmj_font_init(const char *path_hz, const char *path_asc);
void fmj_font_deinit(void);
bool fmj_font_ready(void);

/* 像素级绘制一个 GBK 字符串（遇 0 结尾）。
 * fg/bg 为 FMJ_BLACK/FMJ_WHITE；反白由调用者互换传入。 */
void fmj_draw_text(fmj_canvas_t *c, const uint8_t *gbk, int x, int y, int fg, int bg);

/* 绘制在矩形 (x0,y0)-(x1,y1) 内，自动折行；gbk[start] 开始。
 * 返回下一个未绘制字节的下标（用于分页）。返回值越界表示画完。 */
int fmj_draw_text_rect(fmj_canvas_t *c, const uint8_t *gbk, int start,
                       int x0, int y0, int x1, int y1, int fg, int bg);

/* GBK 字符串显示宽度（像素）；遇 0 结尾 */
int fmj_text_width(const uint8_t *gbk);

/* 卷轴字幕：整段文本按行绘制，首行在 y_start；仅绘制落在
 * [y_top, y_bottom) 内的行（超出屏顶的裁掉）。返回最后绘制的 y。 */
int fmj_draw_text_scroll(fmj_canvas_t *c, const uint8_t *gbk, int y_start,
                         int y_top, int y_bottom, int fg, int bg);

/* 居中黑框提示（原版 Util.showMessage）：每行最多 8 个汉字，最多 5 行 */
void fmj_draw_msg_box(fmj_canvas_t *c, const uint8_t *text);

/* 整数倍放大绘制文本（标题用）。返回像素宽度。 */
int fmj_draw_text_scaled(fmj_canvas_t *c, const uint8_t *gbk, int x, int y,
                         int scale, int fg, int bg);

/* 字形访问（屏幕级绘制用）：
 * 汉字返回 32 字节 16x16 点阵；非法返回 NULL。 */
const uint8_t *fmj_font_hz_glyph(int hi, int lo);
/* ASCII 返回 16 字节 8x16 点阵；超界返回 NULL。 */
const uint8_t *fmj_font_asc_glyph(int code);

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_FONT_H */
