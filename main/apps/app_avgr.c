/*
 * app_avgr.c —【推理游戏】入口薄壳：全部逻辑在 GameCore + avgr 插件。
 * 本文件只做 app_desc_t ↔ game_core 桥接，无任何游戏状态。
 */
#include "app.h"
#include "game_core.h"
#include "ui_gesture.h"

#include "plugins/avgr.h"   /* gc_plugin_avgr */

static void on_enter(app_ctx_t* ctx) { gc_app_enter(&gc_plugin_avgr, NULL, ctx); }

const app_desc_t app_avgr = {
    .title = "推理游戏",
    .detail = "逆转裁判 · 文字推理 AVG",
    .enter_full = true,
    .owns_keys = true,
    .defer_middle_short = true,
    .render = gc_app_render,
    .on_gesture = gc_app_gesture,
    .on_key = gc_app_key,        /* 中短=强刷 / 右键=退出（恒语义） */
    .on_key_long = gc_app_key_long,  /* 中长=退出（恒语义） */
    .on_tick = gc_app_tick,
    .present = gc_app_present,   /* AREA 走 GL16 定稿刷（游戏灰阶画质） */
    .on_enter = on_enter,
    .area_hint = gc_app_area,
    .on_exit = gc_app_exit,
    .on_media_lost = gc_app_media_lost,
};
