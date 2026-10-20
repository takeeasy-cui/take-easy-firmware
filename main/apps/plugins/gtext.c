/*
 * gtext.c — 通用文字游戏插件（text-<name>.zip，包根 story.txt）。
 *
 * 极简剧本格式（一行一令；# 注释；无需编译）：
 *   * title 游戏名
 *   * bg image.png          背景图（可选，包内文件）
 *   * name 说话人            （下一句的名牌；省略=旁白）
 *   * > 正文文本             （冒号后为打字机正文）
 *   * choice a|b|c           分支选项（按钮竖排）
 *   * goto 标签
 *   * label 标签
 *   * set 标志 / * if 标签 g 目标 / * ifnot 标签 g 目标
 *   * end                    结局页
 *
 * 全部界面走 game_host_ui 共享原语（观感与推理游戏一致）。
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

#define GTEXT_DIALOG_TOP  840
#define GTEXT_LH          44
#define GTEXT_CHOICE_Y0   700
#define GTEXT_CHOICE_H    64

/* ===================== 剧本数据（包根 story.txt 整读 PSRAM） ===================== */
typedef struct { char* text; size_t len; int lines; } gtext_script_t;

typedef struct {
    gtext_script_t script;
    int      pc;                 /* 当前行号（0 基；存档字段） */
    int      shown;              /* 打字机字节进度 */
    char     title[96];
    char     name[48];
    char     text[640];
    char     bg[96];
    char     bg_key[96];         /* 已解码背景名（变更才重解） */
    unsigned bg_w, bg_h;        /* 解码图像实际尺寸 */
    uint8_t* bg_gray;            /* 684×700 会话缓冲（launch 分配/quit 释放） */
    int64_t  last_ms;
    bool     choosing;
    char     choices[4][168];
    int      choice_count;
    char     flags[8][24];
    char     save_path[192];
} gtext_state_t;

/* gtext_decode：book_image 管线薄封装（背景 684×700 灰度，保比例）。 */
static bool gtext_decode(gc_ctx_t* g, const char* file, uint8_t* out, size_t cap,
                         unsigned* w, unsigned* h);

/* 行访问器 + 前缀判定。 */
static char* line_at(gtext_state_t* s, int index) {
    if (index < 0 || index >= s->script.lines) return NULL;
    char* p = s->script.text;
    for (int i = 0; i < index; ++i) { p = strchr(p, '\n'); if (!p) return NULL; ++p; }
    return p;
}
static bool line_is(char* l, const char* tag, char** arg) {
    if (!l || strncmp(l, tag, strlen(tag)) != 0) return false;
    *arg = l + strlen(tag);
    while (**arg == ' ') ++*arg;
    char* nl = strchr(*arg, '\n');
    if (nl) *nl = 0;                       /* 就地截断（读态安全：text 可变） */
    return true;
}
static void copy_trim(char* dst, size_t cap, char* src, size_t n) {
    size_t take = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, take); dst[take] = 0;
    char* r = strchr(dst, '\r'); if (r) *r = 0;
}

static bool load_script(gtext_state_t* s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 2 * 1024 * 1024) { fclose(f); return false; }
    s->script.text = (char*)heap_caps_malloc((size_t)n + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->script.text) { fclose(f); return false; }
    size_t got = fread(s->script.text, 1, (size_t)n, f);
    fclose(f);
    s->script.text[got] = '\n'; s->script.text[got + 1] = 0;
    s->script.len = got + 1;
    s->script.lines = 0;
    for (size_t i = 0; i < s->script.len; ++i)
        if (s->script.text[i] == '\n') ++s->script.lines;
    return true;
}

static bool flag_has(gtext_state_t* s, const char* name) {
    for (int i = 0; i < 8; ++i) if (!strcmp(s->flags[i], name)) return true;
    return false;
}
static void flag_set(gtext_state_t* s, const char* name) {
    if (flag_has(s, name)) return;
    for (int i = 0; i < 8; ++i) if (!s->flags[i][0]) {
        snprintf(s->flags[i], sizeof(s->flags[i]), "%s", name);
        return;
    }
}
static int find_label(gtext_state_t* s, const char* name) {
    for (int i = 0; i < s->script.lines; ++i) {
        char* l = line_at(s, i); char* arg;
        if (line_is(l, "* label", &arg) && !strcmp(arg, name)) return i + 1;
    }
    return -1;
}

/* ===================== 走剧本（到下一句正文/选项/结局） ===================== */
static void run_to_pause(gc_ctx_t* g, gtext_state_t* s) {
    s->choosing = false; s->choice_count = 0; s->text[0] = 0; s->name[0] = 0;
    while (s->pc < s->script.lines) {
        char* l = line_at(s, s->pc); char* arg;
        if (!l) break;
        if (line_is(l, "* title", &arg)) { snprintf(s->title, sizeof(s->title), "%s", arg); s->pc++; continue; }
        if (line_is(l, "* bg", &arg))    { snprintf(s->bg, sizeof(s->bg), "%s", arg); s->pc++; g->area = (EpdRect){0,0,684,GTEXT_DIALOG_TOP}; g->want_full = false; continue; }
        if (line_is(l, "* name", &arg))  { snprintf(s->name, sizeof(s->name), "%s", arg); s->pc++; continue; }
        if (line_is(l, "* set", &arg))   { flag_set(s, arg); s->pc++; continue; }
        if (line_is(l, "* goto", &arg))  { int t = find_label(s, arg); if (t >= 0) s->pc = t; else s->pc++; continue; }
        if (line_is(l, "* ifnot", &arg) || line_is(l, "* if", &arg)) {   /* 长 tag 先试，防前缀误配 */
            bool not = l[3] == 'n';
            char name[24]; char* sp = strchr(arg, ' ');
            if (sp) {
                size_t n = (size_t)(sp - arg);
                copy_trim(name, sizeof(name), arg, n);
                char* target = sp + 1; while (*target == ' ') ++target;
                bool hit = flag_has(s, name) != not;
                int t = hit ? find_label(s, target) : -1;
                s->pc = hit && t >= 0 ? t : s->pc + 1;
            } else s->pc++;
            continue;
        }
        if (line_is(l, "* label", &arg)) { s->pc++; continue; }
        if (line_is(l, "* choice", &arg)) {
            char* tok = arg;
            while (*tok && s->choice_count < 4) {
                char* bar = strchr(tok, '|');
                size_t n = bar ? (size_t)(bar - tok) : strlen(tok);
                copy_trim(s->choices[s->choice_count++], sizeof(s->choices[0]), tok, n);
                if (!bar) break;
                tok = bar + 1;
            }
            s->choosing = s->choice_count > 0;
            s->pc++;
            s->shown = 0; s->last_ms = 0;
            return;
        }
        if (line_is(l, "* end", &arg)) { (void)arg; s->pc = s->script.lines + 1; return; }
        if (!strncmp(l, "#", 1)) { s->pc++; continue; }
        if (!strncmp(l, "* >", 3)) {          /* 正文：* > 文本 */ 
            copy_trim(s->text, sizeof(s->text), l + 3, strlen(l + 3));
            s->pc++; s->shown = 0; s->last_ms = 0;
            return;
        }
        if (!strncmp(l, ">", 1)) {            /* 正文：> 文本（推荐写法） */
            copy_trim(s->text, sizeof(s->text), l + 1, strlen(l + 1));
            s->pc++; s->shown = 0; s->last_ms = 0;
            return;
        }
        s->pc++;                              /* 其余空行/未知行跳过（宽容脚本） */
    }
}

/* ===================== 存档（pc + flags，单行文本） ===================== */
static void save(gtext_state_t* s) {
    if (!s->save_path[0]) return;
    FILE* f = fopen(s->save_path, "w");
    if (!f) return;
    fprintf(f, "%d", s->pc);
    for (int i = 0; i < 8; ++i) if (s->flags[i][0]) fprintf(f, " %s", s->flags[i]);
    fclose(f);
}
static bool restore(gtext_state_t* s) {
    FILE* f = fopen(s->save_path, "r");
    if (!f) return false;
    int pc = -1;
    if (fscanf(f, "%d", &pc) != 1 || pc < 0) { fclose(f); return false; }
    char name[24];
    while (fscanf(f, "%23s", name) == 1) flag_set(s, name);
    fclose(f);
    s->pc = pc;
    return true;
}

/* ===================== paint ===================== */
static void draw_line(void* user, const char* line, int y) {
    ui_text_vc((uint8_t*)user, 48, y, 28, line, EPD_DRAW_ALIGN_LEFT, false);
}
static void paint(gc_ctx_t* g, uint8_t* fb) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s) { gc_ui_nopkg(fb, "文字游戏", GC_CAT_TEXT); return; }
    if (s->pc > s->script.lines) {            /* * end 结局 */
        ui_clear_page(fb);
        ui_text_vc(fb, 342, 500, 60, "完", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 620, 24, s->title, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 1120, 18, "点按屏幕返回标题", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (s->choosing) {                        /* 选择页独立整页（先于场景绘制，省一次重画） */
        ui_clear_page(fb);
        gc_ui_dialog_frame(fb, 520);
        if (s->name[0]) gc_ui_name_tag(fb, 520, s->name);
        ui_text_vc(fb, 342, 584, 22, s->text[0] ? s->text : "请选择", EPD_DRAW_ALIGN_CENTER, false);
        for (int i = 0; i < s->choice_count; ++i)
            gc_ui_button(fb, (EpdRect){120, 660 + i * 80, 444, 64}, s->choices[i], false);
        return;
    }
    ui_clear_page(fb);
    if (s->bg[0] && s->bg_gray) {
        /* 背景缓存：文件名变更才重新解码（会话级，避免每帧重复 IO）。 */
        if (strcmp(s->bg_key, s->bg) != 0) {
            if (gtext_decode(g, s->bg, s->bg_gray, 684u * 700u, &s->bg_w, &s->bg_h))
                snprintf(s->bg_key, sizeof(s->bg_key), "%s", s->bg);
            else s->bg_key[0] = 0;
        }
        if (s->bg_key[0])
            gc_ui_blit_gray(fb, s->bg_gray, s->bg_w, s->bg_h, 0, 0, 684, GTEXT_DIALOG_TOP);
        else gc_ui_missing(fb, s->bg, 0, 0, 684, GTEXT_DIALOG_TOP);
    }
    gc_ui_dialog_frame(fb, GTEXT_DIALOG_TOP);
    gc_ui_name_tag(fb, GTEXT_DIALOG_TOP, s->name);
    gc_ui_typewrap(s->text, s->shown, 4, 20, draw_line, fb, 886, GTEXT_LH);
    if (s->text[0] && (size_t)s->shown >= strlen(s->text))
        gc_ui_wait_cursor(fb, 624, 1088);
}

/* gtext_decode：薄包一下 book_image 管线（launch 里为 bg_gray 分配 PSRAM）。 */
static bool gtext_decode(gc_ctx_t* g, const char* file, uint8_t* out, size_t cap,
                         unsigned* w, unsigned* h) {
    char path[224];
    snprintf(path, sizeof(path), "%s/%s", g->pkg_dir, file);
    unsigned ow = 0, oh = 0;
    if (!book_image_file_dimensions(path, true, &ow, &oh) || !ow || !oh) return false;
    unsigned fit_w = 684, fit_h = (unsigned)((uint64_t)oh * fit_w / ow);
    if (fit_h > 700) { fit_h = 700; fit_w = (unsigned)((uint64_t)ow * fit_h / oh); }
    if ((size_t)fit_w * fit_h > cap) return false;
    if (!book_image_file_grayscale(path, true, fit_w, fit_h, out)) return false;
    *w = fit_w; *h = fit_h;
    return true;
}

/* ===================== 输入 / tick ===================== */
static gc_ev_t ev_area(gc_ctx_t* g, EpdRect a) { g->area = a; g->want_full = false; return GC_EV_DIRTY; }

static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (ev->type != UI_GESTURE_TAP) return GC_EV_NONE;
    if (s->pc > s->script.lines) {            /* 结局→回标题 */
        s->pc = 0; s->flags[0][0] = 0; s->flags[1][0] = 0; s->flags[2][0] = 0;
        s->flags[3][0] = 0; s->flags[4][0] = 0; s->flags[5][0] = 0; s->flags[6][0] = 0; s->flags[7][0] = 0;
        run_to_pause(g, s);
        return ev_area(g, (EpdRect){0, 0, 684, 1216});
    }
    if (s->choosing) {
        for (int i = 0; i < s->choice_count; ++i) {
            EpdRect r = {120, 600 + i * 80, 444, 64};
            if (ui_rect_hit(r, ev->x0, ev->y0)) {
                char mark[32];
                snprintf(mark, sizeof(mark), "c%d", i);
                flag_set(s, mark);            /* 选项记标志：脚本可用 if c0 g 分支 */
                s->choosing = false;
                run_to_pause(g, s);
                return ev_area(g, (EpdRect){0, 0, 684, 1216});
            }
        }
        return GC_EV_NONE;
    }
    if ((size_t)s->shown < strlen(s->text)) {  /* 打字中点按=整句直出 */
        s->shown = (int)strlen(s->text);
        return ev_area(g, (EpdRect){0, GTEXT_DIALOG_TOP, 684, 1216 - GTEXT_DIALOG_TOP});
    }
    run_to_pause(g, s);
    save(s);
    return ev_area(g, (EpdRect){0, 0, 684, 1216});
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (k != UI_KEY_1) return GC_EV_NONE;
    ui_gesture_event_t tap = { .type = UI_GESTURE_TAP, .x0 = 342, .y0 = 900 };
    return input(g, &tap);
}

static gc_ev_t tick(gc_ctx_t* g) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (s->choosing || s->pc > s->script.lines) return GC_EV_NONE;
    size_t total = strlen(s->text);
    if ((size_t)s->shown >= total || total == 0) return GC_EV_NONE;
    if (g->app->now_ms - s->last_ms < 60) return GC_EV_NONE;
    s->last_ms = g->app->now_ms;
    unsigned char c0 = (unsigned char)s->text[s->shown];
    s->shown += c0 & 0x80 ? ((c0 & 0xf0) == 0xe0 ? 3 : 2) : 1;
    g->area = (EpdRect){0, GTEXT_DIALOG_TOP, 684, 1216 - GTEXT_DIALOG_TOP};
    g->want_full = false;
    return GC_EV_DIRTY;
}

/* ===================== 生命周期 ===================== */
static bool launch(gc_ctx_t* g, const char* title_arg) {
    (void)title_arg;
    gtext_state_t* s = (gtext_state_t*)calloc(1, sizeof(gtext_state_t));
    if (!s) return false;
    g->state = s;
    if (!g->pkg_dir[0]) { snprintf(s->title, sizeof(s->title), "文字游戏"); return true; }
    char path[224];
    snprintf(path, sizeof(path), "%s/story.txt", g->pkg_dir);
    if (!load_script(s, path)) return false;
    s->bg_gray = (uint8_t*)heap_caps_malloc(684u * 700u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(s->save_path, sizeof(s->save_path), "%s/.gtsave", g->pkg_dir);
    /* 开场跑到第一句（吸收头部 * title/bg）；随后若有存档则跳到存档点。 */
    run_to_pause(g, s);
    if (!s->title[0]) snprintf(s->title, sizeof(s->title), "%s", g->pkg_name);
    if (restore(s)) { s->shown = 0; run_to_pause(g, s); }
    return true;
}

static void quit(gc_ctx_t* g) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s) return;
    if (s->script.text) heap_caps_free(s->script.text);
    if (s->bg_gray) heap_caps_free(s->bg_gray);
    free(s);
    g->state = NULL;
}

const gc_plugin_t gc_plugin_gtext = {
    .id = "text",
    .name = "文字游戏",
    .cat = GC_CAT_TEXT,
    .launch = launch,
    .quit = quit,
    .paint = paint,
    .input = input,
    .key = key,
    .tick = tick,
};
