/*
 * gquiz.c — 习测插件（quiz-<name>.zip / 目录，包根 quiz.txt）。
 *
 * 独立入口（2026-10-27 用户拍板：考试从对话游戏拆出单独应用，名称「习测」）。
 * 渲染与逻辑全部复用 main/game/quiz_engine.c；本文件只是 GameCore 插件壳：
 * launch 调 quiz_load（quiz.txt + .quizprog），paint/input 桥接引擎。
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "game_core.h"
#include "quiz_engine.h"

typedef struct {
    quiz_engine_t* quiz;                     /* NULL=无包（paint 画引导页） */
} gquiz_state_t;

static bool launch(gc_ctx_t* g, const char* title_arg) {
    (void)title_arg;
    gquiz_state_t* s = (gquiz_state_t*)calloc(1, sizeof(gquiz_state_t));
    if (!s) return false;
    g->state = s;
    if (!g->pkg_dir[0]) return true;         /* 无包：引导页 */

    char qpath[256], ppath[192];
    snprintf(qpath, sizeof(qpath), "%s/quiz.txt", g->pkg_dir);
    snprintf(ppath, sizeof(ppath), "%s/.quizprog", g->pkg_dir);
    s->quiz = quiz_load(qpath, ppath);
    return s->quiz != NULL;                  /* 解析失败=launch 失败（内核清场） */
}

static void quit(gc_ctx_t* g) {
    gquiz_state_t* s = (gquiz_state_t*)g->state;
    if (!s) return;
    if (s->quiz) quiz_free(s->quiz);
    free(s);
    g->state = NULL;
}

static void paint(gc_ctx_t* g, uint8_t* fb) {
    gquiz_state_t* s = (gquiz_state_t*)g->state;
    if (!s || !s->quiz) { gc_ui_nopkg(fb, "习测", GC_CAT_QUIZ); return; }
    quiz_paint(s->quiz, fb);
}

static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    gquiz_state_t* s = (gquiz_state_t*)g->state;
    if (!s || !s->quiz || ev->type != UI_GESTURE_TAP) return GC_EV_NONE;
    if (!quiz_tap(s->quiz, (int)ev->x0, (int)ev->y0)) return GC_EV_NONE;
    g->area = (EpdRect){0, 0, 684, 1216};
    g->want_full = false;
    return GC_EV_DIRTY;
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    gquiz_state_t* s = (gquiz_state_t*)g->state;
    if (k != UI_KEY_1 || !s || !s->quiz) return GC_EV_NONE;
    if (!quiz_middle_key(s->quiz)) return GC_EV_NONE;
    g->area = (EpdRect){0, 0, 684, 1216};
    g->want_full = false;
    return GC_EV_DIRTY;
}

const gc_plugin_t gc_plugin_gquiz = {
    .id = "quiz",
    .name = "习测",
    .cat = GC_CAT_QUIZ,
    .launch = launch,
    .quit = quit,
    .paint = paint,
    .input = input,
    .key = key,
    .tick = NULL,                             /* 无动画 */
};
