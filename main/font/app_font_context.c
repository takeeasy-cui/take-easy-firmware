/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户修订：异常启动暂用内建字体，避免自动重载故障资源；不覆盖用户字体设置。
 * User revision: recovery starts use the built-in face without retrying faulty resources or overwriting saved font choices.
 * 中文：系统默认使用内建思源黑体子集，TF 卡黑体补足缺字；用户选择的字体覆盖默认外观。
 * English: The built-in Source Han Sans face is the default; the SD face fills missing glyphs and explicit choices override it.
 * 用户修订：票根始终借用系统字体，浅睡解锁恢复锁屏前的实际字体和上下文，不覆盖保存的选择。
 * User revision: tickets always borrow the system face; light-sleep wake restores the actual previous face/context without altering saved choices.
 */
#include "app_font_context.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "settings.h"
#include "boot_state.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_nav.h"

static bool s_reading;
// Failed unchanged files are not repeatedly opened (opening unloads the active slot).
static char s_rejected[TTF_FONT_PATH_MAX];
static time_t s_rejected_mtime;
static off_t s_rejected_size;
/* 瞬时失败计数（2026-10-18“烧录后开机即回滚”根因）：开机阶段 PSRAM 峰值
 * 一次 open 失败（FreeType 层，非 NO_MEM 码）就进黑名单=永久回滚太激进。
 * 同路径连续失败 ≥3 次才拉黑；任何一次成功打开清零——瞬时峰值最多
 * 静默一拍，750ms 重试即恢复用户字体。 */
#define FONT_REJECT_THRESHOLD 3
static int s_fail_streak;
static bool font_file(const char *path, struct stat *st) {
    return path && stat(path, st) == 0 && S_ISREG(st->st_mode) && st->st_size > 0;
}

static const char* system_path(char fallback[TTF_FONT_PATH_MAX]) {
    if (pico_boot_recovery()) return TTF_FONT_BUILTIN;
    const char* chosen = app_settings_system_font_path();
    if (chosen[0]) return chosen;
    if (snprintf(fallback, TTF_FONT_PATH_MAX, "%s/Hei.ttf", app_settings_fonts_dir()) >= TTF_FONT_PATH_MAX)
        return TTF_FONT_BUILTIN;
    struct stat st;
    if (stat(fallback, &st) == 0 && S_ISREG(st.st_mode)) return fallback;
    if (!ttf_font_count()) ttf_font_scan();
    const ttf_font_item_t *item = ttf_font_item(0);
    return item && font_file(item->path, &st) ? item->path : TTF_FONT_BUILTIN;
}

static bool activate(const char* path) {
    struct stat st = {0};
    bool builtin = ttf_font_path_is_builtin(path);
    bool valid = builtin || font_file(path, &st);
    bool rejected = !builtin && !strcmp(s_rejected, path) &&
        s_rejected_mtime == st.st_mtime && s_rejected_size == st.st_size;
    if (!valid || rejected) path = TTF_FONT_BUILTIN;
    if (ttf_font_ready() && !strcmp(ttf_font_path(), path)) return false;
    esp_err_t err = ttf_font_open(path);
    if (err == ESP_OK) {
        s_fail_streak = 0;              /* 成功即清零计数 */
        if (!ttf_font_path_is_builtin(path)) s_rejected[0] = 0;
        return true;
    }
    /* 诊断哨（2026-10-18“烧录后开机即回滚”）：失败码+路径直接进日志，
     * 下次复现看一眼就知道是哪类失败撞了黑名单阈值。 */
    ESP_LOGW("font_ctx", "activate fail path=%s err=%s streak=%d",
             path, esp_err_to_name(err), s_fail_streak + 1);
    if (!ttf_font_path_is_builtin(path)) {
        /* 拉黑门（v2 2026-10-18）：文件级失败（缺文件/解析坏）即时拉黑；
         * 打开成功过的瞬时失败（内存峰值，多为 ESP_ERR_NO_MEM 或 FreeType
         * 层 ESP_FAIL）需连续 3 次才拉黑——开机峰值一次失败不再永久
         * 回滚用户字体。 */
        if (err == ESP_ERR_NO_MEM) {
            ++s_fail_streak;
            if (s_fail_streak >= FONT_REJECT_THRESHOLD) {
                snprintf(s_rejected, sizeof(s_rejected), "%s", path);
                s_rejected_mtime = st.st_mtime;
                s_rejected_size = st.st_size;
            }
        } else {
            /* 其它错误码：文件本体问题的概率高，但保底也数 1 次——
             * 万一是瞬时资源失败，连续 3 次才定性。 */
            ++s_fail_streak;
            if (s_fail_streak >= FONT_REJECT_THRESHOLD) {
                snprintf(s_rejected, sizeof(s_rejected), "%s", path);
                s_rejected_mtime = st.st_mtime;
                s_rejected_size = st.st_size;
            }
        }
    }
    if (ttf_font_ready() && ttf_font_is_builtin()) return false;
    return ttf_font_open_builtin() == ESP_OK;
}

bool app_font_activate_system(void) {
    s_reading = false;
    ttf_set_stroke_adjust(0);
    ttf_set_antialias(true);
    const char* chosen = app_settings_system_font_path();
    char fallback[TTF_FONT_PATH_MAX];
    const char* path = system_path(fallback);
    bool changed = activate(path);
    // 未选字体时由内建思源黑体绘制界面；TTF 仍保留完整字库供文件名缺字回退。
    // Built-in Source Han Sans draws default UI; the full TTF remains loaded for missing glyphs.
    ui_text_set_system_font(!pico_boot_recovery() && chosen[0] && ttf_font_ready() &&
                           !strcmp(chosen, ttf_font_path()) && !ttf_font_is_builtin());
    ui_text_set_system_scale(true);
    ui_nav_status_font_context(false, changed);
    return changed;
}

bool app_font_activate_reading(void) {
    s_reading = true;
    ttf_set_stroke_adjust_fine(app_settings_book_weight_fine());
    ttf_set_antialias(app_settings_book_antialias());
    const char* saved = pico_boot_recovery() ? TTF_FONT_BUILTIN : app_settings_font_path();
    bool changed = activate(ttf_font_path_is_builtin(saved) ? TTF_FONT_BUILTIN : saved);
    ui_text_set_system_font(false);
    ui_text_set_system_scale(false);
    ui_nav_status_font_context(true, changed);
    return changed;
}

bool app_font_retry_active(void) {
    return s_reading ? app_font_activate_reading() : app_font_activate_system();
}

void app_font_begin_lock(app_lock_font_t *saved) {
    if (!saved) return;
    snprintf(saved->path, sizeof(saved->path), "%s", ttf_font_path());
    saved->reading = s_reading;
    app_font_activate_system();
}

void app_font_end_lock(const app_lock_font_t *saved) {
    if (!saved) return;
    (void)activate(saved->path);
    s_reading = saved->reading;
    ttf_set_stroke_adjust_fine(s_reading ? app_settings_book_weight_fine() : 0);
    ttf_set_antialias(!s_reading || app_settings_book_antialias());
    const char *chosen = app_settings_system_font_path();
    ui_text_set_system_font(!s_reading && !pico_boot_recovery() && chosen[0] &&
                           !strcmp(chosen, ttf_font_path()) && !ttf_font_is_builtin());
    ui_text_set_system_scale(!s_reading);
    ui_nav_status_font_context(s_reading, false);
}
