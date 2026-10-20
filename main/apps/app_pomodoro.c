/*
 * 番茄时钟（用户 2026-10-20 第二十一批 c 段；交互参考小纸官方文档
 * rand_0/start/features/pomodoro.mdx + 本机触摸/三键习惯融合）：
 *  - 标准 25/5：专注与休息自动交替，直到离开本页；阶段结束播放短提醒音；
 *  - 时长可调：专注 15..60、休息 5..60，步进 5（节奏设置页）；
 *  - 主页点阵大数字显示剩余分钟，每分钟局部 DU 刷新一次（数字用时间点阵样式，
 *    用户 2026-10-20 拍板与首页/锁屏时间同款）；
 *  - 离开页面 = 暂停（保留剩余与阶段）；锁屏浅睡不暂停（esp_timer 走时），
 *    回页后 on_tick 自动结算跨过的阶段；
 *  - 双入口：首页点击时间区 / 应用页图标（推理游戏前，游戏殿后）；
 *  - 闹钟设置入口在本页（用户拍板）：PMU 硬件闹钟，关机也会响，独立于锁屏模式。
 */
#include <stdio.h>
#include <string.h>

#include "alarm.h"
#include "app.h"
#include "display.h"
#include "esp_timer.h"
#include "read_pico_buzzer.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nothing.h"

typedef enum { VIEW_MAIN = 0, VIEW_ALARM, VIEW_SETUP } pomo_view_t;
typedef enum { RUN_IDLE = 0, RUN_FOCUS, RUN_BREAK } pomo_run_t;

static pomo_view_t s_view;
static pomo_run_t s_run;
static bool s_paused;
static int64_t s_end_us;          /* 运行中：本阶段结束时刻（esp_timer us） */
static int64_t s_pause_left_us;   /* 暂停时剩余 us */
static int s_done;                /* 本次会话完成番茄数 */
static int s_show_min = -1;       /* 上次上屏的分钟数 */
static int s_alarm_h, s_alarm_m;  /* 闹钟编辑缓冲 */
static int s_edit_w, s_edit_b;    /* 节奏编辑缓冲 */
static char s_notice[80];

/* ---- 布局（绘制与命中共用一套几何）---- */
static EpdRect btn_row(int row) { return (EpdRect){36, 720 + row * 110, 296, 84}; }
static EpdRect btn_row_r(int row) { return (EpdRect){352, 720 + row * 110, 296, 84}; }
/* 闹钟/设置子页的独立按钮带。 */
static EpdRect sub_btn(int row, bool right) {
    return (EpdRect){right ? 352 : 36, 480 + row * 110, 296, 84};
}

static int pomo_remaining_min(void) {
    if (s_run == RUN_IDLE) return app_settings_pomo_work();
    int64_t left = s_paused ? s_pause_left_us : s_end_us - esp_timer_get_time();
    if (left < 0) left = 0;
    return (int)((left + 59999999) / 60000000);   /* 向上取整：秒级剩余显示分钟 */
}

static void pomo_chime(void) {
    /* 阶段切换提示音（官方文档「短暂提醒音」）：单声 300ms。 */
    read_pico_buzzer_tone(2400, 300);
}

static const char* pomo_state_label(void) {
    if (s_run == RUN_IDLE) return "准备开始";
    if (s_paused) return s_run == RUN_FOCUS ? "专注已暂停" : "休息已暂停";
    return s_run == RUN_FOCUS ? "专注中" : "休息中";
}

/* ---- 主页 ---- */
static void render_main(uint8_t* fb) {
    ui_text(fb, 36, 54, 40, "番茄时钟", EPD_DRAW_ALIGN_LEFT, false);
    ui_text_fixed(fb, 648, 66, 24, app_settings_alarm_enabled() ? "闹钟开" : "闹钟关",
                  EPD_DRAW_ALIGN_RIGHT, false);
    ui_hairline(fb, 116, 36, 612, NX8_GRID);
    ui_text_vc(fb, 342, 210, 34, pomo_state_label(), EPD_DRAW_ALIGN_CENTER, false);
    char cs[8];
    snprintf(cs, sizeof(cs), "%d", pomo_remaining_min());
    const int pitch = 36, dr = 16;
    int w = nothing_dot_text_w(cs, pitch);
    nothing_dot_text(fb, (684 - w) / 2, 250, cs, dr, pitch, NX8_INK);
    ui_text_vc(fb, 342, 548, 24, "分钟", EPD_DRAW_ALIGN_CENTER, false);
    char done[48];
    snprintf(done, sizeof(done), "本次已完成 %d 个番茄", s_done);
    ui_text_vc(fb, 342, 612, 26, done, EPD_DRAW_ALIGN_CENTER, false);
    if (s_notice[0]) ui_text_vc(fb, 342, 660, 24, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    const char* main_label = s_run == RUN_IDLE ? "开始专注"
                             : s_paused ? "继续" : "暂停";
    ui_draw_button(fb, btn_row(0), main_label, false);
    ui_draw_button(fb, btn_row_r(0), "重置", false);
    ui_draw_button(fb, btn_row(1), "闹钟", app_settings_alarm_enabled());
    ui_draw_button(fb, btn_row_r(1), "节奏设置", false);
    ui_text_vc(fb, 342, 1010, 22,
               "专注与休息自动交替 · 阶段结束有提示音", EPD_DRAW_ALIGN_CENTER, false);
}

static void pomo_reset(void) {
    s_run = RUN_IDLE;
    s_paused = false;
    s_done = 0;
    s_show_min = -1;
    s_notice[0] = 0;
}

static app_redraw_t main_tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (ui_rect_hit(btn_row(0), x, y)) {
        if (s_run == RUN_IDLE) {
            s_run = RUN_FOCUS;
            s_end_us = esp_timer_get_time() + (int64_t)app_settings_pomo_work() * 60000000LL;
            s_paused = false;
            s_show_min = -1;
            snprintf(s_notice, sizeof(s_notice), "专注 %d 分钟，加油",
                     app_settings_pomo_work());
        } else if (s_paused) {
            s_end_us = esp_timer_get_time() + s_pause_left_us;
            s_paused = false;
            s_show_min = -1;
            s_notice[0] = 0;
        } else {
            s_pause_left_us = s_end_us - esp_timer_get_time();
            if (s_pause_left_us < 0) s_pause_left_us = 0;
            s_paused = true;
            s_show_min = -1;
            snprintf(s_notice, sizeof(s_notice), "已暂停");
        }
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(btn_row_r(0), x, y)) { pomo_reset(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(btn_row(1), x, y)) {
        s_view = VIEW_ALARM;
        s_alarm_h = app_settings_alarm_hour();
        s_alarm_m = app_settings_alarm_minute();
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(btn_row_r(1), x, y)) {
        s_view = VIEW_SETUP;
        s_edit_w = app_settings_pomo_work();
        s_edit_b = app_settings_pomo_break();
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

/* ---- 闹钟子页 ---- */
static void render_alarm(uint8_t* fb) {
    ui_text(fb, 36, 54, 40, "闹钟", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 116, 36, 612, NX8_GRID);
    char cs[8];
    snprintf(cs, sizeof(cs), "%02d:%02d", s_alarm_h, s_alarm_m);
    const int pitch = 24, dr = 11;
    int w = nothing_dot_text_w(cs, pitch);
    nothing_dot_text(fb, (684 - w) / 2, 190, cs, dr, pitch, NX8_INK);
    char st[64];
    if (app_settings_alarm_enabled())
        snprintf(st, sizeof(st), "已开启 · 每天 %02d:%02d 响",
                 app_settings_alarm_hour(), app_settings_alarm_minute());
    else
        snprintf(st, sizeof(st), "未开启");
    ui_text_vc(fb, 342, 400, 26, st, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, sub_btn(0, false), "时 −", false);
    ui_draw_button(fb, sub_btn(0, true), "时 +", false);
    ui_draw_button(fb, sub_btn(1, false), "分 −", false);
    ui_draw_button(fb, sub_btn(1, true), "分 +", false);
    ui_draw_button(fb, sub_btn(2, false), app_settings_alarm_enabled() ? "关闭闹钟" : "开启闹钟",
                   app_settings_alarm_enabled());
    ui_draw_button(fb, sub_btn(2, true), "返回", false);
    ui_text_vc(fb, 342, 900, 22, "闹钟由电源芯片独立计时 · 关机也会响",
               EPD_DRAW_ALIGN_CENTER, false);
    if (s_notice[0]) ui_text_vc(fb, 342, 950, 24, s_notice, EPD_DRAW_ALIGN_CENTER, false);
}

static app_redraw_t alarm_tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)ctx;
    if (ui_rect_hit(sub_btn(0, false), x, y)) s_alarm_h = (s_alarm_h + 23) % 24;
    else if (ui_rect_hit(sub_btn(0, true), x, y)) s_alarm_h = (s_alarm_h + 1) % 24;
    else if (ui_rect_hit(sub_btn(1, false), x, y)) s_alarm_m = (s_alarm_m + 59) % 60;
    else if (ui_rect_hit(sub_btn(1, true), x, y)) s_alarm_m = (s_alarm_m + 1) % 60;
    else if (ui_rect_hit(sub_btn(2, false), x, y)) {
        bool on = app_settings_alarm_enabled();
        if (app_settings_set_alarm(!on, (uint8_t)s_alarm_h, (uint8_t)s_alarm_m) != ESP_OK
            || alarm_pmu_arm() != ESP_OK) {
            snprintf(s_notice, sizeof(s_notice), "闹钟设置失败，请重试");
        } else {
            snprintf(s_notice, sizeof(s_notice), on ? "闹钟已关闭" : "闹钟已开启");
        }
        return APP_REDRAW_PAGE;
    }
    else if (ui_rect_hit(sub_btn(2, true), x, y)) {
        s_view = VIEW_MAIN;
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    } else {
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_PAGE;   /* 时/分步进：整页轻刷（子页面积小，无局部优化必要） */
}

/* ---- 节奏设置子页 ---- */
static void render_setup(uint8_t* fb) {
    ui_text(fb, 36, 54, 40, "节奏设置", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 116, 36, 612, NX8_GRID);
    ui_text_vc(fb, 342, 170, 28, "专注", EPD_DRAW_ALIGN_CENTER, false);
    char cw[8];
    snprintf(cw, sizeof(cw), "%d", s_edit_w);
    const int pitch = 22, dr = 10;
    int w = nothing_dot_text_w(cw, pitch);
    nothing_dot_text(fb, (684 - w) / 2, 210, cw, dr, pitch, NX8_INK);
    ui_text_vc(fb, 342, 380, 22, "分钟", EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, sub_btn(0, false), "专注 −", false);
    ui_draw_button(fb, sub_btn(0, true), "专注 +", false);
    ui_text_vc(fb, 342, 490, 28, "休息", EPD_DRAW_ALIGN_CENTER, false);
    char cb[8];
    snprintf(cb, sizeof(cb), "%d", s_edit_b);
    w = nothing_dot_text_w(cb, pitch);
    nothing_dot_text(fb, (684 - w) / 2, 530, cb, dr, pitch, NX8_INK);
    ui_text_vc(fb, 342, 700, 22, "分钟", EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, sub_btn(1, false), "休息 −", false);
    ui_draw_button(fb, sub_btn(1, true), "休息 +", false);
    ui_draw_button(fb, sub_btn(2, false), "保存", false);
    ui_draw_button(fb, sub_btn(2, true), "返回", false);
    ui_text_vc(fb, 342, 900, 22, "专注 15..60 · 休息 5..60 · 步进 5",
               EPD_DRAW_ALIGN_CENTER, false);
    if (s_notice[0]) ui_text_vc(fb, 342, 950, 24, s_notice, EPD_DRAW_ALIGN_CENTER, false);
}

static app_redraw_t setup_tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)ctx;
    if (ui_rect_hit(sub_btn(0, false), x, y)) { if (s_edit_w > 15) s_edit_w -= 5; }
    else if (ui_rect_hit(sub_btn(0, true), x, y)) { if (s_edit_w < 60) s_edit_w += 5; }
    else if (ui_rect_hit(sub_btn(1, false), x, y)) { if (s_edit_b > 5) s_edit_b -= 5; }
    else if (ui_rect_hit(sub_btn(1, true), x, y)) { if (s_edit_b < 60) s_edit_b += 5; }
    else if (ui_rect_hit(sub_btn(2, false), x, y)) {
        esp_err_t err = app_settings_set_pomo((uint8_t)s_edit_w, (uint8_t)s_edit_b);
        snprintf(s_notice, sizeof(s_notice),
                 err == ESP_OK ? "已保存，下一轮生效" : "保存失败，请重试");
        return APP_REDRAW_PAGE;
    }
    else if (ui_rect_hit(sub_btn(2, true), x, y)) {
        s_view = VIEW_MAIN;
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    } else {
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_PAGE;
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (s_view == VIEW_ALARM) render_alarm(fb);
    else if (s_view == VIEW_SETUP) render_setup(fb);
    else render_main(fb);
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (s_view == VIEW_ALARM) return alarm_tap(ctx, ev->x0, ev->y0);
    if (s_view == VIEW_SETUP) return setup_tap(ctx, ev->x0, ev->y0);
    return main_tap(ctx, ev->x0, ev->y0);
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    /* 右键：子页回主页；主页返回切页来源（首页时间区 / 应用页）。 */
    if (key == UI_KEY_3) {
        if (s_view != VIEW_MAIN) {
            s_view = VIEW_MAIN;
            s_notice[0] = 0;
            return APP_REDRAW_PAGE;
        }
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    return APP_REDRAW_NONE;          /* 左键无绑定 → 全局快捷菜单 */
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    (void)ctx;
    if (s_view != VIEW_MAIN || s_run == RUN_IDLE || s_paused) return APP_REDRAW_NONE;
    int64_t now = esp_timer_get_time();
    if (now >= s_end_us) {
        /* 阶段自动交替（官方语义）；锁屏期间跨过的阶段一次性结算到最新状态。 */
        while (now >= s_end_us) {
            if (s_run == RUN_FOCUS) {
                ++s_done;
                s_run = RUN_BREAK;
                s_end_us += (int64_t)app_settings_pomo_break() * 60000000LL;
                snprintf(s_notice, sizeof(s_notice), "专注完成，休息 %d 分钟",
                         app_settings_pomo_break());
            } else {
                s_run = RUN_FOCUS;
                s_end_us += (int64_t)app_settings_pomo_work() * 60000000LL;
                snprintf(s_notice, sizeof(s_notice), "休息结束，继续专注");
            }
            pomo_chime();
        }
        s_show_min = -1;
        return APP_REDRAW_PAGE;
    }
    int m = pomo_remaining_min();
    if (m != s_show_min) {
        s_show_min = m;
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    /* 主页分钟翻转只刷状态+点阵数字+单位行（DU 局部，与首页时钟行同款机制）。 */
    return (EpdRect){36, 180, 612, 420};
}

static void pomo_on_enter(app_ctx_t* ctx) {
    (void)ctx;
    /* 离开页面即暂停（保留剩余与阶段）；回来显示暂停态，由用户「继续」。 */
    if (s_run != RUN_IDLE && !s_paused) {
        s_pause_left_us = s_end_us - esp_timer_get_time();
        if (s_pause_left_us < 0) s_pause_left_us = 0;
        s_paused = true;
    }
    s_show_min = -1;
}

static void pomo_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    /* 同 on_enter：离开即暂停，状态 static 保留，下次进入继续。 */
    if (s_run != RUN_IDLE && !s_paused) {
        s_pause_left_us = s_end_us - esp_timer_get_time();
        if (s_pause_left_us < 0) s_pause_left_us = 0;
        s_paused = true;
    }
    s_notice[0] = 0;
}

static bool main_visible(app_ctx_t* ctx) { (void)ctx; return s_view == VIEW_MAIN; }

const app_desc_t app_pomodoro = {
    .title = "番茄时钟",
    .detail = "专注与休息的节奏",
    .enter_full = true,
    .owns_keys = true,
    .main_page_visible = main_visible,
    .on_enter = pomo_on_enter,
    .on_exit = pomo_on_exit,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
