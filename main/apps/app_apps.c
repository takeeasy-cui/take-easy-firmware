/*
 * 应用页（APPS）：固定 4 列 × 5 行图标架构（最多 20 格）。
 * RickyOS 风格圆角方 tile（112px / 圆角 28 / 3px 描边）+ 56px 线性图标 +
 * tile 下 12px 居中标签。排序规则（用户 2026-10-20 拍板）：
 * 所有新增应用排在推理游戏前，三个游戏图标保持在最后——
 * 首行：文件管理/传输文件/计算器/便签；第二行：番茄时钟/木鱼/推理/文字；步步高殿后。
 * 其余格子留空白（不加占位框），后续新应用按序插入游戏之前。
 * 微信读书 tile 已删（微信传书入口在传输文件页）。
 *
 * Fixed 4x5 icon grid (up to 20 cells), RickyOS-style 112 px tiles with 56 px
 * line icons and centered labels. New apps precede the games (user rule);
 * only real apps draw; empty cells stay blank. V1.0.3 恢复推理游戏入口
 * （GameCore + avgr 插件一直在编译，TF 卡包 /sdcard/games/avgr-*.zip）。
 */
#include <math.h>
#include <stdio.h>

#include "app.h"
#include "app_registry.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_nothing.h"

#define APPS_TILE   112
#define APPS_TILE_R 28
#define APPS_STROKE 3
#define APPS_LABEL_PX 20
#define APPS_ICON_INK 56
/* 4x5 网格几何：内容区 48..636 宽；网格 200..NX_NAV_TOP-16 高。 */
#define APPS_COLS 4
#define APPS_ROWS 5

typedef struct {
    const char* label;
    void (*icon)(uint8_t*, int, int, uint8_t);
    const app_desc_t* (*page)(void);
    bool enabled;
} apps_entry_t;

static const app_desc_t* page_files(void) { return app_at(4); }
static const app_desc_t* page_calc(void) { return app_at(5); }
extern const app_desc_t app_avgr;
extern const app_desc_t app_notes;
extern const app_desc_t app_pomodoro;
extern const app_desc_t app_muyu;
static const app_desc_t* page_avgr(void)   { return &app_avgr; }
static const app_desc_t* page_notes(void) { return &app_notes; }
static const app_desc_t* page_pomodoro(void) { return &app_pomodoro; }
static const app_desc_t* page_muyu(void) { return &app_muyu; }
static const app_desc_t* page_transfer(void) {
    extern const app_desc_t app_transfer;
    return &app_transfer;
}

/* ---------------- 56px 线性图标（Nothing 线描语言） ---------------- */
static void icon_files(uint8_t* fb, int cx, int cy, uint8_t ink) {
    const int w = 5;
    int x0 = cx - 19, y0 = cy - 21, x1 = cx + 19, y1 = cy + 21;
    nothing_thick_line(fb, x0 + 3, y0, x1 - 8, y0, w, ink);
    nothing_thick_line(fb, x1 - 8, y0, x1, y0 + 8, w, ink);
    nothing_thick_line(fb, x1, y0 + 8, x1, y1, w, ink);
    nothing_thick_line(fb, x1, y1, x0, y1, w, ink);
    nothing_thick_line(fb, x0, y1, x0, y0, w, ink);
    nothing_thick_line(fb, x1 - 8, y0, x1 - 8, y0 + 8, w, ink);
    nothing_thick_line(fb, x0 + 7, y0 + 13, x1 - 7, y0 + 13, w, ink);
    nothing_thick_line(fb, x0 + 7, y0 + 20, x1 - 7, y0 + 20, w, ink);
    nothing_thick_line(fb, x0 + 7, y0 + 27, x0 + 17, y0 + 27, w, ink);
}

static void icon_transfer(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 信号弧 + 天线（与状态栏 WiFi/传输语言一致，56px） */
    for (int r = 8; r <= 16; r += 8) {
        for (int a = 200; a <= 340; a += 8) {
            float rad = a * 0.01745f;
            epd_fill_circle(cx + (int)(cosf(rad) * r),
                            cy + 3 + (int)(sinf(rad) * r), 2, ink, fb);
        }
    }
    nothing_thick_line(fb, cx - 2, cy - 4, cx - 2, cy + 12, 5, ink);
    nothing_thick_line(fb, cx + 2, cy - 4, cx + 2, cy + 12, 5, ink);
}

static void icon_calc(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 计算器：圆角外框 + 顶部显示条 + 右下小方块 */
    EpdRect body = {cx - 17, cy - 21, 34, 42};
    ui_fill_round_rect(fb, body, 5, ink);
    ui_fill_round_rect(fb, (EpdRect){body.x + 2, body.y + 2, body.width - 4, body.height - 4},
                       3, 0xf0);
    ui_fill_round_rect(fb, (EpdRect){body.x + 5, body.y + 5, 24, 7}, 2, ink);
    ui_fill_round_rect(fb, (EpdRect){body.x + 5, body.y + 26, 7, 7}, 2, ink);
}

static void icon_avgr(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 法槌：斜置槌头 + 柄 + 垫块（推理游戏入口）。 */
    ui_fill_round_rect(fb, (EpdRect){cx - 20, cy - 17, 26, 16}, 4, ink);
    nothing_thick_line(fb, cx + 4, cy - 4, cx + 19, cy + 13, 7, ink);
    nothing_thick_line(fb, cx - 16, cy + 16, cx + 14, cy + 16, 5, ink);
}

static void icon_notes(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 便签：圆角卡描边 + 两行内容线 + 右下铅笔斜置（新增/编辑语义）。 */
    EpdRect body = {cx - 17, cy - 20, 34, 40};
    ui_draw_round_rect(fb, body, 6, ink);
    ui_draw_round_rect(fb, (EpdRect){body.x + 1, body.y + 1, body.width - 2, body.height - 2}, 5, ink);
    nothing_thick_line(fb, cx - 11, cy - 9, cx + 10, cy - 9, 4, ink);
    nothing_thick_line(fb, cx - 11, cy - 1, cx + 4, cy - 1, 4, ink);
    nothing_thick_line(fb, cx + 6, cy + 1, cx + 13, cy + 8, 4, ink);
    nothing_thick_line(fb, cx - 11, cy + 7, cx - 2, cy + 7, 4, ink);
}

static void icon_pomodoro(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 番茄钟：圆表盘 + 两根指针 + 顶部小叶柄。 */
    epd_draw_circle(cx, cy + 4, 21, ink, fb);
    epd_draw_circle(cx, cy + 4, 20, ink, fb);
    nothing_thick_line(fb, cx, cy + 4, cx + 11, cy - 10, 4, ink);
    nothing_thick_line(fb, cx, cy + 4, cx - 8, cy - 1, 5, ink);
    nothing_thick_line(fb, cx - 3, cy - 18, cx + 3, cy - 21, 4, ink);
    epd_fill_circle(cx, cy - 23, 4, ink, fb);
}

static void icon_muyu(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 木鱼：圆身 + 右前开口 + 圆点眼 + 斜置槌。 */
    const int bx = cx - 4;
    epd_draw_circle(bx, cy, 19, ink, fb);
    epd_draw_circle(bx, cy, 18, ink, fb);
    ui_fill_rect_fast((EpdRect){bx + 11, cy - 12, 12, 24}, 0xf0, fb);
    nothing_thick_line(fb, bx + 11, cy - 12, bx + 11, cy + 12, 3, ink);
    epd_fill_circle(bx - 5, cy - 11, 3, ink, fb);
    nothing_thick_line(fb, bx + 15, cy + 16, cx + 18, cy + 20, 5, ink);
    epd_fill_circle(cx + 20, cy + 21, 6, ink, fb);
}

static const apps_entry_t k_entries[7] = {
    {"文件管理", icon_files, page_files, true},
    {"文件传输", icon_transfer, page_transfer, true},
    {"计算器", icon_calc, page_calc, true},
    {"便签", icon_notes, page_notes, true},
    {"番茄时钟", icon_pomodoro, page_pomodoro, true},
    {"木鱼", icon_muyu, page_muyu, true},
    {"推理游戏", icon_avgr, page_avgr, true},
};

static EpdRect cell_rect(int index) {
    /* 4 列 × 5 行；绘制与命中共用这一套几何。 */
    /* 4x5 grid; drawing and hit testing share this geometry. */
    int grid_top = 96;
    int grid_bottom = NX_NAV_TOP - 16;
    int col = index % APPS_COLS, row = index / APPS_COLS;
    int w = (684 - 2 * NX_MARGIN) / APPS_COLS;
    int h = (grid_bottom - grid_top) / APPS_ROWS;
    return (EpdRect){NX_MARGIN + col * w, grid_top + row * h, w, h};
}

static void draw_tile(uint8_t* fb, EpdRect cell, const apps_entry_t* e) {
    int block_h = APPS_TILE + 12 + 30;
    int block_y = cell.y + (cell.height - block_h) / 2;
    EpdRect tile = {cell.x + (cell.width - APPS_TILE) / 2, block_y, APPS_TILE, APPS_TILE};
    uint8_t line = e->enabled ? NX8_INK : NX8_DIM;
    /* 3px 粗描边：实心块 + 内缩铺页底色 */
    ui_fill_round_rect(fb, tile, APPS_TILE_R, line);
    ui_fill_round_rect(fb, (EpdRect){tile.x + APPS_STROKE, tile.y + APPS_STROKE,
                                     APPS_TILE - 2 * APPS_STROKE, APPS_TILE - 2 * APPS_STROKE},
                       APPS_TILE_R - APPS_STROKE, 0xf0);
    e->icon(fb, tile.x + APPS_TILE / 2, tile.y + APPS_TILE / 2, line);
    ui_text_vc(fb, cell.x + cell.width / 2, block_y + APPS_TILE + 12 + 15,
               APPS_LABEL_PX, e->label, EPD_DRAW_ALIGN_CENTER, false);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, 684, NX_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    /* 仅画真实应用，空格留白；不再有“更多应用正在接入”占位框 */
    for (int i = 0; i < (int)(sizeof(k_entries) / sizeof(k_entries[0])); ++i)
        draw_tile(fb, cell_rect(i), &k_entries[i]);
    ui_nav_draw(fb, 2);
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    /* v2（用户 2026-10-20）：左右滑动换主页面（与底部导航按键等效；起点在导航热区不触发）。 */
    if (ev->type == UI_GESTURE_SWIPE_L && ev->y0 < UI_NAV_TOUCH_TOP) { ui_nav_request(ctx, 3); return APP_REDRAW_NONE; }
    if (ev->type == UI_GESTURE_SWIPE_R && ev->y0 < UI_NAV_TOUCH_TOP) { ui_nav_request(ctx, 1); return APP_REDRAW_NONE; }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    for (int i = 0; i < (int)(sizeof(k_entries) / sizeof(k_entries[0])); ++i) {
        if (!k_entries[i].enabled || !k_entries[i].page) continue;
        if (ui_rect_hit(cell_rect(i), ev->x0, ev->y0)) {
            ctx->request_app = k_entries[i].page();
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static bool apps_main_visible(app_ctx_t* ctx) { (void)ctx; return true; }

const app_desc_t app_apps = {
    .title = "应用 Apps",
    .detail = "文件、传输与工具",
    .enter_full = false,
    .owns_keys = false,
    .main_page_visible = apps_main_visible,
    .render = render,
    .on_gesture = on_gesture,
};
