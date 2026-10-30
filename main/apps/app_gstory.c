/*
 * app_gstory.c —【文绎】入口薄壳：GameCore + story 合体插件
 * （text- 对话游戏 + novel- 互动小说，2026-10-30 取代双 tile）。
 */
#include "app.h"
#include "game_core.h"
#include "ui_gesture.h"

#include "plugins/gstory.h"

static void on_enter(app_ctx_t* ctx) { gc_app_enter(&gc_plugin_gstory, NULL, ctx); }

const app_desc_t app_gstory = {
    .title = "文绎",
    .detail = "对话游戏 · 互动小说",
    .enter_full = true,
    .owns_keys = true,
    .defer_middle_short = true,   /* 中短=弹菜单（插件处理）/中长=退出 */
    .render = gc_app_render,
    .on_gesture = gc_app_gesture,
    .on_key = gc_app_key,
    .on_key_long = gc_app_key_long,
    .on_tick = gc_app_tick,
    .present = gc_app_present,   /* AREA 走 GL16 定稿刷（游戏灰阶画质） */
    .on_enter = on_enter,
    .area_hint = gc_app_area,
    .on_exit = gc_app_exit,
    .on_media_lost = gc_app_media_lost,
};
