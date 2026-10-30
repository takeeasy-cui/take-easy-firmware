/*
 * gbbk.c — 步步高 BBK 游戏插件（伏魔记）。bbk_ 前缀插件体系第一员。
 *
 * v3（2026-10-30 真机反馈）：恢复用户认可的旧版「方案 B 王者风格圆形按键」
 *  ——左下大圆盘十字方向盘 + 右下三键弧排（菜单/确认/取消），坐标沿用旧版；
 *  ——对话/字幕/黑框提示/战斗消息/状态页浮层时自动隐藏按键，点屏任意处推进；
 *  ——退出＝长按物理中键（内核恒语义）；左物理键＝确认。
 *  刷新修正：键帽区不再"侥幸恒存"——覆盖态切换走整屏重绘，屏内走动只推游戏
 *  脏区；圆盘压在游戏区上时在 game blit 之后再叠画，保证叠合区推屏正确。
 *
 * 素材：原版 DAT.LIB 点阵 6x（无 FMJHD）。资源：/sdcard/games/bbk-fmj/。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "ui_kit.h"
#include "ui_menu.h"

/* 资源路径拼接（pkg_dir+文件名）的保守截断告警豁免。 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "game_core.h"

#include "bbk_present.h"
#include "fmj_canvas.h"
#include "fmj_datlib.h"
#include "fmj_font.h"
#include "fmj_game.h"
#include "fmj_script.h"
#include "fmj_combat.h"
#include "fmj_screen.h"

typedef struct {
    bool running;          /* 资源齐备且引擎已启动 */
    bool diff_paint;       /* true=本次 paint 由 input 触发（差分）；否则全量 */
    bool overlay_last;     /* 上次绘制的覆盖态（按键是否隐藏），用于检测切换 */
    int  du_count;         /* 2026-10-30：连续 DU 计数（每 8 次插一次 GL16 清残影） */
    fmj_canvas_t *canvas;  /* 160x96 画布（PSRAM） */
} gbbk_state_t;

/* ---------- 圆形虚拟按键（横屏逻辑坐标，沿用旧版方案 B 位置） ----------
 * 左下：大圆盘十字方向盘，中心 (152,534) 半径 108；
 * 右下：三键弧排 —— 菜单 (1000,574) 确认 (1112,462) 取消 (1176,580)。 */
typedef struct { int x0, y0, x1, y1; } zone_t;
#define ZONE(cx, cy, hw, hh) { (cx) - (hw), (cy) - (hh), (cx) + (hw), (cy) + (hh) }
static const zone_t Z_UP     = ZONE(152, 480, 26, 32);
static const zone_t Z_DOWN   = ZONE(152, 594, 26, 32);
static const zone_t Z_LEFT   = ZONE(80, 534, 32, 26);
static const zone_t Z_RIGHT  = ZONE(224, 534, 32, 26);
static const zone_t Z_MENU   = ZONE(1000, 574, 44, 44);
static const zone_t Z_OK     = ZONE(1112, 462, 62, 62);
static const zone_t Z_CANCEL = ZONE(1176, 580, 48, 48);

static const uint8_t G_OKTXT[]   = {0xC8, 0xB7, 0xC8, 0xCF, 0};  /* 确认 */
static const uint8_t G_CANTXT[]  = {0xC8, 0xA1, 0xCF, 0xFB, 0};  /* 取消 */
static const uint8_t G_MENUTXT[] = {0xB2, 0xCB, 0xB5, 0xA5, 0};  /* 菜单 */

static bool in_zone(const zone_t *z, int x, int y) {
    return x >= z->x0 && x <= z->x1 && y >= z->y0 && y <= z->y1;
}

static void feed_key(int key) {
    fmj_game_input(key, false);
    fmj_game_input(key, true);
}

/* 覆盖态：对话/字幕/黑框提示/战斗消息/状态物品页 → 隐藏按键、点屏推进。 */
static bool overlay_active(void) {
    return fmj_script_saying() || fmj_script_gut_active() || fmj_script_msg_active() ||
           fmj_combat_msg_active() || fmj_game_menu_overlay();
}

/* ---------- 圆形绘制原语（扫描线填圆 + 描边环） ---------- */
static void fill_circle(uint8_t *fb, int cx, int cy, int r, uint8_t v) {
    for (int dy = -r; dy <= r; ++dy) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        fmj_screen_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, v, fb);
    }
}

static void ring_circle(uint8_t *fb, int cx, int cy, int r, int lw, uint8_t v) {
    for (int dy = -r; dy <= r; ++dy) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        if (dx < lw) { fmj_screen_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, v, fb); continue; }
        fmj_screen_fill_rect(cx - dx, cy + dy, lw, 1, v, fb);
        fmj_screen_fill_rect(cx + dx - lw + 1, cy + dy, lw, 1, v, fb);
    }
}

static void tri(uint8_t *fb, int cx, int cy, int dir, int size, uint8_t ink) {
    for (int k = 0; k < size; ++k) {
        int half = k;
        switch (dir) {
        case 0: fmj_screen_fill_rect(cx - half, cy - size + k, half * 2 + 1, 1, ink, fb); break;
        case 1: fmj_screen_fill_rect(cx - half, cy + size - k - 1, half * 2 + 1, 1, ink, fb); break;
        case 2: fmj_screen_fill_rect(cx - size + k, cy - half, 1, half * 2 + 1, ink, fb); break;
        default: fmj_screen_fill_rect(cx + size - k - 1, cy - half, 1, half * 2 + 1, ink, fb); break;
        }
    }
}

/* 圆形键面文字（水平/垂直居中在圆心）。 */
static void circle_label(uint8_t *fb, const uint8_t *gbk, int cx, int cy,
                         int scale, uint8_t fg) {
    int w = fmj_screen_text_width(gbk, scale);
    int h = 16 * scale;
    fmj_screen_text(gbk, cx - w / 2, cy - h / 2, scale, fg, fb);
}

static void draw_keypad(uint8_t *fb) {
    /* 左下：十字方向盘（大圆底盘 + 圆头臂 + 方向三角 + 中心圆） */
    fill_circle(fb, 152, 534, 108, 0xE8);            /* 半透明浅灰底盘 */
    ring_circle(fb, 152, 534, 108, 3, 0x4A);          /* 底盘深灰描边 */
    ring_circle(fb, 152, 534, 94, 2, 0x98);           /* 底盘中灰内环 */
    fmj_screen_fill_rect(126, 472, 52, 128, 0xD6, fb); /* 竖臂主体 */
    fill_circle(fb, 152, 472, 26, 0xD6);               /* 竖臂上圆头 */
    fill_circle(fb, 152, 600, 26, 0xD6);               /* 竖臂下圆头 */
    fmj_screen_fill_rect(78, 510, 148, 48, 0xD6, fb);  /* 横臂主体 */
    fill_circle(fb, 78, 534, 24, 0xD6);                /* 横臂左圆头 */
    fill_circle(fb, 226, 534, 24, 0xD6);               /* 横臂右圆头 */
    tri(fb, 152, 478, 0, 10, 0x3C);                    /* 上 */
    tri(fb, 152, 590, 1, 10, 0x3C);                    /* 下 */
    tri(fb, 94, 534, 2, 10, 0x3C);                     /* 左 */
    tri(fb, 210, 534, 3, 10, 0x3C);                    /* 右 */
    fill_circle(fb, 152, 534, 32, 0x4A);              /* 中心圆描边层 */
    fill_circle(fb, 152, 534, 29, 0xCF);              /* 中心圆面 */
    /* 右下：三键弧排（深色描边层 + 浅灰键面 + 文字标签） */
    fill_circle(fb, 1000, 574, 43, 0x55);             /* 菜单：描边层 */
    fill_circle(fb, 1000, 574, 40, 0xED);             /* 菜单：键面 */
    circle_label(fb, G_MENUTXT, 1000, 574, 2, 0x3C);
    fill_circle(fb, 1112, 462, 61, 0x3C);             /* 确认：描边层（主键） */
    fill_circle(fb, 1112, 462, 58, 0xE2);             /* 确认：键面 */
    ring_circle(fb, 1112, 462, 49, 2, 0x8A);          /* 确认：内环装饰 */
    circle_label(fb, G_OKTXT, 1112, 462, 2, 0x2E);
    fill_circle(fb, 1176, 580, 47, 0x55);             /* 取消：描边层 */
    fill_circle(fb, 1176, 580, 44, 0xED);             /* 取消：键面 */
    circle_label(fb, G_CANTXT, 1176, 580, 2, 0x3C);
}

/* ---------- 生命周期 ---------- */
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
    char dat[224], hzk[224], asc[224];
    snprintf(dat, sizeof(dat), "%s/DAT.LIB", g->pkg_dir);
    snprintf(hzk, sizeof(hzk), "%s/HZK16", g->pkg_dir);
    snprintf(asc, sizeof(asc), "%s/ASC16", g->pkg_dir);
    if (!file_exists(dat) || !file_exists(hzk) || !file_exists(asc)) return true;

    s->canvas = (fmj_canvas_t*)heap_caps_malloc(sizeof(fmj_canvas_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->canvas) return false;
    if (fmj_datlib_init(dat) != 0) return false;
    if (fmj_font_init(hzk, asc) != 0) { fmj_datlib_deinit(); return false; }
    if (bbk_present_init() != 0) {
        fmj_font_deinit();
        fmj_datlib_deinit();
        return false;
    }
    fmj_game_start();
    s->running = true;
    return true;
}

static void quit(gc_ctx_t* g) {
    gbbk_state_t* s = (gbbk_state_t*)g->state;
    if (s) {
        if (s->running) {
            fmj_game_stop();          /* 内含 fmj_script_reset：释放全部剧情 op 堆块 */
            fmj_res_image_flush();    /* 2026-10-30 内存审计：128 槽数 MB 图片缓存 */
            bbk_present_deinit();     /* 退出前从未清空——跨插件残留，挤压下一游戏 */
            fmj_font_deinit();        /* + ASC16 4KB                       */
            fmj_datlib_deinit();      /* HZK16 262KB + DAT.LIB 557KB + 索引 24KB */
            s->running = false;
        }
        if (s->canvas) heap_caps_free(s->canvas);
        free(s);
    }
    g->state = NULL;
}

/* ---------- 渲染 ----------
 * 内核调用链（fb 在 render 前均被 ui_clear_page 整屏清白）：
 *  - 进游戏/中键强刷/锁屏恢复（diff_paint=false）：整屏重绘（白底+游戏全量+键帽），
 *    宿主 FULL 整屏推；覆盖态时不画键帽。
 *  - input 触发（diff_paint=true）：差分只 blit 游戏脏区到 fb → 无覆盖态则在游戏
 *    之上叠画圆形键帽（圆盘压在游戏区，叠合区随之推屏）→ 申报脏区 GL16 局部推。
 *    覆盖态切换（对话/菜单浮现或消失）时升级整屏重绘。
 * 想走整屏刷新：g->want_full 恒置 false（仅宿主 FULL 走整屏 GC16）。 */
static void paint(gc_ctx_t* g, uint8_t* fb) {
    gbbk_state_t* s = (gbbk_state_t*)g->state;
    if (!s || !s->running) { gc_ui_nopkg_land(fb, "步步高游戏 · 伏魔记", GC_CAT_BBK); return; }

    bool overlay = overlay_active();
    bool full_mode = !s->diff_paint || (overlay != s->overlay_last);

    fmj_game_draw(s->canvas);
    EpdRect area;
    bool full;

    if (full_mode) {
        fmj_screen_fill_rect(0, 0, FMJ_LW, FMJ_LH, 0xFF, fb);   /* 横屏全屏白底 */
        bbk_present_reset();
        (void)bbk_present(s->canvas, fb, &area, &full);
        if (!overlay) draw_keypad(fb);
        g->area = (EpdRect){0, 0, FMJ_LW, FMJ_LH};
        g->want_full = false;   /* 整屏已由本次 paint 填满；宿主 FULL 时忽略 area */
    } else {
        if (bbk_present(s->canvas, fb, &area, &full)) {
            if (!overlay) draw_keypad(fb);   /* 游戏 blit 后再叠圆盘，叠合区正确 */
            /* 2026-10-30 闪烁优化：原版点阵本纯黑白（仅 AA 中灰），小区域
             * 走动差分改 DU 快刷（不反色直写，墨水屏几乎无闪）；大面积差分
             * （相机跳滚）/战斗（PIC 灰阶）/覆盖态仍走 GL16 保画质；连续 8 次
             * DU 后插一次 GL16 清残影。 */
            bool du = !overlay && !fmj_combat_active() &&
                      (int)area.width * (int)area.height * 4 < FMJ_LW * FMJ_LH;
            if (du && s->du_count >= 8) { du = false; s->du_count = 0; }
            else if (du) ++s->du_count;
            g->want_du = du;
            g->area = area;
        } else {
            g->area = (EpdRect){0, 0, 2, 2}; /* 零变化：顶角 2x2 无感兑底 */
        }
        g->want_full = full;
    }
    s->overlay_last = overlay;
    s->diff_paint = false;
}

/* ---------- 输入 ---------- */
static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    gbbk_state_t* s = (gbbk_state_t*)g->state;
    if (!s || !s->running || ev->type != UI_GESTURE_TAP) return GC_EV_NONE;
    /* 插件 landscape=true：手势坐标已由内核 to_landscape_ev 转为横屏 */
    int x = (int)ev->x0, y = (int)ev->y0;

    /* 覆盖态：点击屏幕任意处推进（状态/物品页=返回） */
    if (overlay_active()) {
        feed_key(fmj_game_menu_overlay() ? FMJ_KEY_CANCEL : FMJ_KEY_ENTER);
        s->diff_paint = true;
        return GC_EV_DIRTY;
    }

    int key = -1;
    if (in_zone(&Z_UP, x, y)) key = FMJ_KEY_UP;
    else if (in_zone(&Z_DOWN, x, y)) key = FMJ_KEY_DOWN;
    else if (in_zone(&Z_LEFT, x, y)) key = FMJ_KEY_LEFT;
    else if (in_zone(&Z_RIGHT, x, y)) key = FMJ_KEY_RIGHT;
    else if (in_zone(&Z_MENU, x, y)) key = FMJ_KEY_CANCEL;  /* 菜单：取消键=呼出主菜单 */
    else if (in_zone(&Z_OK, x, y)) key = FMJ_KEY_ENTER;
    else if (in_zone(&Z_CANCEL, x, y)) key = FMJ_KEY_CANCEL;
    if (key > 0) {
        feed_key(key);
        s->diff_paint = true;
        return GC_EV_DIRTY;
    }
    return GC_EV_NONE;
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    gbbk_state_t* s = (gbbk_state_t*)g->state;
    if (!s || !s->running) return GC_EV_NONE;
    if (k == UI_KEY_1) {                      /* 左键=确认（GameCore 统一） */
        feed_key(FMJ_KEY_ENTER);
        s->diff_paint = true;
        return GC_EV_DIRTY;
    }
    return GC_EV_NONE;
}

static gc_ev_t tick(gc_ctx_t* g) {
    (void)g;
    return GC_EV_NONE;                        /* 原版事件驱动步进，无帧动画 */
}

const gc_plugin_t gc_plugin_gbbk = {
    .id = "bbk",
    .name = "步步高游戏",
    .cat = GC_CAT_BBK,
    .landscape = true,      /* 横屏插件：内核包旋转窗口+手势坐标互换（同 avgr） */
    .launch = launch,
    .quit = quit,
    .paint = paint,
    .input = input,
    .key = key,
    .tick = tick,
};