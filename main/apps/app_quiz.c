/*
 * app_quiz.c —【习测】入口薄壳：GameCore + quiz 题库插件（quiz-<name> 包）。
 */
#include "app.h"
#include "game_core.h"
#include "ui_gesture.h"

#include "plugins/gquiz.h"

static void on_enter(app_ctx_t* ctx) { gc_app_enter(&gc_plugin_gquiz, NULL, ctx); }

const app_desc_t app_quiz = {
    .title = "习测",
    .detail = "应知应会 · 随机题库",
    .enter_full = true,
    .owns_keys = true,
    .defer_middle_short = true,
    .render = gc_app_render,
    .on_gesture = gc_app_gesture,
    .on_key = gc_app_key,
    .on_key_long = gc_app_key_long,
    .on_tick = gc_app_tick,
    .present = gc_app_present,   /* AREA 走 GL16 定稿刷（文本画质） */
    .on_enter = on_enter,
    .area_hint = gc_app_area,
    .on_exit = gc_app_exit,
    .on_media_lost = gc_app_media_lost,
};
