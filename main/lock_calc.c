/* SPDX-License-Identifier: Apache-2.0
 * 中文：计算器锁屏交互（用户 2026-10-19）：唤醒后计算器可用，正常按键照常计算；
 * 输入恰为四位数字按"＝"走 lock_pin 验证，成功即解锁。
 * English: Calculator lock interaction: the calculator works after wake; an exactly
 * four-digit equals runs lock_pin verification and unlocks on success.
 * 冻结：触摸轮询/PMU 电源键语义复用 lock_screen 的模式；显示区 DU 局部刷新；
 * 解锁前任何失败路径都回到浅睡，绝不进入未验证页面。
 * Frozen: the touch/power-key loop mirrors lock_screen; only the display area refreshes
 * with DU; every non-unlock exit returns to light sleep, never an unauthenticated page.
 */
#include "lock_calc.h"
#include "app.h"
#include "calc_core.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "read_pico_pmu.h"
#include "ble_page_turner.h"
#include "ui_gesture.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CALC_LOCK_IDLE_MS 30000

bool lock_calc_session(EpdiyHighlevelState* hl, cst836u_handle_t touch) {
    uint8_t* fb = epd_hl_get_framebuffer(hl);
    app_lock_wait_key_idle(800);
    int64_t active_at = esp_timer_get_time() / 1000;
    ui_gesture_t gesture = {0};
    bool was_down = false, draining = true;
    cst836u_touch_t latest = {0};
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - active_at >= CALC_LOCK_IDLE_MS) return false;
        if (read_pico_pmu_take_key_action() != READ_PICO_PMU_KEY_NONE) return false;
        cst836u_touch_t sample = {0};
        esp_err_t error = cst836u_read(touch, &sample);
        bool down = error == ESP_OK && sample.touched;
        if (draining) {
            if (!down && error == ESP_OK) draining = false;
            was_down = down;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (down) latest = sample;
        else { latest.touched = false; latest.count = 0; }
        app_ctx_t ctx = {.now_ms = now, .touch = &latest,
                         .pressed = down && !was_down, .released = !down && was_down,
                         .consumed = error != ESP_OK};
        ui_gesture_event_t event;
        if (ui_gesture_feed(&gesture, &ctx, &event)) {
            active_at = now;
            if (event.type == UI_GESTURE_TAP) {
                const char* key = calc_core_key_at(event.x0, event.y0);
                if (key) {
                    calc_core_press(key);
                    if (calc_core_unlocked()) {
                        read_pico_pmu_drain_events();
                        ble_pt_event_t e;
                        while (ble_pt_pop_key(&e)) {}
                        ble_pt_raw_t raw;
                        while (ble_pt_pop_raw(&raw)) {}
                        return true;
                    }
                    calc_core_paint_display(fb);
                    guard_draw_result(hl, update_display_area_diff_with(
                        hl, &E0470_WAVEFORM, MODE_DU, calc_core_display_area()));
                }
            }
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
