/*
 * gbbk.c — 步步高 BBK 游戏插件（伏魔记）。bbk_ 前缀插件体系第一员。
 *
 * v1（2026-10-18 骨架）：分类/包/入口/互斥先就位；资源就绪性检测
 * （DAT.LIB/HZK16/ASC16，FMJHD.BIN 可选），资源缺失画统一引导页。
 * FMJ 引擎移植（160×96 画布 ×7 横屏输出+触摸虚拟按键）按既定方案
 * 从伏魔记-固件集成目录逐文件接入（见 docs/gamedev 开发文档路线图）。
 *
 * 资源包：/sdcard/games/bbk-<name>/ 目录直挂（DAT.LIB 数十 MB，不走 zip）。
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ui_kit.h"
#include "ui_menu.h"

/* 资源路径拼接（pkg_dir+文件名）的保守截断告警豁免。 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "game_core.h"

typedef struct {
    bool ready;
    bool hd_ready;
    char dat[192], hzk[192], asc[192], hd[192];
} gbbk_state_t;

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static bool launch(gc_ctx_t* g, const char* title_arg) {
    (void)title_arg;
    gbbk_state_t* s = (gbbk_state_t*)calloc(1, sizeof(gbbk_state_t));
    if (!s) return false;
    g->state = s;
    if (!g->pkg_dir[0]) return true;          /* 无包：paint 引导页 */
    /* 就绪检测：三个基础库齐 = 可启动；HD 库可选（缺失降级为原版点阵）。 */
    snprintf(s->dat, sizeof(s->dat), "%s/DAT.LIB", g->pkg_dir);
    snprintf(s->hzk, sizeof(s->hzk), "%s/HZK16", g->pkg_dir);
    snprintf(s->asc, sizeof(s->asc), "%s/ASC16", g->pkg_dir);
    snprintf(s->hd,  sizeof(s->hd),  "%s/FMJHD.BIN", g->pkg_dir);
    s->ready = file_exists(s->dat) && file_exists(s->hzk) && file_exists(s->asc);
    s->hd_ready = s->ready && file_exists(s->hd);
    return true;
}

static void quit(gc_ctx_t* g) {
    free(g->state);
    g->state = NULL;
}

static void paint(gc_ctx_t* g, uint8_t* fb) {
    gbbk_state_t* s = (gbbk_state_t*)g->state;
    if (!s || !s->ready) { gc_ui_nopkg(fb, "步步高游戏 · 伏魔记", GC_CAT_BBK); return; }
    /* 资源就绪占位页：引擎接入中（v1 骨架）。 */
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){120, 300, 444, 3}, 0x00, fb);
    ui_text_vc(fb, 342, 240, 46, "伏魔记", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 356, 24, "资源已就绪 · 引擎接入中", EPD_DRAW_ALIGN_CENTER, false);
    epd_fill_rect((EpdRect){120, 414, 444, 3}, 0x00, fb);
    ui_text_vc(fb, 342, 540, 20, "DAT.LIB / HZK16 / ASC16 已找到", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 584, 18, s->hd_ready ? "FMJHD.BIN 高清库已找到（7x 横屏）"
                                             : "FMJHD.BIN 未找到（将使用原版点阵）",
               EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 1120, 15, "长按中键退出 · GameCore 内核 v1", EPD_DRAW_ALIGN_CENTER, false);
}

const gc_plugin_t gc_plugin_gbbk = {
    .id = "bbk",
    .name = "步步高游戏",
    .cat = GC_CAT_BBK,
    .launch = launch,
    .quit = quit,
    .paint = paint,
    /* input/key/tick：骨架期留空（内核 NULL 安全跳过）。 */
};
