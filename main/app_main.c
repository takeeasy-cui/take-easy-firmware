/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 用户修订：硬件前建立持久化启动保护；深睡恢复入口先消费再开书，中断图书进入可操作恢复页。
 * User revision: persist startup protection before hardware; consume deep-sleep checkpoints before opening books, and show an operable recovery page after interrupted opening.
 * 只做开机装配：拉起板级硬件、读设置、定 VCOM、放开机图、开字体；
 * 未标定则先拦住进设定页。然后把控制权交给 app_loop。
 *
 * Boot wiring only: board init, settings, PMU RTC restore, VCOM, splash, fonts. If VCOM
 * is unset, the factory page blocks first. Then control goes to app_loop.
 */

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "app_loop.h"
#include "app_content_open.h"
#include "boot_state.h"
#include "book_store.h"
#include "esp_system.h"
#include "read_pico_sd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/stat.h>
#include "app_registry.h"
#include "display.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "esp_log.h"
#include "ota_update.h"
#include "alarm.h"
#include "pmu_selftest.h"
#include "soc/rtc_cntl_reg.h"
#include "read_pico_board.h"
#include "read_pico_crashlog.h"
#include "read_pico_init.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "settings.h"
#include "sleep.h"
#include "ttf_font.h"
#include "usb_storage.h"
#include "app_font_context.h"
#include "ui_kit.h"
#include "vcom_setup.h"

static const char* TAG = "read_pico";
extern const app_desc_t app_boot_recovery;

// 软复位保留有效的 S3 RTC；断电才用 PMU 的实时秒和毫秒恢复。
// Retain a valid S3 RTC across soft reset; restore fresh PMU seconds and milliseconds after power loss.
static void restore_time_from_pmu(void) {
    struct timeval current;
    if (gettimeofday(&current, NULL) == 0 && current.tv_sec >= 1704067200 &&
        current.tv_sec <= 4102444799U) return;
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return;
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (!pmu || !pmu->time_ok || !pmu->time_synced || pmu->unix_millis >= 1000 ||
        pmu->unix_sec < 1704067200 || pmu->unix_sec > 4102444799U) return;
    struct timeval tv = {.tv_sec = (time_t)pmu->unix_sec, .tv_usec = pmu->unix_millis * 1000};
    if (settimeofday(&tv, NULL) == 0) ESP_LOGI(TAG, "system clock restored from PMU");
}

extern const uint8_t lock_4bpp_bin_z_start[] asm("_binary_lock_4bpp_bin_z_start");
extern const uint8_t lock_4bpp_bin_z_end[] asm("_binary_lock_4bpp_bin_z_end");
extern const uint8_t loading_4bpp_bin_z_start[] asm("_binary_loading_4bpp_bin_z_start");
extern const uint8_t loading_4bpp_bin_z_end[] asm("_binary_loading_4bpp_bin_z_end");

// 已标定则开机读一次喂给 epdiy。未标定先用板级默认出开机图，随后拦住进标定页。
// Load VCOM once when set. Otherwise keep the board default for the splash,
// then gate into the setup page.
static bool resolve_vcom_at_boot(void) {
    int mv = 0;
    for (int i = 0; i < 3; i++) {
        if (read_pico_pmu_vcom_get(&mv) == ESP_OK) {
            epd_set_vcom((uint16_t)mv);
            ESP_LOGI(TAG, "panel VCOM loaded from PMU");
            return true;
        }
    }
    ESP_LOGW(TAG, "panel VCOM unset, keeping board default until setup");
    return false;
}

// 开机图和锁屏图以 zlib(9) 压缩嵌入，此处试解压并核对整屏尺寸：解不开或尺寸
// 对不上说明资源和面板不匹配，与旧版原样嵌入时的字节比对同一语义。
// Splash and lock images are zlib(9)-compressed; inflate once and check the
// full-frame size here. A failure means the assets do not match the panel,
// the same semantics as the old raw byte comparison.
static bool images_match_panel(void) {
    if (!ui_z_asset_valid(loading_4bpp_bin_z_start, loading_4bpp_bin_z_end) ||
        !ui_z_asset_valid(lock_4bpp_bin_z_start, lock_4bpp_bin_z_end)) {
        ESP_LOGE(TAG, "Embedded splash/lock asset failed inflate validation");
        return false;
    }
    return true;
}

void app_main(void) {
    // 共享 TF 卡期间复位后，先把内部 PHY 交回串口，再清理已用完的 BOOT 请求。
    // Reclaim Serial/JTAG after a reset during card sharing, then clear the served BOOT request.
    usb_storage_phy_init();
    REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    const esp_reset_reason_t reset = esp_reset_reason();
    ESP_LOGI(TAG, "startup reset=%d stack=%u", (int)reset, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    app_settings_init();
    pico_boot_init(reset == ESP_RST_PANIC || reset == ESP_RST_INT_WDT ||
                   reset == ESP_RST_TASK_WDT || reset == ESP_RST_WDT);

    read_pico_handle_t hw;
    if (read_pico_init(&hw) != ESP_OK) return;
    rp_boot_mark("hw");
    if (hw.pmu_ready) restore_time_from_pmu();
    const bool vcom_ok = resolve_vcom_at_boot();
    rp_boot_mark("vcom");
    pmu_selftest_bind(hw.sensor);
    app_sleep_bind(hw.touch, hw.sensor);

    if (!images_match_panel()) return;

    EpdiyHighlevelState hl = hw.hl;
    uint8_t* framebuffer = hw.framebuffer;

    epd_poweron();
    read_pico_i2c_census_take();
    epd_clear();
    epd_hl_set_all_white(&hl);
    if (!ui_draw_full_image_z(framebuffer, loading_4bpp_bin_z_start, loading_4bpp_bin_z_end)) {
        ESP_LOGW(TAG, "splash inflate failed, showing cleared panel");
    }
    update_display_from_white(&hl);
    rp_boot_mark("splash");

    app_font_activate_system();
    rp_boot_mark("font");

    if (!hw.touch_ready) {
        ESP_LOGE(TAG, "No touch controller, UI cannot run");
        return;
    }

    if (hw.pmu_ready && !vcom_ok) {
        vcom_setup_run(&hl, framebuffer, hw.touch);
    }

    // 硬件、显示、触摸与必要的出厂设置均成功后确认新槽。
    // Confirm a new slot only after hardware, display, touch, and required setup succeed.
    pico_ota_confirm_running();

    /* 闹钟响铃（第二十一批 c 段，2026-10-20）：PMU 闹钟到点自动开机
     * （wake_reason=6）或事件锁存——开机先响铃再走锁屏验证
     * （被闹钟叫醒不必先输密码）。响铃停止时已重排下一个 HH:MM。 */
    if (hw.pmu_ready && alarm_boot_should_ring()) {
        alarm_ring_page(&hl, framebuffer, hw.touch);
    }
    /* 开机自愈：闹钟开时重设 PMU 闹钟（CW 断电/复位后闹钟丢失，协议明示）。 */
    if (hw.pmu_ready) (void)alarm_pmu_arm();

    app_lock_boot_gate(&hl, hw.touch, hw.sensor);
    rp_boot_mark("lock");
    const app_desc_t *first = app_home_page();
    pico_resume_t resume;
    if (pico_boot_take_resume(&resume)) {
        if (!resume.reader) first = app_at(resume.tab);
        else {
            // 挂载等待有界；恢复入口已消费，无卡或资源失败不会反复自动开书。
            // Bound mount waiting after consuming the checkpoint; absent media or failed resources cannot auto-open in a loop.
            if (!strncmp(resume.path, "/sdcard/", 8)) {
                read_pico_sd_info_t info;
                for (unsigned wait = 0; wait < 150 && read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED; ++wait)
                    vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!strncmp(resume.path, "/flash/books/", 13) && !book_store_flash_ready()) {
                book_store_root_t roots[BOOK_STORE_ROOT_MAX]; int count;
                (void)book_store_roots(roots, &count);
            }
            struct stat st;
            if (!stat(resume.path, &st) && S_ISREG(st.st_mode) &&
                app_book_request_resume(resume.path, resume.fullscreen)) first = app_at(1);
        }
    }
    char interrupted[PICO_BOOT_PATH_MAX];
    if (pico_boot_interrupted_book(interrupted, sizeof(interrupted))) first = &app_boot_recovery;
    rp_boot_mark("resume");
    app_loop_run(&(app_loop_config_t){
        .hl = &hl,
        .fb = framebuffer,
        .acc = hw.sensor,
        .tp = hw.touch,
        .sensor_ready = hw.sensor_ready,
        // 用户修订：深睡一次性恢复上次阅读或主页面；异常启动安全回首页。
        // User revision: resume the last reader/main tab once after deep sleep; abnormal starts safely enter Home.
        .first_app = first,
    });
}
