/*
 * 便签（用户 2026-10-20 新增，锤子便签参考）：
 * TF 卡 /sdcard/notes/ 一条一文件（note_YYYYMMDD_HHMMSS.txt，UTF-8 单段）。
 * 列表页：最新在上（文件名时间戳倒序），预览一行 + 日期；左右滑翻页（每页 6 条）；
 * 长按条目出删除确认；底部「新建便签」。编辑页：追加式单段文本（光标恒尾部），
 * 中文键盘 top=694，与首页天气编辑器同构的局部刷新呈现；
 * DONE / 中键 / 右键 = 冲刷组合并保存回列表；左键沿用全局快捷菜单。
 * 首次进入若目录为空：一次性导入旧备忘录（NVS todo1..3），写 .imported 标记防重。
 * 墨水屏增量编辑成本高，本版为最低限实现：不换行（单段追加），全文保存不截断。
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "app_registry.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "display_main_waveform.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_nothing.h"
#include "ui_keyboard.h"
#include "ui_text_input.h"

#define NOTES_DIR      "/sdcard/notes"
#define NOTES_CAP      64            /* 最多列出 64 条（名字倒序） */
#define NOTES_PER_PAGE 6
#define NOTE_TEXT_MAX  512
#define NOTES_ROW_H    82
#define NOTE_LINES     11            /* 编辑区可见行数（行高 46） */

typedef struct {
    char name[80];                   /* 文件名（不含路径） */
    char text[120];                  /* 首行预览（119B，换行折叠空格） */
} note_item_t;

static note_item_t s_items[NOTES_CAP];
static int s_count;
static int s_page;
static bool s_loaded;
static bool s_editing;
static bool s_edit_new;              /* 新建（无旧文件） */
static char s_edit_name[80];
static char s_buf[NOTE_TEXT_MAX];
static ui_text_edit_t s_edit;
static EpdRect s_kb_area;
static bool s_kb_layout, s_kb_settle;
static int s_delete_idx = -1;

static EpdRect notes_box(void) { return (EpdRect){36, 150, 612, 530}; }

static bool date_now(struct tm *out) {
    time_t now = time(NULL);
    if (now < 1704067200) {
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        if (pmu && pmu->time_synced) now = pmu->unix_sec;
    }
    if (now < 1704067200) { memset(out, 0, sizeof(*out)); return false; }
    now += 8 * 3600;
    gmtime_r(&now, out);
    return true;
}

static void note_preview(const char *name, char *out, size_t cap) {
    out[0] = 0;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", NOTES_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t got = fread(out, 1, cap - 1, f);
    fclose(f);
    out[got] = 0;
    for (char *p = out; *p; ++p) if (*p == '\n' || *p == '\r') *p = ' ';
    while (got && out[got - 1] == ' ') out[--got] = 0;
}

/* 首次导入旧备忘录（目录为空且未导入过，含已删空防重放）。 */
static void notes_import_todo_once(void) {
    struct stat st;
    char marker[96];
    snprintf(marker, sizeof(marker), "%s/.imported", NOTES_DIR);
    DIR *d = opendir(NOTES_DIR);
    if (!d) { mkdir(NOTES_DIR, 0775); d = opendir(NOTES_DIR); }
    if (!d) return;
    bool any = false;
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') { any = true; break; }
    closedir(d);
    if (any || stat(marker, &st) == 0) return;
    struct tm tm_bj;
    if (!date_now(&tm_bj)) { time_t t = 1704067200; gmtime_r(&t, &tm_bj); }
    for (int i = 2; i >= 0; --i) {               /* i=2 先写：todo1 名字最大排最上 */
        const char *t = app_settings_todo(i);
        if (!t || !t[0]) continue;
        char path[160];
        snprintf(path, sizeof(path), "%s/note_%04d%02d%02d_01%02d00.txt", NOTES_DIR,
                 tm_bj.tm_year + 1900, tm_bj.tm_mon + 1, tm_bj.tm_mday, i);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(t, 1, strlen(t), f); fclose(f); }
    }
    FILE *f = fopen(marker, "wb");
    if (f) { fputc('1', f); fclose(f); }
}

static void notes_scan(void) {
    s_count = 0; s_loaded = true;
    DIR *d = opendir(NOTES_DIR);
    if (!d) { notes_import_todo_once(); d = opendir(NOTES_DIR); }
    if (!d) { if (s_page) s_page = 0; return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5 || strcmp(e->d_name + n - 4, ".txt")) continue;
        if (!s_count) notes_import_todo_once();  /* 首扫即空目录时补导入 */
        int pos = s_count < NOTES_CAP ? s_count++ : NOTES_CAP - 1;
        while (pos > 0 && strcmp(e->d_name, s_items[pos - 1].name) > 0) {
            if (pos < NOTES_CAP) s_items[pos] = s_items[pos - 1];
            --pos;
        }
        snprintf(s_items[pos].name, sizeof(s_items[pos].name), "%s", e->d_name);
        s_items[pos].text[0] = 0;
    }
    closedir(d);
    for (int i = 0; i < s_count; ++i)
        if (!s_items[i].text[0]) note_preview(s_items[i].name, s_items[i].text, sizeof(s_items[i].text));
    if (s_page * NOTES_PER_PAGE >= s_count) s_page = 0;
}

static bool notes_save(void) {
    struct tm tm_bj;
    if (!date_now(&tm_bj)) { time_t t = 1704067200; gmtime_r(&t, &tm_bj); }
    char path[160];
    if (!s_edit_new && s_edit_name[0]) {
        snprintf(path, sizeof(path), "%s/%s", NOTES_DIR, s_edit_name);
    } else {
        snprintf(s_edit_name, sizeof(s_edit_name), "note_%04d%02d%02d_%02d%02d%02d.txt",
                 tm_bj.tm_year + 1900, tm_bj.tm_mon + 1, tm_bj.tm_mday,
                 tm_bj.tm_hour, tm_bj.tm_min, tm_bj.tm_sec);
        snprintf(path, sizeof(path), "%s/%s", NOTES_DIR, s_edit_name);
    }
    FILE *f = fopen(path, "wb");
    if (!f) { mkdir(NOTES_DIR, 0775); f = fopen(path, "wb"); }
    if (!f) return false;
    fwrite(s_buf, 1, strlen(s_buf), f);
    fclose(f);
    return true;
}

/* ============ 编辑器 ============ */

/* 折行绘制 s_buf 到卡内（28px / 行高 46 / 内容宽 548），尾部加光标。 */
static void editor_paint_text(uint8_t *fb, EpdRect box, bool cursor) {
    const char *p = s_buf;
    int line_y = box.y + 38;
    for (int row = 0; row < NOTE_LINES && *p; ++row) {
        char line[96];
        size_t use = 0;
        while (*p && use + 4 < sizeof(line)) {
            size_t len = ((unsigned char)*p & 0x80)
                       ? ((unsigned char)*p & 0xe0) == 0xc0 ? 2
                       : ((unsigned char)*p & 0xf0) == 0xe0 ? 3 : 4 : 1;
            memcpy(line + use, p, len); use += len; line[use] = 0; p += len;
            if (ttf_text_width_px(ui_text_effective_px(28), line) > 548) { use -= len; p -= len; break; }
        }
        line[use] = 0;
        if (!*p && cursor) snprintf(line + use, sizeof(line) - use, "_");
        ui_text_vc(fb, box.x + 32, line_y, 28, line, EPD_DRAW_ALIGN_LEFT, false);
        line_y += 46;
    }
    if (!s_buf[0] && cursor) ui_text_vc(fb, box.x + 32, box.y + 38, 28, "_", EPD_DRAW_ALIGN_LEFT, false);
}

static void render_editor(uint8_t *fb) {
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, 684, NX_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    ui_text_vc(fb, 342, 106, 34, s_edit_new ? "新建便签" : "编辑便签", EPD_DRAW_ALIGN_CENTER, false);
    /* 21f2（真机反馈）：完成按钮原 y=128 底沿 152 与输入框边框顶 150 重叠 2px——
     * 上移到 112（底沿 136，与边框留 14px）；命中区同步收紧到 y<148 避开框角。 */
    ui_text(fb, 642, 112, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    EpdRect box = notes_box();
    ui_fill_round_rect(fb, box, NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, box, NX_R_CARD, NX8_INK);
    editor_paint_text(fb, box, true);
    ui_keyboard_draw(fb, 694);
}

/* 键盘局部刷新：标准输入框先被 ui_keyboard_update 画进 field，我们随后整卡覆盖为折行视图。 */
static app_redraw_t editor_paint(app_ctx_t *ctx, bool force_field) {
    EpdRect box = notes_box();
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, 694, box, 28, false, NULL, force_field);
    if (update.field) {
        ui_clear_rect_fast(ctx->fb, box);
        ui_fill_round_rect(ctx->fb, box, NX_R_CARD, NX8_PAPER);
        ui_draw_control_frame(ctx->fb, box, NX_R_CARD, NX8_INK);
        editor_paint_text(ctx->fb, box, true);
        update.area = ui_rect_union(update.area, box);
    }
    s_kb_area = update.area; s_kb_layout = update.layout; s_kb_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}

static app_redraw_t editor_finish(void) {
    ui_keyboard_commit_pending();
    (void)notes_save();               /* 存储失败不阻断回列表（缓冲已尽力保存） */
    ui_keyboard_end();
    s_editing = false;
    s_loaded = false;
    return APP_REDRAW_PAGE;
}

static app_redraw_t editor_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, 694, ctx->now_ms) ? editor_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? editor_paint(ctx, false) : APP_REDRAW_NONE;
    if (ev->y0 < 148 && ev->x0 > 510) return editor_finish();    /* 「完成」 */
    ui_keyboard_result_t result = ui_keyboard_tap(ev->x0, ev->y0, 694, ctx->now_ms);
    if (result == UI_KEYBOARD_DONE) return editor_finish();
    return result == UI_KEYBOARD_CHANGED ? editor_paint(ctx, false) : APP_REDRAW_NONE;
}

/* ============ 列表页 ============ */

static EpdRect row_rect(int i) { return (EpdRect){36, 156 + i * NOTES_ROW_H, 612, NOTES_ROW_H - 8}; }
static EpdRect new_button_rect(void) { return (EpdRect){36, 1000, 612, 84}; }

static void render_list(uint8_t *fb) {
    /* 21f（2026-10-20 真机反馈，三 bug 同根因）：notes_scan 原只在进 app（on_enter）
     * 触发，保存/删除后 s_loaded=false 但列表从未重扫——新建不显示、编辑不更新、
     * 删除后条目还在（文件其实已写/已删，列表是旧缓存）。统一在渲染前兜底重扫。 */
    if (!s_loaded) notes_scan();
    int shown = s_count - s_page * NOTES_PER_PAGE;
    if (shown > NOTES_PER_PAGE) shown = NOTES_PER_PAGE;
    if (shown < 0) shown = 0;
    ui_text_vc(fb, 342, 106, 34, "便签", EPD_DRAW_ALIGN_CENTER, false);
    /* 21f（用户拍板）：删除「共 N 条 · TF 卡」提示行，仅删除确认态保留提示。 */
    if (s_delete_idx >= 0)
        ui_text_vc(fb, 342, 150, 18, "确认删除这条便签？", EPD_DRAW_ALIGN_CENTER, false);
    for (int i = 0; i < shown; ++i) {
        int idx = s_page * NOTES_PER_PAGE + i;
        EpdRect r = row_rect(i);
        if (s_delete_idx == idx) {
            ui_fill_round_rect(fb, r, NX_R_CARD, 0xd0);
            ui_draw_control_frame(fb, r, NX_R_CARD, NX8_INK);
            ui_text_vc(fb, r.x + 26, r.y + r.height / 2, 24, "点本行左侧确认删除", EPD_DRAW_ALIGN_LEFT, false);
            ui_text_vc(fb, r.x + r.width - 24, r.y + r.height / 2, 20, "取消", EPD_DRAW_ALIGN_RIGHT, false);
            continue;
        }
        ui_fill_round_rect(fb, r, NX_R_CARD, NX8_PAPER);
        ui_draw_control_frame(fb, r, NX_R_CARD, NX8_INK);
        char text[104];
        snprintf(text, sizeof(text), "%.96s", s_items[idx].text[0] ? s_items[idx].text : "（空白）");
        while (text[0] && ttf_text_width_px(ui_text_effective_px(24), text) > 440) {
            size_t n = strlen(text) - 1;
            while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
            text[n] = 0;
        }
        ui_text_vc(fb, r.x + 26, r.y + r.height / 2, 24, text, EPD_DRAW_ALIGN_LEFT, false);
        /* 日期段：note_YYYYMMDD_HHMM → “MM-DD HH:MM” */
        if (strlen(s_items[idx].name) >= 20) {
            char d[14];
            snprintf(d, sizeof(d), "%.2s-%.2s %.2s:%.2s", s_items[idx].name + 11, s_items[idx].name + 13,
                     s_items[idx].name + 16, s_items[idx].name + 18);
            ui_text_vc(fb, r.x + r.width - 22, r.y + r.height / 2, 18, d, EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    if (!s_count && s_delete_idx < 0)
        ui_text_vc(fb, 342, 430, 26, "还没有便签，点下方新建", EPD_DRAW_ALIGN_CENTER, false);
    int pages = (s_count + NOTES_PER_PAGE - 1) / NOTES_PER_PAGE;
    if (pages > 1 && s_delete_idx < 0) {
        char pg[48];
        snprintf(pg, sizeof(pg), "%d / %d · 左右滑翻页", s_page + 1, pages);
        ui_text_vc(fb, 342, 1086, 17, pg, EPD_DRAW_ALIGN_CENTER, false);
    }
    if (s_delete_idx < 0) ui_draw_button(fb, new_button_rect(), "＋ 新建便签", true);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, 684, NX_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    if (s_editing) render_editor(fb);
    else render_list(fb);   /* 用户 2026-10-21：去掉列表页多余的导航栏 */
}

static bool present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_NONE) return true;
    if (redraw == APP_REDRAW_AREA && s_editing) {
        if (s_kb_settle) {
            s_kb_settle = false;
            guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_kb_area));
            return true;
        }
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
            s_kb_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
            s_kb_layout ? MODE_GL16 : MODE_DU, s_kb_area));
        return true;
    }
    return false;    /* PAGE/FULL 交默认呈现（render + 相应刷新） */
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_editing) return editor_gesture(ctx, ev);
    if (s_delete_idx >= 0) {
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        int idx = s_delete_idx; s_delete_idx = -1;
        EpdRect row = row_rect(idx - s_page * NOTES_PER_PAGE);
        if (ui_rect_hit(row, ev->x0, ev->y0) && ev->x0 < row.x + 440) {
            char path[160];
            snprintf(path, sizeof(path), "%s/%s", NOTES_DIR, s_items[idx].name);
            remove(path);
            s_loaded = false;
        }
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_L && (s_page + 1) * NOTES_PER_PAGE < s_count) { ++s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_SWIPE_R && s_page > 0) { --s_page; return APP_REDRAW_PAGE; }
    if (ev->type == UI_GESTURE_LONG_PRESS) {
        int shown = s_count - s_page * NOTES_PER_PAGE;
        if (shown > NOTES_PER_PAGE) shown = NOTES_PER_PAGE;
        for (int i = 0; i < shown; ++i) {
            int idx = s_page * NOTES_PER_PAGE + i;
            if (ui_rect_hit(row_rect(i), ev->x0, ev->y0)) { s_delete_idx = idx; return APP_REDRAW_PAGE; }
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (ui_rect_hit(new_button_rect(), ev->x0, ev->y0)) {
        s_editing = true; s_edit_new = true; s_edit_name[0] = 0; s_buf[0] = 0;
        ui_text_edit_init(&s_edit, s_buf, sizeof(s_buf));
        ui_keyboard_begin(&s_edit, false);
        return APP_REDRAW_FULL;
    }
    int shown = s_count - s_page * NOTES_PER_PAGE;
    if (shown > NOTES_PER_PAGE) shown = NOTES_PER_PAGE;
    for (int i = 0; i < shown; ++i) {
        int idx = s_page * NOTES_PER_PAGE + i;
        if (ui_rect_hit(row_rect(i), ev->x0, ev->y0)) {
            char path[160];
            snprintf(path, sizeof(path), "%s/%s", NOTES_DIR, s_items[idx].name);
            FILE *f = fopen(path, "rb");
            if (!f) return APP_REDRAW_PAGE;
            size_t got = fread(s_buf, 1, NOTE_TEXT_MAX - 1, f);
            fclose(f);
            s_buf[got] = 0;
            for (char *p = s_buf; *p; ++p) if (*p == '\n' || *p == '\r') *p = ' ';
            s_editing = true; s_edit_new = false;
            snprintf(s_edit_name, sizeof(s_edit_name), "%s", s_items[idx].name);
            ui_text_edit_init(&s_edit, s_buf, sizeof(s_buf));
            ui_text_edit_place(&s_edit, strlen(s_buf));
            ui_keyboard_begin(&s_edit, false);
            return APP_REDRAW_FULL;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_editing) {
        if (key == UI_KEY_2 || key == UI_KEY_3) return editor_finish();
        return APP_REDRAW_NONE;          /* 左键无绑定 → 全局快捷菜单 */
    }
    if (key == UI_KEY_3) { ctx->request_app = app_at(2); return APP_REDRAW_NONE; }
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    /* 21h（真机反馈）：中键长按=退出（request_return 恒语义，同计算器，返回来源页）；
     * 21g 误用 ui_nav_request(ctx,0)，那是跳首页不是退出。编辑态先保存再退。 */
    if (key != UI_KEY_2) return APP_REDRAW_NONE;
    if (s_editing) (void)editor_finish();
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_editing) {
        ui_keyboard_result_t r = ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms);
        return r == UI_KEYBOARD_CHANGED ? editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

static void notes_on_enter(app_ctx_t *ctx) { (void)ctx; if (!s_loaded) notes_scan(); }

static void notes_on_exit(app_ctx_t *ctx) {
    (void)ctx;
    if (s_editing) { ui_keyboard_commit_pending(); (void)notes_save(); ui_keyboard_end(); s_editing = false; }
    s_delete_idx = -1;
    s_loaded = false;
}

static void on_media_lost(app_ctx_t *ctx) { (void)ctx; s_loaded = false; }
static void on_media_ready(app_ctx_t *ctx) { (void)ctx; s_loaded = false; }
static bool always_visible(app_ctx_t *ctx) { (void)ctx; return !s_editing; }

const app_desc_t app_notes = {
    .title = "便签",
    .detail = "随手记",
    .enter_full = true,
    .owns_keys = true,
    .main_page_visible = always_visible,
    .on_enter = notes_on_enter,
    .on_exit = notes_on_exit,
    .on_media_lost = on_media_lost,
    .on_media_ready = on_media_ready,
    .render = render,
    .present = present,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .on_tick = on_tick,
};
