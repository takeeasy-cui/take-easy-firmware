/*
 * gtext.c — 对话游戏插件（text-<name>.zip / 目录，包根 story.txt）。
 *
 * Story Engine v1 的 game 渲染端（M2，2026-10-26）：脚本执行共用
 * main/game/story_kernel.c，本文件只做渲染与交互——背景/立绘/对话框/
 * 打字机/选项/结局页，全走 game_host_ui 共享原语。
 *
 * 与旧版 gtext 的差别（用户拍板：游戏出口固定、便于大量移植）：
 *  - 执行内核共用（novel- 同一内核），变量四则/条件跳/直跳式选项/多结局；
 *  - 立绘 3 槽（* char），透明 PNG 或 .g8（解码器沿 avgr 冻结实现搬用）；
 *  - * ending 文案页 + flag 结局收集；存档向后兼容旧 .gtsave。
 *
 * Script grammar: see story_kernel.h. Rendering only; the shared interpreter
 * lives in main/game/story_kernel.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book_cover.h"
#include "esp_heap_caps.h"
#include "epdiy.h"
#include "ui_gesture.h"
#include "ui_image_dither.h"
#include "ui_kit.h"
#include "ui_menu.h"

/* 剧情/存档路径拼接的保守截断告警豁免（缓冲与内核包路径体系一致）。 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "game_core.h"
#include "story_kernel.h"

#include "avg_asset.h"

#define GTEXT_DIALOG_TOP  840
#define GTEXT_LH           44
#define GTEXT_BG_W         684
#define GTEXT_BG_H         840
#define GTEXT_CH_W         470
#define GTEXT_CH_H         640
#define GTEXT_CHOICE_Y0    640
#define GTEXT_CHOICE_H     64
#define GTEXT_CHOICE_STEP  72

/* ===================== 状态 ===================== */

typedef struct {
    sk_state_t*  sk;                       /* PSRAM（脚本+变量+label 表） */
    avg_assets_t assets;                   /* bg/char 灰度+蒙版 LRU 缓存 */
    sk_out_t     cur;                      /* 最近暂停事件 */
    int          shown;                    /* 打字机字节进度 */
    int          pause_line;               /* 暂停行：存档/恢复重放当前句（不跳句） */
    bool         choosing;
    bool         ended;                    /* 结局终页 */
    char         save_path[192];
    uint64_t     gen;                      /* assets 代次（场景重建依据） */
    int64_t      last_ms;
} gtext_state_t;

/* ===================== 图片解码注入（沿 avgr 冻结实现，竖屏几何） =====================
 *  - bg：contain 684×840（等比、不裁切、16 档量化）；
 *  - char：contain 470×640（底对齐由 paint 决定）；
 *  - .g8 快路径（转换器预烘焙 30ms vs PNG 500ms）+ PNG 兼容回退。 */

static void quantize16(uint8_t* px, size_t n) {
    for (size_t i = 0; i < n; ++i) px[i] = (uint8_t)(px[i] & 0xF0);
}

static bool g8_load(const char* path, unsigned max_w, unsigned max_h,
                    uint8_t* out, uint8_t** mask, unsigned* w, unsigned* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "EVG8\x01", 5) != 0) {
        fclose(f);
        return false;
    }
    unsigned gw = hdr[5] | ((unsigned)hdr[6] << 8);
    unsigned gh = hdr[7] | ((unsigned)hdr[8] << 8);
    bool has_mask = (hdr[9] & 1) != 0;
    if (!gw || !gh || (uint64_t)gw * gh > (uint64_t)max_w * max_h) {
        fclose(f);
        return false;
    }
    if (fread(out, 1, (size_t)gw * gh, f) != (size_t)gw * gh) {
        fclose(f);
        return false;
    }
    uint8_t* m = NULL;
    if (has_mask) {
        size_t bytes = (size_t)gh * ((gw + 7) / 8);
        m = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m || fread(m, 1, bytes, f) != bytes) {
            if (m) heap_caps_free(m);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    *w = gw; *h = gh;
    if (mask) *mask = m;
    else if (m) heap_caps_free(m);
    return true;
}

static bool gtext_image_decode(const char* path, unsigned max_w, unsigned max_h,
                               uint8_t* out, uint8_t** mask, unsigned* w, unsigned* h) {
    if (mask) *mask = NULL;
    /* .png 引用时伴生同名 .g8 优先（转换器烘焙）。 */
    char g8path[296];
    const char* ext = strrchr(path, '.');
    if (ext && !strcmp(ext, ".png") && (size_t)(ext - path) + 3 < sizeof(g8path)) {
        snprintf(g8path, sizeof(g8path), "%.*s.g8", (int)(ext - path), path);
        FILE* probe = fopen(g8path, "rb");
        if (probe) {
            fclose(probe);
            path = g8path;
            ext = ".g8";
        }
    }
    if (ext && !strcmp(ext, ".g8"))
        return g8_load(path, max_w, max_h, out, mask, w, h);
    unsigned ow = 0, oh = 0;
    if (!book_image_file_dimensions(path, true, &ow, &oh) || !ow || !oh) return false;
    unsigned fw = max_w, fh = (unsigned)((uint64_t)oh * max_w / ow);
    if (fh > max_h || fh == 0) {
        fh = max_h;
        fw = (unsigned)((uint64_t)ow * max_h / oh);
    }
    if (!fw || !fh) return false;
    if (!book_image_file_grayscale(path, true, fw, fh, out)) return false;
    quantize16(out, (size_t)fw * fh);
    *w = fw; *h = fh;
    return true;
}

/* 包内素材路径多级回退：images/<sub>/<名> → images/<名> → <名>。
 * 名无扩展名时自动试 .png / .g8（脚本书写友好；g8 由解码器快路径接手）。 */
static bool resolve_asset(const gc_ctx_t* g, const char* sub, const char* name,
                          char* out, size_t cap) {
    char base[128];
    snprintf(base, sizeof(base), "%s", name);
    bool has_ext = strrchr(base, '.') != NULL && strlen(strrchr(base, '.')) <= 4;
    static const char* exts[3] = { ".png", ".g8", "" };
    int ext_first = has_ext ? 2 : 0;        /* 已带扩展名只试原样 */
    char tries[3][256];
    for (int e = ext_first; e < 3; ++e) {
        const char* ext = has_ext ? "" : exts[e];
        snprintf(tries[0], sizeof(tries[0]), "%s/images/%s/%s%s", g->pkg_dir, sub, base, ext);
        snprintf(tries[1], sizeof(tries[1]), "%s/images/%s%s", g->pkg_dir, base, ext);
        snprintf(tries[2], sizeof(tries[2]), "%s/%s%s", g->pkg_dir, base, ext);
        for (int t = 0; t < 3; ++t) {
            FILE* f = fopen(tries[t], "rb");
            if (f) { fclose(f); snprintf(out, cap, "%s", tries[t]); return true; }
        }
    }
    return false;
}

/* ===================== 走剧本（到下一暂停点） ===================== */

static void run_to_pause(gtext_state_t* s) {
    s->choosing = false;
    s->ended = false;
    for (;;) {
        int prev = s->sk->pc;             /* 暂停行（存档/恢复重放该句） */
        sk_event_t ev = sk_step(s->sk, &s->cur);
        if (ev == SK_EV_TEXT)  { s->pause_line = prev; s->shown = 0; s->last_ms = 0; return; }
        if (ev == SK_EV_CHOICE) { s->pause_line = prev; s->choosing = true; s->shown = 0; return; }
        if (ev == SK_EV_END || ev == SK_EV_EOF) { s->pause_line = prev; s->ended = true; return; }
        /* SK_EV_NONE：副作用已消化，继续 */
    }
}

/* ===================== 存档 ===================== */

static void save(gtext_state_t* s) {
    if (!s->save_path[0]) return;
    FILE* f = fopen(s->save_path, "w");
    if (!f) return;
    char buf[512];
    int keep = s->sk->pc;                  /* 存暂停行：恢复时重放当前句（不跳句） */
    s->sk->pc = s->pause_line;
    sk_save(s->sk, -1, buf, sizeof(buf));
    s->sk->pc = keep;
    fprintf(f, "%s\n", buf);
    fclose(f);
}
static bool restore(gtext_state_t* s) {
    FILE* f = fopen(s->save_path, "r");
    if (!f) return false;
    char line[768];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    fclose(f);
    int page = -1;
    if (!sk_restore(s->sk, line, &page)) return false;
    run_to_pause(s);                        /* 从存档行重放到暂停点 */
    return true;
}

/* ===================== paint ===================== */

static void draw_line(void* user, const char* line, int y) {
    ui_text_vc((uint8_t*)user, 48, y, 28, line, EPD_DRAW_ALIGN_LEFT, false);
}

static void paint_scene(gtext_state_t* s, gc_ctx_t* g, uint8_t* fb) {
    ui_clear_page(fb);
    sk_state_t* sk = s->sk;

    /* 背景（684×840 区域内 contain；缺图画占位）。 */
    if (sk->bg[0]) {
        char path[256];
        if (resolve_asset(g, "bg", sk->bg, path, sizeof(path))) {
            unsigned w = 0, h = 0;
            const uint8_t* gray = avg_assets_get(&s->assets, path, GTEXT_BG_W, GTEXT_BG_H,
                                                 &w, &h, (uint64_t)g->app->now_ms, NULL);
            if (gray) gc_ui_blit_gray(fb, gray, w, h, 0, 0, GTEXT_BG_W, GTEXT_BG_H);
            else gc_ui_missing(fb, sk->bg, 0, 0, GTEXT_BG_W, GTEXT_BG_H);
        } else {
            gc_ui_missing(fb, sk->bg, 0, 0, GTEXT_BG_W, GTEXT_BG_H);
        }
        sk->bg_dirty = false;
    }

    /* 立绘 3 槽（底对齐对话框；L/C/R 横向落位；含透明蒙版）。 */
    if (sk->char_dirty) {
        for (int i = 0; i < 3; ++i) {
            if (!sk->chars[i][0]) continue;
            char path[256];
            if (!resolve_asset(g, "char", sk->chars[i], path, sizeof(path))) continue;
            unsigned w = 0, h = 0;
            const uint8_t* mask = NULL;
            const uint8_t* gray = avg_assets_get(&s->assets, path, GTEXT_CH_W, GTEXT_CH_H,
                                                 &w, &h, (uint64_t)g->app->now_ms, &mask);
            if (!gray) continue;
            int x = sk->char_pos[i] == 0 ? 8
                  : sk->char_pos[i] == 2 ? (int)(684 - w - 8)
                  : (int)((684 - w) / 2);
            int y = GTEXT_DIALOG_TOP - (int)h;
            gc_ui_blit_gray_a(fb, gray, w, h, mask, x, y, 684, GTEXT_DIALOG_TOP);
        }
        sk->char_dirty = false;
    }

    gc_ui_dialog_frame(fb, GTEXT_DIALOG_TOP);
    gc_ui_name_tag(fb, GTEXT_DIALOG_TOP, s->cur.name);
    gc_ui_typewrap(s->cur.text, s->shown, 4, 20, draw_line, fb, 886, GTEXT_LH);
    if (s->cur.text[0] && (size_t)s->shown >= strlen(s->cur.text))
        gc_ui_wait_cursor(fb, 624, 1088);
}

static void paint(gc_ctx_t* g, uint8_t* fb) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s) { gc_ui_nopkg(fb, "对话游戏", GC_CAT_TEXT); return; }

    if (s->ended) {                         /* 结局页（* ending 文案或无名 * end） */
        ui_clear_page(fb);
        ui_text_vc(fb, 342, 480, 72, "完", EPD_DRAW_ALIGN_CENTER, false);
        if (s->cur.text[0])
            ui_text_vc(fb, 342, 600, 26, s->cur.text, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 660, 22, s->sk->title, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 1118, 18, "点按屏幕重新开始 · 中长按退出", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (s->choosing) {                       /* 选择页独立整页（≤6 项竖排） */
        ui_clear_page(fb);
        gc_ui_dialog_frame(fb, 520);
        if (s->cur.name[0]) gc_ui_name_tag(fb, 520, s->cur.name);
        if (s->cur.text[0])
            ui_text_vc(fb, 342, 584, 22, s->cur.text, EPD_DRAW_ALIGN_CENTER, false);
        for (int i = 0; i < s->cur.choice_count; ++i)
            gc_ui_button(fb, (EpdRect){120, GTEXT_CHOICE_Y0 + i * GTEXT_CHOICE_STEP,
                                       444, GTEXT_CHOICE_H}, s->cur.choice_text[i], false);
        return;
    }
    paint_scene(s, g, fb);
}

/* ===================== 输入 / tick ===================== */

static gc_ev_t ev_area(gc_ctx_t* g, EpdRect a) { g->area = a; g->want_full = false; return GC_EV_DIRTY; }

static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (ev->type != UI_GESTURE_TAP) return GC_EV_NONE;

    if (s->ended) {                         /* 结局 → 回标题（重跑） */
        sk_restart(s->sk);
        run_to_pause(s);
        return ev_area(g, (EpdRect){0, 0, 684, 1216});
    }
    if (s->choosing) {
        for (int i = 0; i < s->cur.choice_count; ++i) {
            EpdRect r = {120, GTEXT_CHOICE_Y0 + i * GTEXT_CHOICE_STEP, 444, GTEXT_CHOICE_H};
            if (ui_rect_hit(r, ev->x0, ev->y0)) {
                sk_choose(s->sk, i);
                run_to_pause(s);
                save(s);
                return ev_area(g, (EpdRect){0, 0, 684, 1216});
            }
        }
        return GC_EV_NONE;
    }
    if ((size_t)s->shown < strlen(s->cur.text)) {   /* 打字中点按=整句直出 */
        s->shown = (int)strlen(s->cur.text);
        return ev_area(g, (EpdRect){0, GTEXT_DIALOG_TOP, 684, 1216 - GTEXT_DIALOG_TOP});
    }
    run_to_pause(s);
    save(s);
    return ev_area(g, (EpdRect){0, 0, 684, 1216});
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    if (k != UI_KEY_1) return GC_EV_NONE;
    ui_gesture_event_t tap = { .type = UI_GESTURE_TAP, .x0 = 342, .y0 = 900 };
    return input(g, &tap);
}

static gc_ev_t tick(gc_ctx_t* g) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (s->choosing || s->ended) return GC_EV_NONE;
    size_t total = strlen(s->cur.text);
    if ((size_t)s->shown >= total || total == 0) return GC_EV_NONE;
    if (g->app->now_ms - s->last_ms < 60) return GC_EV_NONE;
    s->last_ms = g->app->now_ms;
    unsigned char c0 = (unsigned char)s->cur.text[s->shown];
    s->shown += c0 & 0x80 ? ((c0 & 0xf0) == 0xe0 ? 3 : 2) : 1;
    g->area = (EpdRect){0, GTEXT_DIALOG_TOP, 684, 1216 - GTEXT_DIALOG_TOP};
    g->want_full = false;
    return GC_EV_DIRTY;
}

/* ===================== 生命周期 ===================== */

static void quit(gc_ctx_t* g);              /* 前向：launch 失败自清用 */

static bool launch(gc_ctx_t* g, const char* title_arg) {
    (void)title_arg;
    gtext_state_t* s = (gtext_state_t*)calloc(1, sizeof(gtext_state_t));
    if (!s) return false;
    g->state = s;
    if (!g->pkg_dir[0]) return true;        /* 无包：paint 画引导页 */

    s->sk = (sk_state_t*)heap_caps_calloc(1, sizeof(sk_state_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->sk) return false;

    char path[256];
    snprintf(path, sizeof(path), "%s/story.txt", g->pkg_dir);
    if (!sk_load(s->sk, path)) { quit(g); return false; }   /* 失败自清防泄漏 */
    snprintf(s->save_path, sizeof(s->save_path), "%s/.gtsave", g->pkg_dir);

    avg_assets_init(&s->assets);
    avg_assets_set_decoder(gtext_image_decode);

    run_to_pause(s);
    if (!s->sk->title[0]) snprintf(s->sk->title, sizeof(s->sk->title), "%s", g->pkg_name);
    if (restore(s)) { s->shown = 0; }      /* restore 内已重放暂停句；不再二次推进 */
    return true;
}

static void quit(gc_ctx_t* g) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s) return;
    if (s->sk) { sk_free(s->sk); heap_caps_free(s->sk); }
    avg_assets_release(&s->assets);
    free(s);
    g->state = NULL;
}

const gc_plugin_t gc_plugin_gtext = {
    .id = "text",
    .name = "对话游戏",
    .cat = GC_CAT_TEXT,
    .launch = launch,
    .quit = quit,
    .paint = paint,
    .input = input,
    .key = key,
    .tick = tick,
};
