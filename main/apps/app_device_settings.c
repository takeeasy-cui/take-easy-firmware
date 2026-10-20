/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：分组设置页；网络短时对时后 PMU 持续走时，开机恢复系统时钟。
 * English: Grouped settings; temporary WiFi sync seeds the PMU, whose RTC restores time at boot.
 * 用户修订：设置列表滑动仅差分刷新内容，滑动过程中不周期插入黑白清屏。
 * User revision: settings lists scroll with content-only differentials and no periodic black/white wipe during swipes.
 * 用户授权新输入法：资料卡和签名共用九宫格/全键盘及离线词语候选，保存仍由设置页负责。
 * Authorized keyboard revision: profile and signature share T9/QWERTY and offline phrases; this page still owns saving.
 * 用户修订：资料卡与签名输入只画变化区域，不因输入次数触发整屏黑白清屏。
 * User revision: profile/signature input paints changed regions without whole-screen wipes based on typing counts.
 * 用户修订：快刷说明标注棋盘格亚克力，普通与水波纹继续保留灰阶；三种主页面刷新模式均固定底栏。
 * User revision: the fast-mode description identifies checkerboard acrylic; ordinary/water keep grays and all main-screen modes retain navigation.
 * 用户修订：联网升级先画反馈再启动任务；进度只追加黑色段，不擦除已完成段，不累计整页清屏。升级前释放不在显示的封面缓存。
 * User revision: present feedback before starting online work; append black progress without erasing completed segments or counting page cleanup. Release off-screen cover caches before updating.
 * 用户确认：字号并入系统字体并预览；选择字体后询问同步阅读字体，设置按连接/显示/个性化/升级分组。
 * Approved revision: size and preview live with the UI face; ask before syncing the reading face, and group connection/display/personalization/maintenance.
 * 用户撤回跟手滚动：三种主页模式统一恢复一次手势推进一段列表并差分刷新的原逻辑，无惯性、浅墨或停稳补刷。
 * User rollback: all main modes return to one discrete list step per gesture with differential updates, without inertia, light motion ink or idle restoration.
 */
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "app_content_open.h"
#include "app_registry.h"
#include "app_transfer_mode.h"
#include "settings.h"
#include "lock_pin_flow.h"
#include "ui_pinpad.h"
#include "book_lock_collage.h"
#include "boot_state.h"
#include "ble_page_turner.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "esp_log.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nothing.h"
#include "ui_text_input.h"
#include "ui_keyboard.h"
#include "ui_nav.h"
#include "ui_wallpaper.h"
#include "ui_avatar.h"
#include "read_pico_search.h"
#include "book_store.h"
#include "book_cover.h"
#include <zlib.h>
#include "ota_update.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "display.h"
#include "display_main_waveform.h"
#include "e0470_epaper_waveform.h"
#include "pmu_selftest.h"
#include "soc/rtc_cntl_reg.h"

static void fit_value(char *value, int width);
static void settings_card(uint8_t *fb, EpdRect rect, int radius, uint8_t fill, uint8_t edge);
static char s_notice[96];
typedef enum { SETTINGS_MAIN, SETTINGS_WIFI, SETTINGS_TIME,
               SETTINGS_TIME_EDIT, SETTINGS_SHELF_STYLE, SETTINGS_SYSTEM_FONT,
               SETTINGS_SYSTEM_SIZE, SETTINGS_SYSTEM_CONTRAST, SETTINGS_MAIN_REFRESH, SETTINGS_LOCK_STYLE,
               SETTINGS_WALLPAPER, SETTINGS_WALLPAPER_PREVIEW,
               SETTINGS_CONFIG, SETTINGS_UPGRADE, SETTINGS_BOOT,
               SETTINGS_POWER_SLEEP, SETTINGS_AUTO_LOCK, SETTINGS_PROFILE, SETTINGS_AVATAR,
               SETTINGS_BLUETOOTH, SETTINGS_BLE_SCAN,
               SETTINGS_LOCK_PIN, SETTINGS_PIN_ENTRY, SETTINGS_TEXT_EDIT, SETTINGS_GOAL } settings_page_t;
static settings_page_t s_page;
static ui_pinpad_t s_pinpad;
static lock_pin_flow_t s_pinflow;
static bool s_pin_pending;
static unsigned s_pin_failures;
static int64_t s_pin_retry_at;
/* 计算器锁屏引导：从锁屏设置进入 PIN 设置，完成后自动切样式 3（用户 2026-10-19）。 */
static bool s_pin_for_lock_style;
/* 离开计算器锁屏的样式切换闸：目标样式暂存，验证原密码并关闭绑定密码后切换。 */
static uint8_t s_pin_style_after = 0xFF;
static void pin_setup_close(void) {
    ui_pinpad_end(&s_pinpad); lock_pin_flow_end(&s_pinflow); s_pin_pending = false;
    s_pin_for_lock_style = false;
    s_pin_style_after = 0xFF;
}
static void pin_setup_open(app_ctx_t *ctx, bool disable) {
    pin_setup_close(); lock_pin_flow_begin(&s_pinflow, disable);
    ui_pinpad_begin(&s_pinpad, ctx->fb, lock_pin_flow_title(&s_pinflow));
    s_page = SETTINGS_PIN_ENTRY;
}
static app_redraw_t pin_paint(app_ctx_t *ctx, EpdRect area) {
    ui_pinpad_paint(ctx->fb, &s_pinpad, area);
    guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, area));
    return APP_REDRAW_DONE;
}
static app_redraw_t pin_setup_tick(app_ctx_t *ctx) {
    if (s_pinpad.blocked && ctx->now_ms >= s_pin_retry_at) {
        ui_pinpad_reset(&s_pinpad, NULL, "请重新输入");
        return pin_paint(ctx, ui_pinpad_entry_area());
    }
    if (!s_pin_pending) return APP_REDRAW_NONE;
    s_pin_pending = false;
    lock_pin_step_t prior = s_pinflow.step;
    esp_err_t error = lock_pin_flow_input(&s_pinflow, s_pinpad.digits);
    if (error == ESP_OK && s_pinflow.step == PIN_FLOW_DONE) {
        bool calc_lock = s_pin_for_lock_style;
        uint8_t style_after = s_pin_style_after;
        /* 离开计算器锁屏的清除流程需要旧密码：在 pin_setup_close() 抹掉前复制。 */
        char old_pin[5] = {0};
        if (style_after != 0xFF && s_pinflow.old[0]) memcpy(old_pin, s_pinflow.old, 5);
        pin_setup_close();
        if (calc_lock && lock_pin_enabled()) {
            /* 计算器锁屏引导完成：启用样式 3 并绑定该密码（离开样式 3 时自动关闭）。 */
            app_settings_set_lock_style(3);
            app_settings_set_pin_calc_bound(true);
            s_page = SETTINGS_LOCK_STYLE;
            snprintf(s_notice, sizeof(s_notice), "计算器锁屏已开启");
        } else if (style_after != 0xFF) {
            /* 离开计算器锁屏（用户 2026-10-19 定稿）：原密码验证通过→关闭该密码
             * （lock_pin_replace(old,NULL) 清除凭据，恢复无密码状态）→切换样式。
             * 密码只属于计算器锁屏：切走即弃，不残留为全局密码；全局密码
             * （密码页设置的）不受影响——闸只在绑定标志为真时开启。 */
            if (lock_pin_enabled()) {
                if (lock_pin_replace(old_pin[0] ? old_pin : NULL, NULL) != ESP_OK) {
                    /* 清除失败（存储异常）：保守保持锁定并留在密码页提示重试。 */
                    s_page = SETTINGS_LOCK_PIN;
                    snprintf(s_notice, sizeof(s_notice), "关闭密码失败，请重试");
                    return APP_REDRAW_PAGE;
                }
            }
            app_settings_set_pin_calc_bound(false);
            app_settings_set_lock_style(style_after);
            lock_pin_wipe(old_pin, sizeof(old_pin));
            s_page = SETTINGS_LOCK_STYLE;
            snprintf(s_notice, sizeof(s_notice), "已切换锁屏样式，密码已关闭");
        } else {
            /* 密码页流程：设置/修改/关闭后均为全局密码，不绑定计算器锁屏。 */
            app_settings_set_pin_calc_bound(false);
            s_page = SETTINGS_LOCK_PIN;
            snprintf(s_notice, sizeof(s_notice), "%s", lock_pin_enabled() ? "锁屏密码已保存" : "锁屏密码已关闭");
        }
        return APP_REDRAW_PAGE;
    }
    const char *notice = error == ESP_OK ? "" : error == ESP_ERR_INVALID_RESPONSE ? "两次输入不同，请重设" :
        error == ESP_ERR_INVALID_ARG ? "原密码不正确，请重试" : "保存失败，请重试";
    ui_pinpad_reset(&s_pinpad, lock_pin_flow_title(&s_pinflow), notice);
    if (error == ESP_ERR_INVALID_ARG && prior == PIN_FLOW_OLD && ++s_pin_failures >= 3) {
        s_pin_failures = 0; s_pin_retry_at = ctx->now_ms + 10000;
        snprintf(s_pinpad.notice, sizeof(s_pinpad.notice), "请等待 10 秒后重试"); s_pinpad.blocked = true;
    }
    return pin_paint(ctx, (EpdRect){122, 218, 440, 247});
}
/* v2 锁屏样式切换闸（用户 2026-10-19）：离开计算器锁屏（样式 3）且当前密码是
 * 计算器锁屏引导设置（绑定）时，先走「验证原密码→关闭」流程，成功后切换样式；
 * 取消或验证失败则保持样式 3。全局密码（密码页设置）不受影响：直接切换，密码保留
 * ——除计算器锁屏外保持目前锁屏密码使用逻辑。 */
static app_redraw_t apply_lock_style_gated(app_ctx_t *ctx, uint8_t style) {
    if (style > 3 || style == app_settings_lock_style()) return APP_REDRAW_NONE;
if (app_settings_lock_style() == 3 && style != 3 &&
        lock_pin_enabled() && app_settings_pin_calc_bound()) {
        s_pin_style_after = style;
        pin_setup_open(ctx, true);
        return APP_REDRAW_PAGE;
    }
    app_settings_set_lock_style(style);
    return APP_REDRAW_PAGE;
}
static int s_style_scroll, s_main_scroll;
static bool s_collage_prepare_pending;
static bool s_scroll_drag_consumed, s_scroll_present_pending;
// 蓝牙翻页器子页：滚动位置、正在学习哪个动作（0 未学，1 上一页，2 下一页）、提示行。
// Bluetooth sub-page: scroll offset, which action is being learned (0 idle, 1 prev, 2 next),
// and a notice line.
static int s_ble_scroll, s_ble_learning;
static char s_ble_notice[64];
static char s_ble_feedback[64];
static int s_font_page, s_wallpaper_page;
static bool s_sync_pending;
static bool s_font_sync_confirm;
static char s_font_sync_path[TTF_FONT_PATH_MAX];
static bool s_config_confirm;
// 屏幕修复：点一下就开始。整屏盲刷要几秒，活儿交给 tick——不能在触摸回调里做。
// Panel repair: one tap starts it. The blind full-screen drive takes seconds, so the work goes to the
// tick rather than a touch callback.
static bool s_panel_repair_pending;
static bool s_boot_pending;
static bool s_upgrade_confirm, s_local_pending, s_upgrade_restart;
static pico_ota_info_t s_local_update;
static char s_upgrade_notice[96];
// TF 卡升级确认按钮：绘制与命中共用同一份几何。/ Confirm buttons; drawing and hit tests share one geometry.
static EpdRect upgrade_confirm_button(bool accept) {
    return (EpdRect){accept ? 364 : 60, 613, 260, 64};
}
static void upgrade_open(void) {
    (void)pico_ota_inspect(PICO_OTA_UPDATE_PATH, &s_local_update);
    s_upgrade_confirm = s_local_pending = s_upgrade_restart = false;
    s_upgrade_notice[0] = 0;
    s_page = SETTINGS_UPGRADE;
}
static const char *const TAG = "device_settings";
// 主设置始终单行；大字号只改变字形，字体预览单独保留足够空间。
// Main rows never stack; larger glyphs share one centerline, with extra space only in font previews.
static bool settings_stacked(void) { return app_settings_system_font_size() >= 150; }
static int settings_profile_height(void) { return 160; }  /* 2026-10-17：资料卡加高 1/2 */
static int settings_main_row_height(void) {
    return 82;
}
#define SETTINGS_ROW_H settings_main_row_height()
/* 2026-10-17：一页化：资料卡+9 行满页、间距均匀，不滚动；
 * “设置”组标题删除；书架样式与签名选项已删。
 * v1（用户 2026-10-17 真机反馈）：资料卡与行组间隙 44→16，前五行上移。 */
/* v1（用户 2026-10-17）：资料卡上提 118→96，五行明显上移（WIFI 组锚=96+160+16）。 */
#define SETTINGS_MAIN_TOP    96
#define SETTINGS_BODY_TOP    90
#define SETTINGS_MAIN_ROWS   9
#define SETTINGS_WIRELESS_Y  (SETTINGS_MAIN_TOP + settings_profile_height() + 16)
// 设置行图标：32 像素盒，在行内垂直居中；墨色统一，避免一行一个灰度。
// Setting row icons: a 32 px box centred in the row with one shared ink level.
#define SETTINGS_ICON_PX 32
#define SETTINGS_ICON_INK 0x58
// 主页面滚动上限：把最后一行刚好推到导航栏之上，多留 12px 余量。
// Main-page scroll limit: just enough to bring the last row above the nav bar, with 12 px to spare.
/* 2026-10-17：9 行一页放满（末行底 1099 < 导航顶 1101），主页面不再滚动。 */
#define SETTINGS_SCROLL_MAX 0
static char s_editor[96], s_editor_notice[80];
static ui_text_edit_t s_editor_input;
static const char *system_font_label(const char *path) {
    if (!path || !path[0]) return "思源黑体";
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    static char stem[TTF_FONT_NAME_MAX];
    snprintf(stem, sizeof(stem), "%s", name);
    size_t len = strlen(stem);
    if (len >= 4 && !strcasecmp(stem + len - 4, ".ttf")) stem[len - 4] = 0;
    if (!strcasecmp(stem, "Hei")) return "思源黑体（TF 卡）";
    return ttf_font_localized_name(stem);
}
typedef struct {
    EpdRect size, preview;
    int preview_title, font_title, first, row_height, slots;
} system_font_layout_t;
static system_font_layout_t system_font_layout(void) {
    system_font_layout_t l = settings_stacked()
        ? (system_font_layout_t){{36,132,612,183},{36,406,612,216},348,657,720,142,2}
        : (system_font_layout_t){{36,132,612,100},{36,314,612,154},257,495,550,99,4};
    return l;
}
static EpdRect system_font_item_rect(int slot) {
    system_font_layout_t l = system_font_layout();
    return (EpdRect){36,l.first + slot * l.row_height,612,l.row_height - 16};
}
static EpdRect system_font_sync_button(bool sync) { return (EpdRect){sync ? 351 : 72,683,261,74}; }
static void draw_system_font_sync(uint8_t *fb) {
    // 仅一个有界弹窗，不额外打开阅读字体。/ One bounded dialog; never load the reading face here.
    settings_card(fb,(EpdRect){48,389,588,401},28,UI_GRAY_WHITE,0x58);
    ui_text_fixed_vc(fb,342,448,32,"同步到阅读字体？",EPD_DRAW_ALIGN_CENTER,false);
    char name[80]; snprintf(name,sizeof(name),"已选择「%s」",system_font_label(s_font_sync_path));
    while (ui_text_fixed_width_px(27,name) > 520 && strlen(name) > 6) {
        size_t n = strlen(name) - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
        name[n] = 0;
    }
    ui_text_fixed_vc(fb,342,516,27,name,EPD_DRAW_ALIGN_CENTER,false);
    ui_text_fixed_vc(fb,342,570,24,"是否让阅读正文也使用这个字体？",EPD_DRAW_ALIGN_CENTER,false);
    ui_text_fixed_vc(fb,342,614,23,s_notice[0]?s_notice:"不同步时，保留原来的阅读字体。",EPD_DRAW_ALIGN_CENTER,false);
    for (int i=0;i<2;++i) {
        EpdRect r=system_font_sync_button(i);
        settings_card(fb,r,20,i ? UI_GRAY_BLACK : UI_GRAY_WHITE,0x58);
        ui_text_fixed_vc(fb,r.x+r.width/2,r.y+r.height/2,25,i ? "同步阅读字体" : "仅系统字体",EPD_DRAW_ALIGN_CENTER,i);
    }
}
static int s_year, s_month, s_day, s_hour, s_minute;
#define WALLPAPER_MAX 64
typedef struct { char path[288]; char name[96]; } wallpaper_item_t;
// 壁纸列表放 PSRAM，理由同上。
// The wallpaper list lives in PSRAM for the same reason.
static wallpaper_item_t *s_wallpapers;

static bool wallpapers_alloc(void) {
    if (!s_wallpapers)
        s_wallpapers = heap_caps_calloc(WALLPAPER_MAX, sizeof(wallpaper_item_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_wallpapers != NULL;
}
static int s_wallpaper_count;
static int s_wallpaper_selected;
static bool s_wallpaper_confirm, s_wallpaper_preview_ok;
static bool s_pick_cover;   /* v1 透明书封：复用壁纸列表/预览，确认写 coverart 键 */

static void wallpaper_scan_dir(const char *root, bool png_only) {
    if (!wallpapers_alloc()) return;
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    while (s_wallpaper_count < WALLPAPER_MAX && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.' || !strncmp(entry->d_name, "._", 2)) continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png"))) continue;
        if (png_only && strcasecmp(ext, ".png")) continue;   /* v1：书封仅收透明 PNG */
        wallpaper_item_t *item = &s_wallpapers[s_wallpaper_count];
        if (snprintf(item->path, sizeof(item->path), "%s/%s", root, entry->d_name) >= sizeof(item->path)) continue;
        struct stat st;
        if (stat(item->path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > BOOK_IMAGE_FILE_MAX) continue;
        size_t name_len = strnlen(entry->d_name, sizeof(item->name) - 1);
        memcpy(item->name, entry->d_name, name_len);
        item->name[name_len] = 0;
        ++s_wallpaper_count;
    }
    closedir(dir);
}
static void wallpaper_scan(void) {
    s_wallpaper_count = s_wallpaper_page = 0;
    wallpaper_scan_dir("/sdcard/pictures", false);
    wallpaper_scan_dir("/sdcard/images", false);
    wallpaper_scan_dir("/sdcard", false);
}
static void coverscan(void) {   /* v1：透明书封候选（仅 PNG）。 */
    s_wallpaper_count = s_wallpaper_page = 0;
    wallpaper_scan_dir("/sdcard/pictures", true);
    wallpaper_scan_dir("/sdcard/images", true);
    wallpaper_scan_dir("/sdcard", true);
}


static void profile_editor_open(void) {
    /* 状态栏签名功能已删（2026-10-17），编辑器仅设备名。 */
    snprintf(s_editor, sizeof(s_editor), "%s", app_settings_device_name());
    ui_text_edit_init(&s_editor_input, s_editor, sizeof(s_editor));
    s_editor_notice[0] = 0;
    ui_keyboard_begin(&s_editor_input, false);
    s_page = SETTINGS_TEXT_EDIT;
}




static int days_in_month(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
    return days[month - 1];
}

// 公历日期转 UTC 天数；界面统一按北京时间 UTC+8 编辑。
// Convert a Gregorian date to UTC days; the editor uses Beijing time (UTC+8).
static int64_t civil_days(int year, int month, int day) {
    year -= month <= 2;
    int era = year / 400;
    int yoe = year - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static void time_open(void) {
    time_t now = time(NULL);
    if (now < 1704067200) {
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        if (pmu && pmu->time_synced && pmu->unix_sec >= 1704067200) now = pmu->unix_sec;
    }
    if (now < 1704067200) {
        char month[4] = "Jan"; int day = 1, year = 2024;
        if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) {
            strcpy(month, "Jan"); day = 1; year = 2024;
        }
        static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int m = 1; while (m < 12 && strcmp(month, months[m - 1])) ++m;
        s_year = year; s_month = m; s_day = day; s_hour = s_minute = 0;
    } else {
        struct tm date;
        time_t local = now + 8 * 3600;
        gmtime_r(&local, &date);
        s_year = date.tm_year + 1900; s_month = date.tm_mon + 1; s_day = date.tm_mday;
        s_hour = date.tm_hour; s_minute = date.tm_min;
    }
    s_notice[0] = 0;
    s_page = SETTINGS_TIME_EDIT;
}

static void time_adjust(int field, int step) {
    if (field == 0) s_year = s_year + step > 2099 ? 2024 : s_year + step < 2024 ? 2099 : s_year + step;
    if (field == 1) s_month = s_month + step > 12 ? 1 : s_month + step < 1 ? 12 : s_month + step;
    if (field == 2) s_day = s_day + step > days_in_month(s_year, s_month) ? 1 :
                            s_day + step < 1 ? days_in_month(s_year, s_month) : s_day + step;
    if (field == 3) s_hour = (s_hour + step + 24) % 24;
    if (field == 4) s_minute = (s_minute + step + 60) % 60;
    if (s_day > days_in_month(s_year, s_month)) s_day = days_in_month(s_year, s_month);
}

static bool time_save(void) {
    int64_t epoch = civil_days(s_year, s_month, s_day) * 86400 +
                    s_hour * 3600 + s_minute * 60 - 8 * 3600;
    if (epoch < 1704067200 || epoch > 4102444799LL) return false;
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL)) return false;
    uint8_t payload[4] = {(uint8_t)epoch, (uint8_t)(epoch >> 8),
                          (uint8_t)(epoch >> 16), (uint8_t)(epoch >> 24)};
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    if (err == ESP_OK) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (err != ESP_OK || !pmu || !pmu->time_synced) {
        snprintf(s_notice, sizeof(s_notice), "系统时间已设置，PMU 同步失败");
        return false;
    }
    snprintf(s_notice, sizeof(s_notice), "时间已保存，并同步到电源管理芯片");
    s_page = SETTINGS_TIME;
    return true;
}

static void settings_card(uint8_t *fb, EpdRect rect, int radius, uint8_t fill, uint8_t edge) {
    /* v1（用户 2026-10-17）：边框统一 2px 纯黑，忽略传入的灰色。 */
    (void)edge;
    ui_fill_round_rect(fb, rect, radius, fill);
    ui_draw_control_frame(fb, rect, radius, 0x00);
}
static void settings_divider(uint8_t *fb, int y, int x, int width) {
    ui_draw_separator(fb, y, x, width, 0x00);
}
static void settings_value(uint8_t *fb,int x,int cy,int px,const char *value) {
    char shown[128]; snprintf(shown,sizeof(shown),"%s",value);
    size_t n=strlen(shown);
    bool arrow=n>=3 && !strcmp(shown+n-3,"›");
    if (arrow) {
        shown[n-=3]=0;
        while (n && shown[n-1]==' ') shown[--n]=0;
        ui_draw_icon(fb,x-4,cy,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
    ui_text_vc(fb,x-(arrow?28:0),cy,px,shown,EPD_DRAW_ALIGN_RIGHT,false);
}
static void row(uint8_t *fb, int y, const char *label, const char *value) {
    ui_text(fb, 54, y + 17, 27, label, EPD_DRAW_ALIGN_LEFT, false);
    settings_value(fb,627,y+20+ui_text_effective_px(21)/2,21,value);
    settings_divider(fb, y + 68, 54, 575);
}
static void section(uint8_t *fb, int y, const char *title) {
    ui_text(fb, 40, y, 22, title, EPD_DRAW_ALIGN_LEFT, false);
}

/* ---- 蓝牙翻页器子页的版式 / Layout of the Bluetooth sub-page ---- */

// 行高比 setting_group 的 68 略大，因为这一页每行右侧常常要放一个按钮。
// Rows are a little taller than setting_group's 68 because these rows often carry a button.
#define BLE_ROW_H 84
// 按钮右缘与 row() 的值右缘对齐；值文字右端再让出按钮宽度，两者不重叠。
// The button's right edge aligns with row()'s value edge; the value text then stops short of the
// button, so the two never overlap.
#define BLE_BTN_W 118
#define BLE_BTN_RIGHT 624
#define BLE_VALUE_RIGHT (BLE_BTN_RIGHT - BLE_BTN_W - 17)

// 渲染和命中测试都调这个函数，两边看到的是同一份坐标——否则会出现「按钮画在这儿、
// 点在别处」，这一页之前就是这么坏的。
// Rendering and hit testing both call this, so they see identical coordinates. Without it a
// control ends up drawn in one place and tapped in another, which is exactly how this page was
// broken before.
typedef struct {
    int toggle_top;                  // 开关卡片顶；-1 表示被学习提示取代 / switch card top, -1 when the learn prompt replaces it
    int status_top;
    int notice_top;                  // -1 表示没有提示 / -1 when absent
    int bonds_title_top;
    int bond_top[BLE_PT_MAX_BONDS];  // 已配对行顶；-1 表示该位不存在 / bonded row tops, -1 when absent
    int bond_rows;
    int scan_entry_top;              // 「扫描设备 ›」入口行顶 / the "scan devices" entry row
    int learn_title_top;
    int learn_top[2];
    int footer_top;
    int content_bottom;              // 内容底，用于滚动上限 / content bottom, for the scroll limit
} ble_layout_t;

static ble_layout_t ble_layout(void) {
    ble_layout_t l;
    memset(&l, 0, sizeof(l));
    int y = 132 - s_ble_scroll;   /* v1：顶部排版统一（原 202） */
    l.toggle_top = s_ble_learning || s_ble_feedback[0] ? -1 : y;
    y += 126 + 38;
    l.status_top = y;
    y += BLE_ROW_H + 20;
    l.notice_top = s_ble_notice[0] ? y : -1;
    if (s_ble_notice[0]) y += 38;
    l.bonds_title_top = y;
    y += 38;
    l.bond_rows = (int)ble_pt_bond_count();
    for (int i = 0; i < BLE_PT_MAX_BONDS; ++i)
        l.bond_top[i] = i < l.bond_rows ? y + i * BLE_ROW_H : -1;
    y += (l.bond_rows ? l.bond_rows : 1) * BLE_ROW_H;
    l.scan_entry_top = y + 16;
    y = l.scan_entry_top + BLE_ROW_H + 30;
    l.learn_title_top = y;
    l.learn_top[0] = y + 38;
    l.learn_top[1] = l.learn_top[0] + BLE_ROW_H;
    l.footer_top = l.learn_top[1] + BLE_ROW_H + 25;
    l.content_bottom = l.footer_top + 66;
    return l;
}

// 一行的左标签 + 让出按钮后的右值，外加分隔线。与 row() 同高，但右端不会压到按钮。
// A row's left label plus a right value that stops short of the button, with the usual divider.
// Same height as row(), but the value never runs under the button.
// 学到的键按整帧显示：帧里非零的字节照原样列出来。同一个键每次按下发的字节一样，所以这串
// 标识每次都一样；不同的键只要有一个字节不同，读出来就不同。
// A learned key shows its whole frame: the non-zero bytes, in order. The same key sends the same
// bytes every press, so this identifier never changes, and two keys that differ in any byte read
// differently.
static void format_bound_key(const ble_pt_frame_t *frame, char *out, size_t size) {
    if (!frame || !frame->len) {
        if (size) out[0] = 0;
        return;
    }
    size_t used = 0;
    unsigned shown = 0;
    for (size_t i = 0; i < frame->len && used + 4 < size; ++i) {
        if (!frame->bytes[i]) continue;
        if (shown == 3) {  // 值列只有 140px：最多列 3 个字节，多的用 + 表示 / the value column holds three
            snprintf(out + used, size - used, " +");
            return;
        }
        const int written = snprintf(out + used, size - used, shown ? " %02X" : "%02X", frame->bytes[i]);
        if (written <= 0) break;
        used += (size_t)written;
        ++shown;
    }
    if (!shown) snprintf(out, size, "00");  // 理论上到不了：全零帧不给绑 / unreachable: a zero frame is never bound
}

static void ble_row(uint8_t *fb, int y, const char *label, const char *value) {
    if (y + BLE_ROW_H < 190 || y >= UI_NAV_TOP) return;
    settings_card(fb, (EpdRect){36, y, 612, BLE_ROW_H - 8}, 18, UI_GRAY_WHITE, 0x70);
    char name[96]; snprintf(name, sizeof(name), "%s", label);
    fit_value(name, 290);
    ui_text_vc(fb, 60, y + 38, 26, name, EPD_DRAW_ALIGN_LEFT, false);
    if (value && value[0]) {
        char shown[96]; snprintf(shown, sizeof(shown), "%s", value); fit_value(shown, 140);
        settings_value(fb,BLE_VALUE_RIGHT,y+38,21,shown);
    }
}

// 行右侧的按钮。竖直居中于该行，右缘与值文字对齐。
// The button on a row's right: vertically centred in the row, right edge aligned with the value.
static EpdRect ble_button_rect(int row_top) {
    return (EpdRect){BLE_BTN_RIGHT - BLE_BTN_W, row_top + 10, BLE_BTN_W, 56};
}

// 三级页：进页即持续扫描，选中一台连上就退回二级页。列表长了也不影响二级页。
// Third-level page: scanning starts on entry and runs continuously; picking a device connects
// and returns to level two. A long list never affects the page above it.
typedef struct {
    int devices_top;
    int device_rows;
    int status_top;
    int content_bottom;
} ble_scan_layout_t;

// 扫描列表只列真的广播了名字的设备。没有广播名时组件把地址回落成名字，那一行对用户
// 认不出是什么设备，而且地址太长会把信号强度挤掉。
// The scan list shows only devices that actually advertised a name. Without one the component
// falls back to the address: nothing the user can recognise, and long enough to crowd the signal
// strength off the row.
static bool ble_scan_visible(const ble_pt_device_t *dev) {
    return dev != NULL && dev->has_name;
}

static int ble_scan_visible_count(void) {
    int visible = 0;
    const uint8_t count = ble_pt_device_count();
    for (uint8_t i = 0; i < count; ++i) {
        if (ble_scan_visible(ble_pt_device(i))) ++visible;
    }
    return visible;
}

// 取第 row 台有名字的设备；遍历顺序仍是组件的原始顺序。
// The row-th device that has a name; the walk keeps the component's order.
static const ble_pt_device_t *ble_scan_visible_at(int row) {
    if (row < 0) return NULL;
    const uint8_t count = ble_pt_device_count();
    int seen = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const ble_pt_device_t *dev = ble_pt_device(i);
        if (!ble_scan_visible(dev)) continue;
        if (seen++ == row) return dev;
    }
    return NULL;
}

static ble_scan_layout_t ble_scan_layout(void) {
    ble_scan_layout_t l;
    memset(&l, 0, sizeof(l));
    l.status_top = 132;    /* v1：顶部排版统一（原 200/300） */
    l.devices_top = 232;
    l.device_rows = ble_scan_visible_count();
    l.content_bottom = l.devices_top + (l.device_rows ? l.device_rows * BLE_ROW_H : 68) + 40;
    return l;
}
static void back_header(uint8_t *fb, const char *title) {
    /* 2026-10-17：设置子页标题与返回按钮全部移除，仅保留状态栏；返回走三键（右键/中长按）。 */
    (void)title;
    ui_nav_status(fb);
}

// 阅读目标页的 −30 / +30 按钮：绘制与命中共用同一份几何。
// The goal page's -30 / +30 buttons; drawing and hit testing share one geometry.
static EpdRect goal_adjust_button(int plus) { return (EpdRect){plus ? 351 : 72, 322, 261, 84}; }

static void setting_icon(uint8_t *fb, int index, int cx, int cy) {
    // 每个设置项对应一个 Lucide 图标；索引顺序由各分组传入的 icons[] 数组决定。
    // One Lucide icon per setting row; the index order comes from each group's icons[] array.
    static const ui_icon_t icons[] = {
        UI_ICON_WIFI,              // 0 无线连接
        UI_ICON_BLUETOOTH,         // 1 蓝牙
        UI_ICON_TYPE,              // 2 系统字体、阅读操作
        UI_ICON_A_LARGE_SMALL,     // 3 系统字号
        UI_ICON_LIBRARY_BIG,       // 4 书架样式
        UI_ICON_TICKET,            // 5 锁屏样式
        UI_ICON_CLOCK,             // 6 日期与时间
        UI_ICON_CONTRAST,          // 7 系统对比度
        UI_ICON_COPY,              // 8 保存与恢复
        UI_ICON_POWER,             // 9 关机睡眠
        UI_ICON_TEXT_ALIGN_START,  // 10 状态栏签名
        UI_ICON_REFRESH_CW,        // 11 首页强刷
        UI_ICON_DOWNLOAD,          // 12 系统升级
        UI_ICON_CPU,               // 13 BOOT 刷机
        UI_ICON_EYE_CLOSED,        // 14 自动休眠
        UI_ICON_SLIDERS_HORIZONTAL, // 15 主页刷新模式 / Main refresh mode
        UI_ICON_LOCK,              // 16 锁屏密码 / lock screen password
        UI_ICON_ROTATE_CW,         // 17 屏幕修复 / panel repair
        UI_ICON_TIMER,             // 18 阅读目标 / daily reading goal
    };
    if (index < 0 || index >= (int)(sizeof(icons) / sizeof(icons[0]))) return;
    ui_draw_icon(fb, cx, cy, SETTINGS_ICON_PX, icons[index], SETTINGS_ICON_INK);
}

static void settings_fit_text(char *value, int px, int width) {
    while (value[0] && ui_text_fixed_width_px(ui_text_effective_px(px),value)>width) {
        size_t n=strlen(value)-1;while(n&&((unsigned char)value[n]&0xc0)==0x80)--n;value[n]=0;
    }
}
static void fit_value(char *value, int width) { settings_fit_text(value,20,width); }

static void setting_group(uint8_t *fb, int title_y, const char *title,
                          int card_y, const int icons[], const char *const labels[],
                          const char *const values[], int count) {
    const int row_height = SETTINGS_ROW_H;
    title_y -= s_main_scroll;
    card_y -= s_main_scroll;
    // 屏外的行不测字、不生成字形；保留卡片边界与原滚动坐标。
    // Skip off-screen glyph work while preserving card boundaries and scroll coordinates.
    if (card_y + count * row_height <= SETTINGS_BODY_TOP || title_y >= UI_NAV_TOP) return;
    if (title && title_y + ui_text_effective_px(20) > SETTINGS_BODY_TOP && title_y < UI_NAV_TOP)
        ui_text(fb, 42, title_y, 20, title, EPD_DRAW_ALIGN_LEFT, false);
    settings_card(fb, (EpdRect){36, card_y, 612, count * row_height}, 22,
                  UI_GRAY_WHITE, 0x70);
    for (int i = 0; i < count; ++i) {
        int y = card_y + i * row_height;
        if (y + row_height <= SETTINGS_BODY_TOP || y >= UI_NAV_TOP) continue;
        if (i) settings_divider(fb, y, 88, 544);
        setting_icon(fb, icons[i], 67, y + row_height / 2);
        char shown[128]; snprintf(shown,sizeof(shown),"%s",values[i]);
        int value_width = 594 - 100 - 20 - ui_text_fixed_width_px(ui_text_effective_px(25),labels[i]);
        if (value_width > 215) value_width = 215;
        if (value_width < 0) value_width = 0;
        fit_value(shown,value_width);
        ui_text_vc(fb,100,y+row_height/2,25,labels[i],EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,594,y+row_height/2,20,shown,EPD_DRAW_ALIGN_RIGHT,false);
        // 导航是图标，不依赖用户字体是否包含 › 字形，也不会被截短掉。
        // Navigation is an icon, independent of the font cmap and value truncation.
        ui_draw_icon(fb,625,y+row_height/2,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
}

static void setting_toggle(uint8_t *fb, int y, const char *title, const char *detail, bool active) {
    settings_card(fb, (EpdRect){36, y, 612, 126}, 22, UI_GRAY_WHITE, 0x70);
    ui_text(fb, 59, y + 24, 27, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, 59, y + 75, 19, detail, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect track = {555, y + 43, 69, 39};
    ui_fill_round_rect(fb, track, 19, active ? 0x58 : 0xc4);
    int cx = active ? track.x + 49 : track.x + 20;
    epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_WHITE, fb);
    epd_draw_circle(cx, track.y + 19, 16, 0x78, fb);
}

/* v1：行内开关（行高与 row() 一致 68，右侧开关条；锁屏设置页“老黄历与待办”用）。 */
static void settings_row_toggle(uint8_t *fb, int y, const char *label, bool on) {
    ui_text(fb, 54, y + 17, 27, label, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect track = {551, y + 14, 69, 39};
    ui_fill_round_rect(fb, track, 19, on ? 0x58 : 0xc4);
    int cx = on ? track.x + 49 : track.x + 20;
    epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_WHITE, fb);
    epd_draw_circle(cx, track.y + 19, 16, 0x78, fb);
    settings_divider(fb, y + 68, 54, 575);
}

static void draw_style_thumbnail(uint8_t *fb, int style, int top) {
    if (top < 242 || top + 170 >= UI_NAV_TOP) return;
    if (style == 5) {
        ui_fill_round_rect(fb, (EpdRect){68,top,548,170}, 12, UI_GRAY_WHITE);
        for (int i=0;i<3;++i) {
            int y=top+9+i*52;
            ui_fill_rect_fast((EpdRect){86,y,31,43},i==1?0x80:0x40,fb);
            ui_fill_rect_fast((EpdRect){137,y+6,218,3},0,fb);
            ui_fill_rect_fast((EpdRect){137,y+19,84,2},0x70,fb);
            ui_fill_rect_fast((EpdRect){137,y+34,435,4},0xd0,fb);
            ui_fill_rect_fast((EpdRect){137,y+34,120+i*75,4},0x40,fb);
        }
        return;
    }
    const int left = 99;
    for (int i = 0; i < 3; ++i) {
        int x = left + 20 + i * 155;
        EpdRect cover = {x, top, 116, 145};
        ui_fill_rect_fast(cover, i == 1 ? 0x60 : i == 2 ? 0xa0 : 0x40, fb);
        ui_draw_round_rect(fb, cover, 1, 0x30);
        ui_draw_round_rect(fb, (EpdRect){x + 7, top + 7, 102, 130}, 1, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 47, x + 28, 60, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 54, x + 39, 38, i == 1 ? 0xc0 : 0xe0);
        ui_hairline(fb, top + 111, x + 44, 28, i == 1 ? 0xc0 : 0xe0);
    }
    if (style == 1) {
        ui_fill_rect_fast((EpdRect){left, top + 143, 486, 16}, 0x30, fb);
        ui_hairline(fb, top + 143, left, 486, 0x80);
    } else if (style == 2) {
        ui_draw_acrylic_guard(fb, (EpdRect){left, top + 94, 486, 70});

    }
}

// 仅 82x82 灰阶头像，不缓存整屏或第二份字库；文件变更/拔卡会使缓存失效。
// One 82x82 gray avatar, never a full frame or second font; file changes/removal invalidate it.
static EXT_RAM_BSS_ATTR uint8_t s_avatar_pixels[82*82];
static char s_avatar_path[288];
static bool s_avatar_ready, s_avatar_pending;
static char s_main_wifi[33];
static bool s_main_wifi_saved;
/* 默认头像（内嵌狐狸 82x82）由 ui_nothing 统一提供：状态栏与设置页同一来源。
 * The default fox avatar comes from ui_nothing: one source for statusbar and settings. */
static bool draw_cover_preview(uint8_t *fb, const char *path, EpdRect r) {
    /* v1 透明书封预览：PNG 带 alpha 合成到预览卡白底（透明处显白）。 */
    const size_t plane = (size_t)r.width * r.height;
    uint8_t *gray = heap_caps_malloc(plane, MALLOC_CAP_SPIRAM);
    uint8_t *alpha = heap_caps_malloc(plane, MALLOC_CAP_SPIRAM);
    bool ok = gray && alpha && book_image_file_cover_layer(path, r.width, r.height, gray, alpha);
    if (ok) {
        for (int y = 0; y < r.height; ++y)
            for (int x = 0; x < r.width; ++x) {
                const size_t i = (size_t)y * r.width + x;
                const uint8_t a = alpha[i];
                const uint8_t v = a >= 250 ? gray[i]
                    : (uint8_t)((gray[i] * (uint32_t)a + 0xE8u * (255u - a) + 127) / 255);
                epd_draw_pixel(r.x + x, r.y + y, v, fb);
            }
    }
    free(gray);
    free(alpha);
    return ok;
}

/* v2（2026-10-17）：内嵌默认图（灰8+alpha8，assets/cover_default.bin.z）的预览。
 * 与 draw_cover_preview 同一混白公式，最近邻缩放到预览卡内等比放置。 */
extern const uint8_t cover_default_bin_z_start[] asm("_binary_cover_default_bin_z_start");
extern const uint8_t cover_default_bin_z_end[] asm("_binary_cover_default_bin_z_end");
static bool draw_default_preview(uint8_t *fb, EpdRect r) {
    const int SW = 684, SH = 1216;
    const size_t plane = (size_t)SW * SH;
    uint8_t *buf = heap_caps_malloc(plane * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    uLongf inflated = (uLongf)(plane * 2);
    bool ok = uncompress(buf, &inflated, cover_default_bin_z_start,
        (uLong)(cover_default_bin_z_end - cover_default_bin_z_start)) == Z_OK &&
        inflated == plane * 2;
    if (ok) {
        int tw = r.width;
        int th = (int)(((int64_t)r.width * SH) / SW);
        if (th > r.height) { th = r.height; tw = (int)(((int64_t)r.height * SW) / SH); }
        const int x0 = r.x + (r.width - tw) / 2, y0 = r.y + (r.height - th) / 2;
        const uint8_t *gray = buf, *alpha = buf + plane;
        for (int y = 0; y < th; ++y) {
            const int sy = (int)((int64_t)y * SH / th);
            for (int x = 0; x < tw; ++x) {
                const int sx = (int)((int64_t)x * SW / tw);
                const size_t i = (size_t)sy * SW + sx;
                const uint8_t a = alpha[i];
                const uint8_t v = a >= 250 ? gray[i]
                    : (uint8_t)((gray[i] * (uint32_t)a + 0xE8u * (255u - a) + 127) / 255);
                epd_draw_pixel(x0 + x, y0 + y, v, fb);
            }
        }
    }
    free(buf);
    return ok;
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_page == SETTINGS_PIN_ENTRY) { ui_pinpad_paint(fb, &s_pinpad, ui_pinpad_full()); return; }
    ui_clear_page(fb);

    if (s_page == SETTINGS_MAIN || s_page == SETTINGS_SHELF_STYLE)
        ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_TOP},
                      app_settings_main_fast_refresh() && s_page == SETTINGS_MAIN ? 0xf0 : 0xe0, fb);
    ui_nav_status(fb);
    if (s_page == SETTINGS_LOCK_PIN) {
        back_header(fb, "锁屏密码");
        section(fb, 96, "四位数字密码");
        row(fb, 132, "四位数字密码", lock_pin_enabled() ? "已开启" : "已关闭");
        if (!lock_pin_available()) ui_text_vc(fb, 342, 300, 24, "凭据读取失败，请重启后重试", EPD_DRAW_ALIGN_CENTER, false);
        else if (lock_pin_enabled()) {
            row(fb, 272, "修改密码", "验证原密码  ›");
            row(fb, 378, "关闭密码", "验证原密码  ›");
        } else row(fb, 272, "开启密码", "设置四位密码  ›");
        if (s_notice[0]) ui_text_vc(fb, 342, 560, 23, s_notice, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 640, 21, "修改和关闭密码需验证原密码", EPD_DRAW_ALIGN_CENTER, false);
        ui_nav_draw(fb, 3); return;
    }
    if (s_page == SETTINGS_PROFILE) {
        back_header(fb, "个人资料");
        section(fb, 96, "自定义名称与头像");
        row(fb, 132, "设备名称", app_settings_device_name());
        row(fb, 232, "更换头像", app_settings_avatar_path()[0] ? "已选择图片  ›" : "默认图标  ›");
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_AVATAR) {
        back_header(fb, "选择头像");
        section(fb, 96, "TF 卡 pictures 文件夹中的 JPG / PNG 图片");
        row(fb, 132, "使用默认图标", app_settings_avatar_path()[0] ? "选择  ›" : "当前  ✓");
        if (!s_wallpaper_count) ui_text(fb, 52, 260, 23, "未找到图片，请放入 pictures 文件夹", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 8; ++i) {
            int index = s_wallpaper_page * 8 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 250 + i * 103, 612, 88};
            bool active = !strcmp(s_wallpapers[index].path, app_settings_avatar_path());
            settings_card(fb, box, 17, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 44, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(611, box.y + 44, 7, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TEXT_EDIT) {
        back_header(fb, "设备名称");
        ui_text(fb, 642, 95, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
        ui_text(fb, 36, 132, 21, "显示在设置页的资料卡", EPD_DRAW_ALIGN_LEFT, false);
        ui_text_input_draw(fb, &s_editor_input, (EpdRect){36, 168, 612, 82}, 28, false, NULL);
        if (s_editor_notice[0]) ui_text_fixed_vc(fb, 36, 510, 22, s_editor_notice, EPD_DRAW_ALIGN_LEFT, false);
        /* v2（用户 2026-10-19）：输入法不保留导航栏位置——键盘下移到屏底（694..1216），
         * 覆盖导航区，本页不再绘制导航。 */
        ui_keyboard_draw(fb, 694);
        return;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        ui_nav_back(fb, 36, 79);
        ui_text_vc(fb, 342, 107, 34, "书架样式", EPD_DRAW_ALIGN_CENTER, false);
        ui_text(fb, 36, 207, 23, "选择书架外观与排列方式", EPD_DRAW_ALIGN_LEFT, false);
        static const char *const styles[] = {"深色书轨", "亚克力书架", "列表书架"};
        static const char *const descriptions[] = {"每页 9 本", "每页 9 本", "作者与进度 · 每页 4 本"};
        for (int i = 0; i < 3; ++i) {
            int y = 263 + i * 253 - s_style_scroll;
            if (y + 230 < 242 || y > 1095) continue;
            EpdRect card = {36, y, 612, 230};
            int style = i == 2 ? 5 : i + 1;
            bool active = app_settings_shelf_style() == style;
            settings_card(fb, card, 24, active ? 0xd0 : UI_GRAY_WHITE,
                          active ? 0x58 : 0x70);
            draw_style_thumbnail(fb, style, y + 11);
            if (y + 190 < 1096) ui_text(fb, 68, y + 188, 23, styles[i], EPD_DRAW_ALIGN_LEFT, false);
            if (y + 195 < 1096) ui_text(fb, 310, y + 192, 17, descriptions[i], EPD_DRAW_ALIGN_LEFT, false);
            if (y + 220 < 1096) {
                epd_draw_circle(609, y + 207, 13, UI_GRAY_BLACK, fb);
                if (active) epd_fill_circle(609, y + 207, 6, UI_GRAY_BLACK, fb);
            }
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TIME_EDIT) {
        /* v1：顶部排版统一（说明 96 / 内容 132 系）。 */
        back_header(fb, "日期与时间");
        section(fb, 96, "北京时间 · UTC+8 · 手动设置");
        static const char *const labels[] = {"年", "月", "日", "时", "分"};
        int values[] = {s_year, s_month, s_day, s_hour, s_minute};
        for (int i = 0; i < 5; ++i) {
            int x = 36 + i * 122;
            ui_text(fb, x + 52, 206, 24, labels[i], EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_control_frame(fb, (EpdRect){x, 254, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 285, 30, "+", EPD_DRAW_ALIGN_CENTER, false);
            char value[8]; snprintf(value, sizeof(value), i == 0 ? "%04d" : "%02d", values[i]);
            ui_text(fb, x + 53, 356, i == 0 ? 34 : 39, value, EPD_DRAW_ALIGN_CENTER, false);
            ui_draw_control_frame(fb, (EpdRect){x, 435, 106, 62}, 8, UI_GRAY_BLACK);
            ui_text_vc(fb, x + 53, 466, 30, "−", EPD_DRAW_ALIGN_CENTER, false);
        }
        ui_text(fb, 36, 550, 23, "点上方 + 或下方 − 调整数字", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 640, 612, 78}, "保存时间", false);
        ui_draw_button(fb, (EpdRect){36, 748, 612, 78}, "取消", false);
        if (s_notice[0]) ui_text(fb, 36, 900, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    if (s_page == SETTINGS_WIFI) {
        /* v1：说明行对齐文件管理顶部（96），内容统一从 132 起。 */
        back_header(fb, "WiFi");
        section(fb, 96, "已保存网络");
        row(fb, 132, s_main_wifi_saved ? s_main_wifi : "尚未配置", s_main_wifi_saved ? "已保存" : "");
        row(fb, 244, "连接或更改网络", "前往传输  ›");
        ui_text(fb, 54, 365, 21, "配网后可在日期与时间中进行 WiFi 对时。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        back_header(fb, "系统字体");
        system_font_layout_t l = system_font_layout();
        settings_card(fb,l.size,22,UI_GRAY_WHITE,0x70);
        char size[16]; snprintf(size,sizeof(size),"%u%%",app_settings_system_font_size());
        if (settings_stacked()) {
            ui_text_vc(fb,60,182,27,"系统字号",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,589,182,27,size,EPD_DRAW_ALIGN_RIGHT,false);
            ui_text_vc(fb,60,263,18,"轻点切换，最大 200%",EPD_DRAW_ALIGN_LEFT,false);
        } else {
            ui_text_vc(fb,58,162,25,"系统字号",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,58,204,18,"轻点切换，最大 200%",EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,589,182,26,size,EPD_DRAW_ALIGN_RIGHT,false);
        }
        ui_draw_icon(fb,619,182,28,UI_ICON_ROTATE_CW,0x38);
        section(fb,l.preview_title,"效果预览");
        settings_card(fb,l.preview,22,UI_GRAY_WHITE,0x70);
        ui_text_vc(fb,60,l.preview.y+(settings_stacked()?68:51),29,"阅读，让时间慢下来",EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,60,l.preview.y+(settings_stacked()?151:107),25,"Aa 123",EPD_DRAW_ALIGN_LEFT,false);
        section(fb,l.font_title,"选择字体");
        const char *selected = app_settings_system_font_path();
        int count = ttf_font_count(), first = s_font_page*l.slots;
        for (int slot=0;slot<l.slots && first+slot<=count;++slot) {
            int index=first+slot;
            const ttf_font_item_t *item=index?ttf_font_item(index-1):NULL;
            bool active=item?!strcmp(selected,item->path):!selected[0];
            EpdRect box=system_font_item_rect(slot);
            settings_card(fb,box,20,active?0xe0:UI_GRAY_WHITE,0x70);
            char label[TTF_FONT_NAME_MAX];
            snprintf(label,sizeof(label),"%s",item?system_font_label(item->path):"思源黑体（内建）");
            settings_fit_text(label,26,box.width-100);
            ui_text_vc(fb,60,box.y+box.height/2,26,label,EPD_DRAW_ALIGN_LEFT,false);
            if (active) epd_fill_circle(613,box.y+box.height/2,6,UI_GRAY_BLACK,fb);
        }
        char pages[24];snprintf(pages,sizeof(pages),"%d / %d",s_font_page+1,(count+l.slots)/l.slots);
        ui_text_fixed_vc(fb,648,1080,18,pages,EPD_DRAW_ALIGN_RIGHT,false);
        if (s_notice[0]) ui_text_fixed_vc(fb,42,1080,18,s_notice,EPD_DRAW_ALIGN_LEFT,false);
        ui_nav_draw(fb,3);
        if (s_font_sync_confirm) draw_system_font_sync(fb);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        back_header(fb, "系统字号");
        section(fb, 96, "界面文字大小");
        for (int i = 0; i < 11; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36 + (i % 2) * 316, 132 + (i / 2) * 122, 296, 102};
            bool active = app_settings_system_font_size() == value;
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            char label[48]; snprintf(label, sizeof(label), "%d%%", value);
            ui_text_vc(fb, box.x + 24, box.y + 51, 29, label, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(box.x + box.width - 25, box.y + 51, 8, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
/* v1（用户 2026-10-17）：子页选项卡贴顶：section=88、卡=122 起
 * （状态栏底 70 + 18 呼吸；修 section 文字与卡顶重叠遮挡问题）。 */
    if (s_page == SETTINGS_MAIN_REFRESH) {
        back_header(fb, "刷新模式");
        section(fb, 96, "首页 · 书架 · 文件管理 · 设置");
        const char* names[] = {"普通", "快刷", "水波纹"};
        const char* details[] = {"保留灰阶层次，兼顾画面清晰度", "黑白快刷，亚克力用细棋盘格", "以水波纹切换页面，保留灰阶"};
        for (int i = 0; i < 3; ++i) {
            const EpdRect box = {36, 132 + i * 134, 612, 112};
            const bool active = app_settings_main_refresh_mode() == (app_main_refresh_mode_t)i;
            settings_card(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE, 0x58);
            ui_text_vc(fb, 64, box.y + 32, 29, names[i], EPD_DRAW_ALIGN_LEFT, false);
            char detail[128]; snprintf(detail, sizeof(detail), "%s", details[i]); fit_value(detail, 498);
            ui_text_vc(fb, 64, box.y + 78, 20, detail, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(607, box.y + 32, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 42, 556, 20, "普通与水波纹保留灰阶；快刷转黑白点阵", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 42, 592, 20, "屏幕修复可清除长期翻页残留的残影", EPD_DRAW_ALIGN_LEFT, false);
        /* 底部两项（用户 2026-10-17）：首页强刷开关 + 屏幕修复按钮沉底，
         * 底缘与休眠锁屏页对齐（≈1056）；命中与绘制同坐标。 */
        setting_toggle(fb, 810, "首页强刷", "进入首页时整屏强刷一次", app_settings_home_full_refresh());
        ui_draw_button(fb, (EpdRect){36, 972, 612, 78}, "屏幕修复（清除残影条纹）", false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        back_header(fb, "系统对比度");
        section(fb, 96, "调整系统界面的灰阶层次与清晰度");
        static const char *const names[] = {"柔和", "标准", "清晰", "鲜明", "高对比"};
        for (int i = 0; i < 5; ++i) {
            int value = 100 + i * 10;
            EpdRect box = {36, 132 + i * 116, 612, 90};
            bool active = app_settings_system_contrast() == value;
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text_vc(fb, 64, box.y + 45, 28, names[i], EPD_DRAW_ALIGN_LEFT, false);
            char shown[16]; snprintf(shown, sizeof(shown), "%d%%", value);
            ui_text_vc(fb, 570, box.y + 45, 22, shown, EPD_DRAW_ALIGN_RIGHT, false);
            if (active) epd_fill_circle(605, box.y + 45, 8, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_LOCK_STYLE) {
        back_header(fb, "锁屏设置");
        section(fb, 96, "选择电源键锁屏后的画面");
        /* v5（用户 2026-10-19）：卡序——壁纸锁屏 → 透明书封 → 时钟模式 → 计算器锁屏（新）。
         * 时钟卡内嵌「时间以外内容」开关（默认开）：点开关只切内容，点卡其余区域选样式。
         * 页尾新增「锁屏密码」入口（RC95 原有 SETTINGS_LOCK_PIN 页，用户 2026-10-19）。 */
        const char *labels[] = {"壁纸锁屏", "时钟模式", "透明书封", "计算器锁屏"};
        const char *details[] = {"使用 TF 卡中的 JPG / PNG 图片", "关闭开关则只显示时间",
                                 "使用 TF 卡中的 PNG 图片", "锁屏后可用计算器，密码按＝解锁"};
        const uint8_t values[] = {1, 0, 2, 3};
        const uint8_t order[] = {0, 2, 1, 3};
        for (int i = 0; i < 4; ++i) {
            int k = order[i];
            EpdRect box = {36, 132 + i * 134, 612, 112};
            bool active = app_settings_lock_style() == values[k];
            settings_card(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text(fb, 64, box.y + 19, 29, labels[k], EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 64, box.y + 67, 19, details[k], EPD_DRAW_ALIGN_LEFT, false);
            if (k == 1) {
                const bool on = app_settings_clock_almanac();
                EpdRect track = {551, box.y + 46, 69, 39};
                ui_fill_round_rect(fb, track, 19, on ? 0x58 : 0xc4);
                int cx = on ? track.x + 49 : track.x + 20;
                epd_fill_circle(cx, track.y + 19, 16, UI_GRAY_WHITE, fb);
                epd_draw_circle(cx, track.y + 19, 16, 0x78, fb);
                if (active) epd_fill_circle(609, box.y + 32, 8, UI_GRAY_BLACK, fb);
            } else if (active) epd_fill_circle(609, box.y + 56, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 54, 694, 22, "时钟模式永不关机 · 选中的样式可从 TF 卡更换图片", EPD_DRAW_ALIGN_LEFT, false);
        /* 页尾「锁屏密码」入口：RC95 原有锁屏密码页（SETTINGS_LOCK_PIN）。 */
        EpdRect pin_row = {36, 1010, 612, 78};
        settings_card(fb, pin_row, 18, UI_GRAY_WHITE, 0x70);
        ui_text_vc(fb, 60, pin_row.y + 39, 24, "锁屏密码", EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(fb, 560, pin_row.y + 39, 20, lock_pin_enabled() ? "已开启 ›" : "未设置 ›", EPD_DRAW_ALIGN_RIGHT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_WALLPAPER) {
        back_header(fb, s_pick_cover ? "选择书封" : "选择壁纸");
        section(fb, 96, s_pick_cover ? "TF 卡 PNG · 或选内置默认图"
                                     : "TF 卡 JPG / PNG · 或选内置默认图");
        /* v2：首行固定“默认图片”（内嵌狐狸），无 TF 卡图片也能用壁纸锁屏/透明书封。 */
        EpdRect def = {36, 132, 612, 78};
        bool def_active = s_pick_cover ? !app_settings_coverart_path()[0]
                                       : !app_settings_wallpaper_path()[0];
        settings_card(fb, def, 18, def_active ? 0xd0 : UI_GRAY_WHITE, 0x70);
        ui_text_vc(fb, 60, def.y + 39, 24, "默认图片（take easy 狐狸）", EPD_DRAW_ALIGN_LEFT, false);
        if (def_active) epd_fill_circle(609, def.y + 39, 6, UI_GRAY_BLACK, fb);
        if (!s_wallpaper_count)
            ui_text(fb, 54, 238, 22, s_pick_cover ? "未在 TF 卡找到 PNG，可使用上面的默认图"
                                                  : "未在 TF 卡找到图片，可使用上面的默认图", EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < 7; ++i) {
            int index = s_wallpaper_page * 7 + i;
            if (index >= s_wallpaper_count) break;
            EpdRect box = {36, 132 + 92 + i * 92, 612, 78};
            const char *chosen = s_pick_cover ? app_settings_coverart_path()
                                              : app_settings_wallpaper_path();
            bool active = !strcmp(s_wallpapers[index].path, chosen);
            settings_card(fb, box, 18, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            char name[96]; snprintf(name, sizeof(name), "%s", s_wallpapers[index].name);
            fit_value(name, 490);
            ui_text_vc(fb, 60, box.y + 39, 24, name, EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 39, 6, UI_GRAY_BLACK, fb);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        back_header(fb, s_pick_cover ? "预览书封" : "预览壁纸");
        EpdRect image = {166, 132, 352, 700};
        ui_fill_round_rect(fb, image, 8, UI_GRAY_WHITE);
        s_wallpaper_preview_ok = s_wallpaper_selected == -1
            ? draw_default_preview(fb, image)
            : s_wallpaper_selected >= 0 && s_wallpaper_selected < s_wallpaper_count &&
              (s_pick_cover ? draw_cover_preview(fb, s_wallpapers[s_wallpaper_selected].path, image)
                            : ui_wallpaper_draw(fb, s_wallpapers[s_wallpaper_selected].path, image));
        ui_draw_round_rect(fb, image, 8, 0x70);
        if (!s_wallpaper_preview_ok)
            ui_text_vc(fb, 342, 482, 23, "图片无法预览", EPD_DRAW_ALIGN_CENTER, false);
        if (s_wallpaper_selected >= -1 && s_wallpaper_selected < s_wallpaper_count) {
            char name[96];
            snprintf(name, sizeof(name), "%s", s_wallpaper_selected == -1 ? "默认图片"
                                             : s_wallpapers[s_wallpaper_selected].name);
            fit_value(name, 570);
            ui_text_vc(fb, 342, 862, 24, name, EPD_DRAW_ALIGN_CENTER, false);
            char position[40];
            if (s_wallpaper_selected == -1)
                snprintf(position, sizeof(position), "内置 · 上下滑动切换");
            else
                snprintf(position, sizeof(position), "%d / %d · 上下滑动切换",
                         s_wallpaper_selected + 1, s_wallpaper_count);
            ui_text_vc(fb, 342, 911, 20, position, EPD_DRAW_ALIGN_CENTER, false);
        }
        if (s_wallpaper_preview_ok) {
            EpdRect button = {94, 960, 496, 70};
            ui_fill_round_rect(fb, button, 22, 0x30);
            ui_text_vc(fb, 342, 995, 25, s_pick_cover ? "设为透明书封" : "设为锁屏壁纸", EPD_DRAW_ALIGN_CENTER, true);
        }
        if (s_wallpaper_confirm) {
            EpdRect dialog = {68, 435, 548, 306};
            ui_fill_round_rect(fb, dialog, 26, UI_GRAY_WHITE);
            ui_draw_control_frame(fb, dialog, 26, 0x50);
            ui_text_vc(fb, 342, 503, 31, s_pick_cover ? "设为透明书封？" : "设为壁纸锁屏？", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 565, 20, s_pick_cover ? "确认后，透明书封锁屏将叠加此 PNG（透明处透出画面）"
                                                     : "确认后，短按电源键将显示此图片", EPD_DRAW_ALIGN_CENTER, false);
            EpdRect cancel = {94, 643, 232, 68}, confirm = {358, 643, 232, 68};
            ui_fill_round_rect(fb, cancel, 18, 0xe0);
            ui_fill_round_rect(fb, confirm, 18, 0x30);
            ui_text_vc(fb, 210, 677, 24, "取消", EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 474, 677, 24, "确认", EPD_DRAW_ALIGN_CENTER, true);
        }
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_TIME) {
        back_header(fb, "日期与时间");
        time_t now = time(NULL);
        char shown[64] = "未设置";
        if (now >= 1704067200) {
            struct tm tm; time_t local = now + 8 * 3600;
            if (gmtime_r(&local, &tm)) snprintf(shown, sizeof(shown), "%04d-%02d-%02d  %02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        }
        section(fb, 96, "当前时间 · 北京时间");
        row(fb, 132, shown, "");
        row(fb, 243, "通过 WiFi 对时", "立即同步  ›");
        row(fb, 354, "手动设置", "编辑  ›");
        ui_text(fb, 54, 470, 21, "对时成功后由内部时钟持续走时；无需保持联网。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 508, 21, "长时间离线可能产生误差，建议定期重新对时。", EPD_DRAW_ALIGN_LEFT, false);
        if (s_notice[0]) ui_text(fb, 54, 586, 22, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_AUTO_LOCK) {
        back_header(fb, "休眠锁屏");
        section(fb, 96, "无操作后自动锁屏");
        const char *labels[] = {"1 分钟", "5 分钟", "10 分钟", "关闭"};
        const uint8_t values[] = {1, 5, 10, 0};
        for (int i = 0; i < 4; ++i) {
            EpdRect box = {36, 132 + i * 112, 612, 92};
            bool active = app_settings_auto_lock_minutes() == values[i];
            settings_card(fb, box, 20, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text_vc(fb, 64, box.y + box.height / 2, 30, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + box.height / 2, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 48, 600, 22, "操作后重新计时，传输和升级期间暂停", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 648, 22, "锁屏先浅睡，10 分钟后进入深睡", EPD_DRAW_ALIGN_LEFT, false);
        /* 关机睡眠开关（归并到本页；底部余量与顶部一致）。 */
        setting_toggle(fb, 930, "关机睡眠", "关机时先浅睡，10 分钟后彻底断电", app_settings_staged_shutdown());
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_GOAL) {
        back_header(fb, "阅读目标");
        section(fb, 96, "每日阅读时长目标");
        unsigned goal = app_settings_home_goal_minutes();
        char hero[32];
        snprintf(hero, sizeof(hero), "%u 分钟", goal);
        settings_card(fb, (EpdRect){36, 132, 612, 150}, 22, UI_GRAY_WHITE, 0x70);
        ui_text_fixed_vc(fb, 342, 182, 30, hero, EPD_DRAW_ALIGN_CENTER, false);
        char per_cell[48];
        snprintf(per_cell, sizeof(per_cell), "阅读数据卡每格 %u 分钟", goal ? goal / 6 : 0);
        ui_text_fixed_vc(fb, 342, 232, 22, per_cell, EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, goal_adjust_button(0), "- 30 分钟", false);
        ui_draw_button(fb, goal_adjust_button(1), "+ 30 分钟", false);
        ui_text(fb, 48, 520, 22, "范围 30 至 180 分钟，每次增减 30 分钟", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 570, 22, "首页阅读数据：每天 6 格涂满即达成目标", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_POWER_SLEEP) {
        back_header(fb, "关机睡眠");
        section(fb, 96, "选择电源菜单中“关机”的方式");
        static const char *const labels[] = {"直接断电", "先浅睡后断电"};
        static const char *const details[] = {"关机后立即断开供电；下次开机重新启动", "先浅睡 10 分钟，无唤醒才彻底断电"};
        for (int i = 0; i < 2; ++i) {
            EpdRect box = {36, 132 + i * 154, 612, 132};
            bool active = app_settings_staged_shutdown() == (i == 1);
            settings_card(fb, box, 22, active ? 0xd0 : UI_GRAY_WHITE, 0x70);
            ui_text(fb, 64, box.y + 23, 29, labels[i], EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 64, box.y + 79, 20, details[i], EPD_DRAW_ALIGN_LEFT, false);
            if (active) epd_fill_circle(609, box.y + 66, 8, UI_GRAY_BLACK, fb);
        }
        ui_text(fb, 54, 480, 21, "浅睡时按键直接返回，深睡时会重新开机。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 54, 524, 21, "锁屏样式可选壁纸、时钟模式或透明书封。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_BOOT) {
        back_header(fb, "电脑刷机");
        section(fb, 96, "用于电脑完整刷机或故障恢复");
        settings_card(fb, (EpdRect){36, 132, 612, 298}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 166, 29, "进入 BOOT 模式", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 228, 22, "用于电脑完整刷机或故障恢复", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){60, 316, 564, 78}, s_boot_pending ? "正在进入" : "进入 BOOT 模式", false);
        ui_text(fb, 48, 500, 22, "请用 USB 连接电脑，在电脑刷机工具中操作", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3); return;
    }
    if (s_page == SETTINGS_UPGRADE) {
        /* v1（用户 2026-10-17）：改名“关于系统”；顶部排版统一（132）。 */
        back_header(fb, "关于系统");
        settings_card(fb, (EpdRect){36, 132, 612, 120}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 154, 22, "当前版本", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 194, 29, esp_app_get_description()->version, EPD_DRAW_ALIGN_LEFT, false);
        if (s_upgrade_confirm) {
            settings_card(fb, (EpdRect){36, 274, 612, 344}, 22, UI_GRAY_WHITE, 0x70);
            ui_text_fixed(fb, 60, 301, 31, "确认 TF 卡升级", EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed(fb, 60, 365, 28, s_local_update.candidate_version,
                    EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed(fb, 60, 431, 24, "升级期间请保持供电", EPD_DRAW_ALIGN_LEFT, false);
            ui_text_fixed(fb, 60, 478, 23, "设置、阅读记录与 TF 卡文件保留", EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, upgrade_confirm_button(false), "取消", false);
            ui_draw_button(fb, upgrade_confirm_button(true), "确认升级", true);
        } else {
            settings_card(fb, (EpdRect){36, 274, 612, 278}, 22, UI_GRAY_WHITE, 0x70);
            ui_text(fb, 60, 301, 29, "TF 卡升级", EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 60, 354, 23, "升级包放在 TF 卡根目录", EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, 60, 404, 21, s_local_update.ready ? s_local_update.candidate_version : "把固件升级包放入 TF 卡根目录", EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, (EpdRect){60, 468, 564, 62}, s_local_pending ? "正在安装，请勿断电" :
                s_local_update.ready ? "安装 TF 卡升级包" : "重新检查 TF 卡", s_local_update.ready);
        }
        if (s_upgrade_notice[0]) ui_text(fb, 48, 660, 21, s_upgrade_notice, EPD_DRAW_ALIGN_LEFT, false);
        /* 保存与恢复、BOOT 刷机入口（2026-10-17 归并到本页，位置不变）。 */
        row(fb, 928, "保存与恢复", "配置备份 ›");
        row(fb, 1024, "BOOT 刷机", "电脑完整刷机 ›");
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_CONFIG) {
        back_header(fb, "保存与恢复配置");
        section(fb, 96, "换机或刷机后，快速恢复个性化设置");
        settings_card(fb, (EpdRect){36, 132, 612, 197}, 22, UI_GRAY_WHITE, 0x70);
        ui_text(fb, 60, 159, 26, "TF 卡根目录", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 210, 23, "配置备份文件", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 60, 268, 19, "文字排版、设备资料、阅读记录与书签", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){36, 360, 612, 83}, "保存当前配置到 TF 卡", false);
        ui_draw_button(fb, (EpdRect){36, 480, 612, 83},
                       s_config_confirm ? "再次点按，确认恢复配置" : "从 TF 卡恢复配置", false);
        if (s_notice[0]) ui_text(fb, 48, 618, 21, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 700, 19, "包含阅读记录、设备资料和已存 WiFi。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 740, 19, "书籍、字体和图片仍需留在 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, 780, 19, "备份含 WiFi 密码，请妥善保管 TF 卡。", EPD_DRAW_ALIGN_LEFT, false);
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_BLE_SCAN) {
        ui_nav_status(fb);
        const ble_scan_layout_t l = ble_scan_layout();
        const int scroll = s_ble_scroll;
        // 持续扫描中：告诉用户正在找，以及找到几台。
        // Continuous scan: say that it is running and how many turned up.
        char head[64];
        if (ble_pt_scanning()) snprintf(head, sizeof(head), "正在搜索…已找到 %u 台", (unsigned)ble_scan_visible_count());
        else snprintf(head, sizeof(head), "扫描已停止");
        ui_text(fb, 44, l.status_top - scroll, 23, head, EPD_DRAW_ALIGN_LEFT, false);
        for (int i = 0; i < l.device_rows; ++i) {
            const ble_pt_device_t *dev = ble_scan_visible_at(i);
            if (!dev) continue;
            char label[48];
            snprintf(label, sizeof(label), "%s", dev->name);
            fit_value(label, 360);
            char value[44];
            snprintf(value, sizeof(value), "%d dBm%s%s", dev->rssi, dev->hid ? " · HID" : "",
                     dev->bonded ? " · 已配对" : "");
            int top = l.devices_top - scroll + i * BLE_ROW_H;
            if (top + BLE_ROW_H >= 132 && top < UI_NAV_TOP) {
                settings_card(fb, (EpdRect){36, top, 612, BLE_ROW_H - 8}, 18, UI_GRAY_WHITE, 0x70);
                ui_text(fb, 60, top + 12, 25, label, EPD_DRAW_ALIGN_LEFT, false);
                ui_text(fb, 60, top + 45, 19, value, EPD_DRAW_ALIGN_LEFT, false);
                ui_draw_icon(fb,(624)-8,top + 38,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
            }
        }
        if (!l.device_rows)
            ui_text(fb, 44, l.devices_top - scroll + 18, 23, "附近还没有发现设备",
                    EPD_DRAW_ALIGN_LEFT, false);
        ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, 70}, UI_GRAY_WHITE, fb);
        ui_nav_status(fb);
        back_header(fb, "扫描设备");
        ui_nav_draw(fb, 3);
        return;
    }
    if (s_page == SETTINGS_BLUETOOTH) {
        ui_nav_status(fb);
        const ble_layout_t l = ble_layout();

        // 学习按键时把提示顶到最上面，用户不用滚回去看。
        // While learning, the prompt is pinned to the top so the user need not scroll back.
        if (s_ble_learning || s_ble_feedback[0]) {
            settings_card(fb, (EpdRect){36, 132 - s_ble_scroll, 612, 126}, 22, 0xd0, 0x58);
            ui_text_vc(fb, 342, 195 - s_ble_scroll, 24,
                       !s_ble_learning ? s_ble_feedback :
                       s_ble_learning == 1 ? "请按翻页器上「上一页」要用的键"
                                           : "请按翻页器上「下一页」要用的键",
                       EPD_DRAW_ALIGN_CENTER, false);
            if (!s_ble_learning)
                ui_text_vc(fb, 342, 235 - s_ble_scroll, 19, "轻点此处返回蓝牙开关", EPD_DRAW_ALIGN_CENTER, false);
        } else {
            setting_toggle(fb, l.toggle_top, "启用蓝牙", "开启后可配对蓝牙翻页器；关闭会释放蓝牙内存",
                           app_settings_ble_turner());
        }

        section(fb, l.status_top - 38, "状态");
        char ble_status[80];
        if (!app_settings_ble_turner()) snprintf(ble_status, sizeof(ble_status), "已关闭");
        else if (!ble_pt_running()) {
            read_pico_transfer_status_t network;
            read_pico_transfer_get_status(&network);
            snprintf(ble_status, sizeof(ble_status), network.state != READ_PICO_TRANSFER_STOPPED ? "WiFi 使用中，蓝牙暂停" : "启动中…");
        }
        else if (ble_pt_connected()) snprintf(ble_status, sizeof(ble_status), "已连接 %s", ble_pt_connected_name());
        else if (ble_pt_connecting()) snprintf(ble_status, sizeof(ble_status), "连接中…");
        else snprintf(ble_status, sizeof(ble_status), "未连接");
        ble_row(fb, l.status_top, "当前", ble_status);
        if (l.notice_top >= 0) ui_text(fb, 44, l.notice_top, 21, s_ble_notice, EPD_DRAW_ALIGN_LEFT, false);

        // 已配对：点行连接，右侧按钮删除。/ Bonded peers: the row connects, the button forgets.
        section(fb, l.bonds_title_top, "已配对");
        if (!l.bond_rows)
            ble_row(fb, l.bonds_title_top + 38, "暂无已配对设备", "");
        for (int i = 0; i < l.bond_rows && i < BLE_PT_MAX_BONDS; ++i) {
            const ble_pt_bond_t *bond = ble_pt_bond((uint8_t)i);
            if (!bond) continue;
            char label[40];
            snprintf(label, sizeof(label), "%s", bond->name[0] ? bond->name : bond->addr);
            fit_value(label, 300);
            const bool live = ble_pt_connected() && !strcmp(ble_pt_connected_name(), bond->name);
            ble_row(fb, l.bond_top[i], label, live ? "已连接" : "连接  ›");
            ui_draw_button(fb, ble_button_rect(l.bond_top[i]), "删除", false);
        }

        ble_row(fb, l.scan_entry_top, "添加翻页器", "扫描设备");
        ui_draw_icon(fb,(624)-8,l.scan_entry_top + 38,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);

        section(fb, l.learn_title_top, "按键映射");
        static const char *const learn_names[] = {"上一页", "下一页"};
        static const ble_pt_action_t learn_actions[] = {BLE_PT_ACTION_PREV, BLE_PT_ACTION_NEXT};
        for (int i = 0; i < 2; ++i) {
            // 学到的键直接显示出来；没学过就走内置映射。
            // Show the learned key itself; with none, the built-in map applies.
            char bound_label[40];
            ble_pt_frame_t bound;
            if (ble_pt_binding(learn_actions[i], &bound)) format_bound_key(&bound, bound_label, sizeof(bound_label));
            else snprintf(bound_label, sizeof(bound_label), "内置按键");
            ble_row(fb, l.learn_top[i], learn_names[i], bound_label);
            ui_draw_button(fb, ble_button_rect(l.learn_top[i]), "学习", false);
        }
        ui_text(fb, 48, l.footer_top, 20, "上下、左右及 PageUp / PageDown 均可翻页", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, 48, l.footer_top + 34, 20, "仅支持 BLE 翻页器，按键只在阅读时生效", EPD_DRAW_ALIGN_LEFT, false);
        /* v2：状态栏底仅 70，旧 190 白遮罩盖住贴顶卡片（132 起）导致"顶部显示不全"。 */
        ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, 70}, UI_GRAY_WHITE, fb);
        ui_nav_status(fb);
        back_header(fb, "蓝牙翻页器");
        ui_nav_draw(fb, 3);
        return;
    }
    const int profile_y=SETTINGS_MAIN_TOP-s_main_scroll, profile_h=settings_profile_height();
    if (profile_y+profile_h>SETTINGS_BODY_TOP) {
        settings_card(fb,(EpdRect){36,profile_y,612,profile_h},24,UI_GRAY_WHITE,0x70);
        int avatar_y=profile_y+(profile_h-82)/2;
        bool avatar_ok=s_avatar_ready;
        /* 头像：自定义图片或内嵌狐狸默认头像，同一圆角+抖动管线绘制。 */
        if (avatar_ok) {
            ui_wallpaper_draw_avatar(fb,s_avatar_pixels,(EpdRect){57,avatar_y,82,82},20);
        } else {
            ui_wallpaper_draw_avatar(fb,nothing_default_avatar82(),(EpdRect){57,avatar_y,82,82},20);
        }
        if (app_settings_main_fast_refresh()) ui_image_bw_rect(fb,(EpdRect){57,avatar_y,82,82});
        char name[64];snprintf(name,sizeof(name),"%s",app_settings_device_name());
        int width=588-ui_text_fixed_width_px(ui_text_effective_px(20),"编辑")-188;
        while (name[0] && ui_text_fixed_width_px(ui_text_effective_px(32),name)>width) {
            size_t n=strlen(name)-1;while(n&&((unsigned char)name[n]&0xc0)==0x80)--n;name[n]=0;
        }
        ui_text_vc(fb,164,profile_y+profile_h/2,32,name,EPD_DRAW_ALIGN_LEFT,false);
        ui_text_vc(fb,588,profile_y+profile_h/2,20,"编辑",EPD_DRAW_ALIGN_RIGHT,false);
        ui_draw_icon(fb,625,profile_y+profile_h/2,26,UI_ICON_CHEVRON_RIGHT,UI_GRAY_BLACK);
    }
    /* 2026-10-17：所有选项合并为一个“设置”组，无组间副标题，行行相连。 */
    char wifi[56];snprintf(wifi,sizeof(wifi),"%s",s_main_wifi_saved?s_main_wifi:"未配置");    char font_name[96],contrast[24],idle[24],lock_style[32],version[40];
    snprintf(font_name,sizeof(font_name),"%s · %u%%",system_font_label(app_settings_system_font_path()),app_settings_system_font_size());
    snprintf(contrast,sizeof(contrast),"%u%%",app_settings_system_contrast());
    snprintf(version,sizeof(version),"v%s",esp_app_get_description()->version);
    unsigned minutes=app_settings_auto_lock_minutes();
    if(minutes)snprintf(idle,sizeof(idle),"%u 分钟",minutes);else strcpy(idle,"关闭");
    snprintf(lock_style,sizeof(lock_style),"%s",app_settings_lock_style()==2?"透明书封":app_settings_lock_style()==1?"壁纸":app_settings_lock_style()==3?"计算器锁屏":"时钟模式");
    static const char *const labels[]={"WiFi","蓝牙翻页","日期与时间","系统字体","系统对比度",
        "刷新模式","休眠锁屏","锁屏设置","关于系统"};
    const char *const values[]={wifi,app_settings_ble_turner()?"已开启":"已关闭","设置",
        font_name,contrast,
        app_settings_main_refresh_mode()==APP_MAIN_REFRESH_FAST?"快刷":app_settings_main_refresh_mode()==APP_MAIN_REFRESH_WATER?"水波纹":"普通",
        idle,lock_style,version};
    static const int icons[]={0,1,6,2,7,15,14,5,12};
    setting_group(fb,0,NULL,SETTINGS_WIRELESS_Y,
                  icons,labels,values,SETTINGS_MAIN_ROWS);
    // 主列表也要有一行提示位：确认、修复结果都写在这里，不然点了没有任何回音。
    // The main list needs a notice line too: confirmation and the repair result land here, or a tap
    // would have no answer at all.
    ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, SETTINGS_BODY_TOP},
                  app_settings_main_fast_refresh() ? 0xf0 : 0xe0, fb);
    ui_nav_status(fb);
    /* 2026-10-17：主页面删“设置”标题，仅保留状态栏下一行提示位。 */
    if (s_notice[0]) ui_text_fixed_vc(fb, 36, 88, 18, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_nav_draw(fb, 3);
}

static void settings_prepare_profile(void) {
    const char *path=app_settings_avatar_path();
    if(pico_boot_recovery()||!path[0]){ui_avatar_request("",60);s_avatar_ready=s_avatar_pending=false;s_avatar_path[0]=0;return;}
    if(strcmp(path,s_avatar_path))s_avatar_ready=false;
    snprintf(s_avatar_path,sizeof(s_avatar_path),"%s",path);
    ui_avatar_request(path,82);
    ui_avatar_state_t state=ui_avatar_poll(path,82,s_avatar_pixels);
    s_avatar_ready=state==UI_AVATAR_READY;s_avatar_pending=state==UI_AVATAR_PENDING;
}
static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    pin_setup_close();
    s_notice[0] = 0;
    s_ble_feedback[0] = 0;
    s_page = SETTINGS_MAIN;
    s_style_scroll = s_main_scroll = s_font_page = s_wallpaper_page = 0;
    s_collage_prepare_pending = false;
    s_scroll_drag_consumed = s_scroll_present_pending = false;
    s_wallpaper_selected = -1;
    s_wallpaper_confirm = s_wallpaper_preview_ok = false;
    s_sync_pending = false;
    s_font_sync_confirm = false; s_font_sync_path[0] = 0;
    s_config_confirm = false;
    s_panel_repair_pending = false;
    s_boot_pending = s_local_pending = s_upgrade_restart = false;
    s_upgrade_confirm = false;
    (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved);
    settings_prepare_profile();
}

// 主循环主动消费按键；识别不需要再触摸屏幕。/ Consume key edges on ticks without requiring another touch.
static bool ble_receive_feedback(void) {
    ble_pt_event_t unused;
    while (ble_pt_pop_key(&unused)) {}
    bool changed = false;
    ble_pt_raw_t raw;
    while (ble_pt_pop_raw(&raw)) {
        if (!raw.pressed || !ble_pt_connected()) continue;
        if (s_ble_learning) {
            const ble_pt_action_t action = s_ble_learning == 1 ? BLE_PT_ACTION_PREV : BLE_PT_ACTION_NEXT;
            const esp_err_t error = ble_pt_bind(action, &raw.frame);
            snprintf(s_ble_feedback, sizeof(s_ble_feedback), "%s",
                     error == ESP_OK ? (action == BLE_PT_ACTION_PREV ? "识别成功 · 已绑定上一页" : "识别成功 · 已绑定下一页")
                                     : "识别成功 · 绑定保存失败，请重试");
            s_ble_learning = 0;
            s_ble_scroll = 0;
            changed = true;
            // 同一帧的其余字节不得覆盖这次学习结果。/ Drain remaining bytes without overwriting this learned result.
            while (ble_pt_pop_raw(&raw)) {}
            break;
        }
        if (!s_ble_feedback[0]) {
            snprintf(s_ble_feedback, sizeof(s_ble_feedback), "识别成功 · 已收到翻页器按键");
            s_ble_scroll = 0;
            changed = true;
        }
    }
    return changed;
}

static app_redraw_t profile_editor_paint(app_ctx_t *ctx, bool field);
static app_redraw_t on_tick(app_ctx_t *ctx) {
    // 屏幕修复：先画最终结果，再在 tick 中整屏恢复；成功后不重复推页。
    // Paint the final notice before the tick's full restoration; do not push the page twice on success.
    if (s_panel_repair_pending) {
        s_panel_repair_pending = false;
        if (s_page != SETTINGS_MAIN) return APP_REDRAW_NONE;
        s_scroll_present_pending = false;
        snprintf(s_notice, sizeof(s_notice), "屏幕修复完成；若条纹仍在可再做一次");
        render(ctx, ctx->fb);
        if (display_panel_repair(ctx->hl) != EPD_DRAW_SUCCESS) {
            snprintf(s_notice, sizeof(s_notice), "屏幕输出未完成，请稍后重试");
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_DONE;
    }

    if (s_page==SETTINGS_MAIN && s_avatar_pending && !(ctx->touch && ctx->touch->touched)) {
        ui_avatar_state_t state=ui_avatar_poll(s_avatar_path,82,s_avatar_pixels);
        s_avatar_pending=state==UI_AVATAR_PENDING;
        s_avatar_ready=state==UI_AVATAR_READY;
        if (s_avatar_ready && s_main_scroll < SETTINGS_MAIN_TOP+settings_profile_height()-SETTINGS_BODY_TOP) {
            render(ctx,ctx->fb);
            EpdRect area={57,SETTINGS_MAIN_TOP-s_main_scroll+(settings_profile_height()-82)/2,82,82};
            if (area.y<SETTINGS_BODY_TOP) { area.height-=SETTINGS_BODY_TOP-area.y; area.y=SETTINGS_BODY_TOP; }
            if(area.height>0)guard_draw_result(ctx->hl,update_display_area_diff_with(ctx->hl,
                app_settings_main_refresh_mode()==APP_MAIN_REFRESH_NORMAL?display_main_normal_waveform():display_main_gray_waveform(),MODE_GL16,area));
            return APP_REDRAW_DONE;
        }
    }
    if (s_page == SETTINGS_PIN_ENTRY) return pin_setup_tick(ctx);
    if (s_collage_prepare_pending && s_page == SETTINGS_LOCK_STYLE) {
        s_collage_prepare_pending = false;
        // 先让选中反馈可见，再生成卡上缓存；render 本身只绘图。
        // Show selection feedback first, then prepare the SD cache; render stays paint-only.
        const size_t bytes = UI_LOCK_WIDTH * UI_LOCK_HEIGHT / 2u;
        size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *preview = available > bytes + 1024u * 1024u
            ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (preview) {
            app_lock_font_t saved;
            app_font_begin_lock(&saved);
            (void)book_lock_collage_draw(preview);
            app_font_end_lock(&saved);
            free(preview);
        }
        return APP_REDRAW_NONE;
    }

    if (s_page == SETTINGS_TEXT_EDIT) {
        if (ctx->consumed) return APP_REDRAW_NONE;
        bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
            ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED) return profile_editor_paint(ctx, false);
        return ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED
            ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLUETOOTH && ble_receive_feedback()) return APP_REDRAW_PAGE;
    // 蓝牙启动失败的原因转成提示。放在这里而不是 render()：render 必须是纯绘制，
    // 而 take_* 会清空状态。
    // Fold a Bluetooth start failure into the notice here rather than in render(), which must
    // stay pure, and because take_* clears the state.
    // 蓝牙页要跟着栈的状态重绘：开关拨开后栈是异步起来的，配对表、扫描结果和连接状态
    // 都在页面画完以后才变。不重绘的话用户看到的永远是进页那一刻的快照，只能退出去再进来。
    // The Bluetooth pages redraw with the stack: enabling it starts it asynchronously, and the
    // bond list, scan results and link state all change after the page was painted. Without a
    // poll the user only ever sees the snapshot from when they entered, and has to leave and
    // come back.
    if (s_page == SETTINGS_BLUETOOTH || s_page == SETTINGS_BLE_SCAN) {
        static uint32_t ble_seen;
        const uint32_t sig = (app_settings_ble_turner() ? 1u : 0u) | (ble_pt_running() ? 2u : 0u) |
                             (ble_pt_connected() ? 4u : 0u) | (ble_pt_connecting() ? 8u : 0u) |
                             (ble_pt_scanning() ? 16u : 0u) |
                             ((uint32_t)ble_pt_bond_count() << 8) |
                             ((uint32_t)ble_scan_visible_count() << 16);
        if (sig != ble_seen) {
            ble_seen = sig;
            return APP_REDRAW_PAGE;
        }
    }
    if (s_page == SETTINGS_BLUETOOTH && !s_ble_notice[0]) {
        char why[64];
        if (ble_pt_take_start_failure(why, sizeof(why))) {
            snprintf(s_ble_notice, sizeof(s_ble_notice), "%s", why);
            return APP_REDRAW_PAGE;
        }
    }

    (void)ctx;
    if (s_boot_pending) {
        s_boot_pending = false;
        pmu_selftest_prepare_powerdown();
        uint8_t req[2] = {0, 0};
        esp_err_t error = read_pico_pmu_cmd(PMU_CMD_HOST_REQUEST_RESET, req, sizeof(req));
        if (error != ESP_OK) ESP_LOGW(TAG, "BOOT PMU notice: %s", esp_err_to_name(error));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart(); return APP_REDRAW_NONE;
    }
    if (s_upgrade_restart) { s_upgrade_restart = false; esp_restart(); return APP_REDRAW_NONE; }
    if (s_page == SETTINGS_UPGRADE) {
        if (s_local_pending) {
            s_local_pending = false;
            esp_err_t error = pico_ota_install(PICO_OTA_UPDATE_PATH, s_upgrade_notice, sizeof(s_upgrade_notice));
            s_upgrade_restart = error == ESP_OK;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (!s_sync_pending) return APP_REDRAW_NONE;
    s_sync_pending = false;
    uint32_t utc = 0;
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    esp_err_t err = read_pico_transfer_sync_time(&utc);
    bool network_time_received = err == ESP_OK;
    if (network_time_received) {
        // 临时 WiFi 收尾可能耗时；写入 PMU 时取当前 S3 时间，避免回写旧秒数。
        // Temporary WiFi teardown takes time; seed the PMU from the current S3 clock.
        utc = (uint32_t)time(NULL);
        uint8_t payload[4] = {(uint8_t)utc, (uint8_t)(utc >> 8), (uint8_t)(utc >> 16), (uint8_t)(utc >> 24)};
        err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
        if (err == ESP_OK) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
        if (err == ESP_OK) {
            const pmu_snapshot_t *pmu = read_pico_pmu_get();
            if (!pmu || !pmu->time_synced || pmu->unix_sec < utc || pmu->unix_sec - utc > 10)
                err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(TAG, "time sync network=%d ready=%d utc=%lu result=%s",
             network.mode, network.network_ready, (unsigned long)utc, esp_err_to_name(err));
    const char *notice = err == ESP_OK ? "WiFi 对时完成，内部时钟已接管" :
        network_time_received ? "已获取网络时间，内部时钟写入失败" :
        err == ESP_ERR_NOT_FOUND ? "请先在 WiFi 设置中配置网络" :
        err == ESP_ERR_INVALID_STATE ? "WiFi 尚未连接，请稍后再试" :
        err == ESP_ERR_TIMEOUT ? "网络对时超时，请检查网络后重试" :
        "网络对时失败，请检查网络后重试";
    snprintf(s_notice, sizeof(s_notice), "%s", notice);
    return APP_REDRAW_PAGE;
}

static EpdRect s_input_area;
static bool s_input_layout, s_input_settle;
static app_redraw_t profile_editor_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, 694, (EpdRect){36, 168, 612, 82}, 28, false, NULL, field);
    s_input_area = update.area; s_input_layout = update.layout; s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}
static app_redraw_t profile_editor_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, 694, ctx->now_ms) ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 160) {
        if (x < 160) { ui_keyboard_end(); s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (x > 510) goto save_text;
    }
    if (ui_text_input_tap(&s_editor_input, (EpdRect){36, 168, 612, 82}, 28,
                          false, NULL, x, y)) return profile_editor_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, 694, esp_timer_get_time() / 1000);
    if (result == UI_KEYBOARD_DONE) goto save_text;
    return result == UI_KEYBOARD_CHANGED ? profile_editor_paint(ctx, false) : APP_REDRAW_NONE;
save_text:
    if (ui_keyboard_pending()) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "请先选择候选字");
        return APP_REDRAW_PAGE;
    }
    if (s_editor[0]) app_settings_set_device_name(s_editor);
    else { snprintf(s_editor_notice, sizeof(s_editor_notice), "设备名称不能为空"); return APP_REDRAW_PAGE; }
    const char *saved = app_settings_device_name();
    if (strcmp(saved, s_editor)) {
        snprintf(s_editor_notice, sizeof(s_editor_notice), "保存失败，请检查设置存储空间");
        return APP_REDRAW_PAGE;
    }
    ui_keyboard_end();
    s_page = SETTINGS_PROFILE;
    return APP_REDRAW_PAGE;
}

// 越过阈值按段推进，一次手势只更新一次，松手不再播放惯性。
// Advance one discrete step after crossing the threshold, with one update per gesture and no release inertia.
static app_redraw_t scroll_gesture(const ui_gesture_event_t *ev, int *offset, int limit, bool *handled) {
    *handled = ev->type != UI_GESTURE_TAP || s_scroll_drag_consumed;
    if (ev->type == UI_GESTURE_PRESS) {
        s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP || ev->type == UI_GESTURE_CANCEL) {
        s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_MOVE && ev->type != UI_GESTURE_SWIPE_U && ev->type != UI_GESTURE_SWIPE_D)
        return APP_REDRAW_NONE;
    if (ev->y0 < (s_page == SETTINGS_MAIN ? SETTINGS_BODY_TOP : 190) || ev->y0 >= UI_NAV_TOP)
        return APP_REDRAW_NONE;
    if (s_scroll_drag_consumed) {
        if (ev->type != UI_GESTURE_MOVE) s_scroll_drag_consumed = false;
        return APP_REDRAW_NONE;
    }
    int dy = (int)ev->y - ev->y0, dx = (int)ev->x - ev->x0;
    int distance = abs(dy);
    if (ev->type == UI_GESTURE_MOVE && (distance < 108 || distance <= abs(dx))) return APP_REDRAW_NONE;
    int step = distance < 204 ? 204 : distance > 450 ? 450 : distance;
    bool up = ev->type == UI_GESTURE_SWIPE_U || (ev->type == UI_GESTURE_MOVE && dy < 0);
    int next = *offset + (up ? step : -step);
    if (limit < 0) limit = 0;
    if (next < 0) next = 0;
    if (next > limit) next = limit;
    if (ev->type == UI_GESTURE_MOVE) s_scroll_drag_consumed = true;
    if (next == *offset) return APP_REDRAW_NONE;
    *offset = next;
    s_scroll_present_pending = true;
    return APP_REDRAW_AREA;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_page==SETTINGS_SYSTEM_FONT && s_font_sync_confirm) {
        if (ev->type!=UI_GESTURE_TAP) return APP_REDRAW_NONE;
        for (int i=0;i<2;++i) {
            if (!ui_rect_hit(system_font_sync_button(i),ev->x0,ev->y0)) continue;
            if (i) {
                app_settings_set_font_path(s_font_sync_path);
                if (strcmp(app_settings_font_path(),s_font_sync_path)) {
                    snprintf(s_notice,sizeof(s_notice),"保存失败，请重试");return APP_REDRAW_PAGE;
                }
            }
            s_notice[0]=0;s_font_sync_confirm=false;return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_PIN_ENTRY) {
        EpdRect dirty; ui_pin_result_t result = ui_pinpad_handle(&s_pinpad, ev, &dirty);
        if (result == UI_PIN_CANCEL) {
            /* v2：计算器锁屏引导/样式切换取消→回锁屏设置页；密码页流程→回密码页。 */
            bool from_style = s_pin_for_lock_style || s_pin_style_after != 0xFF;
            pin_setup_close(); s_page = from_style ? SETTINGS_LOCK_STYLE : SETTINGS_LOCK_PIN;
            return APP_REDRAW_PAGE;
        }
        if (result == UI_PIN_COMPLETE) s_pin_pending = true;
        if (result == UI_PIN_NONE) return APP_REDRAW_NONE;
        ui_pinpad_paint_input(ctx->fb, &s_pinpad);
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, dirty));
        return APP_REDRAW_DONE;
    }
    if (s_page == SETTINGS_LOCK_PIN && ev->type == UI_GESTURE_TAP && lock_pin_available() &&
        (ui_rect_hit((EpdRect){36,272,612,70},ev->x0,ev->y0) ||
         (lock_pin_enabled() && ui_rect_hit((EpdRect){36,378,612,70},ev->x0,ev->y0)))) {
        if (ctx->now_ms < s_pin_retry_at) { snprintf(s_notice, sizeof(s_notice), "请等待后重试"); return APP_REDRAW_PAGE; }
        pin_setup_open(ctx, ev->y0 >= 378); return APP_REDRAW_PAGE;
    }

    if (s_page == SETTINGS_BOOT) {
        if (ev->type != UI_GESTURE_TAP || s_boot_pending) return APP_REDRAW_NONE;
        if (ev->y0 < 132) { s_page = SETTINGS_MAIN; (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit((EpdRect){60, 316, 564, 78}, ev->x0, ev->y0)) { s_boot_pending = true; return APP_REDRAW_PAGE; }
        int tab = ui_nav_hit(ev->x0, ev->y0);
        if (tab >= 0) ui_nav_request(ctx, tab);
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_UPGRADE) {
        if (ev->type != UI_GESTURE_TAP || s_local_pending || s_upgrade_restart) return APP_REDRAW_NONE;
        int y = ev->y0;
        if (y < 132) {
            if (s_upgrade_confirm) s_upgrade_confirm = false;
            else { s_page = SETTINGS_MAIN; (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); }
            return APP_REDRAW_PAGE;
        }
        int tab = ui_nav_hit(ev->x0, y);
        if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
        if (s_upgrade_confirm) {
            bool accept = ui_rect_hit(upgrade_confirm_button(true), ev->x0, y);
            if (accept || ui_rect_hit(upgrade_confirm_button(false), ev->x0, y)) {
                s_upgrade_confirm = false;
                if (accept) s_local_pending = true;
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (ev->x0 < 60 || ev->x0 >= 624) return APP_REDRAW_NONE;
        if (y >= 468 && y < 530) {
            s_upgrade_notice[0] = 0;
            (void)pico_ota_inspect(PICO_OTA_UPDATE_PATH, &s_local_update);
            if (s_local_update.ready) s_upgrade_confirm = true;
            else snprintf(s_upgrade_notice, sizeof(s_upgrade_notice), "%s", s_local_update.message);
            return APP_REDRAW_PAGE;
        }
        /* 归并入口：保存与恢复 / BOOT 刷机（2026-10-17）。 */
        if (y >= 928 && y < 1024) { s_page = SETTINGS_CONFIG; s_config_confirm = false; s_notice[0] = 0; return APP_REDRAW_PAGE; }
        if (y >= 1024) { s_boot_pending = false; s_page = SETTINGS_BOOT; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_TEXT_EDIT) return profile_editor_gesture(ctx, ev);
    if (s_page == SETTINGS_MAIN || s_page == SETTINGS_BLUETOOTH || s_page == SETTINGS_BLE_SCAN) {
        int limit = SETTINGS_SCROLL_MAX;
        int *offset = &s_main_scroll;
        if (s_page != SETTINGS_MAIN) {
            int bottom = s_page == SETTINGS_BLUETOOTH ? ble_layout().content_bottom + s_ble_scroll : ble_scan_layout().content_bottom;
            limit = bottom - UI_NAV_REFRESH_END + 12;
            offset = &s_ble_scroll;
        }
        bool handled;
        app_redraw_t redraw = scroll_gesture(ev, offset, limit, &handled);
        if (handled) return redraw;
    }

    if (s_page == SETTINGS_AVATAR && (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int pages = (s_wallpaper_count + 7) / 8;
        int old = s_wallpaper_page;
        if (ev->type == UI_GESTURE_SWIPE_U && s_wallpaper_page + 1 < pages) ++s_wallpaper_page;
        if (ev->type == UI_GESTURE_SWIPE_D && s_wallpaper_page > 0) --s_wallpaper_page;
        if (s_wallpaper_page == old) return APP_REDRAW_NONE;
        s_scroll_present_pending = true;
        return APP_REDRAW_AREA;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW && !s_wallpaper_confirm &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int next = s_wallpaper_selected + (ev->type == UI_GESTURE_SWIPE_U ? 1 : -1);
        if (next < -1 || next >= s_wallpaper_count) return APP_REDRAW_NONE;
        s_wallpaper_selected = next;
        s_wallpaper_page = next < 0 ? 0 : next / 7;
        return APP_REDRAW_PAGE;
    }
    if ((s_page == SETTINGS_SYSTEM_FONT || s_page == SETTINGS_WALLPAPER) &&
        (ev->type == UI_GESTURE_SWIPE_U || ev->type == UI_GESTURE_SWIPE_D)) {
        int *page = s_page == SETTINGS_SYSTEM_FONT ? &s_font_page : &s_wallpaper_page;
        int count = s_page == SETTINGS_SYSTEM_FONT ? ttf_font_count() + 1 : s_wallpaper_count;
        int slots=s_page==SETTINGS_SYSTEM_FONT?system_font_layout().slots:7;
        int pages = count > 0 ? (count + slots - 1) / slots : 1;
        int old = *page;
        if (ev->type == UI_GESTURE_SWIPE_U && *page + 1 < pages) ++*page;
        if (ev->type == UI_GESTURE_SWIPE_D && *page > 0) --*page;
        if (*page == old) return APP_REDRAW_NONE;
        s_scroll_present_pending = true;
        return APP_REDRAW_AREA;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_WALLPAPER_PREVIEW && s_wallpaper_confirm) {
        if (ev->y0 >= 643 && ev->y0 < 711 && ev->x0 >= 358 && ev->x0 < 590) {
            /* v2：-1 = 内置默认图，把路径存空串即恢复默认。 */
            const char *path = s_wallpaper_selected >= 0 ? s_wallpapers[s_wallpaper_selected].path : "";
            if (s_pick_cover) {
                app_settings_set_coverart_path(path);
                if (!strcmp(app_settings_coverart_path(), path)) {
                    /* v2（用户 2026-10-19）：样式切换经闸（绑定密码先验证关闭）；
                     * 闸开启 PIN 验证时停在密码页，完成后由 tick 切回锁屏设置页。 */
                    s_pick_cover = false;
                    (void)apply_lock_style_gated(ctx, 2);
                    if (s_pin_style_after == 0xFF) s_page = SETTINGS_LOCK_STYLE;
                    else s_page = SETTINGS_PIN_ENTRY;
                }
            } else {
                app_settings_set_wallpaper_path(path);
                if (!strcmp(app_settings_wallpaper_path(), path)) {
                    (void)apply_lock_style_gated(ctx, 1);
                    if (s_pin_style_after == 0xFF) s_page = SETTINGS_LOCK_STYLE;
                    else s_page = SETTINGS_PIN_ENTRY;
                }
            }
        }
        s_wallpaper_confirm = false;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_TIME_EDIT) {
        if (ev->y0 < 132) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        if (ev->y0 >= 254 && ev->y0 < 497) {
            int field = (ev->x0 - 36) / 122;
            if (ev->x0 >= 36 && field >= 0 && field < 5 && (ev->x0 - 36) % 122 < 106) {
                if (ev->y0 < 316) time_adjust(field, 1);
                else if (ev->y0 >= 435) time_adjust(field, -1);
                else return APP_REDRAW_NONE;
                return APP_REDRAW_PAGE;
            }
        }
        if (ev->y0 >= 640 && ev->y0 < 718) { (void)time_save(); return APP_REDRAW_PAGE; }
        if (ev->y0 >= 748 && ev->y0 < 826) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    int y = ev->y0;
    if (s_page == SETTINGS_PROFILE) {
        if (y < 132) { s_page = SETTINGS_MAIN; (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        if (y >= 132 && y < 216) { profile_editor_open(); return APP_REDRAW_PAGE; }
        if (y >= 232 && y < 316) { wallpaper_scan(); s_page = SETTINGS_AVATAR; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_AVATAR) {
        if (y < 132) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 132 && y < 216) { app_settings_set_avatar_path(""); s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
        if (y >= 250 && y < 1074) {
            int i = (y - 250) / 103, index = s_wallpaper_page * 8 + i;
            if (index >= 0 && index < s_wallpaper_count && (y - 250) % 103 < 88) {
                app_settings_set_avatar_path(s_wallpapers[index].path);
                s_page = SETTINGS_PROFILE;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        if (y < 132) { s_page = SETTINGS_WALLPAPER; return APP_REDRAW_PAGE; }
        if (s_wallpaper_preview_ok && y >= 194 && y < 1032) {
            s_wallpaper_confirm = true;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER && y < 132) {
        s_page = SETTINGS_LOCK_STYLE;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_SHELF_STYLE) {
        if (y < 132) { s_page = SETTINGS_MAIN; (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
        for (int i = 0; i < 3; ++i) {
            if (y >= 263 + i * 253 - s_style_scroll && y < 493 + i * 253 - s_style_scroll && y < 1096) {
                app_settings_set_shelf_style((uint8_t)(i == 2 ? 5 : i + 1));
                s_page = SETTINGS_MAIN;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLE_SCAN && y < 132) {
        ble_pt_scan_stop(); s_page = SETTINGS_BLUETOOTH; s_ble_scroll = 0; return APP_REDRAW_PAGE;
    }
    if (s_page != SETTINGS_MAIN && y < 132) { s_page = SETTINGS_MAIN; (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_LOCK_PIN) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_AUTO_LOCK) {
        const uint8_t values[] = {1, 5, 10, 0};
        for (int i = 0; i < 4; ++i) if (ui_rect_hit((EpdRect){36, 132 + i * 112, 612, 92}, ev->x0, y)) {
            app_settings_set_auto_lock_minutes(values[i]);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit((EpdRect){36, 930, 612, 126}, ev->x0, y)) {
            app_settings_set_staged_shutdown(!app_settings_staged_shutdown());
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_GOAL) {
        unsigned goal = app_settings_home_goal_minutes();
        if (ui_rect_hit(goal_adjust_button(0), ev->x0, y)) {
            if (goal > 30) { (void)app_settings_set_home_goal_minutes(goal - 30); return APP_REDRAW_PAGE; }
        } else if (ui_rect_hit(goal_adjust_button(1), ev->x0, y)) {
            if (goal < 180) { (void)app_settings_set_home_goal_minutes(goal + 30); return APP_REDRAW_PAGE; }
        }
        return APP_REDRAW_NONE;
    }

    if (s_page == SETTINGS_WIFI) {
        if (y >= 243 && y < 313) {
            extern const app_desc_t app_transfer;
            app_transfer_request_wifi_setup();
            ctx->request_app = &app_transfer;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_FONT) {
        system_font_layout_t l=system_font_layout();
        if (ui_rect_hit(l.size,ev->x0,y)) {
            unsigned prior=app_settings_system_font_size();
            unsigned next=prior>=200?100:prior+10;
            app_settings_set_system_font_size(next);
            if (app_settings_system_font_size()!=next) {
                snprintf(s_notice,sizeof(s_notice),"保存失败，请重试");return APP_REDRAW_PAGE;
            }
            s_font_page=0;
            if (s_main_scroll>SETTINGS_SCROLL_MAX) s_main_scroll=SETTINGS_SCROLL_MAX;
            ui_text_set_system_scale(true);s_notice[0]=0;return APP_REDRAW_PAGE;
        }
        for (int slot=0;slot<l.slots;++slot) {
            if (!ui_rect_hit(system_font_item_rect(slot),ev->x0,y)) continue;
            int index=s_font_page*l.slots+slot;
            if (index>ttf_font_count()) return APP_REDRAW_NONE;
            const ttf_font_item_t *item=index?ttf_font_item(index-1):NULL;
            if (index && !item) return APP_REDRAW_NONE;
            snprintf(s_font_sync_path,sizeof(s_font_sync_path),"%s",item?item->path:"");
            app_settings_set_system_font_path(s_font_sync_path);
            if (strcmp(app_settings_system_font_path(),s_font_sync_path)) {
                snprintf(s_notice,sizeof(s_notice),"保存失败，请重试");return APP_REDRAW_PAGE;
            }
            app_font_activate_system();s_notice[0]=0;
            s_font_sync_confirm=strcmp(app_settings_font_path(),s_font_sync_path)!=0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_MAIN_REFRESH) {
        for (int i = 0; i < 3; ++i) {
            if (!ui_rect_hit((EpdRect){36, 132 + i * 134, 612, 112}, ev->x0, y)) continue;
            app_settings_set_main_refresh_mode((app_main_refresh_mode_t)i);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit((EpdRect){36, 810, 612, 126}, ev->x0, y)) {
            app_settings_set_home_full_refresh(!app_settings_home_full_refresh());
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit((EpdRect){36, 972, 612, 78}, ev->x0, y)) {
            s_panel_repair_pending = true;
            snprintf(s_notice, sizeof(s_notice), "屏幕修复中…");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_SIZE) {
        for (int index = 0; index < 11; ++index) {
            EpdRect box = {36 + (index % 2) * 316, 132 + (index / 2) * 122, 296, 102};
            if (!ui_rect_hit(box, ev->x0, y)) continue;
            app_settings_set_system_font_size((uint8_t)(100 + index * 10));
            ui_text_set_system_scale(true);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_SYSTEM_CONTRAST) {
        if (y < 132 || y >= 1064) return APP_REDRAW_NONE;
        int index = (y - 132) / 116;
        app_settings_set_system_contrast((uint8_t)(100 + index * 10));
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_LOCK_STYLE) {
        /* v5 命中：行1=壁纸锁屏(132..244)/行2=透明书封(266..378)/行3=时钟模式(400..512)/
         * 行4=计算器锁屏(534..646)。时钟卡的「时间以外内容」开关（track 551..620 / y 446..485 外扩）
         * 优先命中：点它只切换日期/黄历/待办，不影响选样式。页底 1010..1088=锁屏密码入口。 */
        if (ev->x0 >= 540 && ev->x0 < 630 && y >= 438 && y < 492) {
            app_settings_set_clock_almanac(!app_settings_clock_almanac());
            return APP_REDRAW_PAGE;
        }
        if (y >= 132 && y < 244) {
            wallpaper_scan();
            s_pick_cover = false;
            s_page = SETTINGS_WALLPAPER;
            return APP_REDRAW_PAGE;
        }
        if (y >= 266 && y < 378) {
            coverscan();
            s_pick_cover = true;      /* 透明书封选择（仅 PNG，选完设样式2） */
            s_page = SETTINGS_WALLPAPER;
            return APP_REDRAW_PAGE;
        }
        if (y >= 400 && y < 512) {
            /* v2（用户 2026-10-19）：经样式切换闸——计算器锁屏绑定密码需先验证关闭。 */
            return apply_lock_style_gated(ctx, 0);
        }
        if (y >= 534 && y < 646) {
            /* 计算器锁屏：无密码先走 PIN 设置（设完自动切样式3）；已有密码直接启用。 */
            if (!lock_pin_available()) {
                snprintf(s_notice, sizeof(s_notice), "凭据读取失败，请重启后重试");
                return APP_REDRAW_PAGE;
            }
if (!lock_pin_enabled()) {
                s_pin_for_lock_style = true;
                pin_setup_open(ctx, false);
                return APP_REDRAW_PAGE;
            }
            app_settings_set_lock_style(3);
            return APP_REDRAW_PAGE;
        }
        if (y >= 1010 && y < 1088) {
            /* 锁屏密码入口（页最底，用户 2026-10-19）：RC95 原有锁屏密码页。
             * 除计算器锁屏外保持目前锁屏密码使用逻辑。 */
            s_page = SETTINGS_LOCK_PIN;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_WALLPAPER) {
        if (y >= 132 && y < 224) {              /* 默认图片行 */
            s_wallpaper_selected = -1;
            s_wallpaper_confirm = false;
            s_page = SETTINGS_WALLPAPER_PREVIEW;
            return APP_REDRAW_PAGE;
        }
        if (y >= 224 && y < 868) {
            int index = s_wallpaper_page * 7 + (y - 224) / 92;
            if (index >= s_wallpaper_count) return APP_REDRAW_NONE;
            s_wallpaper_selected = index;
            s_wallpaper_confirm = false;
            s_page = SETTINGS_WALLPAPER_PREVIEW;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_TIME) {
        if (y >= 243 && y < 313) {
            s_notice[0] = 0; s_sync_pending = true;
            snprintf(s_notice, sizeof(s_notice), "正在获取网络时间…");
            return APP_REDRAW_PAGE;
        }
        if (y >= 354 && y < 424) { time_open(); return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_POWER_SLEEP) {
        if (y >= 132 && y < 264) {
            app_settings_set_staged_shutdown(false);
            return APP_REDRAW_PAGE;
        }
        if (y >= 286 && y < 418) {
            app_settings_set_staged_shutdown(true);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_CONFIG) {
        if (y >= 360 && y < 443) {
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_save();
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? "已保存到 TF 卡根目录" :
                     err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试" :
                     "保存失败，请检查 TF 卡剩余空间");
            return APP_REDRAW_PAGE;
        }
        if (y >= 480 && y < 563) {
            if (!s_config_confirm) {
                s_config_confirm = true;
                snprintf(s_notice, sizeof(s_notice), "恢复将覆盖当前设置，请再点按一次确认");
                return APP_REDRAW_PAGE;
            }
            s_config_confirm = false;
            esp_err_t err = app_settings_backup_restore();
            if (err == ESP_OK) {
                ttf_font_scan();
                app_font_activate_system();
                ui_text_set_system_scale(true);
                // 阅读记录恢复后令首页和书架重新取进度、时长与排序。
                // Rebuild home and shelf caches after restoring reading records.
                book_store_notify_changed();
            }
            char saved_ssid[33] = {0};
            bool wifi_saved = false;
            if (err == ESP_OK)
                (void)read_pico_transfer_get_saved_wifi(saved_ssid, &wifi_saved);
            snprintf(s_notice, sizeof(s_notice), "%s",
                     err == ESP_OK ? (wifi_saved ? "配置与阅读记录已恢复；WiFi 请重新连接" :
                                                   "配置与阅读记录已恢复") :
                     err == ESP_ERR_INVALID_STATE ? "未识别到 TF 卡，请插卡后重试" :
                     err == ESP_ERR_NOT_FOUND ? "未找到配置备份文件" :
                     err == ESP_ERR_INVALID_RESPONSE ? "配置文件损坏或版本不兼容" :
                     "恢复未完成，请重试");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_page == SETTINGS_BLUETOOTH) {
        const ble_layout_t l = ble_layout();
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        // 版式给的是屏幕坐标（已减去滚动量），所以这里直接用 y 比，不再加 scroll。
        // The layout yields screen coordinates (scroll already subtracted), so compare y directly.
        const int ty = y;

        // 学习时轻点取消；识别结果轻点后返回开关。/ Tap to cancel learning or dismiss the received-key result.
        if (s_ble_learning) {
            s_ble_learning = 0;
            snprintf(s_ble_notice, sizeof(s_ble_notice), "已取消学习");
            return APP_REDRAW_PAGE;
        }
        if (s_ble_feedback[0] && ty >= 132 - s_ble_scroll && ty < 258 - s_ble_scroll) {
            s_ble_feedback[0] = 0;
            return APP_REDRAW_PAGE;
        }
        if (ty < 132 || ty >= UI_NAV_TOP) return APP_REDRAW_NONE;

        if (l.toggle_top >= 0 && ty >= l.toggle_top && ty < l.toggle_top + 126) {
            app_settings_set_ble_turner(!app_settings_ble_turner());
            // 明确关掉时把失败计数和原因一起清掉：否则自锁之后用户没有重试的途径。
            // Switching it off explicitly clears the failure count and reason; without this a
            // self-locked stack leaves the user no way to try again.
            if (!app_settings_ble_turner()) ble_pt_reset_failure();
            snprintf(s_ble_notice, sizeof(s_ble_notice), "%s",
                     app_settings_ble_turner() ? "已开启，正在等待设备" : "已关闭，蓝牙内存已释放");
            return APP_REDRAW_PAGE;
        }
        if (ty >= l.scan_entry_top && ty < l.scan_entry_top + BLE_ROW_H) {
            if (!app_settings_ble_turner() || !ble_pt_running()) {
                snprintf(s_ble_notice, sizeof(s_ble_notice), "请先开启蓝牙翻页器"); return APP_REDRAW_PAGE;
            }
            // 进扫描页就开持续扫描，离页时停掉。/ Scan continuously while the page is open.
            ble_pt_scan_start(BLE_PT_SCAN_FOREVER);
            s_ble_scroll = 0;
            s_ble_notice[0] = 0;
            s_page = SETTINGS_BLE_SCAN;
            return APP_REDRAW_PAGE;
        }
        for (int i = 0; i < 2; ++i) {
            const EpdRect btn = ble_button_rect(l.learn_top[i]);
            // 命中判定必须同时比 x：按钮的纵向范围几乎覆盖整行，只比 y 会让整行都算按钮。
            // The hit test needs the horizontal test too: the button's vertical span covers almost
            // the whole row, so comparing y alone makes the entire row count as the button.
            if (ev->x0 >= btn.x && ev->x0 < btn.x + btn.width && ty >= btn.y && ty < btn.y + btn.height) {
                if (!ble_pt_connected()) {
                    snprintf(s_ble_notice, sizeof(s_ble_notice), "请先连接翻页器再学习按键");
                    return APP_REDRAW_PAGE;
                }
                ble_pt_raw_t stale;
                while (ble_pt_pop_raw(&stale)) {}
                // 把此刻那一帧当基准：点「学习」时用户还没按键，外设发的就是静止帧。有些遥控器
                // 的静止帧带着非零常量，不这样标一下，那串常量会被当成一次按下学进去。
                // Pin the frame present now as the baseline: the user has not pressed yet, so what the
                // remote sends is its resting frame. Some remotes keep non-zero constants there, and
                // without this the constants get learned as if they were a press.
                ble_pt_learn_begin();
                s_ble_feedback[0] = s_ble_notice[0] = 0;
                s_ble_learning = i + 1;
                s_ble_scroll = 0;
                return APP_REDRAW_PAGE;
            }
        }
        // 已配对行：右侧按钮忘记，行本身连接。/ Bonded rows: the button forgets, the row connects.
        for (int i = 0; i < l.bond_rows && i < BLE_PT_MAX_BONDS; ++i) {
            const ble_pt_bond_t *bond = ble_pt_bond((uint8_t)i);
            if (!bond) continue;
            const EpdRect btn = ble_button_rect(l.bond_top[i]);
            if (ev->x0 >= btn.x && ev->x0 < btn.x + btn.width && ty >= btn.y && ty < btn.y + btn.height) {
                if (ble_pt_connected()) ble_pt_disconnect();
                ble_pt_forget(bond->addr);
                snprintf(s_ble_notice, sizeof(s_ble_notice), "已删除该配对");
                return APP_REDRAW_PAGE;
            }
            if (ty >= l.bond_top[i] && ty < l.bond_top[i] + BLE_ROW_H) {
                if (ble_pt_connected() || ble_pt_connecting()) ble_pt_disconnect();
                snprintf(s_ble_notice, sizeof(s_ble_notice),
                         ble_pt_connect(bond->addr) == ESP_OK ? "正在连接…" : "连接失败，请重试");
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
        if (s_page == SETTINGS_BLE_SCAN) {
        const ble_scan_layout_t l = ble_scan_layout();
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (y < 132 || y >= UI_NAV_TOP) return APP_REDRAW_NONE;
        const int ty = y + s_ble_scroll;
        // 选一台就连，连上后退回二级页。扫描在离页时停。
        // Picking one connects, then returns to level two; scanning stops on the way out.
        for (int i = 0; i < l.device_rows; ++i) {
            const ble_pt_device_t *dev = ble_scan_visible_at(i);
            if (!dev) continue;
            const int top = l.devices_top + i * BLE_ROW_H;
            if (ty < top || ty >= top + BLE_ROW_H) continue;
            ble_pt_scan_stop();
            if (ble_pt_connected() || ble_pt_connecting()) ble_pt_disconnect();
            snprintf(s_ble_notice, sizeof(s_ble_notice),
                     ble_pt_connect(dev->addr) == ESP_OK ? "正在连接…" : "连接失败，请重试");
            s_page = SETTINGS_BLUETOOTH;
            s_ble_scroll = 0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (y<SETTINGS_BODY_TOP || y>=UI_NAV_TOP || ev->x0<36 || ev->x0>=648) return APP_REDRAW_NONE;
    y+=s_main_scroll;
    if (y>=SETTINGS_MAIN_TOP && y<SETTINGS_MAIN_TOP+settings_profile_height()) { s_page=SETTINGS_PROFILE;return APP_REDRAW_PAGE; }
    int row;
    if (y>=SETTINGS_WIRELESS_Y && y<SETTINGS_WIRELESS_Y+SETTINGS_MAIN_ROWS*SETTINGS_ROW_H) {
        row=(y-SETTINGS_WIRELESS_Y)/SETTINGS_ROW_H;
        switch(row) {
            case 0: {
                /* WiFi：拉起传输应用的 WiFi 配置。 */
                extern const app_desc_t app_transfer;app_transfer_request_wifi_setup();ctx->request_app=&app_transfer;return APP_REDRAW_NONE;
            }
            case 1:
                s_page=SETTINGS_BLUETOOTH;s_ble_scroll=s_ble_learning=0;s_ble_notice[0]=s_ble_feedback[0]=0;
                ble_pt_raw_t unused;while(ble_pt_pop_raw(&unused)){}break;
            case 2: s_page=SETTINGS_TIME;break;
            case 3: ttf_font_scan();s_font_page=0;s_notice[0]=0;s_page=SETTINGS_SYSTEM_FONT;break;
            case 4: s_page=SETTINGS_SYSTEM_CONTRAST;break;
            case 5: s_page=SETTINGS_MAIN_REFRESH;break;
            case 6: s_page=SETTINGS_AUTO_LOCK;break;
            case 7: s_page=SETTINGS_LOCK_STYLE;break;
            default: upgrade_open();break;
        }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if(s_font_sync_confirm) {
        s_font_sync_confirm=false;s_notice[0]=0;
        if(key!=1)return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_PIN_ENTRY) {
        bool from_style = s_pin_for_lock_style || s_pin_style_after != 0xFF;
        pin_setup_close(); s_page = from_style ? SETTINGS_LOCK_STYLE : SETTINGS_LOCK_PIN; return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_UPGRADE) {
        if (s_local_pending || s_upgrade_restart) return APP_REDRAW_NONE;
        s_upgrade_confirm = false;
    }
    if (s_page == SETTINGS_BLE_SCAN) {
        ble_pt_scan_stop();
        if (key != 1) { s_page = SETTINGS_BLUETOOTH; s_ble_scroll = 0; return APP_REDRAW_PAGE; }
    }
    /* 2026-10-17 统一三键：中键短按=整屏强刷；左键无动作；右键=返回上一级（子页→主页→首页）。 */
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    if (key == UI_KEY_1) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_WALLPAPER_PREVIEW) {
        if (s_wallpaper_confirm) s_wallpaper_confirm = false;
        else s_page = SETTINGS_WALLPAPER;
        return APP_REDRAW_PAGE;
    }
    if (s_page == SETTINGS_WALLPAPER) { s_page = SETTINGS_LOCK_STYLE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_AVATAR) { s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TEXT_EDIT) { ui_keyboard_end(); s_page = SETTINGS_PROFILE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_LOCK_PIN) { s_page = SETTINGS_LOCK_STYLE; return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_PROFILE) { s_page = SETTINGS_MAIN; (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    if (s_page == SETTINGS_TIME_EDIT) { s_page = SETTINGS_TIME; return APP_REDRAW_PAGE; }
    if (s_page != SETTINGS_MAIN) { s_page = SETTINGS_MAIN; (void)read_pico_transfer_get_saved_wifi(s_main_wifi,&s_main_wifi_saved); settings_prepare_profile(); return APP_REDRAW_PAGE; }
    ui_nav_request(ctx, 0);
    return APP_REDRAW_NONE;
}
/* 2026-10-17 统一三键：中键长按=退出设置回首页。 */
static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    if (key != UI_KEY_2) return APP_REDRAW_NONE;
    if (s_page == SETTINGS_PIN_ENTRY) {
        bool from_style = s_pin_for_lock_style || s_pin_style_after != 0xFF;
        pin_setup_close(); s_page = from_style ? SETTINGS_LOCK_STYLE : SETTINGS_LOCK_PIN;
    }
    if (s_page == SETTINGS_BLE_SCAN) { ble_pt_scan_stop(); s_page = SETTINGS_BLUETOOTH; s_ble_scroll = 0; }
    ui_nav_request(ctx, 0);
    return APP_REDRAW_NONE;
}
static void settings_exit(app_ctx_t *ctx) {
    s_font_sync_confirm=false;s_font_sync_path[0]=0;
    pin_setup_close();
    s_collage_prepare_pending = false;
    (void)ctx; ui_keyboard_end(); ble_pt_scan_stop(); s_ble_learning = 0;
}
static void on_before_lock(app_ctx_t *ctx) {
    s_font_sync_confirm=false;s_font_sync_path[0]=0;
    if (s_page == SETTINGS_PIN_ENTRY) {
        bool from_style = s_pin_for_lock_style || s_pin_style_after != 0xFF;
        pin_setup_close(); s_page = from_style ? SETTINGS_LOCK_STYLE : SETTINGS_LOCK_PIN;
    }
    (void)ctx; ble_pt_scan_stop(); s_ble_learning = 0;
    (void)ble_pt_stop(2000);
}
static bool no_menu_handle(app_ctx_t *ctx) { (void)ctx; return false; }
static void settings_media_lost(app_ctx_t *ctx) {
    (void)ctx; s_avatar_ready=s_avatar_pending=false; s_avatar_path[0]=0;
}
static void settings_media_ready(app_ctx_t *ctx) { (void)ctx; settings_prepare_profile(); }
static bool main_page_visible(app_ctx_t *ctx) { (void)ctx; return s_page == SETTINGS_MAIN; }
static bool settings_present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (s_page == SETTINGS_PIN_ENTRY && (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL)) {
        display_main_transition_cancel(); render(ctx, ctx->fb);
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, ui_pinpad_full()));
        return true;
    }
    if (redraw == APP_REDRAW_AREA && s_page == SETTINGS_TEXT_EDIT) {
        if (s_input_settle) {
            guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_input_area));
            s_input_settle = false;
            return true;
        }
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
            s_input_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
            s_input_layout ? MODE_GL16 : MODE_DU, s_input_area));
        return true;
    }
    if (redraw == APP_REDRAW_AREA && s_scroll_present_pending &&
        (s_page == SETTINGS_MAIN || s_page == SETTINGS_BLUETOOTH || s_page == SETTINGS_BLE_SCAN ||
         s_page == SETTINGS_SHELF_STYLE || s_page == SETTINGS_SYSTEM_FONT ||
         s_page == SETTINGS_WALLPAPER || s_page == SETTINGS_AVATAR)) {
        s_scroll_present_pending = false;
        render(ctx, ctx->fb);
        int top = s_page == SETTINGS_MAIN ? SETTINGS_BODY_TOP : s_page == SETTINGS_SHELF_STYLE ? 242 : 190;
        EpdRect area = {0, top, UI_LOCK_WIDTH, UI_NAV_REFRESH_END - top};
        // 只驱动变化像素，滚动次数不触发黑白清屏；题头与底栏保持稳定。
        // Drive changed pixels only; scroll counts never trigger a wipe, and header/nav stay stable.
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, area));
        return true;
    }
if (s_page != SETTINGS_UPGRADE) return false;
    if (redraw == APP_REDRAW_PAGE) {
        display_main_transition_cancel();
        render(ctx, ctx->fb);
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM,
                          MODE_GL16, (EpdRect){0, 0, UI_LOCK_WIDTH, UI_NAV_REFRESH_END}));
        return true;
    }
    return false;
}

const app_desc_t app_device_settings = {
    .title = "设置 Settings", .detail = "显示、连接与设备", .enter_full = false,
    .owns_keys = true, .menu_handle_enabled = no_menu_handle,
    .main_page_visible = main_page_visible,
    .on_enter = on_enter, .on_exit = settings_exit, .on_before_lock = on_before_lock, .render = render, .on_tick = on_tick, .on_gesture = on_gesture, .on_key = on_key,
    .on_key_long = on_key_long, .defer_middle_short = true,
    .present = settings_present, .on_media_lost=settings_media_lost, .on_media_ready=settings_media_ready,
};
