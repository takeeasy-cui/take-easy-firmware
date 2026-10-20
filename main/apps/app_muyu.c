/*
 * 木鱼（用户 2026-10-20 第二十一批 c 段；交互参考小纸官方文档
 * rand_0/start/features/merit.mdx + 本机触摸习惯融合）：
 *  - 点击木鱼：功德 +1，短促「笃」声（蜂鸣器）；
 *  - 功德计数用时间同款点阵数字（用户 2026-10-20 拍板）；
 *  - 长按木鱼：重置确认（点阵风格几何自绘木鱼：圆身 + 前开口 + 圆点眼 + 槌）；
 *  - 计数持久化 NVS（muyu_c，u32）；官方「设定→重置木鱼计数」在此内置为长按。
 * 21f2（2026-10-20 用户拍板）：音效开关 + 音效选择——主页底部全宽按钮
 *  「音效 · XXX」点击循环切换，切换即试听；首页 widget 敲击共用同一设置。
 * 21g（真机反馈）：曲谱/复音异步任务音效连续切换试听死机，删除——只留单音节
 *  关/笃/尖锐/低沉（全 tone 阻塞直调）；中键长按=退出回首页；功德计数点击只改
 *  内存、退出统一落盘（根治每次点击写 NVS 卡顿）。
 */
#include <stdio.h>

#include "app.h"
#include "app_muyu.h"
#include "read_pico_buzzer.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_nothing.h"

#define MUYU_DISPLAY_CAP 999999

/* 木鱼 + 槌热区（绘制几何与命中共用）。 */
static EpdRect muyu_zone(void) { return (EpdRect){120, 470, 460, 380}; }
/* 重置确认按钮。 */
static EpdRect confirm_btn(bool accept) { return (EpdRect){accept ? 36 : 352, 940, 296, 84}; }
/* 21f2：音效选择按钮（全宽，点击循环切换并试听）。 */
static EpdRect snd_btn(void) { return (EpdRect){36, 940, 612, 84}; }

static bool s_confirm_reset;
/* 21f：敲击局部自绘计数区（定义在 on_gesture 之后，先声明）。 */
static void muyu_paint_count(uint8_t* fb);

/* ---- 21g：音效选择（全单音节 tone 直调）----
 * 21f2 版含曲谱（score_play）/复音（1bit RMT）异步任务音效，真机连续切换试听
 * 死机，用户拍板删除；只留单音节，全部 read_pico_buzzer_tone 阻塞直调。 */
const char *app_muyu_snd_name(uint8_t snd) {
    static const char *const k_names[4] = {"关", "笃", "尖锐", "低沉"};
    return snd < 4 ? k_names[snd] : k_names[0];
}

void app_muyu_snd_play(void) {
    switch (app_settings_muyu_snd()) {
        case 1: read_pico_buzzer_tone(1800, 80); break;   /* 笃（21c 默认单音） */
        case 2: read_pico_buzzer_tone(4000, 80); break;   /* 尖锐 */
        case 3: read_pico_buzzer_tone(1500, 80); break;   /* 低沉 */
        default: break;                                  /* 关 */
    }
}

static void draw_muyu(uint8_t* fb) {
    /* 圆身（双线描边）+ 前开口（右侧白块擦出）+ 圆点眼 + 斜置槌。 */
    const int cx = 300, cy = 660;
    epd_draw_circle(cx, cy, 96, NX8_INK, fb);
    epd_draw_circle(cx, cy, 94, NX8_INK, fb);
    ui_fill_rect_fast((EpdRect){cx + 40, cy - 70, 70, 140}, 0xFF, fb);
    nothing_thick_line(fb, cx + 40, cy - 70, cx + 40, cy + 70, 3, NX8_INK);
    epd_fill_circle(cx - 22, cy - 58, 9, NX8_INK, fb);
    nothing_thick_line(fb, cx + 100, cy + 90, cx + 190, cy + 150, 10, NX8_INK);
    epd_fill_circle(cx + 196, cy + 155, 24, NX8_INK, fb);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    /* 21f（2026-10-20 真机反馈）：进入木鱼页不清屏，旧页面内容残留——补清屏。 */
    ui_clear_page(fb);
    ui_text(fb, 36, 54, 40, "木鱼", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 116, 36, 612, NX8_GRID);
    ui_text_vc(fb, 342, 200, 28, "功德", EPD_DRAW_ALIGN_CENTER, false);
    uint32_t count = app_settings_muyu_count();
    if (count > MUYU_DISPLAY_CAP) count = MUYU_DISPLAY_CAP;
    char cs[16];
    snprintf(cs, sizeof(cs), "%u", (unsigned)count);
    const int pitch = 30, dr = 13;
    int w = nothing_dot_text_w(cs, pitch);
    nothing_dot_text(fb, (684 - w) / 2, 240, cs, dr, pitch, NX8_INK);
    draw_muyu(fb);
    ui_text_vc(fb, 342, 890, 24, "点击木鱼 · 长按重置计数", EPD_DRAW_ALIGN_CENTER, false);
    /* 21f2：音效选择按钮（显示当前音效名，点击循环切换）。 */
    char slabel[40];
    snprintf(slabel, sizeof(slabel), "音效 · %s",
             app_muyu_snd_name(app_settings_muyu_snd()));
    ui_draw_button(fb, snd_btn(), slabel, false);
    if (s_confirm_reset) {
        /* 重置确认（便签删除确认同款：白底卡 + 双按钮）。 */
        EpdRect card = {66, 880, 552, 180};
        ui_fill_round_rect(fb, card, NX_R_CARD, 0xFF);
        ui_draw_control_frame(fb, card, NX_R_CARD, NX8_INK);
        ui_text_vc(fb, 342, 910, 30, "功德清零，重新开始？", EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, confirm_btn(true), "确认重置", false);
        ui_draw_button(fb, confirm_btn(false), "取消", false);
    }
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_confirm_reset) {
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (ui_rect_hit(confirm_btn(true), ev->x0, ev->y0)) {
            app_settings_set_muyu_count(0);
            s_confirm_reset = false;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(confirm_btn(false), ev->x0, ev->y0)) {
            s_confirm_reset = false;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP && ui_rect_hit(snd_btn(), ev->x0, ev->y0)) {
        /* 21g：音效循环 关→笃→尖锐→低沉→关，切换即试听（全单音节，无死机源）。 */
        app_settings_set_muyu_snd((uint8_t)((app_settings_muyu_snd() + 1) % 4));
        app_muyu_snd_play();
        return APP_REDRAW_PAGE;
    }
    if (!ui_rect_hit(muyu_zone(), ev->x0, ev->y0)) return APP_REDRAW_NONE;
    if (ev->type == UI_GESTURE_TAP) {
        app_settings_set_muyu_count(app_settings_muyu_count() + 1);
        app_muyu_snd_play();               /* 21f2：按当前音效设置发声（含关） */
        /* 21f：APP_REDRAW_AREA 只推屏不调 render，回调自己画计数区再返回。 */
        muyu_paint_count(ctx->fb);
        return APP_REDRAW_AREA;
    }
    if (ev->type == UI_GESTURE_LONG_PRESS) {
        s_confirm_reset = true;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (s_confirm_reset) {
        if (key == UI_KEY_2 || key == UI_KEY_3) { s_confirm_reset = false; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_3) { ctx->request_return = true; return APP_REDRAW_NONE; }
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    return APP_REDRAW_NONE;          /* 左键无绑定 → 全局快捷菜单 */
}

static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    /* 21h（真机反馈）：中键长按=退出（request_return 恒语义，同计算器，返回来源页；
     * 21g 误用 ui_nav_request(ctx,0)，那是跳首页不是退出）。 */
    if (key == UI_KEY_2) ctx->request_return = true;
    return APP_REDRAW_NONE;
}

/* 21f：敲击自绘计数区（APP_REDRAW_AREA 框架只推屏不调 render，回调自己画）。 */
static void muyu_paint_count(uint8_t* fb) {
    epd_fill_rect((EpdRect){36, 230, 612, 230}, NX8_PAPER, fb);
    uint32_t count = app_settings_muyu_count();
    if (count > MUYU_DISPLAY_CAP) count = MUYU_DISPLAY_CAP;
    char cs[16];
    snprintf(cs, sizeof(cs), "%u", (unsigned)count);
    const int pitch = 30, dr = 13;
    int w = nothing_dot_text_w(cs, pitch);
    nothing_dot_text(fb, (684 - w) / 2, 240, cs, dr, pitch, NX8_INK);
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    /* 21f：只推功德计数条带（与 muyu_paint_count 的擦画区一致）。 */
    return (EpdRect){36, 230, 612, 230};
}

static void muyu_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    s_confirm_reset = false;
    app_settings_muyu_flush();   /* 21g：功德计数点击只改内存，退出时统一落盘。 */
}
static bool always_visible(app_ctx_t* ctx) { (void)ctx; return !s_confirm_reset; }

const app_desc_t app_muyu = {
    .title = "木鱼",
    .detail = "敲一声，攒一点功德",
    .enter_full = true,
    .owns_keys = true,
    .main_page_visible = always_visible,
    .on_exit = muyu_on_exit,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .area_hint = area_hint,
};
