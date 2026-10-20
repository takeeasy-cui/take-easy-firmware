/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：底部导航与状态栏的统一入口，绘制与命中委托 ui_nothing 设计系统：
 *       状态栏为 take-easy 品牌 + WiFi + 电池（无时钟，首页用点阵时钟）；
 *       导航为四键圆盘（首页/书架/应用/设置）。返回键与 WiFi 标志复用 rc95
 *       原实现（含点击反馈注册）；状态栏时钟接口保留为空实现，页面无需改动。
 * English: Single entry for the bottom navigation and status bar. Drawing and
 *          hit testing delegate to the ui_nothing design system: the status bar
 *          shows the take-easy brand, Wi-Fi and battery (no clock; the home page
 *          carries the dot-matrix clock) and the navigation is the four-disc
 *          bar. The back control and Wi-Fi glyph reuse the rc95 originals; the
 *          status-clock hooks stay as no-ops so pages need no changes.
 */
#include "ui_nav.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_registry.h"
#include "read_pico_transfer.h"
#include "read_pico_pmu.h"
#include "ui_kit.h"
#include "ui_nothing.h"
#include "ui_click_feedback.h"
#include "ttf_font.h"
#include "ui_font.h"

// 底栏返回键箭头边长，与 rc95 原实现一致。
// Back-chevron size, unchanged from rc95.
#define UI_NAV_BACK_ICON_PX 38

void ui_nav_back(uint8_t *fb, int x, int y) {
    ui_click_feedback_register(fb, (EpdRect){x - 10, y + 3, 64, 64}, UI_CLICK_BACK_SMALL, UI_ICON_CHEVRON_LEFT);
    // 参考系统题头缩小返回键，中心仍保持在 y=113。/ Match the smaller reference back control while retaining the title centerline.
    ui_fill_round_rect(fb, (EpdRect){x, y + 13, 44, 44}, 22, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, (EpdRect){x, y + 13, 44, 44}, 22, UI_NAV_BACK_BORDER_GRAY);
    ui_draw_round_rect(fb, (EpdRect){x + 1, y + 14, 42, 42}, 21, UI_NAV_BACK_BORDER_GRAY);
    // Lucide chevron-left 的笔画占画布 14/24 高，取 38 像素复现原来 22 像素的视觉高度。
    // Lucide chevron-left spans 14/24 of its canvas, so 38 px reproduces the old 22 px visual height.
    ui_draw_icon(fb, x + 22, y + 35, UI_NAV_BACK_ICON_PX, UI_ICON_CHEVRON_LEFT, UI_GRAY_BLACK);
}

void ui_nav_wifi_icon(uint8_t *fb, int cx, int cy, int size, uint8_t gray) {
    if (!fb || size < 16 || size > 40) return;
    ui_draw_icon(fb, cx, cy, size, UI_ICON_WIFI, gray);
}

void ui_nav_draw(uint8_t *fb, int active) {
    // 导航带先铺纸白：上游页面可能用浅灰铺底，导航区统一回到纸面。
    // Lay paper white under the band: upstream pages may paint a light-gray
    // backdrop; the nav band always returns to paper.
    ui_fill_rect_fast((EpdRect){0, UI_NAV_TOP, UI_LOCK_WIDTH, UI_LOCK_HEIGHT - UI_NAV_TOP},
                      NX8_PAPER, fb);
    nothing_draw_nav(fb, active);
}

void ui_nav_status(uint8_t *fb) {
    // Nothing 状态栏：take-easy 品牌 + 电池 + 墨色 hairline（ui_nothing 内绘制）。
    // Nothing status bar: take-easy brand, battery and the ink hairline (drawn in ui_nothing).
    nothing_draw_statusbar(fb);
    read_pico_transfer_status_t network = {0};
    read_pico_transfer_get_status(&network);
    if (network.network_ready && network.mode == READ_PICO_TRANSFER_MODE_STA) {
        // WiFi 标志固定放在电池左侧，避开最宽的 "100%" 文本。
        // The Wi-Fi glyph sits at a fixed spot left of the battery, clear of the widest "100%".
        ui_nav_wifi_icon(fb, 486, 48, 30, UI_GRAY_BLACK);
    }
}

int ui_nav_hit(uint16_t x, uint16_t y) {
    if (x >= UI_LOCK_WIDTH || y >= UI_LOCK_HEIGHT) return -1;
    return nothing_nav_hit(x, y);
}

void ui_nav_request(app_ctx_t *ctx, int index) {
    if (index >= 0 && index < 4) ctx->request_app = app_at(index);
}

/* 状态栏时钟已随 V3.7 设计移除（首页用点阵时钟）；保留空实现，
 * 页面与主循环的调用无需改动。Clock hooks stay as no-ops for V3.7. */
void ui_nav_status_font_context(bool reading, bool face_changed) {
    (void)reading;
    (void)face_changed;
}

int64_t ui_nav_clock_minute(void) { return -1; }

void ui_nav_clock_draw(uint8_t *fb) { (void)fb; }
