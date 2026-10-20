/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 微读传书：扫码、书架、含插图的离线下载。后台服务独占网络与文件写入。
 * WeRead transfer: QR login, shelf and illustrated offline downloads; worker owns writes.
 * 冻结：绘制无副作用；退出、锁屏、失卡前取消并等待；不修改云端阅读进度。
 * Frozen: pure render; cancel and join before exit/lock/media loss; no cloud-progress writes.
 */
#include "app.h"
#include "app_content_open.h"
#include "app_transfer_mode.h"
#include "book_store.h"
#include "display.h"
#include "read_pico_sd.h"
#include "ttf_font.h"
#include "weread_service.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_menu.h"
#include "ui_gesture.h"
#include "ui_wifi_qr.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static weread_snapshot_t *s_view_storage;
#define s_view (*s_view_storage)
static bool s_ready, s_qr_ok, s_images = true, s_logout_confirm;
static int s_selected = -1;
static unsigned s_changed;
static bool s_multiselect, s_batch_panel;
static weread_selection_t *s_selection;
static unsigned s_selection_count, s_selection_capacity;
static char s_message[96];
static int64_t s_next_tick;
static char s_qr[320];
static EpdRect sync_rect(void) { return (EpdRect){36, 229, 260, 64}; }
static EpdRect logout_rect(void) { return (EpdRect){468, 229, 180, 64}; }
static EpdRect select_rect(void) { return (EpdRect){316, 229, 132, 64}; }
static EpdRect batch_rect(void) { return (EpdRect){220, 939, 244, 66}; }
static EpdRect cancel_rect(void) { return (EpdRect){36, 676, 612, 78}; }
static EpdRect row_rect(int i) { return (EpdRect){36, 320 + i * 84, 612, 84}; }
static EpdRect prev_rect(void) { return (EpdRect){36, 939, 164, 66}; }
static EpdRect next_rect(void) { return (EpdRect){484, 939, 164, 66}; }
static EpdRect image_rect(void) { return (EpdRect){36, 392, 612, 84}; }
static EpdRect download_rect(void) { return (EpdRect){36, 506, 612, 78}; }
static EpdRect redownload_rect(void) { return (EpdRect){36, 604, 612, 78}; }
static EpdRect shelf_rect(void) { return (EpdRect){36, 702, 612, 78}; }

static void card(uint8_t *fb, EpdRect r, int radius) {
    ui_fill_round_rect(fb, r, radius, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, r, radius, 0x60);
    ui_draw_round_rect(fb, (EpdRect){r.x + 1, r.y + 1, r.width - 2, r.height - 2}, radius - 1, 0x60);
}
static void button(uint8_t *fb, EpdRect r, const char *label, bool primary) {
    card(fb, r, 20);
    if (primary) ui_draw_round_rect(fb, (EpdRect){r.x + 2, r.y + 2, r.width - 4, r.height - 4}, 18, 0x30);
    ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 25, label, EPD_DRAW_ALIGN_CENTER, false);
}
static void fit_text(char *dst, size_t cap, const char *src, int px, int width) {
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n); dst[n] = 0;
    while (n && ui_text_fixed_width_px(px, dst) > width) {
        --n;
        while (n && ((unsigned char)dst[n] & 0xc0) == 0x80) --n;
        dst[n] = 0;
    }
}
// 跨页选择按云端书号保存，内存按需增长，退出时统一释放。
// Keep cross-page selections by remote ID; grow on demand and free on exit.
static int selection_index(const char *id) {
    for (unsigned i = 0; i < s_selection_count; ++i)
        if (!strcmp(s_selection[i].id, id)) return (int)i;
    return -1;
}
static bool toggle_selection(unsigned row) {
    if (row >= s_view.count || !s_view.books[row].id[0]) return false;
    int found = selection_index(s_view.books[row].id);
    if (found >= 0) {
        memmove(s_selection + found, s_selection + found + 1,
                (s_selection_count - (unsigned)found - 1) * sizeof(*s_selection));
        --s_selection_count; return true;
    }
    if (s_selection_count == s_selection_capacity) {
        if (s_selection_capacity >= WEREAD_BATCH_MAX) {
            snprintf(s_message, sizeof(s_message), "一次最多选择 %u 本", WEREAD_BATCH_MAX); return false;
        }
        unsigned capacity = s_selection_capacity + 32;
        weread_selection_t *next = heap_caps_calloc(capacity, sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) { snprintf(s_message, sizeof(s_message), "选择未保存，内存不足"); return false; }
        if (s_selection_count) memcpy(next, s_selection, s_selection_count * sizeof(*next));
        heap_caps_free(s_selection); s_selection = next; s_selection_capacity = capacity;
    }
    weread_selection_t *entry = s_selection + s_selection_count++;
    snprintf(entry->id, sizeof(entry->id), "%s", s_view.books[row].id);
    entry->index = s_view.page * WEREAD_ROWS + row;
    s_message[0] = 0; return true;
}
static const char *status_text(void) {
    if (s_message[0]) return s_message;
    if (!s_view_storage) return "内存不足，请重启后重试";
    if (!s_ready) return "下载书籍需要插入 TF 卡";
    if (s_view.state == WEREAD_CONNECTING) return "正在连接 WiFi";
    if (s_view.state == WEREAD_QR) return "请用手机微信扫码确认登录";
    if (s_view.state == WEREAD_WORKING) {
        if (s_view.action == WEREAD_LOAD) return "正在读取本地书架";
        switch (s_view.stage) {
        case WEREAD_PREPARING: return "正在准备封面与书籍";
        case WEREAD_IMAGES: return "正在下载正文插图";
        case WEREAD_PACKAGING: return "正在生成 EPUB 文件";
        default: return (s_view.action == WEREAD_DOWNLOAD || s_view.action == WEREAD_BATCH) ?
            (s_view.target ? "正在下载书籍章节" : "正在获取书籍信息") : "正在同步书架，请稍候";
        }
    }
    if (s_view.state == WEREAD_CANCELLED) return "已取消，书架与已下载书籍保留";
    if (s_view.state == WEREAD_FAILED) {
        switch (s_view.error) {
        case 100: return "请先配置可访问互联网的 WiFi";
        case 101: return "连接失败，请检查 WiFi";
        case 102: return "对时失败，请检查网络后重试";
        case 103: case 6: return "存储失败，请检查 TF 卡与空间";
        case 7: return "内容校验失败，请重新下载";
        case 2: return "网络请求失败，请检查连接";
        case 3: case 4: return "登录失效，请重新同步扫码";
        case 8: case 11: return "此书暂不可下载，请换一本";
        case 9: return "设备时间无效，请重新联网";
        case 10: return "内存不足，请重启后重试";
        default: return "操作失败，可以重新尝试";
        }
    }
    if (s_view.state == WEREAD_COMPLETE && s_view.output[0])
        return s_view.skipped_images ? "下载完成，部分插图未能获取" : "下载完成，可以离线阅读";
    return s_view.logged_in ? "选择书籍，下载后可离线阅读" : "扫码登录后，获取微信读书书架";
}
static void refresh_snapshot(void) {
    if (!s_view_storage) return;
    weread_snapshot(&s_view);
    if (s_view.changed != s_changed) { s_changed = s_view.changed; book_store_notify_changed(); }
    if (strcmp(s_qr, s_view.qr)) {
        snprintf(s_qr, sizeof(s_qr), "%s", s_view.qr);
        s_qr_ok = s_qr[0] && ui_wifi_qr_prepare_weread(s_qr);
        if (!s_qr[0]) ui_wifi_qr_clear();
    }
}
static void configure(void) {
    if (!s_view_storage) s_view_storage = heap_caps_calloc(1, sizeof(*s_view_storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_view_storage) { s_ready = false; return; }
    read_pico_sd_info_t sd;
    read_pico_sd_get_info(&sd);
    book_store_root_t root;
    s_ready = sd.mounted && book_store_upload_root(&root) == ESP_OK && !root.is_flash &&
              weread_configure("/sdcard/.readpico/weread", root.path);
    if (s_ready) (void)weread_start(WEREAD_LOAD, 0, 0);
}
static void start_action(weread_action_t action, unsigned page, unsigned index) {
    if (!s_ready || s_view.active) return;
    s_message[0] = 0;
    ttf_font_cache_clear();
    (void)weread_set_include_images(s_images);
    (void)weread_start(action, page, index);
    refresh_snapshot();
}
static void on_enter(app_ctx_t *ctx) {
    s_selected = -1; s_multiselect = s_batch_panel = false; s_selection_count = 0; s_message[0] = 0; s_qr[0] = 0; s_qr_ok = s_logout_confirm = false;
    s_next_tick = ctx->now_ms;
    display_set_bulk_io(true);
    ui_wifi_qr_clear();
    configure(); refresh_snapshot();
}
static void weread_on_exit(app_ctx_t *ctx) {
    (void)ctx;
    weread_stop(); refresh_snapshot(); ui_wifi_qr_clear(); s_qr[0] = 0;
    heap_caps_free(s_view_storage); s_view_storage = NULL;
    heap_caps_free(s_selection); s_selection = NULL; s_selection_count = s_selection_capacity = 0;
    s_multiselect = s_batch_panel = false;
    display_set_bulk_io(false);
}
static void on_media_lost(app_ctx_t *ctx) { weread_on_exit(ctx); s_ready = false; s_selected = -1; }
static void on_media_ready(app_ctx_t *ctx) { (void)ctx; display_set_bulk_io(true); configure(); refresh_snapshot(); }
static void on_before_lock(app_ctx_t *ctx) { (void)ctx; weread_stop(); refresh_snapshot(); }
static void draw_images_option(uint8_t *fb) {
    card(fb, image_rect(), 20);
    ui_text_vc(fb, 58, 434, 25, "包含正文插图", EPD_DRAW_ALIGN_LEFT, false);
    ui_fill_round_rect(fb, (EpdRect){548, 415, 76, 38}, 19, s_images ? 0x40 : 0xb0);
    ui_fill_round_rect(fb, (EpdRect){s_images ? 587 : 551, 418, 32, 32}, 16, UI_GRAY_WHITE);
}
static void draw_progress(uint8_t *fb) {
    card(fb, (EpdRect){36, 320, 612, 312}, 24);
    char progress[80], title[192];
    if (s_view.batch_total && s_view.batch_current) snprintf(progress, sizeof(progress), "第 %u / %u 本", s_view.batch_current, s_view.batch_total);
    else snprintf(progress, sizeof(progress), "正在下载");
    ui_text_vc(fb, 342, 358, 29, progress, EPD_DRAW_ALIGN_CENTER, false);
    fit_text(title, sizeof(title), s_view.batch_title, 26, 544);
    ui_text_fixed(fb, 342, 399, 26, title, EPD_DRAW_ALIGN_CENTER, false);
    if (s_view.target) snprintf(progress, sizeof(progress), "%u / %u", s_view.done, s_view.target);
    else snprintf(progress, sizeof(progress), "正在处理");
    ui_text_vc(fb, 342, 460, 30, progress, EPD_DRAW_ALIGN_CENTER, false);
    ui_fill_round_rect(fb, (EpdRect){70, 511, 544, 10}, 5, 0xb0);
    if (s_view.target) {
        unsigned done = s_view.done > s_view.target ? s_view.target : s_view.done;
        int width = (int)((uint64_t)done * 544 / s_view.target);
        if (width) ui_fill_round_rect(fb, (EpdRect){70, 511, width, 10}, 5, 0x30);
    }
    ui_text_vc(fb, 342, 577, 22, "一次只下载一本，完成后继续下一本", EPD_DRAW_ALIGN_CENTER, false);
    button(fb, cancel_rect(), "取消操作", true);
    ui_text(fb, 48, 800, 22, "离开页面或锁屏会停止本次操作", EPD_DRAW_ALIGN_LEFT, false);
}
static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    const bool detail = s_view_storage && s_selected >= 0 && (unsigned)s_selected < s_view.count;
    const bool secondary = detail || s_batch_panel;
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP}, 0xe0, fb);
    ui_nav_status(fb);
    /* 2026-10-17：删标题与返回键（与其他页统一，仅状态栏；右键/中长按返回）。 */
    if (secondary) {
        card(fb, (EpdRect){36, 112, 612, detail ? 174 : 94}, 22);
        char title[192], author[96];
        if (detail) {
            fit_text(title, sizeof(title), s_view.books[s_selected].title, 30, 564);
            fit_text(author, sizeof(author), s_view.books[s_selected].author, 24, 564);
            ui_text_fixed(fb, 58, 139, 30, title, EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed(fb, 58, 195, 24, author, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 58, 243, 22, "封面始终下载", EPD_DRAW_ALIGN_LEFT, false);
        } else {
            snprintf(title, sizeof(title), "已选择 %u 本书籍", s_selection_count);
            ui_text_vc(fb, 58, 159, 29, title, EPD_DRAW_ALIGN_LEFT, false);
        }
        ui_text(fb, 48, detail ? 295 : 229, 23, status_text(), EPD_DRAW_ALIGN_LEFT, false);
    } else {
        card(fb, (EpdRect){36, 112, 612, 94}, 22);
        ui_text(fb, 58, 123, 27, s_view_storage && s_view.logged_in ? "微信读书书架" : "微信读书", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 58, 168, 21, status_text(), EPD_DRAW_ALIGN_LEFT, false);
        if (s_view_storage && s_view.logged_in) {
            char count[48];
            snprintf(count, sizeof(count), "%u 本 · %u/%u 页", s_view.total, s_view.page + 1,
                     s_view.total ? (s_view.total + WEREAD_ROWS - 1) / WEREAD_ROWS : 1);
            ui_text(fb, 624, 127, 21, count, EPD_DRAW_ALIGN_RIGHT, false);
        }
        if (!s_view_storage || !s_view.active) {
            button(fb, sync_rect(), s_multiselect ? "全选本页" :
                   s_view_storage && s_view.logged_in ? "同步书架" : "扫码登录", true);
            button(fb, select_rect(), s_multiselect ? "清空" : "多选", false);
            button(fb, logout_rect(), s_multiselect ? "完成选择" : "退出登录", false);
        }
    }
    if (!s_ready || !s_view_storage) {
        ui_text(fb, 58, 356, 26, "请插入 TF 卡后重新打开", EPD_DRAW_ALIGN_LEFT, false);
    } else if (s_view.state == WEREAD_QR) {
        card(fb, (EpdRect){36, 320, 612, 590}, 24);
        ui_text_vc(fb, 342, 365, 27, "扫描二维码", EPD_DRAW_ALIGN_CENTER, false);
        if (s_qr_ok) ui_wifi_qr_draw(fb, (EpdRect){142, 405, 400, 400});
        else ui_text_vc(fb, 342, 575, 24, "二维码生成失败，请重新同步", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 841, 22, "手机确认后自动继续", EPD_DRAW_ALIGN_CENTER, false);
        button(fb, (EpdRect){36, 926, 612, 70}, "取消操作", true);
    } else if (s_view.active) draw_progress(fb);
    else if (secondary) {
        draw_images_option(fb);
        if (s_batch_panel) {
            char summary[96];
            snprintf(summary, sizeof(summary), "完成 %u 本 · 失败 %u 本 · 未下载 %u 本", s_view.batch_success,
                     s_view.batch_failed, s_view.batch_total > s_view.batch_success + s_view.batch_failed ?
                     s_view.batch_total - s_view.batch_success - s_view.batch_failed : 0);
            if (s_view.action == WEREAD_BATCH) ui_text(fb, 48, 326, 23, summary, EPD_DRAW_ALIGN_LEFT, false);
            button(fb, download_rect(), "开始批量下载", true);
        } else {
            button(fb, download_rect(), s_view.books[s_selected].local_path[0] ? "打开已下载书籍" : "下载到 TF 卡", true);
            if (s_view.books[s_selected].local_path[0]) button(fb, redownload_rect(), "重新下载", false);
        }
        if (s_view.state == WEREAD_FAILED && s_view.error == 100) button(fb, redownload_rect(), "设置 WiFi", false);
        button(fb, shelf_rect(), "返回微信书架", false);
        ui_text(fb, 48, 828, 22, "不下载正文插图时，仍保留书籍封面", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 870, 21, "部分书籍可能受账号或内容权限限制", EPD_DRAW_ALIGN_LEFT, false);
    } else if (!s_view.count) {
        card(fb, (EpdRect){36, 320, 612, 286}, 24);
        ui_text_vc(fb, 342, 381, 28, "把微信读书带到 Pico", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 451, 22, "连接 WiFi → 扫码登录 → 选择书籍", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 497, 22, "下载为 EPUB，保存后可离线阅读", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 553, 20, "需要可访问互联网的 2.4 GHz WiFi", EPD_DRAW_ALIGN_CENTER, false);
        button(fb, (EpdRect){36, 639, 612, 78}, "设置 WiFi", false);
    } else {
        card(fb, (EpdRect){36, 320, 612, s_view.count * 84}, 24);
        for (unsigned i = 0; i < s_view.count; ++i) {
            EpdRect r = row_rect(i); char title[192], author[96];
            if (i) ui_hairline(fb, r.y, 58, 566, 0x80);
            fit_text(title, sizeof(title), s_view.books[i].title, 30, s_multiselect ? 496 : 518);
            fit_text(author, sizeof(author), s_view.books[i].author, 22, 420);
            ui_text_fixed(fb, 58, r.y + 10, 30, title, EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed(fb, 58, r.y + 51, 22, author, EPD_DRAW_ALIGN_LEFT, false);
            if (s_multiselect) {
                EpdRect check = {582, r.y + 20, 38, 38};
                ui_draw_round_rect(fb, check, 8, 0x30);
                ui_draw_round_rect(fb, (EpdRect){583, r.y + 21, 36, 36}, 7, 0x30);
                if (selection_index(s_view.books[i].id) >= 0) ui_fill_round_rect(fb, (EpdRect){589, r.y + 27, 24, 24}, 4, 0x20);
            } else ui_text_vc(fb, 620, r.y + 27, 28, "›", EPD_DRAW_ALIGN_CENTER, false);
            if (s_view.books[i].local_path[0]) ui_text_fixed(fb, 624, r.y + 51, 18, "已下载", EPD_DRAW_ALIGN_RIGHT, false);
        }
        button(fb, prev_rect(), "上一页", false); button(fb, next_rect(), "下一页", false);
        if (s_multiselect) {
            char count[40]; snprintf(count, sizeof(count), "下载 %u 本", s_selection_count);
            button(fb, batch_rect(), count, s_selection_count != 0);
        } else {
            char page[40]; snprintf(page, sizeof(page), "%u / %u", s_view.page + 1, (s_view.total + WEREAD_ROWS - 1) / WEREAD_ROWS);
            ui_text_vc(fb, 342, 972, 24, page, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    ui_nav_draw(fb, 2);
    if (s_logout_confirm) {
        card(fb, (EpdRect){58, 352, 568, 300}, 28);
        ui_text_vc(fb, 342, 413, 30, "退出微信读书登录？", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 472, 22, "已下载的书籍会继续保留", EPD_DRAW_ALIGN_CENTER, false);
        button(fb, (EpdRect){82, 540, 240, 74}, "取消", false);
        button(fb, (EpdRect){362, 540, 240, 74}, "退出登录", true);
    }
}
static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (!s_view_storage || ctx->now_ms < s_next_tick) return APP_REDRAW_NONE;
    s_next_tick = ctx->now_ms + 1200;
    unsigned revision = s_view.revision, changed = s_view.changed, count = s_view.count;
    weread_state_t state = s_view.state;
    weread_stage_t stage = s_view.stage;
    bool active = s_view.active;
    unsigned current = s_view.batch_current, failures = s_view.batch_failed;
    unsigned bucket = s_view.target ? (unsigned)((uint64_t)s_view.done * 20 / s_view.target) : 0;
    refresh_snapshot();
    unsigned next = s_view.target ? (unsigned)((uint64_t)s_view.done * 20 / s_view.target) : 0;
    if (revision == s_view.revision) return APP_REDRAW_NONE;
    if (!s_view.active) return APP_REDRAW_PAGE;
    return state != s_view.state || stage != s_view.stage || active != s_view.active ||
           current != s_view.batch_current || failures != s_view.batch_failed ||
           count != s_view.count || changed != s_view.changed || bucket != next ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}
static app_redraw_t go_back(app_ctx_t *ctx) {
    if (s_logout_confirm) { s_logout_confirm = false; return APP_REDRAW_PAGE; }
    if (s_view_storage && s_view.active) { weread_stop(); refresh_snapshot(); }
    if (s_selected >= 0 || s_batch_panel) { s_selected = -1; s_batch_panel = false; s_message[0] = 0; return APP_REDRAW_PAGE; }
    if (s_multiselect) { s_multiselect = false; return APP_REDRAW_PAGE; }
    // 微信传书的入口在传输文件页：明确返回该上一级，不进入示例工程的菜单返回栈。
    // WeRead transfer is entered from the Transfer page: return explicitly to it.
    extern const app_desc_t app_transfer;
    ctx->request_app = &app_transfer; return APP_REDRAW_NONE;
}
static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP && ev->type != UI_GESTURE_SWIPE_L && ev->type != UI_GESTURE_SWIPE_R) return APP_REDRAW_NONE;
    if (s_logout_confirm) {
        if (ev->type == UI_GESTURE_TAP && ui_rect_hit((EpdRect){362, 540, 240, 74}, ev->x0, ev->y0)) {
            s_logout_confirm = false; s_selected = -1; s_selection_count = 0; start_action(WEREAD_LOGOUT, 0, 0);
        } else if (ev->type == UI_GESTURE_TAP && ui_rect_hit((EpdRect){82, 540, 240, 74}, ev->x0, ev->y0)) s_logout_confirm = false;
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_TAP) {
        int nav = ui_nav_hit(ev->x0, ev->y0);
        if (nav >= 0) { ui_nav_request(ctx, nav); return APP_REDRAW_NONE; }
        if (s_view_storage && s_view.active &&
            ui_rect_hit(s_view.state == WEREAD_QR ? (EpdRect){36, 926, 612, 70} : cancel_rect(), ev->x0, ev->y0)) {
            weread_stop(); refresh_snapshot(); return APP_REDRAW_PAGE;
        }
    }
    if (!s_view_storage || s_view.active || !s_ready) return APP_REDRAW_NONE;
    const bool detail = s_selected >= 0 && (unsigned)s_selected < s_view.count;
    if (detail || s_batch_panel) {
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (ui_rect_hit(shelf_rect(), ev->x0, ev->y0)) { s_selected = -1; s_batch_panel = false; }
        else if (ui_rect_hit(image_rect(), ev->x0, ev->y0)) s_images = !s_images;
        else if (s_view.state == WEREAD_FAILED && s_view.error == 100 && ui_rect_hit(redownload_rect(), ev->x0, ev->y0)) {
            app_transfer_request_wifi_setup(); extern const app_desc_t app_transfer; ctx->request_app = &app_transfer;
        } else if (ui_rect_hit(download_rect(), ev->x0, ev->y0)) {
            if (s_batch_panel) {
                s_message[0] = 0; ttf_font_cache_clear(); (void)weread_set_include_images(s_images);
                if (!weread_start_batch(s_view.page, s_selection, s_selection_count))
                    snprintf(s_message, sizeof(s_message), "批量任务未启动，请重试");
                refresh_snapshot();
            } else if (s_view.books[s_selected].local_path[0]) {
                if (app_book_request_open(s_view.books[s_selected].local_path)) {
                    extern const app_desc_t app_book; ctx->request_app = &app_book;
                }
            } else start_action(WEREAD_DOWNLOAD, s_view.page, s_view.page * WEREAD_ROWS + s_selected);
        } else if (detail && s_view.books[s_selected].local_path[0] && ui_rect_hit(redownload_rect(), ev->x0, ev->y0))
            start_action(WEREAD_DOWNLOAD, s_view.page, s_view.page * WEREAD_ROWS + s_selected);
        return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_TAP) {
        if (ui_rect_hit(sync_rect(), ev->x0, ev->y0)) {
            if (s_multiselect) {
                for (unsigned i = 0; i < s_view.count; ++i)
                    if (selection_index(s_view.books[i].id) < 0 && !toggle_selection(i)) break;
            } else { s_selection_count = 0; start_action(WEREAD_SYNC, s_view.page, 0); }
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(select_rect(), ev->x0, ev->y0)) {
            if (s_multiselect) { s_selection_count = 0; s_message[0] = 0; }
            else if (s_view.count) s_multiselect = true;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(logout_rect(), ev->x0, ev->y0)) {
            if (s_multiselect) s_multiselect = false;
            else if (s_view.logged_in) s_logout_confirm = true;
            return APP_REDRAW_PAGE;
        }
        if (s_multiselect && s_selection_count && ui_rect_hit(batch_rect(), ev->x0, ev->y0)) {
            s_batch_panel = true; return APP_REDRAW_PAGE;
        }
        if (!s_view.count && ui_rect_hit((EpdRect){36, 703, 612, 78}, ev->x0, ev->y0)) {
            app_transfer_request_wifi_setup(); extern const app_desc_t app_transfer; ctx->request_app = &app_transfer;
            return APP_REDRAW_NONE;
        }
        for (unsigned i = 0; i < s_view.count; ++i) if (ui_rect_hit(row_rect(i), ev->x0, ev->y0)) {
            if (s_multiselect) (void)toggle_selection(i); else s_selected = i;
            return APP_REDRAW_PAGE;
        }
    }
    bool previous = ev->type == UI_GESTURE_SWIPE_R || (ev->type == UI_GESTURE_TAP && ui_rect_hit(prev_rect(), ev->x0, ev->y0));
    bool next = ev->type == UI_GESTURE_SWIPE_L || (ev->type == UI_GESTURE_TAP && ui_rect_hit(next_rect(), ev->x0, ev->y0));
    if (previous && s_view.page) start_action(WEREAD_LOAD, s_view.page - 1, 0);
    else if (next && (s_view.page + 1) * WEREAD_ROWS < s_view.total) start_action(WEREAD_LOAD, s_view.page + 1, 0);
    else return APP_REDRAW_NONE;
    return APP_REDRAW_PAGE;
}
static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    /* 2026-10-17 统一：中键短=强刷；右键=返回上一页；左键=无动作；中键长=退出。 */
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    if (key == UI_KEY_1) return APP_REDRAW_NONE;
    return go_back(ctx);
}
static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_2) return APP_REDRAW_NONE;
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}
static bool menu_enabled(app_ctx_t *ctx) { (void)ctx; return false; }
const app_desc_t app_weread = {
    .title = "微读传书", .detail = "微信读书下载", .enter_full = true, .owns_keys = true,
    .defer_middle_short = true,
    .menu_handle_enabled = menu_enabled, .on_enter = on_enter, .on_exit = weread_on_exit,
    .on_media_lost = on_media_lost, .on_media_ready = on_media_ready, .on_before_lock = on_before_lock,
    .render = render, .on_gesture = on_gesture, .on_key = on_key, .on_key_long = on_key_long, .on_tick = on_tick,
};
