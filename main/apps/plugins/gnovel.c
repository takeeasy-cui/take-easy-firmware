/*
 * gnovel.c — 互动小说插件（novel-<name>.zip / 目录，包根 story.txt）。
 *
 * Story Engine v1 的 novel 渲染端（M2，2026-10-26）：脚本执行共用
 * main/game/story_kernel.c；本文件把暂停点之间的正文攒成章节文本流，
 * 交给 book_layout 分页（书字体链路/AA/进度全白拿），选择点暂停出选项。
 *
 * 与 game 端（gtext）的差异：
 *  - 阅读：整章攒流 → book_layout 分页 → 上下半屏翻页（书感）；
 *  - 暂停点之间的正文不再逐句打字机，而是一次整章排版；
 *  - 存档存「章节起点 pc + 页码」：恢复时从章节起点重放攒流（副作用幂等），
 *    再跳回存档页——比存行号/字节偏移都稳（渲染端只信重建）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "epdiy.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "game_core.h"
#include "story_kernel.h"
#include "book_layout.h"

#define GNOVEL_FLOW_CAP   (256 * 1024)   /* 章节流 PSRAM 上限 */
#define GNOVEL_PX         28            /* 正文字号（书阅读同档） */
#define GNOVEL_CHOICE_Y0  640
#define GNOVEL_CHOICE_H    64
#define GNOVEL_CHOICE_STEP 72

typedef enum { NS_READ = 0, NS_CHOICES, NS_FINALE } gnovel_phase_t;

typedef struct {
    sk_state_t*   sk;                     /* PSRAM（脚本+变量+label 表） */
    sk_out_t      cur;
    char*         flow;                  /* 章节文本流（PSRAM，launch 分配） */
    size_t        flow_len;
    int           flow_start_pc;         /* 本章节流攒集起点（存档恢复锚） */
    gnovel_phase_t phase;
    bool          choice_pending;         /* 本暂停点是否停在选项 */
    size_t        page;
    char          save_path[192];
} gnovel_state_t;

/* ===================== 章节流攒集与分页 ===================== */

static void flow_reset(gnovel_state_t* s) {
    s->flow_len = 0;
    s->flow_start_pc = s->sk->pc;
    if (s->flow) s->flow[0] = 0;
}

static void flow_append(gnovel_state_t* s, const char* name, const char* text) {
    char line[SK_TEXT_MAX + 64];
    if (name[0]) snprintf(line, sizeof(line), "%s：%s\n", name, text);
    else snprintf(line, sizeof(line), "%s\n", text);
    size_t n = strlen(line);
    if (s->flow_len + n + 1 > GNOVEL_FLOW_CAP) return;   /* 超上限静默截断（256KB/章足够） */
    memcpy(s->flow + s->flow_len, line, n);
    s->flow_len += n;
    s->flow[s->flow_len] = 0;
}

static EpdRect novel_rect(void) {
    return (EpdRect){32, 40, 620, 1120};
}

/* 走剧本到下一暂停点：正文攒进章节流，选择/结局为暂停事件。 */
static void run_to_pause(gnovel_state_t* s) {
    flow_reset(s);
    s->choice_pending = false;
    for (;;) {
        sk_event_t ev = sk_step(s->sk, &s->cur);
        if (ev == SK_EV_TEXT) { flow_append(s, s->cur.name, s->cur.text); continue; }
        if (ev == SK_EV_CHOICE) s->choice_pending = true;
        break;                              /* CHOICE / END / EOF 均为本章暂停点 */
    }
    /* build 失败（>4096 页等）时 book_layout 内部清空布局 → 空章一页，可继续。 */
    book_layout_free();
    book_layout_build(s->flow, s->flow_len, novel_rect(), GNOVEL_PX);
    s->phase = NS_READ;
    s->page = 0;
}

/* ===================== 存档（章节起点 pc + 页码） ===================== */

static void save(gnovel_state_t* s) {
    if (!s->save_path[0]) return;
    FILE* f = fopen(s->save_path, "w");
    if (!f) return;
    /* 存「章节流起点 pc + 页码 + 变量/标志」：恢复从章节起点重放攒流再跳页。 */
    char buf[512];
    sk_save(s->sk, -1, buf, sizeof(buf));   /* "pc var.. flag.."（无 P token） */
    const char* rest = strchr(buf, ' ');      /* " var.. flag.." */
    fprintf(f, "%d%s P%d\n", s->flow_start_pc, rest ? rest : "", (int)s->page);
    fclose(f);
}

static bool restore(gnovel_state_t* s) {
    FILE* f = fopen(s->save_path, "r");
    if (!f) return false;
    char line[768];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    fclose(f);
    int page = -1;
    if (!sk_restore(s->sk, line, &page)) return false;
    run_to_pause(s);                        /* sk->pc 已是章节起点：重放攒流 */
    size_t count = book_layout_page_count();
    if (count) s->page = (size_t)page < count ? (size_t)page : count - 1;
    if (page < 0) s->page = 0;
    return true;
}

/* ===================== paint ===================== */

static void paint(gc_ctx_t* g, uint8_t* fb) {
    gnovel_state_t* s = (gnovel_state_t*)g->state;
    if (!s) { gc_ui_nopkg(fb, "互动小说", GC_CAT_NOVEL); return; }

    if (s->phase == NS_FINALE) {
        ui_clear_page(fb);
        ui_text_vc(fb, 342, 480, 72, "完", EPD_DRAW_ALIGN_CENTER, false);
        if (s->cur.text[0])
            ui_text_vc(fb, 342, 600, 26, s->cur.text, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 660, 22, s->sk->title, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 1118, 18, "点按屏幕重新开始 · 中长按退出", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (s->phase == NS_CHOICES) {
        ui_clear_page(fb);
        ui_text_vc(fb, 342, 560, 26, "请选择", EPD_DRAW_ALIGN_CENTER, false);
        for (int i = 0; i < s->cur.choice_count; ++i)
            gc_ui_button(fb, (EpdRect){120, GNOVEL_CHOICE_Y0 + i * GNOVEL_CHOICE_STEP,
                                       444, GNOVEL_CHOICE_H}, s->cur.choice_text[i], false);
        return;
    }

    /* NS_READ：书页排版（book_layout 全权绘制正文区）。 */
    ui_clear_page(fb);
    size_t count = book_layout_page_count();
    book_layout_draw_page(fb, s->page, novel_rect(), GNOVEL_PX);
    char foot[32];
    if (count) snprintf(foot, sizeof(foot), "%d / %d", (int)(s->page + 1), (int)count);
    else strcpy(foot, "1 / 1");
    ui_text_vc(fb, 342, 1180, 18, foot, EPD_DRAW_ALIGN_CENTER, false);
    if (count && s->page >= count - 1) {
        const char* hint = s->choice_pending ? "▼ 继续" : "▼ 结束";
        ui_text_vc(fb, 342, 1140, 18, hint, EPD_DRAW_ALIGN_CENTER, false);
    }
}

/* ===================== 输入 ===================== */

static gc_ev_t ev_full(gc_ctx_t* g) { g->area = (EpdRect){0, 0, 684, 1216}; g->want_full = false; return GC_EV_DIRTY; }

static void flip(gnovel_state_t* s, bool forward) {
    size_t count = book_layout_page_count();
    if (!count) return;
    if (forward) {
        if (s->page + 1 < count) { ++s->page; save(s); return; }
        if (s->choice_pending) s->phase = NS_CHOICES;    /* 章末 → 选项 */
        else s->phase = NS_FINALE;                       /* 章末 → 结局页 */
        return;
    }
    if (s->page > 0) --s->page;
    save(s);
}

static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    gnovel_state_t* s = (gnovel_state_t*)g->state;
    if (ev->type != UI_GESTURE_TAP) return GC_EV_NONE;

    if (s->phase == NS_FINALE) {            /* 结局 → 重开 */
        sk_restart(s->sk);
        run_to_pause(s);
        save(s);
        return ev_full(g);
    }
    if (s->phase == NS_CHOICES) {
        for (int i = 0; i < s->cur.choice_count; ++i) {
            EpdRect r = {120, GNOVEL_CHOICE_Y0 + i * GNOVEL_CHOICE_STEP, 444, GNOVEL_CHOICE_H};
            if (ui_rect_hit(r, ev->x0, ev->y0)) {
                sk_choose(s->sk, i);
                run_to_pause(s);
                save(s);
                return ev_full(g);
            }
        }
        return GC_EV_NONE;
    }
    flip(s, ev->y0 >= 608);                 /* 下半屏=下一页（书习惯） */
    return ev_full(g);
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    gnovel_state_t* s = (gnovel_state_t*)g->state;
    if (k != UI_KEY_1) return GC_EV_NONE;
    if (s->phase == NS_READ) { flip(s, true); return ev_full(g); }
    ui_gesture_event_t tap = { .type = UI_GESTURE_TAP, .x0 = 342, .y0 = 700 };
    return input(g, &tap);
}

/* ===================== 生命周期 ===================== */

static void quit(gc_ctx_t* g);              /* 前向：launch 失败自清用 */

static bool launch(gc_ctx_t* g, const char* title_arg) {
    (void)title_arg;
    gnovel_state_t* s = (gnovel_state_t*)calloc(1, sizeof(gnovel_state_t));
    if (!s) return false;
    g->state = s;
    if (!g->pkg_dir[0]) return true;        /* 无包：paint 画引导页 */

    s->sk = (sk_state_t*)heap_caps_calloc(1, sizeof(sk_state_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->sk) return false;
    s->flow = (char*)heap_caps_malloc(GNOVEL_FLOW_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->flow) return false;
    s->flow[0] = 0;

    char path[256];
    snprintf(path, sizeof(path), "%s/story.txt", g->pkg_dir);
    if (!sk_load(s->sk, path)) { quit(g); return false; }   /* 失败自清防泄漏 */
    snprintf(s->save_path, sizeof(s->save_path), "%s/.gnsave", g->pkg_dir);

    run_to_pause(s);
    if (!s->sk->title[0]) snprintf(s->sk->title, sizeof(s->sk->title), "%s", g->pkg_name);
    if (!restore(s)) save(s);
    return true;
}

static void quit(gc_ctx_t* g) {
    gnovel_state_t* s = (gnovel_state_t*)g->state;
    if (!s) return;
    book_layout_free();
    if (s->sk) { sk_free(s->sk); heap_caps_free(s->sk); }
    if (s->flow) heap_caps_free(s->flow);
    free(s);
    g->state = NULL;
}

const gc_plugin_t gc_plugin_gnovel = {
    .id = "novel",
    .name = "互动小说",
    .cat = GC_CAT_NOVEL,
    .launch = launch,
    .quit = quit,
    .paint = paint,
    .input = input,
    .key = key,
    .tick = NULL,                            /* 无动画：翻页即绘 */
};
