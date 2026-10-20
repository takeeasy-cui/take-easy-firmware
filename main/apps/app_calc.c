/*
 * 计算器：四则运算，显示区 + 4×5 键盘铺满窗口（键高 140，键区直达 y≈1180）。
 * 状态机与绘制已抽到 calc_core（与计算器锁屏共享）；本页只保留页面壳。
 *
 * Calculator: four-operation shell page. The state machine and dot-matrix painting
 * live in calc_core (shared with the calculator lock); this file keeps the page shell.
 *
 * v2（用户 2026-10-19，提速）：按键不再整页重绘——键帽是静态目标，按键只改
 * 显示区，paint_display 后以 APP_REDRAW_AREA + area_hint 走 DU 局部刷新；
 * 整页路径（进页/中键强刷）保留。显示区全点阵（calc_core），DU 阈值无损。
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "calc_core.h"
#include "ui_gesture.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "display.h"
#include "read_pico_sd.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_nothing.h"

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    /* 状态栏需要字体；无字体页保留原兜底（键区本身零字体依赖）。 */
    if (!ttf_font_ready()) {
        read_pico_sd_info_t sd = { 0 };
        read_pico_sd_get_info(&sd);
        ui_draw_no_font_page(fb, sd.present, false);
        return;
    }
    calc_core_paint_page(fb);
    /* 无标题/返回键，仅状态栏（2026-10-17）。 */
    ui_nav_status(fb);
}

static void on_enter(app_ctx_t* ctx) {
    ctx->leaf = 0;
    calc_core_set_unlock_mode(false);
    calc_core_reset();
}

/* 按键只刷显示区：fb 重画后返回 AREA，area_hint 给 DU 局部刷新。 */
static EpdRect on_area(app_ctx_t* ctx) {
    (void)ctx;
    return calc_core_display_area();
}

/* 三键接管页契约：中键短按强刷、长按退出（回最近切页来源）；右键返回上一页。 */
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    if (key == UI_KEY_3) { ctx->request_return = true; return APP_REDRAW_NONE; }
    return APP_REDRAW_NONE;
}
static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_2) return APP_REDRAW_NONE;
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

static app_redraw_t gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    (void)ctx;
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const char* key = calc_core_key_at(ev->x, ev->y);
    if (!key) return APP_REDRAW_NONE;
    calc_core_press(key);
    calc_core_paint_display(ctx->fb);
    return APP_REDRAW_AREA;
}

const app_desc_t app_calc = {
    .title = "计算器 Calc",
    .detail = "四则运算",
    .enter_full = true,
    .owns_keys = true,
    .defer_middle_short = true,
    .render = render,
    .on_enter = on_enter,
    .area_hint = on_area,
    .on_gesture = gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
};
