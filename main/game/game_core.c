/*
 * game_core.c — 内核主体：互斥实例、输入路由、刷新裁决、薄壳桥接。
 *
 * 精简原则：内核不猜游戏逻辑——它只保证「同一时刻一个插件、包已挂载、
 * 三键恒语义（中短=强刷/中长=退出/右键=退出）、刷新按插件申报执行」。
 */
#include <string.h>

#include "epdiy.h"        /* 横屏插件的旋转窗口（与 app_book 横屏阅读同一链路）。 */
#include "e0470_epaper_waveform.h" /* E0470_WAVEFORM：GL16 区域刷（present 出口）。 */
#include "display.h"      /* update_display_area_with / guard_draw_result。 */
#include "esp_log.h"
#include "ui_kit.h"       /* ui_clear_page：列表/插件整屏清白基线。 */
#include "ui_menu.h"   /* UI_KEY_*：三键恒语义键值。 */

#include "game_core.h"

static const char* TAG = "game_core";

/* ---------------- 互斥单实例 ---------------- */
static gc_ctx_t        s_ctx;            /* 唯一运行上下文 */
static bool            s_active;

/* 插件刷新申报 → app_redraw_t（桥接函数共用换算）。 */
static app_redraw_t resolve(gc_ev_t ev) {
    switch (ev) {
    case GC_EV_DIRTY: return s_ctx.want_full ? APP_REDRAW_FULL : APP_REDRAW_AREA;
    case GC_EV_PAGE:  return APP_REDRAW_PAGE;
    case GC_EV_QUIT:  return APP_REDRAW_NONE;
    default:          return APP_REDRAW_NONE;
    }
}

/*
 * 插件返回 GC_EV_DIRTY(→APP_REDRAW_AREA) 时的提交修复（2026-10-18 真机）：
 * 宿主 app_present() 的 AREA 分支“只推屏、不重绘”，它假定 fb 已被回调画好；
 * 但插件 input/key/tick 只改内部状态/播音效，并未把新画面画进 fb，于是点击
 * 后只有音效、画面不刷新，必须按中键强刷(FULL→render)才可见。
 * 对策：提交 AREA 前先让内核把当前插件新帧画进 fb（复用 gc_app_render 整帧
 * 重绘，AREA 推屏只推变化区域），保证“点一下即见新画面”。
 * 整屏 want_full 仍走宿主 FULL（宿主会自行 render），无需此处预绘。
 */
static app_redraw_t gc_commit(app_ctx_t* ctx, gc_ev_t ev) {
    if (ev == GC_EV_DIRTY && !s_ctx.want_full &&
        s_active && s_ctx.plugin && s_ctx.state) {
        gc_app_render(ctx, ctx->fb);   /* 先画好新帧，宿主 AREA 直接推屏 */
        return APP_REDRAW_AREA;
    }
    return resolve(ev);
}

/*
 * 自定义 present 出口（2026-10-18 真机：局部刷新显示质量）。
 * 宿主 AREA 分支用 APP_DYNAMIC_REFRESH_MODE=DU（触摸连续反馈用的 2 值
 * 快刷），对游戏 16 级灰画面（背景/立绘）灰阶全糊——中键强刷（GC16）
 * 与点击刷新观感差距悬殊即此因。游戏局部刷属于“定稿”性质，改走
 * MODE_GL16（与整页切换同质量，速度慢于 DU 但可接受）。
 * 只接管 AREA：FULL/PAGE 仍走宿主默认链。失败回退整屏 GL16。
 */
bool gc_app_present(app_ctx_t* ctx, app_redraw_t redraw) {
    enum EpdDrawError err = EPD_DRAW_SUCCESS;
    if (redraw == APP_REDRAW_AREA) {
        /* 档位（2026-10-18）：插件置 want_du=true 的纯黑白 UI 态（按钮
         * 翻转/文字推进）走 DU 快刷；默认 GL16 定稿保灰阶画质（背景/立绘）。 */
        err = update_display_area_with(ctx->hl, &E0470_WAVEFORM,
                                       s_ctx.want_du ? MODE_DU : MODE_GL16,
                                       gc_app_area(ctx));
        if (err != EPD_DRAW_SUCCESS)          /* 区域刷失败：整屏 GL16 补救 */
            err = update_display_mode(ctx->hl, MODE_GL16);
        guard_draw_result(ctx->hl, err);
        return true;                          /* 已自刷，宿主不再处理 */
    }
    return false;                             /* 其余走宿主默认链 */
}

const char* gc_cat_prefix(gc_cat_t cat) {
    switch (cat) {
    case GC_CAT_AVGR: return "avgr";
    case GC_CAT_TEXT: return "text";
    case GC_CAT_BBK:  return "bbk";
    default:          return "";
    }
}

const gc_plugin_t* gc_active(void) { return s_active ? s_ctx.plugin : NULL; }

/* ---------------- 列表状态机（v2，用户 2026-10-18） ----------------
 * 三入口先进入分类列表页（扫到的全部包），点选后才进入游戏；
 * 横屏插件（avgr）的列表与游戏画面都是横屏 1216×684。 */

/* 进入插件分类页 → 扫包 → 停在列表（不选包）。 */
void gc_open_list(const gc_plugin_t* p, app_ctx_t* app) {
    if (!p || !app) return;
    gc_shutdown();
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.app = app;
    s_ctx.plugin = p;
    s_ctx.pkg_count = gc_pkg_scan(p->cat, (char*)s_ctx.pkg_list,
                                  sizeof(s_ctx.pkg_list[0]), 8);
    s_ctx.listing = true;
    s_active = true;
    ESP_LOGI(TAG, "list %s packages=%d", p->id, s_ctx.pkg_count);
}

/* 列表选择 → 挂载包并启动插件（launch 失败也留在本页可重选）。 */
app_redraw_t gc_pick_package(int index) {
    const gc_plugin_t* p = s_ctx.plugin;
    if (!s_active || !p || !s_ctx.listing) return APP_REDRAW_NONE;
    if (index < 0 || index >= s_ctx.pkg_count) return APP_REDRAW_NONE;
    s_ctx.listing = false;
    snprintf(s_ctx.pkg_name, sizeof(s_ctx.pkg_name), "%s", s_ctx.pkg_list[index]);
    const char* dir = gc_pkg_mount(p->cat, index);
    if (dir) snprintf(s_ctx.pkg_dir, sizeof(s_ctx.pkg_dir), "%s", dir);
    else { s_ctx.pkg_dir[0] = 0; ESP_LOGW(TAG, "pkg mount failed: %s", s_ctx.pkg_name); }
    /* launch 失败：仍调 quit 让插件回收半初始化的分配（avgr 每次
     * 漏 ~900KB PSRAM 的教训 2026-10-18：连坐锁屏字体流/书封/字体）。 */
    bool ok = p->launch && p->launch(&s_ctx, s_ctx.pkg_name);
    if (!ok) {
        if (s_ctx.state && p->quit) p->quit(&s_ctx);
        s_ctx.state = NULL;
    }
    ESP_LOGI(TAG, "launch %s pkg=%s ok=%d", p->id, s_ctx.pkg_name, ok);
    return APP_REDRAW_FULL;
}

bool gc_launch(const gc_plugin_t* p, const char* title_arg, app_ctx_t* app) {
    /* 兼容旧 API：直接启动分类内第一个包（新入口走 gc_app_enter 列表）。 */
    if (!p || !app) return false;
    gc_shutdown();
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.app = app;
    s_ctx.plugin = p;
    (void)title_arg;
    s_ctx.pkg_count = gc_pkg_scan(p->cat, (char*)s_ctx.pkg_list,
                                  sizeof(s_ctx.pkg_list[0]), 8);
    if (s_ctx.pkg_count > 0) {
        /* 与 gc_pick_package 同一启动链（含失败回收）。 */
        s_ctx.listing = true;
        app_redraw_t r = gc_pick_package(0);
        (void)r;
        return s_ctx.state != NULL;
    }
    s_active = true;
    return false;
}

void gc_shutdown(void) {
    if (!s_active) return;
    if (s_ctx.plugin && s_ctx.plugin->quit) s_ctx.plugin->quit(&s_ctx);
    gc_pkg_unmount();
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_active = false;
    ESP_LOGI(TAG, "shutdown");
}

/* ---------------- 薄壳桥接：app_desc_t 回调 → 内核 ---------------- */

/* 竟屏 (x,y) →横屏 (X,Y)：物理(y,683-x)＝横屏逻辑（app_book 横屏链路同构）。 */
static ui_gesture_event_t to_landscape_ev(const ui_gesture_event_t* ev) {
    ui_gesture_event_t e = *ev;
    uint16_t nx = e.x0, ny = e.y0;
    e.x0 = ny; e.y0 = (uint16_t)(683 - nx);
    nx = e.x; ny = e.y;
    e.x = ny;  e.y = (uint16_t)(683 - nx);
    return e;
}

/* 横屏插件旋转窗口内直绘（画完必恢复系统常态 INVERTED_PORTRAIT）。 */
static void paint_plugin(const gc_plugin_t* p, uint8_t* fb) {
    if (p->landscape) {
        epd_set_rotation(EPD_ROT_LANDSCAPE);
        if (!s_ctx.state) gc_ui_nopkg_land(fb, p->name, p->cat);
        else if (p->paint) p->paint(&s_ctx, fb);
        epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);
    } else {
        if (!s_ctx.state) gc_ui_nopkg(fb, p->name, p->cat);
        else if (p->paint) p->paint(&s_ctx, fb);
    }
}

void gc_app_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (!s_active || !s_ctx.plugin) return;
    ui_clear_page(fb);                     /* 常态旋转下整屏清白＝物理整屏 */
    if (s_ctx.listing) {
        gc_ui_pkg_list(fb, s_ctx.plugin->name, (const char* const*)s_ctx.pkg_list,
                       s_ctx.pkg_count, s_ctx.plugin->cat);
        return;
    }
    paint_plugin(s_ctx.plugin, fb);
}

app_redraw_t gc_app_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    (void)ctx;
    s_ctx.want_du = false;
    if (!s_active || !s_ctx.plugin) return APP_REDRAW_NONE;
    if (s_ctx.listing) {
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        /* 列表页恒竖屏（用户 2026-10-18）：竖持点选，不做坐标变换；
         * 进入游戏后才切横屏。 */
        return gc_pick_package(gc_ui_pkg_list_hit((int)ev->x0, (int)ev->y0,
                                                  s_ctx.pkg_count));
    }
    if (!s_ctx.state || !s_ctx.plugin->input) return APP_REDRAW_NONE;
    ui_gesture_event_t local = *ev;
    if (s_ctx.plugin->landscape) local = to_landscape_ev(ev);
    return gc_commit(ctx, s_ctx.plugin->input(&s_ctx, &local));
}

app_redraw_t gc_app_key(app_ctx_t* ctx, int key) {
    /* 三键恒语义（v2 2026-10-18 用户修订）：中短=整屏强刷、中长=退出；
     * 右键在游戏内=返回/取消（关浮层/回标题，由插件裁决）——不再直接
     * 退出整个游戏（误触丢进度）；仅列表页/插件不处理右键时维持退出。
     * 其余键发给插件。 */
    s_ctx.want_du = false;
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    if (key == UI_KEY_3) {
        if (s_ctx.listing || !s_active) { ctx->request_return = true; return APP_REDRAW_NONE; }
        if (!s_ctx.state || !s_ctx.plugin->key) { ctx->request_return = true; return APP_REDRAW_NONE; }
        gc_ev_t ev = s_ctx.plugin->key(&s_ctx, key);
        if (ev == GC_EV_QUIT) { ctx->request_return = true; return APP_REDRAW_NONE; }
        return gc_commit(ctx, ev);
    }
    if (s_ctx.listing || !s_active || !s_ctx.state || !s_ctx.plugin->key)
        return APP_REDRAW_NONE;
    gc_ev_t ev = s_ctx.plugin->key(&s_ctx, key);
    if (ev == GC_EV_QUIT) { ctx->request_return = true; return APP_REDRAW_NONE; }
    return gc_commit(ctx, ev);
}

app_redraw_t gc_app_key_long(app_ctx_t* ctx, int key) {
    /* 恒语义（冻结）：长按中键 = 退出游戏回应用页。 */
    if (key == UI_KEY_2) { ctx->request_return = true; return APP_REDRAW_NONE; }
    return APP_REDRAW_NONE;
}

app_redraw_t gc_app_tick(app_ctx_t* ctx) {
    (void)ctx;
    s_ctx.want_du = false;
    if (s_ctx.listing || !s_active || !s_ctx.state || !s_ctx.plugin->tick)
        return APP_REDRAW_NONE;
    return gc_commit(ctx, s_ctx.plugin->tick(&s_ctx));
}

app_redraw_t gc_app_enter(const gc_plugin_t* p, const char* title, app_ctx_t* ctx) {
    (void)title;                /* 入口无外部参数：先进列表页选包。 */
    gc_open_list(p, ctx);
    return APP_REDRAW_FULL;
}

EpdRect gc_app_area(app_ctx_t* ctx) {
    (void)ctx;
    EpdRect a = s_ctx.area.width ? s_ctx.area : (EpdRect){0, 0, 684, 1216};
    if (s_active && s_ctx.plugin && s_ctx.plugin->landscape && !s_ctx.listing) {
        /* 横屏矩形→竖屏逻辑：{684-y-h, x, h, w}（时钟锁屏同构换算）。
         * 竖屏画布 684 宽×1216 高；横屏矩形恒在其内，防御性钳制即可。 */
        int x = 684 - (int)a.y - (int)a.height;
        if (x < 0) x = 0;
        EpdRect p = {(uint16_t)x, a.x, a.height, a.width};
        if (p.x + p.width > 684) p.width = 684 - p.x;
        if (p.y + p.height > 1216) p.height = 1216 - p.y;
        return p;
    }
    return a;
}

void gc_app_exit(app_ctx_t* ctx)       { (void)ctx; gc_shutdown(); }
void gc_app_media_lost(app_ctx_t* ctx) { (void)ctx; gc_shutdown(); }
