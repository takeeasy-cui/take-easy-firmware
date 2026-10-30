/*
 * gnovel.c — 互动小说插件（novel-<name>.zip / 目录，包根 story.txt）。
 *
 * Story Engine v1 的 novel 渲染端：脚本执行共用 story_kernel；正文攒成章节流
 * 交给 book_layout 分页（书字体/AA/进度全白拿），选择点暂停出选项。
 *
 * 2026-10-27 画皮空章根因修复：
 *  book_layout 是带全局态的模块（竖排/字距/章节题头跳过/字体 runs/插图回调），
 *  书阅读页残留的态会污染游戏侧排版——"章节题头跳过 skip_bytes"曾把整章正文
 *  当题头吃空（首进即空章）。现在每次构建前全量复位模块态；
 *  构建失败/0 页时走位图兜底排版（ui 字库），保证正文永远可见。
 *  2026-10-28 贴结局残档自愈 + 空章兜底：旧固件（'>'行堆积时代）留下的
 *  存档若落在结局段附近，恢复重放后几无正文直达 END——玩家一进游戏就撞
 *  “完”页/空白页。现在重放后无选项直达结局且攒流近空（<128B）视为通关
 *  残档，从头重开并覆盖存档；攒流为空的章节不再 build 空白阅读页，直达
 *  选项页/结局页。另每次攒流打一行诊断日志（pc/字节数/事件/页数），
 *  真机再遇异常可凭串口定位是“攒流空”还是“绘制空”。
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
#include "settings.h"          /* 2026-10-28：字号复用书阅读设置（app_settings_book_px） */

#define GNOVEL_FLOW_CAP   (256 * 1024)   /* 章节流 PSRAM 上限 */
#define GNOVEL_PX_FALLBACK 36            /* 设置异常时的兑底字号（正常走书阅读设置 36..72，默认 48） */
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
    int           px;                   /* 2026-10-28：正文字号（launch 时取书阅读设置，本会话内恒定） */
    gnovel_phase_t phase;
    bool          choice_pending;         /* 本暂停点是否停在选项 */
    bool          fb_ok;                  /* book_layout 构建成功（否则兜底排版） */
    size_t        page;
    char          save_path[192];
} gnovel_state_t;

/* ===================== 兜底排版（book_layout 不可用时） =====================
 * 章节流按 ≤42 半宽单位（≈21 汉字）折行、24 行/页的位图排版（ui 字库）。
 * 极少触发：仅 book_layout 构建/页数为 0 时保底。返回总页数；
 * want_page 页的 24 行写入 ls（ls=NULL 仅计数）。 */
static char s_fb_lines[24][96];

static size_t fb_paginate(const char* flow, size_t len, size_t want_page,
                          char ls[24][96], int* out_n) {
    int collected = 0;
    if (out_n) *out_n = 0;
    size_t phys = 0;
    size_t page_first = want_page * 24, page_last = page_first + 24;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && flow[j] != '\n') ++j;
        if (i == j) {                                       /* 空行 */
            if (ls && phys >= page_first && phys < page_last) {
                ls[collected][0] = 0;
                ++collected;
            }
            ++phys;
        } else {
            size_t seg = i, k = i;
            int units = 0;
            while (k < j) {
                unsigned char c = (unsigned char)flow[k];
                int clen = (c & 0x80) ? (((c & 0xf0) == 0xe0) ? 3 : 2) : 1;
                int cw = (c >= 0x80) ? 2 : 1;
                if (units + cw > 42) {                      /* 断行 */
                    size_t n = k - seg; if (n > 95) n = 95;
                    if (ls && phys >= page_first && phys < page_last) {
                        memcpy(ls[collected], flow + seg, n);
                        ls[collected][n] = 0;
                        ++collected;
                    }
                    ++phys;
                    seg = k; units = 0;
                } else {
                    units += cw;
                    k += (size_t)clen;
                }
            }
            size_t n = j - seg; if (n > 95) n = 95;         /* 行尾冲刷 */
            if (ls && phys >= page_first && phys < page_last) {
                memcpy(ls[collected], flow + seg, n);
                ls[collected][n] = 0;
                ++collected;
            }
            ++phys;
        }
        i = j + 1;
    }
    size_t pages = phys ? (phys + 23) / 24 : 1;
    if (out_n) *out_n = collected;
    return pages;
}

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
    if (s->flow_len + n + 1 > GNOVEL_FLOW_CAP) return;   /* 超上限静默截断 */
    memcpy(s->flow + s->flow_len, line, n);
    s->flow_len += n;
    s->flow[s->flow_len] = 0;
}

static EpdRect novel_rect(void) {
    return (EpdRect){32, 40, 620, 1120};
}

static size_t novel_pages(gnovel_state_t* s) {
    if (s->fb_ok) {
        size_t n = book_layout_page_count();
        return n ? n : 1;
    }
    return fb_paginate(s->flow, s->flow_len, (size_t)-1, NULL, NULL);
}

/* book_layout 模块态全量复位（防书阅读残留污染）。 */
static void layout_reset(void) {
    book_layout_set_vertical(false);
    book_layout_set_runs(NULL, 0);
    book_layout_set_spacing(100, 50);
    book_layout_set_typography(0);
    book_layout_set_first_line_indent(0);
    book_layout_set_first_line_indent_adjust(0);
    book_layout_set_reading_line(0);
    book_layout_set_reading_line_offset(0);
    book_layout_set_chapter_lead(0, 0);
    book_layout_set_chapter_lead_width(0);
    book_layout_set_images_visible(true);
    book_layout_set_image_dims(NULL, NULL);
    book_layout_set_image_bleed_width(0);
}

/* 走剧本到下一暂停点：正文攒进章节流，选择/结局为暂停事件。 */
static void run_to_pause(gnovel_state_t* s) {
    layout_reset();                          /* 每章重排前复位（含 restart 路径） */
    flow_reset(s);
    s->choice_pending = false;
    sk_event_t ev = SK_EV_NONE;
    int steps = 0;
    for (;;) {
        ev = sk_step(s->sk, &s->cur);
        if (ev == SK_EV_NONE) {              /* 2026-10-28 根因修复（串口实锤 pc=0/n=0B/ev=0）：
                                              * sk_step 对 #//注释与无暂停副作用命令（title/bg/
                                              * label/goto/set 等）返回 NONE——这些行被内核消化
                                              * 并推进 pc，必须继续跑而不是当暂停点。原实现
                                              * “非 TEXT 一律 break”在第一行注释就退出攒流，
                                              * 0 句直达完页/空白页（restart 也死循环）。 */
            if (++steps > s->sk->lines * 4) break;   /* goto 成环保险 */
            continue;
        }
        if (ev == SK_EV_TEXT) { flow_append(s, s->cur.name, s->cur.text); continue; }
        if (ev == SK_EV_CHOICE) s->choice_pending = true;
        break;                              /* CHOICE / END / EOF 均为本章暂停点 */
    }
    if (s->flow_len == 0) {                  /* 空章兜底：不 build 空白阅读页 */
        printf("GN: ch pc=%d n=0B ev=%d pend=%d -> %s\n",
               s->flow_start_pc, (int)ev, s->choice_pending,
               s->choice_pending ? "choices" : "finale");
        s->phase = s->choice_pending ? NS_CHOICES : NS_FINALE;
        s->page = 0;
        return;
    }
    book_layout_free();
    bool ok = book_layout_build(s->flow, s->flow_len, novel_rect(), s->px);
    s->fb_ok = ok && book_layout_page_count() > 0;
    printf("GN: ch pc=%d n=%dB ev=%d pend=%d pages=%d fb=%d%s\n",
           s->flow_start_pc, (int)s->flow_len, (int)ev, s->choice_pending,
           s->fb_ok ? (int)book_layout_page_count() : 1, s->fb_ok,
           s->fb_ok ? "" : " FALLBACK");
    if (!s->fb_ok) printf("GN: layout fallback (%d B chapter)\n", (int)s->flow_len);
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
    if (s->sk->pc >= s->sk->lines) {         /* 越界存档：视为无档，从头开 */
        sk_restart(s->sk);
        run_to_pause(s);
        return false;
    }
    run_to_pause(s);                        /* sk->pc 已是章节起点：重放攒流 */
    /* 2026-10-28 贴结局残档自愈：重放后无选项直达 END/EOF 且攒流近空
     * （<128B，约 3 句），说明存档点是旧固件留下的通关残档——与其把玩家
     * 摔进“完”页/空白页，不如从头重开并覆盖存档。攒流丰薄的终章不在此列，
     * 用户仍可从存档点把结局看完。 */
    if (!s->choice_pending && s->sk->pc >= s->sk->lines && s->flow_len < 128) {
        printf("GN: stale near-end save (pc=%d, %d B) -> restart\n",
               s->sk->pc, (int)s->flow_len);
        sk_restart(s->sk);
        run_to_pause(s);
        save(s);
        return true;
    }
    size_t count = novel_pages(s);
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
        ui_text_vc(fb, 342, 400, 72, "完", EPD_DRAW_ALIGN_CENTER, false);
        /* 2026-10-28（用户真机反馈：结局文案显示不全）：结局文案最长 75 字，
         * 原 26px 单行只容 26 字必出屏。按 23 汉字/行折行最多 4 行居中绘。 */
        if (s->cur.text[0]) {
            char ls[4][128];
            int n = 0, i = 0, len = (int)strlen(s->cur.text);
            while (i < len && n < 4) {
                int units = 0, j = i, end = i;
                while (j < len) {
                    unsigned char c = (unsigned char)s->cur.text[j];
                    int clen = (c & 0x80) ? (((c & 0xf0) == 0xe0) ? 3 : 2) : 1;
                    int cw = (c >= 0x80) ? 2 : 1;
                    if (units + cw > 46) break;          /* 23 汉字/行（26px×23≈598<屏宽） */
                    units += cw; j += clen; end = j;
                }
                int m = end - i; if (m > 127) m = 127;
                memcpy(ls[n], s->cur.text + i, m); ls[n][m] = 0;
                ++n; i = end;
            }
            for (int k = 0; k < n; ++k)
                ui_text_vc(fb, 342, 470 + k * 44, 26, ls[k], EPD_DRAW_ALIGN_CENTER, false);
        }
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

    /* NS_READ：正文页（书排版为主；book_layout 异常时兜底位图排版）。 */
    ui_clear_page(fb);
    size_t count = novel_pages(s);
    if (s->fb_ok) {
        book_layout_draw_page(fb, s->page, novel_rect(), s->px);
    } else {
        int n = 0;
        fb_paginate(s->flow, s->flow_len, s->page, s_fb_lines, &n);
        for (int r = 0; r < n && r < 24; ++r)
            if (s_fb_lines[r][0])
                ui_text_vc(fb, 48, 92 + r * 44, 26, s_fb_lines[r], EPD_DRAW_ALIGN_LEFT, false);
    }
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
    size_t count = novel_pages(s);
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
        printf("GN: finale tap -> restart\n");
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

    /* 2026-10-28（用户拍板最简实现）：互动小说字号复用书阅读设置——
     * 在书阅读的工具栏里调字号（36..72，默认 48），互动小说 launch 时取同一值，
     * 零新增设置页/键位。本会话内恒定，重进游戏生效新值。 */
    s->px = app_settings_book_px();
    if (s->px < 24 || s->px > 96) s->px = GNOVEL_PX_FALLBACK;

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

/* ===================== 文绎合体入口（gstory）菜单桥接 ===================== */
bool gnovel_save(gc_ctx_t* g) {
    gnovel_state_t* s = (gnovel_state_t*)g->state;
    if (!s || !s->sk) return false;
    save(s);
    return true;
}

bool gnovel_restore(gc_ctx_t* g) {
    gnovel_state_t* s = (gnovel_state_t*)g->state;
    if (!s || !s->sk) return false;
    return restore(s);
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
