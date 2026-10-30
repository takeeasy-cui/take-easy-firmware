/*
 * gstory.c — 文绎合体入口（2026-10-30 用户拍板）。
 *
 * 应用页单入口「文绎」取代此前的「对话游戏」「互动小说」双 tile：
 *  - 列表用 GC_CAT_STORY 复合分类（game_pkg 同时扫 text-/novel- 两前缀），
 *    行尾徽标区分 游戏/小说；
 *  - 点选后按挂载包子类型（gc_pkg_mounted_kind）把 launch/paint/input/key/
 *    tick/quit 原样转发给对应子插件（gtext 对话框流 / gnovel 分页阅读），
 *    包内体验零变化——g->state 直属子插件，转发零拷贝。
 *
 * 交互改造（同日用户指令）：游戏内中短键弹居中菜单——存档/读档/退出/
 * 刷新屏幕；点菜单外或再按中键关闭。退出=先存档再回应用页；刷新=整屏
 * GC16 强刷（吸收原中键强刷语义）。中长键=退出、右键语义保持内核恒语义。
 *
 * 实现注：内核互斥保证单实例，覆盖层（菜单/toast）状态用文件内静态即可。
 */
#include <stdio.h>
#include <string.h>

#include "epdiy.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#include "game_core.h"
#include "plugins/gtext.h"
#include "plugins/gnovel.h"

/* ---- 菜单几何（绘制与命中共用；Nothing 卡片语言同列表页） ---- */
#define GSM_ROW_X    166
#define GSM_ROW_W    352
#define GSM_ROW_H    64
#define GSM_ROW_Y0   448
#define GSM_ROW_STEP 84

static const char* const k_menu_rows[4] = {"存档", "读档", "退出", "刷新屏幕"};

/* 覆盖层状态（单实例；kind 在 launch 时由挂载包写入）。 */
static struct {
    gc_cat_t    kind;          /* GC_CAT_TEXT / GC_CAT_NOVEL */
    bool        menu_open;
    bool        toast_show;
    int64_t     toast_ms;      /* 起始时刻（g->app->now_ms） */
    const char* toast_text;
} s_ov = { .kind = GC_CAT_TEXT };

static EpdRect gsm_row(int i) {
    return (EpdRect){GSM_ROW_X, GSM_ROW_Y0 + i * GSM_ROW_STEP, GSM_ROW_W, GSM_ROW_H};
}

static const gc_plugin_t* sub_of(void) {
    return s_ov.kind == GC_CAT_NOVEL ? &gc_plugin_gnovel : &gc_plugin_gtext;
}

/* ===================== paint ===================== */

static void draw_menu(uint8_t* fb) {
    /* 浅灰蒙层压住游戏画面 + 白卡 + 4 行按钮（退出=黑底强调）。 */
    epd_fill_rect((EpdRect){0, 0, 684, 1216}, 0xE6, fb);
    EpdRect card = {142, 424, 400, 376};
    ui_fill_round_rect(fb, card, 22, 0xff);
    ui_draw_control_frame(fb, card, 22, 0x00);
    for (int i = 0; i < 4; ++i)
        gc_ui_button(fb, gsm_row(i), k_menu_rows[i], i == 2);
}

static void draw_toast(uint8_t* fb) {
    EpdRect card = {172, 552, 340, 112};
    ui_fill_round_rect(fb, card, 18, 0xff);
    ui_draw_control_frame(fb, card, 18, 0x00);
    ui_text_vc(fb, 342, 608, 24, s_ov.toast_text, EPD_DRAW_ALIGN_CENTER, false);
}

static void paint(gc_ctx_t* g, uint8_t* fb) {
    if (g->state && sub_of()->paint) sub_of()->paint(g, fb);
    if (s_ov.menu_open) draw_menu(fb);
    else if (s_ov.toast_show) draw_toast(fb);
}

/* ===================== 菜单动作 ===================== */

static void toast(const char* text, gc_ctx_t* g) {
    s_ov.toast_show = true;
    s_ov.toast_text = text;
    s_ov.toast_ms = g->app ? g->app->now_ms : 0;
}

static gc_ev_t ev_dirty_full(gc_ctx_t* g) {
    g->area = (EpdRect){0, 0, 684, 1216};
    g->want_full = false;
    return GC_EV_DIRTY;
}

static gc_ev_t menu_action(gc_ctx_t* g, int row) {
    s_ov.menu_open = false;
    s_ov.toast_show = false;
    switch (row) {
    case 0:                                     /* 存档 */
        if (s_ov.kind == GC_CAT_NOVEL ? gnovel_save(g) : gtext_save(g))
            toast("已存档 ✓", g);
        else
            toast("本模式无需存档", g);
        return ev_dirty_full(g);
    case 1: {                                   /* 读档 */
        bool ok = s_ov.kind == GC_CAT_NOVEL ? gnovel_restore(g) : gtext_restore(g);
        if (ok) { s_ov.toast_show = false; return ev_dirty_full(g); }
        toast("没有存档", g);
        return ev_dirty_full(g);
    }
    case 2:                                     /* 退出：先存档，回文绎包列表页（2026-10-30
                                                  * 用户拍板：不整体退应用页，返回选择界面）。 */
        if (s_ov.kind == GC_CAT_NOVEL) gnovel_save(g); else gtext_save(g);
        /* gc_open_list 内部先 gc_shutdown：子插件 quit 释放 PSRAM、卸载包，
         * 再重扫分类包停在选择列表。参数在调用前取值（此时 g 仍是旧会话）。 */
        gc_open_list(g->plugin, g->app);
        g->area = (EpdRect){0, 0, 684, 1216};
        g->want_full = true;                    /* 整屏重绘=列表页 */
        return GC_EV_DIRTY;
    default:                                    /* 刷新屏幕：整屏 GC16 强刷 */
        g->area = (EpdRect){0, 0, 684, 1216};
        g->want_full = true;
        return GC_EV_DIRTY;
    }
}

/* ===================== 输入 ===================== */

static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    if (ev->type != UI_GESTURE_TAP) return GC_EV_NONE;
    if (s_ov.menu_open) {
        for (int i = 0; i < 4; ++i)
            if (ui_rect_hit(gsm_row(i), (int)ev->x0, (int)ev->y0))
                return menu_action(g, i);
        s_ov.menu_open = false;                 /* 点菜单外：关闭 */
        return ev_dirty_full(g);
    }
    if (!g->state || !sub_of()->input) return GC_EV_NONE;
    return sub_of()->input(g, ev);
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    if (k == UI_KEY_2) {                        /* 中短键：弹/收菜单 */
        if (s_ov.menu_open) { s_ov.menu_open = false; s_ov.toast_show = false; }
        else s_ov.menu_open = true;
        return ev_dirty_full(g);
    }
    if (s_ov.menu_open && (k == UI_KEY_1 || k == UI_KEY_3)) {
        s_ov.menu_open = false;                 /* 左/右键：关菜单（取消） */
        return ev_dirty_full(g);
    }
    if (!g->state || !sub_of()->key) return GC_EV_NONE;
    return sub_of()->key(g, k);
}

static gc_ev_t tick(gc_ctx_t* g) {
    if (s_ov.toast_show) {                      /* toast 1.4s 后整帧清掉 */
        int64_t now = g->app ? g->app->now_ms : 0;
        if (now >= s_ov.toast_ms && now - s_ov.toast_ms < 1400) return GC_EV_NONE;
        s_ov.toast_show = false;
        return ev_dirty_full(g);
    }
    if (s_ov.menu_open || !g->state || !sub_of()->tick) return GC_EV_NONE;
    return sub_of()->tick(g);
}

/* ===================== 生命周期 ===================== */

static bool launch(gc_ctx_t* g, const char* title_arg) {
    if (!g->pkg_dir[0]) return false;           /* 无包：交给内核 nopkg 引导页 */
    gc_cat_t k = gc_pkg_mounted_kind();
    s_ov.kind = (k == GC_CAT_NOVEL) ? GC_CAT_NOVEL : GC_CAT_TEXT;
    s_ov.menu_open = false;
    s_ov.toast_show = false;
    /* 子插件 launch 会写 g->state（子状态）——之后内核/覆层都拿它转发。 */
    return sub_of()->launch(g, title_arg);
}

static void quit(gc_ctx_t* g) {
    if (g->state && sub_of()->quit) sub_of()->quit(g);
    s_ov.menu_open = false;
    s_ov.toast_show = false;
}

const gc_plugin_t gc_plugin_gstory = {
    .id = "story",
    .name = "文绎",
    .cat = GC_CAT_STORY,                        /* 复合分类：text-+novel- */
    .middle_menu = true,                        /* 中短键下发本插件弹菜单 */
    .launch = launch,
    .quit = quit,
    .paint = paint,
    .input = input,
    .key = key,
    .tick = tick,                               /* toast 定时消退（子插件无动画时兼转发） */
};
