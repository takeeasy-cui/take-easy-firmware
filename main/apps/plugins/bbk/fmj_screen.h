/*
 * fmj_screen.h - 横屏绘制原语（1216x684 横屏坐标，take-easy BBK 插件）
 *
 * GameCore 对 landscape 插件自动包裹 EPD_ROT_LANDSCAPE 旋转窗口，
 * 本层所有函数直接用横屏坐标写 fb（旋转由 epdiy 内部处理）；
 * 手势坐标同样由内核 to_landscape_ev 预转。fb 灰度 0xFF=白 0x00=黑。
 */
#ifndef FMJ_FMJ_SCREEN_H
#define FMJ_FMJ_SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 横屏逻辑尺寸 = 物理竖屏旋转 90° */
#define FMJ_LW 1216
#define FMJ_LH 684

/* 游戏区布局（2026-10-30 真机反馈"整体游戏全屏"：恢复旧版 7x 大画面）：
 * 160x96 画布 x7 = 1120x672，居中 (48,6)，左右 48px 上下 6px 细白边；
 * 圆形虚拟键直接叠画在游戏画面上（左下圆盘 + 右下三键，旧版原位），
 * 对话/菜单等覆盖态自动隐藏规避遮挡。 */
#define FMJ_GAME_X 48
#define FMJ_GAME_Y 6
#define FMJ_HD_SCALE 7

/* 横屏逻辑 (x,y) 单像素写入 */
void fmj_screen_pixel(int x, int y, uint8_t v, uint8_t *fb);

/* 横屏逻辑实心矩形 */
void fmj_screen_fill_rect(int x0, int y0, int w, int h, uint8_t v, uint8_t *fb);

/* 横屏逻辑边框矩形（lw 线宽，向内画） */
void fmj_screen_frame_rect(int x0, int y0, int w, int h, uint8_t v, int lw, uint8_t *fb);

/* 横屏逻辑直接绘制放大 GBK 文本（HZK16/ASC16 点阵，scale 正整数） */
void fmj_screen_text(const uint8_t *gbk, int x, int y, int scale, uint8_t fg, uint8_t *fb);

/* GBK 文本按 scale 放大后的宽度（横屏像素） */
int fmj_screen_text_width(const uint8_t *gbk, int scale);

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_SCREEN_H */
