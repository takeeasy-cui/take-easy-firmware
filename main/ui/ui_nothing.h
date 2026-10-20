/*
 * Nothing 风格 UI 设计系统（Read Pico · take-easy）。
 * 令牌、状态栏、底部导航、点阵字模、点阵天气图样、卡片与 chips。
 * 严格对应设计稿《ReadPico-Nothing风格UI设计.html》：
 * Paper #FFF / Ink #0A0A0A / Grid #D9D9D9 / Hair #C2C2C2 / Dim #8A8A8A / Steel #555555。
 *
 * Nothing-style UI kit: tokens, status bar, bottom nav, dot-matrix glyphs,
 * linear weather icon, cards and chips.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epdiy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 设计令牌 / Design tokens（epdiy 8 位灰，仅高 4 位有效） ---- */
#define NX8_PAPER 0xFF  /* #FFFFFF */
#define NX8_INK   0x00  /* #0A0A0A */
#define NX8_STEEL 0x55 /* #555555 */
#define NX8_DIM   0x8A /* #8A8A8A */
#define NX8_HAIR  0xC2 /* #C2C2C2 */
#define NX8_GRID  0xD9 /* #D9D9D9 */
#define NX8_SCRIM 0xEE /* 卡片下半遮挡区底色（模拟 88% 白） */

/* ---- 布局常量 / Layout ---- */
#define NX_MARGIN    48
#define NX_STATUS_H  40          /* V3.7：状态栏 30→40，take-easy 19px */
#define NX_STATUS_Y  28          /* 状态栏行顶 */
#define NX_RULE_Y    (NX_STATUS_Y + NX_STATUS_H + 2) /* 状态栏下黑 hairline 位置 */
#define NX_NAV_H     115         /* V3.7：导航 96→115（+1/5） */
#define NX_NAV_TOP   (1216 - NX_NAV_H)
#define NX_DISC      83          /* 导航盘边长 69→83 */
#define NX_DISC_R    21
#define NX_DISC_ICON 54          /* 盘内图标逻辑尺寸 45→54 */
#define NX_R_CARD    14
#define NX_R_SM      10
#define NX_R_CHIP    8

/* ---- 状态栏 / Status bar ---- */
/// 画状态栏：左上身份（设置的头像+设备名，无自定义头像时内嵌狐狸），右侧电池。
void nothing_draw_statusbar(uint8_t* fb);
/// 默认身份头像：内嵌狐狸展开为 82x82 8bpp 灰度（首调用懒展开，设置页共用）。
const uint8_t* nothing_default_avatar82(void);
/// 状态栏身份头像是否解码中（首页 tick 据此安排局部重画，不阻塞渲染）。
bool nothing_statusbar_avatar_pending(void);

/* ---- 底部导航 / Bottom nav ---- */
/// 四键：0=首页 1=书架 2=应用 3=设置。阅读页不画导航。
void nothing_draw_nav(uint8_t* fb, int active);
/// 命中返回 0..3，未命中返回 -1。
int nothing_nav_hit(uint16_t x, uint16_t y);

/* ---- 状态栏头像 ---- */
/// 绘制状态栏同款头像（34px，引用设置头像或内嵌默认狐狸）。首页小节行复用。
void nothing_blit_status_avatar(uint8_t* fb, int x, int y);

/* ---- 点阵字模 / Dot-matrix glyphs（5×7） ---- */
/// 在 (x, y_top) 起画点阵文字（支持 0-9 : % / °）。返回下一个字符 x。
int nothing_dot_text(uint8_t* fb, int x, int y_top, const char* str,
                     int dot_r, int pitch, uint8_t color);
int nothing_dot_text_w(const char* str, int pitch);
/// 横屏点阵文字（v1.8 时钟锁屏用）：逻辑 (x, y_top) 为横屏 1216×684 坐标，
/// 每个圆点写物理 epd_draw_pixel(683-y, x)（同 FMJ 横屏链路；圆点旋转不变）。
/// Dot text in landscape coordinates via the FMJ-style pixel mapping.
int nothing_dot_text_rot(uint8_t* fb, int x, int y_top, const char* str,
                         int dot_r, int pitch, uint8_t color);

/* ---- 粗线与几何 / Thick geometry ---- */
/// 圆头粗线：沿线每步画填充圆，宽 w。
void nothing_thick_line(uint8_t* fb, int x0, int y0, int x1, int y1,
                        int w, uint8_t color);
/// 粗描边闭合多边形：顶点圆头连接（round join）。
void nothing_thick_poly(uint8_t* fb, const int* pts, int n, int w, uint8_t color);

/* ---- 点阵天气图样 / Weather icons（Feather 公版造型几何化） ---- */
/// 线性太阳：中心盘 + 8 条放射线，边长 size 见方（建议 120），左上角 (x, y)。
void nothing_draw_sun(uint8_t* fb, int x, int y, int size);
/// 按实际天气码画图标（0晴 1多云 2阴 3雾 4雨 5雪，未知回退太阳），
/// 几何与太阳同规格：size 见方，左上角 (x, y)。weather_code_t 见 weather_service.h。
void nothing_draw_weather(uint8_t* fb, int x, int y, int size, int code);

/* ---- 卡片 / Cards ---- */
/// 书卡（首页 2:3 版，高 rect.height）：封面色块 + 书名，下半遮挡区显示书名/进度/百分比。
/// cover_dark：封面黑底白字，否则灰底黑字。返回遮挡区矩形。
EpdRect nothing_draw_book_card(uint8_t* fb, EpdRect rect, const char* name,
                                unsigned pct, bool has_pct, bool cover_dark,
                                bool focused);
/// 应用页圆形盘图标（69px）+ 下方名称行。icon: 0=文件 1=传输 2=微读 3=计算器。
void nothing_draw_app_disc(uint8_t* fb, int x, int y, int icon);
/// 筛选 chip：圆角胶囊，选中加右下 steel 方点。
void nothing_draw_chip(uint8_t* fb, EpdRect rect, const char* label, bool on);
/// 底部虚线预留框（“更多应用正在接入”）。
void nothing_draw_dashed_box(uint8_t* fb, EpdRect rect, const char* label);

#ifdef __cplusplus
}
#endif
