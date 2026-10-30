/*
 * gtext.c — 对话游戏插件（text-<name>.zip / 目录，包根 story.txt）。
 *
 * Story Engine v1 的 game 渲染端：脚本执行共用 main/game/story_kernel.c，
 * 本文件只做渲染与交互——背景/立绘/对话框/选项/结局页，全走 game_host_ui。
 *
 * 2026-10-27 用户反馈修复：
 *  - 打字机整体移除（墨水屏一字一刷闪烁慢；改整句一次上屏，一击=一刷）；
 *  - 对话正文区扩为 8 行 × 21 字全宽（原 4 行×10 字截断长讲解）；
 *  - 图像原尺寸直读：已按设备分辨率烘焙的图不再缩放重采样（糊的根源）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book_cover.h"
#include "esp_heap_caps.h"
#include "epdiy.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

/* 剧情/存档路径拼接的保守截断告警豁免（缓冲与内核包路径体系一致）。 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "game_core.h"
#include "story_kernel.h"
#include "quiz_engine.h"

#include "avg_asset.h"

#define GTEXT_DIALOG_TOP  840
#define GTEXT_LH          44
#define GTEXT_TEXT_LINES  8     /* 正文行数（0..7）；8 行 × 44px 自 886 到 1208 */
#define GTEXT_TEXT_UNITS  42    /* 半宽单位/行（21 汉字，28px × 21 = 588px 宽） */
#define GTEXT_BG_W        684
#define GTEXT_BG_H        840
#define GTEXT_CH_W        470
#define GTEXT_CH_H        640
#define GTEXT_CHAR_BASE   792   /* 立绘基线：与对话框顶隔 48px 背景带（2026-10-30*/
                                /* 用户：对话框与立绘背景区视觉隔开） */
#define GTEXT_CHOICE_Y0   640
#define GTEXT_CHOICE_H    64
#define GTEXT_CHOICE_STEP 72

typedef struct {
    sk_state_t*  sk;                       /* PSRAM（脚本+变量+label 表） */
    quiz_engine_t* quiz;                  /* 题库模式（包内 quiz.txt；NULL=叙事模式） */
    avg_assets_t assets;                   /* bg/char 灰度+莱版 LRU 缓存 */
    sk_out_t     cur;                      /* 最近暂停事件 */
    int          pause_line;               /* 暂停行：存档/恢复重放当前句（不跳句） */
    bool         choosing;
    bool         ended;                    /* 结局终页 */
    bool         title_page;               /* 2026-10-30：入包先停标题页（新的开始/继续存档） */
    bool         has_save;                 /* 标题页可选项：是否存在存档（launch 时探测） */
    int64_t      save_flashed_ms;          /* 2026-10-30：存档后顶栏反馈时间戳 */
    int64_t      last_step_ms;             /* 2026-10-28 推进节流：GL16 刷屏慢，防连点跳句 */
    int          meta_plays;               /* 2026-10-30 檐下谣：跨周目通关数（.gtmeta） */
    bool         ending_counted;           /* 本局已计通关（防重放/恢复重复计数） */
    char         lock_flash[96];           /* cchoice 锁定项点击提示（选择页顶部，下次点击即滑） */
    char         meta_path[192];           /* .gtmeta 侧车文件（周目持久化） */
    char         save_path[192];
} gtext_state_t;

/* ===================== 图片解码（g8 快路径 + PNG 原尺寸直读） =====================
 * 糊的根源（2026-10-27）：旧版一律 contain 缩放重采样——烘焙图 684×513 再被
 * bilinear 搓一遍，抖动网点变噪声。现规则：
 *  - .g8（转换器预烘焙，含 1bit 蒙版）：fread 直读，零解码零缩放；
 *  - PNG 尺寸已在目标框内：原尺寸直读（fw=ow, fh=oh），跳过重采样；
 *  - PNG 超框：才做 contain 缩放（老行为兜底）。 */

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
    /* .png 引用时伴生同名 .g8 优先（转换器烘焙，带蒙版）。 */
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
    unsigned fw, fh;
    if (ow <= max_w && oh <= max_h) {         /* 原尺寸直读：免重采样（糊的根源） */
        fw = ow; fh = oh;
    } else {
        fw = max_w; fh = (unsigned)((uint64_t)oh * max_w / ow);
        if (fh > max_h || fh == 0) { fh = max_h; fw = (unsigned)((uint64_t)ow * max_h / oh); }
    }
    if (!fw || !fh) return false;
    if (!book_image_file_grayscale(path, true, fw, fh, out)) return false;
    quantize16(out, (size_t)fw * fh);
    *w = fw; *h = fh;
    return true;
}

/* 包内素材路径多级回退：images/<sub>/<名> → images/<名> → <名>。
 * 名无扩展名时自动试 .png / .g8（脚本书写友好）；g8 由解码快路径接手。 */
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

/* ===================== 走剧本（到下一暂停点） =====================
 * 打字机已移除（2026-10-27）：整句一次上屏，一击一刷。
 * 2026-10-30 檐下谣：END 首次触达即计通关（meta_plays+1 落盘 .gtmeta；
 * 存档里的 playCount 变量兼作已计标记——恢复重放/二次进入不重复计）。 */
static void gm_write_meta(gtext_state_t* s) {
    if (!s->meta_path[0]) return;
    FILE* f = fopen(s->meta_path, "w");
    if (!f) return;
    fprintf(f, "%d\n", s->meta_plays);
    fclose(f);
}

static void gm_apply_ng(gtext_state_t* s) {     /* 重开/新局：二周目标记回落（按开局时 counts） */
    if (s->sk && s->meta_plays > 0) sk_flag_set(s->sk, "ng");
}

static void run_to_pause(gtext_state_t* s) {
    s->choosing = false;
    s->ended = false;
    for (;;) {
        int prev = s->sk->pc;             /* 暂停行（存档/恢复重放该句） */
        sk_event_t ev = sk_step(s->sk, &s->cur);
        if (ev == SK_EV_TEXT)  { s->pause_line = prev; return; }
        if (ev == SK_EV_CHOICE) { s->pause_line = prev; s->choosing = true; return; }
        if (ev == SK_EV_END || ev == SK_EV_EOF) { s->pause_line = prev; s->ended = true; break; }  /* 檐下谣：break 后统一结计（end*+eof 皆计） */
        /* SK_EV_NONE：副作用已消化，继续 */
    }
    if (s->ended && !s->ending_counted) {
        if (sk_var_get(s->sk, "playCount") == 0) {          /* 本局首次触达结局：计通关 */
            s->meta_plays++;
            sk_var_set(s->sk, "playCount", (int16_t)s->meta_plays);
            gm_write_meta(s);
        }
        s->ending_counted = true;
    }
}

/* ===================== 存档 ===================== */

static void save(gtext_state_t* s) {
    if (!s->save_path[0]) return;
    FILE* f = fopen(s->save_path, "w");
    if (!f) return;
    char buf[768];
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

/* 国势栏（2026-10-28 明末风云）：背景层之上、对话框之下，顶带 684×44。
 * 半灰底+发丝框+黑字，墨水屏 GL16 全刷时随场景一起上。
 * 2026-10-30（文绎合体入口，用户拍板）：顶栏旧「存/退」小按钮移除——
 * 存读档/退出全部改走 gstory 中短键弹菜单；本栏恢复整宽。
 * 存档反馈：存档后顶栏显示"已存档"替代变量名（墨水屏无 toast，借顶栏
 * 位置反馈；下一次推进句子时自然还原）。 */
static void paint_status(gtext_state_t* s, uint8_t* fb) {
    if (s->sk->status[0] || s->save_flashed_ms) {
        ui_fill_round_rect(fb, (EpdRect){12, 6, 660, 42}, 10, 0xf2);
        ui_draw_round_rect(fb, (EpdRect){12, 6, 660, 42}, 10, 0xc2);   /* GC_HAIR 同值 */
        if (s->save_flashed_ms)
            ui_text_vc(fb, 342, 27, 21, "已存档 ✓", EPD_DRAW_ALIGN_CENTER, false);
        else
            ui_text_vc(fb, 342, 27, 21, s->sk->status, EPD_DRAW_ALIGN_CENTER, false);
        if (s->sk->status[0]) s->sk->status_dirty = false;
    }
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
    }
    sk->bg_dirty = false;   /* 2026-10-28：移到 if 外——* bg none 清空背景时不再残留 dirty */

    /* 立绘 3 槽（底对齐对话框；L/C/R 落位；含透明蒙版）。
     * 2026-10-28 修复：旧版绘制包在 char_dirty 里——dirty=false 的整帧重画
     * （中键强刷/锁屏恢复/结局回退后的推进）画不出立绘，fb 与屏幕脱钩，
     * 真机表现“立绘凭空消失/时有时无”（17sui 实测现象的引擎根因）。
     * paint_scene 本就是全量重画语义：立绘每次都画（avg_assets LRU 命中，
     * 零解码开销）；char_dirty 只由 input 裁决推屏区域（变化→推全屏）。 */
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
        /* 2026-10-30：立绘基线上移到 CHAR_BASE，与对话框顶隔 48px 背景带——
         * 对话推进只刷对话框条时，立绘与分隔带全程静止，观感干净。 */
        int y = GTEXT_CHAR_BASE - (int)h;
        gc_ui_blit_gray_a(fb, gray, w, h, mask, x, y, 684, GTEXT_CHAR_BASE);
    }
    sk->char_dirty = false;

    paint_status(s, fb);                     /* 国势栏：图层在立绘上、对话框下 */

    gc_ui_dialog_frame(fb, GTEXT_DIALOG_TOP);
    /* 说话人标签画在框内顶（2026-10-30 用户：“对话框上面也刷新一部分，没必要”：
     * 旧骑缝标签上探 top-30，局部推屏被迫从 780 起含 60px 背景带。改框内签
     * （top+6 高 40，与正文首行 886 相切零遮挡）后局部推屏可从 840 整起，
     * 对话推进时立绘/背景/国势栏全部静止。） */
    /* 名牌贴对话框顶线（2026-10-30 真机反馈"姓名牌压着对话文字"修正：
     * ui_text_vc 的 y 是垂直中心——旧版名牌底=886 与首行 center=886 的字顶 872
     * 重叠 14px。新几何：名牌 842..874（32 高），正文 y0=892（首行字顶 878，
     * 与名牌隔 4px；末行 center 1200 字底 1214 入屏）。 */
    if (s->cur.name[0]) {
        ui_fill_round_rect(fb, (EpdRect){48, GTEXT_DIALOG_TOP + 2, 176, 32}, 10, 0x00);
        ui_text_vc(fb, 136, GTEXT_DIALOG_TOP + 18, 18, s->cur.name, EPD_DRAW_ALIGN_CENTER, true);
    }
    gc_ui_typewrap(s->cur.text, (int)strlen(s->cur.text), GTEXT_TEXT_LINES, GTEXT_TEXT_UNITS,
                   draw_line, fb, 892, GTEXT_LH);
    if (s->cur.text[0])
        gc_ui_wait_cursor(fb, 648, 1196);
}

static void paint(gc_ctx_t* g, uint8_t* fb) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s) { gc_ui_nopkg(fb, "对话游戏", GC_CAT_TEXT); return; }
    if (s->quiz) { quiz_paint(s->quiz, fb); return; }          /* 题库模式 */

    /* 2026-10-30 标题页：入包先停这（新的开始/继续存档），不直接进剧情。
     * 用户反馈：进入时应有新的开始与选择存档两项。 */
    if (s->title_page && !s->ended) {
        gc_ui_title_page(fb, s->sk->title[0] ? s->sk->title : g->pkg_name,
                         "对话游戏", s->has_save);
        return;
    }

    if (s->ended) {                         /* 结局页（* ending 文案或无名 * end） */
        ui_clear_page(fb);
        ui_text_vc(fb, 342, 400, 64, "完", EPD_DRAW_ALIGN_CENTER, false);
        /* 2026-10-30（用户：完结总结显示不全，参考画皮自动换行）：画皮同款折行
         * 46 半宽单位/行（23 汉字）、26px 字、行高 44，最多 9 行（207 字）。 */
        if (s->cur.text[0]) {
            char ls[9][160];
            int n = 0;
            for (int i = 0; s->cur.text[i] && n < 9; ) {
                int units = 0, j = i;
                while (s->cur.text[j]) {
                    unsigned char c = (unsigned char)s->cur.text[j];
                    int clen = (c & 0x80) ? ((c & 0xf0) == 0xe0 ? 3 : ((c & 0xf0) == 0xf0 ? 4 : 2)) : 1;
                    int cw = (c >= 0x80) ? 2 : 1;
                    if (units + cw > 46) break;
                    units += cw; j += clen;
                }
                int m = j - i;
                if (m > 158) m = 158;
                memcpy(ls[n], s->cur.text + i, (size_t)m);
                ls[n][m] = 0;
                ++n; i = j;
            }
            for (int k = 0; k < n; ++k)
                ui_text_vc(fb, 342, 520 + k * 44, 26, ls[k], EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text_vc(fb, 342, 1024, 22, s->sk->title, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 1118, 18, "点按屏幕重新开始 · 中长按退出", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (s->choosing) {                       /* 选择页独立整页（≤6 项竖排） */
        ui_clear_page(fb);
        paint_status(s, fb);                  /* 决策要看国势：选项页也常驻状态栏 */
        gc_ui_dialog_frame(fb, 520);
        if (s->cur.text[0])
            ui_text_vc(fb, 342, 584, 22, s->cur.text, EPD_DRAW_ALIGN_CENTER, false);
        for (int i = 0; i < s->cur.choice_count; ++i)
            gc_ui_button(fb, (EpdRect){120, GTEXT_CHOICE_Y0 + i * GTEXT_CHOICE_STEP,
                                       444, GTEXT_CHOICE_H}, s->cur.choice_text[i], false);
        return;
    }
    paint_scene(s, g, fb);
}

/* ===================== 输入 ===================== */

static gc_ev_t ev_area(gc_ctx_t* g, EpdRect a) { g->area = a; g->want_full = false; return GC_EV_DIRTY; }

static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (ev->type != UI_GESTURE_TAP) return GC_EV_NONE;
    if (s->quiz) {                                              /* 题库模式 */
        if (quiz_tap(s->quiz, (int)ev->x0, (int)ev->y0))
            return ev_area(g, (EpdRect){0, 0, 684, 1216});
        return GC_EV_NONE;
    }

    /* 2026-10-30 标题页：新的开始 / 继续存档（gc_ui_title_page 同几何命中）。 */
    if (s->title_page && !s->ended) {
        if (ui_rect_hit((EpdRect){222, 620, 240, 64}, ev->x0, ev->y0)) {
            s->title_page = false;
            sk_restart(s->sk);
            gm_apply_ng(s);                  /* 2026-10-30：二周目标记按 meta 回落 */
            s->ending_counted = false;
            run_to_pause(s);
            save(s);
            return ev_area(g, (EpdRect){0, 0, 684, 1216});
        }
        if (s->has_save && ui_rect_hit((EpdRect){222, 704, 240, 64}, ev->x0, ev->y0)) {
            s->title_page = false;
            if (restore(s)) { s->has_save = true; }
            else { sk_restart(s->sk); gm_apply_ng(s); run_to_pause(s); }
            return ev_area(g, (EpdRect){0, 0, 684, 1216});
        }
        return GC_EV_NONE;
    }

    /* 2026-10-30 文绎：旧顶栏「存/退」触摸按钮已移除（存读档改走中键弹菜单），
     * 此处直接进入剧情推进/选项/结局分支。 */

    if (s->ended) {                         /* 结局 → 重新探案：不回标题页 */
        sk_restart(s->sk);                   /* 信任/线索/标记全部清零 */
        gm_apply_ng(s);                      /* 二周目标记（meta_plays 已在 END 时计入） */
        s->ending_counted = false;
        run_to_pause(s);
        save(s);
        return ev_area(g, (EpdRect){0, 0, 684, 1216});
    }
    if (s->choosing) {
        bool had_hint = s->lock_flash[0] != 0;
        s->lock_flash[0] = 0;                /* 提示只活到下一次点击 */
        for (int i = 0; i < s->cur.choice_count; ++i) {
            EpdRect r = {120, GTEXT_CHOICE_Y0 + i * GTEXT_CHOICE_STEP, 444, GTEXT_CHOICE_H};
            if (ui_rect_hit(r, ev->x0, ev->y0)) {
                if (sk_choice_is_locked(s->sk, i)) {       /* 锁定项：不响应，弹提示 */
                    snprintf(s->lock_flash, sizeof(s->lock_flash), "%s",
                             sk_choice_hint(s->sk, i));
                    return ev_area(g, (EpdRect){0, 0, 684, 1216});
                }
                sk_choose(s->sk, i);
                run_to_pause(s);
                save(s);
                return ev_area(g, (EpdRect){0, 0, 684, 1216});
            }
        }
        if (had_hint)                        /* 点了空处：清提示重画 */
            return ev_area(g, (EpdRect){0, 0, 684, 1216});
        return GC_EV_NONE;
    }
    /* 2026-10-28 节流：GL16 全屏刷新约 1-2s，刷屏期间的连点会连跳多句（用户
     * 实测反馈“取消打字机后对话连着走不停顿”）。推进句 500ms 内的重复输入
     * 丢弃；选项选择/结局重开不受限（独立整页，不存在刷新延迟误触）。 */
    int64_t now = g->app ? g->app->now_ms : 0;
    if (now - s->last_step_ms < 500) return GC_EV_NONE;
    s->last_step_ms = now;
    s->save_flashed_ms = 0;                  /* 反馈只显示到下一次推进 */
    run_to_pause(s);                        /* 一击一句：整句上屏一次刷新 */
    save(s);
    /* 2026-10-28 对话框局部刷（用户实测：全屏 GL16 每句闪烁明显，参照逆转裁判的
     * 场景帧缓存思路）：同场景推进只推对话框条（含骑缝名牌与正文 780..1216），
     * 背景立绘屏上保持，推屏面积减 2/3 闪烁大幅减轻；背景/立绘变化时才整屏重推。 */
    /* 2026-10-28 对话框局部刷 + 2026-10-30 修正：标签入框后推屏自对话框顶 840 整起
     * （旧 780 起含 60px 背景带——用户实测“对话框上面也刷新一部分，观感不好”）。
     * 同场景推进只推对话框条，背景/立绘/分隔带/国势栏全部静止；
     * 背景/立绘/国势变化时才整屏重推。 */
    if (s->choosing || s->ended ||
        s->sk->bg_dirty || s->sk->char_dirty || s->sk->status_dirty)
        return ev_area(g, (EpdRect){0, 0, 684, 1216});
    return ev_area(g, (EpdRect){0, GTEXT_DIALOG_TOP, 684, 1216 - GTEXT_DIALOG_TOP});
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    if (k != UI_KEY_1) return GC_EV_NONE;
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (s->quiz) {
        if (quiz_middle_key(s->quiz))
            return ev_area(g, (EpdRect){0, 0, 684, 1216});
        return GC_EV_NONE;
    }
    ui_gesture_event_t tap = { .type = UI_GESTURE_TAP, .x0 = 342, .y0 = 900 };
    return input(g, &tap);
}

/* ===================== 生命周期 ===================== */

static void quit(gc_ctx_t* g);              /* 前向：launch 失败自清用 */

static bool launch(gc_ctx_t* g, const char* title_arg) {
    (void)title_arg;
    gtext_state_t* s = (gtext_state_t*)calloc(1, sizeof(gtext_state_t));
    if (!s) return false;
    g->state = s;
    if (!g->pkg_dir[0]) return true;        /* 无包：paint 画引导页 */

    /* 题库模式优先：包内 quiz.txt（每包二者居一）。 */
    char qpath[256];
    snprintf(qpath, sizeof(qpath), "%s/quiz.txt", g->pkg_dir);
    FILE* probe = fopen(qpath, "r");
    if (probe) {
        fclose(probe);
        char ppath[192];
        snprintf(ppath, sizeof(ppath), "%s/.quizprog", g->pkg_dir);
        s->quiz = quiz_load(qpath, ppath);
        if (s->quiz) return true;
    }

    s->sk = (sk_state_t*)heap_caps_calloc(1, sizeof(sk_state_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->sk) return false;

    char path[256];
    snprintf(path, sizeof(path), "%s/story.txt", g->pkg_dir);
    if (!sk_load(s->sk, path)) { quit(g); return false; }   /* 失败自清防泄漏 */
    snprintf(s->save_path, sizeof(s->save_path), "%s/.gtsave", g->pkg_dir);
    snprintf(s->meta_path, sizeof(s->meta_path), "%s/.gtmeta", g->pkg_dir);

    avg_assets_init(&s->assets);
    avg_assets_set_decoder(gtext_image_decode);

    /* 2026-10-30 流程改造：先停标题页（新的开始/继续存档），不直接进剧情。
     * sk_load 后变量/名牌已复位，run_to_pause 只是探到首句——点“新的开始”才
     * 真正开跑；有存档探测到后亮“继续游戏”。 */
    snprintf(s->save_path, sizeof(s->save_path), "%s/.gtsave", g->pkg_dir);
    {
        FILE* probe = fopen(s->save_path, "r");
        s->has_save = probe != NULL;
        if (probe) fclose(probe);
    }
    if (!s->sk->title[0]) snprintf(s->sk->title, sizeof(s->sk->title), "%s", g->pkg_name);
    s->title_page = true;
    /* 2026-10-30 檐下谣：跨周目通关计数（.gtmeta：结局时 END 首触即 +1 落盘；
     * 开局/重开回落 f:ng，存档内 playCount 变量兼作已计标记）。 */
    {
        FILE* mf = fopen(s->meta_path, "r");
        if (mf) {
            char ml[32];
            if (fgets(ml, sizeof(ml), mf)) s->meta_plays = atoi(ml);
            fclose(mf);
        }
    }
    /* 预热首句（标题页后无论选哪项都从首句重跑/恢复重放，这里不需要推进） */
    return true;
}

static void quit(gc_ctx_t* g) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s) return;
    if (s->quiz) { quiz_free(s->quiz); }
    if (s->sk) { sk_free(s->sk); heap_caps_free(s->sk); }
    avg_assets_release(&s->assets);
    free(s);
    g->state = NULL;
}

/* ===================== 文绎合体入口（gstory）菜单桥接 =====================
 * 中键弹菜单的存/读档出口（2026-10-30）：题库模式无剧情档，返回 false。 */
bool gtext_save(gc_ctx_t* g) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s || !s->sk) return false;
    save(s);
    return true;
}

bool gtext_restore(gc_ctx_t* g) {
    gtext_state_t* s = (gtext_state_t*)g->state;
    if (!s || !s->sk) return false;
    return restore(s);
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
    .tick = NULL,                            /* 打字机已移除：无周期驱动 */
};
