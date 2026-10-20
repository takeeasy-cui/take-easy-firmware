/*
 * 闹钟服务实现（第二十一批 c 段，2026-10-20）。接口语义见 alarm.h。
 */
#include "alarm.h"

#include <stdio.h>
#include <time.h>

#include "display.h"
#include "epdiy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_buzzer.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "ui_kit.h"
#include "ui_nothing.h"

/// 有效 unix 起点取 2020-01-01（与 app_notes date_now 同口径）。
#define ALARM_UNIX_MIN 1577836800
/// 响铃总时长上限（秒）：无人响应自动停。
#define ALARM_RING_MAX_SEC 150

static time_t alarm_now_unix(void) {
    time_t now = time(NULL);
    if (now < ALARM_UNIX_MIN) {
        const pmu_snapshot_t* pmu = read_pico_pmu_get();
        if (pmu && pmu->time_synced) now = pmu->unix_sec;
    }
    return now >= ALARM_UNIX_MIN ? now : 0;
}

/// 下一个 HH:MM（北京时间语义）的 unix 时刻；已过（含 10s 竞态余量）则顺延一天。
static time_t alarm_next_unix(time_t now, uint8_t hour, uint8_t minute) {
    time_t bj = now + 8 * 3600;
    time_t day_start = bj - (bj % 86400);
    time_t target = day_start + (time_t)hour * 3600 + (time_t)minute * 60;
    if (target <= bj + 10) target += 86400;
    return target - 8 * 3600;
}

esp_err_t alarm_pmu_arm(void) {
    if (!app_settings_alarm_enabled()) return read_pico_pmu_alarm_set(0, 0);
    time_t now = alarm_now_unix();
    if (!now) return ESP_ERR_INVALID_STATE;   /* 无时间基准，由下次开机自愈 */
    time_t next = alarm_next_unix(now, app_settings_alarm_hour(), app_settings_alarm_minute());
    uint32_t secs = (uint32_t)(next - now);
    if (secs < 10) secs = 10;                  /* 协议下限 10s */
    return read_pico_pmu_alarm_set(1, secs);
}

bool alarm_boot_should_ring(void) {
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    bool boot_by_alarm = pmu && pmu->wake_reason == 6;
    return boot_by_alarm || read_pico_pmu_take_alarm();
}

uint32_t alarm_next_sleep_sec(void) {
    if (!app_settings_alarm_enabled()) return 0;
    time_t now = alarm_now_unix();
    if (!now) return 0;
    time_t next = alarm_next_unix(now, app_settings_alarm_hour(), app_settings_alarm_minute());
    uint32_t secs = (uint32_t)(next - now) + 1;   /* +1s 余量，避免卡在边界 */
    return secs ? secs : 1;
}

void alarm_ring_page(EpdiyHighlevelState* hl, uint8_t* fb, cst836u_handle_t tp) {
    /* 画面：白底整页 + 「闹钟」标签 + 点阵 HH:MM（用户 2026-10-20：数字用时间点阵样式）
     * + 停止提示 + 底部当前时刻小字。 */
    ui_clear_page(fb);
    char cs[8];
    snprintf(cs, sizeof(cs), "%02d:%02d", app_settings_alarm_hour(), app_settings_alarm_minute());
    const int pitch = 34, dr = 16;
    int w = nothing_dot_text_w(cs, pitch);
    nothing_dot_text(fb, (684 - w) / 2, 470, cs, dr, pitch, NX8_INK);
    ui_text_vc(fb, 342, 330, 48, "闹钟", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 700, 30, "点按屏幕任意处或按电源键停止", EPD_DRAW_ALIGN_CENTER, false);
    char now_cs[8] = "--:--";
    struct tm tm_bj = {0};
    time_t now = alarm_now_unix();
    if (now) {
        now += 8 * 3600;
        gmtime_r(&now, &tm_bj);
        snprintf(now_cs, sizeof(now_cs), "%02d:%02d", tm_bj.tm_hour, tm_bj.tm_min);
    }
    const int np = 14, ndr = 6;
    int nw = nothing_dot_text_w(now_cs, np);
    nothing_dot_text(fb, (684 - nw) / 2, 800, now_cs, ndr, np, NX8_DIM);
    (void)update_display_full(hl);

    /* 蜂鸣循环：曲谱播完停 600ms 再播；触摸 / 电源键停止；超时自动停。 */
    read_pico_buzzer_score_play();
    const int64_t t0 = esp_timer_get_time();
    for (;;) {
        if (!read_pico_buzzer_score_busy()) {
            if ((esp_timer_get_time() - t0) / 1000000LL >= ALARM_RING_MAX_SEC) break;
            vTaskDelay(pdMS_TO_TICKS(600));
            if (!read_pico_buzzer_score_busy()) read_pico_buzzer_score_play();
        } else {
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        cst836u_touch_t touch;
        if (tp && cst836u_read(tp, &touch) == ESP_OK && touch.touched) break;
        if (read_pico_pmu_take_key_short()) break;
    }
    read_pico_buzzer_score_stop();
    read_pico_pmu_drain_events();          /* 清掉响铃期间抬起的按键残留 */
    (void)alarm_pmu_arm();                 /* 重排下一个 HH:MM */
}

bool alarm_consume_ring(EpdiyHighlevelState* hl, uint8_t* fb, cst836u_handle_t tp) {
    if (!read_pico_pmu_take_alarm()) return false;
    alarm_ring_page(hl, fb, tp);
    return true;
}
