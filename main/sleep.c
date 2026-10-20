/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏进睡、浅睡等待按键或拿起、软睡/关机拉掉 EN。
 *
 * Enter lock and sleep, light-sleep wait for key or pickup, and drop EN
 * for soft sleep / power-off.
 * 用户修订：票根从任意页面进入均用系统字体；浅睡唤醒恢复此前阅读字体，深睡由启动检查点恢复。
 * User revision: tickets use the system face from every page; light wake restores the previous reader face, while deep wake uses the startup checkpoint.
 * 用户修订：其他样式/自动锁屏先准备完整前缓冲，再清屏并做原 GC16 输出，缩短白屏等待。
 * User revision: other styles and idle locks prepare the complete front buffer before the original clear/GC16 output, shortening white-screen waits.
 * 用户修订：拼贴的新整帧缓存移到完整显示之后写入，密码层覆盖前提交。
 * User revision: persist new collage frames after complete presentation and before PIN overlays.
 * 用户修订：主动拼贴锁屏立即清白并显示圆锁，随后只用一次原GC16输出完整拼贴；取消占位和四步显影。
 * User revision: manual collage locks promptly present white and a round lock, then present the complete collage once with original GC16; remove placeholders and four-pass reveal.
 * 用户修订：合入官方 a2a63a6 的外设停用、IOE 收口与 CPU 浅睡保持；保留十分钟浅睡、密码和阅读检查点。
 * User revision: port official a2a63a6 peripheral/IOE parking and CPU light-sleep retention while preserving ten-minute light sleep, PIN and reader checkpoints.
 * 冻结：沿用实际后缓冲，不把圆锁当白色；不增加帧副本，自动休眠/其他样式/失败回退保留原流程。
 * Frozen: Retain true history including the badge, add no frame copy and preserve idle/other-style/failure recovery.
 */
#include "ui_avatar.h"
#include "book_cover.h"
#include "book_cover_jobs.h"

#include "sleep.h"
#include "boot_state.h"
#include "calc_core.h"
#include "lock_calc.h"
#include "lock_pin.h"
#include "lock_screen.h"
#include "read_pico_sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "app_font_context.h"
#include "book_cover.h"
#include "book_lock_collage.h"
#include "book_ticket.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_private/sleep_cpu.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pmu_selftest.h"
#include "read_pico_board.h"
#include "read_pico_buzzer.h"
#include "ble_page_turner.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#include "sc7a20h_lab.h"
#include "settings.h"
#include "ui_kit.h"
#include "ui_image_dither.h"
#include "ui_nothing.h"
#include "ui_wallpaper.h"
#include "ui_lock_feedback.h"
#include "ttf_font.h"
#include "esp_attr.h"
#include <zlib.h>
/* v2（用户 2026-10-17）：默认透明书封（灰8+alpha8 双平面，assets/cover_default.bin.z）。
 * 未选书封 / 解码失败时用它代替田卡图片；同图白底合成版即新开机图 loading_4bpp。 */
extern const uint8_t cover_default_bin_z_start[] asm("_binary_cover_default_bin_z_start");
extern const uint8_t cover_default_bin_z_end[] asm("_binary_cover_default_bin_z_end");

static const char* TAG = "read_pico";
static bool s_lock_feedback_visible;

#define APP_LOCK_LIGHT_SLEEP_MS (10U * 60U * 1000U)
#define APP_LOCK_KEY_POLL_US (2LL * 1000 * 1000)
#define APP_LOCK_STUCK_IRQ_US (3LL * 1000 * 1000)
#define APP_LOCK_SLEEP_ERROR_LIMIT 5
// PMU 确认后仍未掉 EN 时恢复调用方，禁止无限等待。/ Restore the caller if EN stays high after acknowledgement instead of waiting forever.
#define HOST_EN_DROP_WAIT_MS 5000

extern const uint8_t lock_4bpp_bin_z_start[] asm("_binary_lock_4bpp_bin_z_start");
extern const uint8_t lock_4bpp_bin_z_end[] asm("_binary_lock_4bpp_bin_z_end");
/* 默认锁屏壁纸 = 开机图（用户 2026-10-16：默认锁屏改壁纸锁屏，使用同一张图）。 */
extern const uint8_t loading_4bpp_bin_z_start[] asm("_binary_loading_4bpp_bin_z_start");
extern const uint8_t loading_4bpp_bin_z_end[] asm("_binary_loading_4bpp_bin_z_end");

void app_lock_feedback(EpdiyHighlevelState *hl) {
    /* 2026-10-17：圆锁反馈随拼贴样式退役；所有锁屏样式统一单次刷新呈现，无预反馈白闪。 */
    (void)hl;
    s_lock_feedback_visible = false;
}
static bool draw_wallpaper(uint8_t *framebuffer);

/* ============ 锁屏农历/老黄历与时钟/透明书封（用户 2026-10-17） ============ */

static bool lock_now_bj(struct tm *tm_bj) {
    time_t now = time(NULL);
    if (now < 946684800) return false;
    time_t bj = now + 8 * 3600;
    return gmtime_r(&bj, tm_bj) != NULL;
}
static const uint32_t k_lunar_tab[201] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
    0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
    0x0d520
};
static int lock_lunar_leap_month(int y) { return (int)(k_lunar_tab[y - 1900] & 0xf); }
static int lock_lunar_leap_days(int y) {
    return lock_lunar_leap_month(y) ? ((k_lunar_tab[y - 1900] & 0x10000) ? 30 : 29) : 0;
}
static int lock_lunar_month_days(int y, int m) {
    return (k_lunar_tab[y - 1900] & (0x10000 >> m)) ? 30 : 29;
}
static int lock_lunar_year_days(int y) {
    int sum = 348;
    for (int i = 0x8000; i > 8; i >>= 1) sum += (k_lunar_tab[y - 1900] & i) ? 1 : 0;
    return sum + lock_lunar_leap_days(y);
}
static int64_t lock_days_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static bool lock_solar_to_lunar(int sy, int sm, int sd, int *ly, int *lm, int *ld, bool *leap) {
    if (sy < 1901 || sy > 2098) return false;
    int64_t days = lock_days_civil(sy, sm, sd) - lock_days_civil(1900, 1, 31);
    int y = 1900;
    while (days >= lock_lunar_year_days(y)) { days -= lock_lunar_year_days(y); ++y; }
    int lp = lock_lunar_leap_month(y);
    int idx = 0;
    for (;;) {
        int md = idx == 12 ? lock_lunar_leap_days(y) : lock_lunar_month_days(y, idx + 1);
        if (idx == 12 && !lp) break;
        if (days < md) break;
        days -= md;
        ++idx;
        if (idx > 12) break;
    }
    *leap = idx == 12;
    *lm = *leap ? lp : idx + 1;
    *ld = (int)days + 1;
    *ly = y;
    return true;
}
/* 宜/忌确定映射（民间黄历风格展示）。 */
static void lock_almanac_fields(int sy, int sm, int sd,
                                char *ganzhi, size_t cg, char *lunar, size_t cl,
                                char *yi, size_t cy, char *ji, size_t cj) {
    static const char *const gan[] = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
    static const char *const zhi[] = {"子鼠","丑牛","寅虎","卯兔","辰龙","巳蛇","午马","未羊","申猴","酉鸡","戌狗","亥猪"};
    static const char *const months[] = {"正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","冬月","腊月"};
    static const char *const days[] = {"初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
        "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
        "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"};
    static const char *const good[] = {"祭祀","祈福","出行","纳财","开市","交易","会友","裁衣","栽种","牧养"};
    static const char *const bad[] = {"动土","安葬","嫁娶","词讼","掘井","移徙","针灸","破屋"};
    int ly, lm, ld; bool leap;
    if (!lock_solar_to_lunar(sy, sm, sd, &ly, &lm, &ld, &leap)) {
        snprintf(ganzhi, cg, "老黄历"); lunar[0] = 0; yi[0] = 0; ji[0] = 0; return;
    }
    int gy = (ly - 4) % 10, gz = (ly - 4) % 12;
    if (gy < 0) gy += 10;
    if (gz < 0) gz += 12;
    snprintf(ganzhi, cg, "%s%s年", gan[gy], zhi[gz]);
    snprintf(lunar, cl, "%s%s%s", leap ? "闰" : "", months[lm - 1], days[ld - 1]);
    int seed = lm * 3 + ld;
    snprintf(yi, cy, "宜 %s · %s", good[seed % 10], good[(seed + 4) % 10]);
    snprintf(ji, cj, "忌 %s · %s", bad[(seed * 3 + 1) % 8], bad[(seed * 5 + 4) % 8]);
}

/* ---- v1.8 横屏时钟锁屏（用户 2026-10-17 反馈）：画面改 1216×684 横屏，
 * 底部文案改“本模式下，设备不会关机”。像素链路与 FMJ 同构：
 * 横屏逻辑 (x,y) 写 epd_draw_pixel(683-y,x)——物理 fb 上横屏坐标即物理坐标，
 * TTF 栅格化 mask 逐像素原位平铺（无需真旋转）。 */

/* 横屏 TTF 文字：覆盖率 mask 与墨色混色，中心对齐（仿 fmj_screen_text，UTF-8 直入）。 */
static void lock_land_text(uint8_t *fb, int cx, int cy, int px, const char *text, uint8_t ink) {
    enum { MW = 660, MH = 76 };
    static EXT_RAM_BSS_ATTR uint8_t mask[MW * MH];
    memset(mask, 0, sizeof(mask));
    if (!ttf_text_mask_px(mask, MW, MH, 0, ttf_ascender_px(px), px, text)) return;
    int minX = MW, maxX = -1, minY = MH, maxY = -1;
    for (int y = 0; y < MH; ++y)
        for (int x = 0; x < MW; ++x)
            if (mask[y * MW + x]) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
    if (maxX < minX) return;
    const int ox = cx - (maxX - minX + 1) / 2 - minX;
    const int oy = cy - (maxY - minY + 1) / 2 - minY;
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x) {
            uint8_t a = mask[y * MW + x];
            if (!a) continue;
            uint8_t v = (uint8_t)((ink * (uint32_t)a + 0xFFu * (255u - a) + 127) / 255);
            epd_draw_pixel(683 - (oy + y), ox + x, v, fb);
        }
}

/* v3（2026-10-19）：lock_land_hline 已无调用方（v1.8 时钟锁屏改版后遗留），
 * 本次 sleep.c 重编触发 -Werror 删除；如需横线分隔可由 ui_hairline 承担。 */

void app_lock_clock_art(uint8_t *fb) {
    struct tm tm_bj = {0};
    bool ok = lock_now_bj(&tm_bj);
    static const char *const wk[] = {"日","一","二","三","四","五","六"};
    ui_clear_page(fb);
    /* v1（用户 2026-10-17）：息屏大时钟风（横屏 1216×684）——
     * 上日期居中/电量右上；中央特大点阵时钟；
     * 「时间以外内容」开关（clock_almanac）关=仅时间，开=日期+左黄历右待办。 */
    /* v2（2026-10-17 第五批）：字号再加大——日期/黄历/宜忌/待办 34px，时间点阵
     * pitch 34→38 / 半径 16→18；clock_almanac 关=纯时间（日期也隐藏）。 */
    /* v4（用户 2026-10-19）：除时间外所有文字比原待办字号（34）大一档→36（分析：
     * 本页字号阶梯 30/32/34→36，再大与时间点阵的层级冲突）；电量 24→26、
     * 底部历法头 20→24 保持次级层级。老黄历与待办整体下调 30px（470→500），
     * 离点阵时间（底约 431）更远。 */
    const bool extras = app_settings_clock_almanac();
    char date[56];
    if (ok) snprintf(date, sizeof(date), "%d年%d月%d日 星期%s",
                     tm_bj.tm_year + 1900, tm_bj.tm_mon + 1, tm_bj.tm_mday, wk[tm_bj.tm_wday]);
    else snprintf(date, sizeof(date), "日期待设置");
    /* v5（用户 2026-10-19 真机反馈）：日期 cx 570→608=屏幕几何中心。原 570 是
     * bbox 中心对齐下的经验偏移，但 lock_land_text 对齐的是文字墨迹包围盒中心
     * （星期几 1-3 字等长不齐），bbox 中心≠版面中心导致视觉偏左；统一用 608
     * 与点阵时钟（(1216-w)/2 视觉居中）同基准。 */
    if (extras) lock_land_text(fb, 608, 78, 36, date, 0x00);
    int pct = pmu_battery_percent(read_pico_pmu_get());
    char bat[10];
    snprintf(bat, sizeof(bat), "%d%%", pct >= 0 ? pct : 0);
    lock_land_text(fb, 1096, 70, 26, bat, 0x00);
    /* 中央特大点阵时钟：pitch 38 / r18，"10:17" 5 字符宽约 1102 居中，高约 266 */
    char cs[8] = "--:--";
    if (ok) snprintf(cs, sizeof(cs), "%02d:%02d", tm_bj.tm_hour, tm_bj.tm_min);
    const int pitch = 38, dr = 18;
    int w = nothing_dot_text_w(cs, pitch);
    const int clock_top = 165;
    nothing_dot_text_rot(fb, (1216 - w) / 2, clock_top, cs, dr, pitch, 0x00);
    /* 开关开启：时钟下方老黄历。v5（用户 2026-10-20）：右侧待办列移除，
     * 宜/忌改为左右两块居中——徽章样式与数据面板老黄历卡一致。
     * 注意：lock fb 为竖屏原始坐标，圆按 (683-视觉y, 视觉x) 映射；文字走 lock_land_text 自带翻转。 */
    if (extras && ok) {
        char gz[32], lunar[48], yi[64], ji[64];
        lock_almanac_fields(tm_bj.tm_year + 1900, tm_bj.tm_mon + 1, tm_bj.tm_mday,
                            gz, sizeof(gz), lunar, sizeof(lunar), yi, sizeof(yi), ji, sizeof(ji));
        /* v6（用户 2026-10-20）：老黄历字体加大——条 26→34、徽章 r16→r19/字 20→23、
         * 历法头 36→40。两块仍以 300/908 为中心，块宽=徽章38+间距10+条目宽。 */
        int w_yi = ttf_text_width_px(34, yi);
        int gx = 300 - (38 + 10 + w_yi) / 2;
        epd_fill_circle(683 - 532, gx + 19, 19, 0x00, fb);
        lock_land_text(fb, gx + 19, 532, 23, "宜", 0xFF);
        lock_land_text(fb, gx + 48 + w_yi / 2, 534, 34, yi, 0x00);
        int w_ji = ttf_text_width_px(34, ji);
        int hx = 908 - (38 + 10 + w_ji) / 2;
        epd_draw_circle(683 - 532, hx + 19, 19, 0x00, fb);
        epd_draw_circle(683 - 532, hx + 19, 18, 0x00, fb);
        lock_land_text(fb, hx + 19, 532, 23, "忌", 0x00);
        lock_land_text(fb, hx + 48 + w_ji / 2, 534, 34, ji, 0x00);
        /* 底部：历法头（原提示位置）。字号 40（用户 2026-10-20 加大）。 */
        char head[112];
        snprintf(head, sizeof(head), "%s · %s", gz, lunar);
        lock_land_text(fb, 608, 652, 40, head, 0x00);
    }
}

/* 秒到下一分钟边界的毫秒数（不到 1s 按 1s）。 */
static uint32_t lock_clock_next_tick_ms(void) {
    time_t now = time(NULL);
    if (now < 946684800) return 60000;
    int sec = (int)(now % 60);
    return (uint32_t)((sec ? 60 - sec : 60) * 1000);
}

/* 每分钟轻量重绘（用户 2026-10-19 功耗）：只擦除并重画中央点阵时钟条，
 * 跳过整页 TTF 光栅化与黄历/待办计算（日期/电量/黄历仅在跨日整屏时更新），
 * 显著缩短每分钟唤醒的 CPU 窗口。竖屏显示坐标 {242,37,281,1142}
 * 对应横屏 log 时钟条（x 37..1179 / y 160..441，见走时循环注释）。 */
static void lock_clock_minute_paint(uint8_t *fb) {
    struct tm tm_bj = {0};
    if (!lock_now_bj(&tm_bj)) return;
    char cs[8];
    snprintf(cs, sizeof(cs), "%02d:%02d", tm_bj.tm_hour, tm_bj.tm_min);
    const int pitch = 38, dr = 18;
    int w = nothing_dot_text_w(cs, pitch);
    ui_fill_rect_fast((EpdRect){242, 37, 281, 1142}, 0xFF, fb);
    nothing_dot_text_rot(fb, (1216 - w) / 2, 165, cs, dr, pitch, 0x00);
}

/* 透明书封（阅星瞳语义，v1.8）：PNG 在电脑端抠好图上传，设备把它盖在当前画面上——
 * 非透明像素按 alpha 合成覆盖、透明处透出原界面（不清屏）。未选书封时回退信息面板。 */
/* 透明书封（阅星瞳语义）：内嵌默认图版（灰8+alpha8 双平面）。解压后
 * 逐像素与当前画面按 alpha 合成 —— a≥250 直接覆盖、其余线性混合，透明处透出原画面。 */
static bool lock_cover_default(uint8_t *fb) {
    const int W = 684, H = 1216;
    const size_t plane = (size_t)W * H;
    uint8_t *buf = heap_caps_malloc(plane * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    uLongf inflated = (uLongf)(plane * 2);
    bool ok = uncompress(buf, &inflated, cover_default_bin_z_start,
        (uLong)(cover_default_bin_z_end - cover_default_bin_z_start)) == Z_OK &&
        inflated == plane * 2;
    if (ok) {
        const uint8_t *gray = buf, *alpha = buf + plane;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                const uint8_t a = alpha[i];
                if (!a) continue;
                if (a >= 250) { epd_draw_pixel(x, y, gray[i], fb); continue; }
                const uint8_t byte = fb[(size_t)y * W / 2 + x / 2];
                const uint8_t base = (x & 1) ? (byte & 0xf0) : ((byte & 0x0f) << 4);
                const uint8_t v = (uint8_t)((gray[i] * (uint32_t)a +
                    (uint32_t)base * (255u - a) + 127) / 255);
                epd_draw_pixel(x, y, v, fb);
            }
    }
    free(buf);
    return ok;
}

static void lock_cover_overlay(uint8_t *fb) {
    const char *path = app_settings_coverart_path();
    if (path[0]) {
        const int W = 684, H = 1216;
        const size_t plane = (size_t)W * H;
        uint8_t *gray = heap_caps_malloc(plane, MALLOC_CAP_SPIRAM);
        uint8_t *alpha = heap_caps_malloc(plane, MALLOC_CAP_SPIRAM);
        bool ok = gray && alpha &&
            book_image_file_cover_layer(path, W, H, gray, alpha);
        if (ok) {
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    const size_t i = (size_t)y * W + x;
                    const uint8_t a = alpha[i];
                    if (!a) continue;
                    if (a >= 250) { epd_draw_pixel(x, y, gray[i], fb); continue; }
                    const uint8_t byte = fb[(size_t)y * W / 2 + x / 2];
                    const uint8_t base = (x & 1) ? (byte & 0xf0) : ((byte & 0x0f) << 4);
                    const uint8_t v = (uint8_t)((gray[i] * (uint32_t)a +
                        (uint32_t)base * (255u - a) + 127) / 255);
                    epd_draw_pixel(x, y, v, fb);
                }
            }
        }
        free(gray);
        free(alpha);
        if (ok) return;
    }
    /* v2：未选书封 / 解码失败 → 内嵌默认图叠加；再失败才回退信息面板。 */
    if (lock_cover_default(fb)) return;
    /* 回退（未选书封 / 解码失败）：原信息面板。 */
    struct tm tm_bj = {0};
    bool ok = lock_now_bj(&tm_bj);
    static const char *const wk[] = {"日","一","二","三","四","五","六"};
    EpdRect panel = {36, 44, 612, 236};
    ui_fill_round_rect(fb, panel, 26, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, panel, 26, 0x60);
    char date[56];
    if (ok) snprintf(date, sizeof(date), "%d月%d日 星期%s",
                     tm_bj.tm_mon + 1, tm_bj.tm_mday, wk[tm_bj.tm_wday]);
    else snprintf(date, sizeof(date), "日期待设置");
    ui_text_vc(fb, 64, panel.y + 14, 22, date, EPD_DRAW_ALIGN_LEFT, false);
    int pct = pmu_battery_percent(read_pico_pmu_get());
    char bat[10];
    snprintf(bat, sizeof(bat), "%d%%", pct >= 0 ? pct : 0);
    ui_text_vc(fb, panel.x + panel.width - 20, panel.y + 14, 22, bat, EPD_DRAW_ALIGN_RIGHT, false);
    ui_draw_separator(fb, panel.y + 50, 64, 544, 0x60);
    char cs[8] = "--:--";
    if (ok) snprintf(cs, sizeof(cs), "%02d:%02d", tm_bj.tm_hour, tm_bj.tm_min);
    const int pitch = 13;
    int w = nothing_dot_text_w(cs, pitch);
    nothing_dot_text(fb, 342 - w / 2, panel.y + 66, cs, 5, pitch, 0x00);
    char sub[96] = "已锁定 · 当前画面保留";
    if (ok) {
        char gz[32], lunar[48], yi[64], ji[64];
        lock_almanac_fields(tm_bj.tm_year + 1900, tm_bj.tm_mon + 1, tm_bj.tm_mday,
                            gz, sizeof(gz), lunar, sizeof(lunar), yi, sizeof(yi), ji, sizeof(ji));
        const char *t = "";
        for (int i = 0; i < 3 && !t[0]; ++i) t = app_settings_todo(i);
        int n = snprintf(sub, sizeof(sub), "%s %s", lunar, yi);
        if (t[0] && n > 0 && (size_t)n < sizeof(sub) - 12)
            snprintf(sub + n, sizeof(sub) - (size_t)n, "  提醒 · %.24s", t);
    }
    ui_text_vc(fb, 342, panel.y + 172, 20, sub, EPD_DRAW_ALIGN_CENTER, false);
}

bool app_lock_boot_art(uint8_t *fb) {
    uint8_t st = app_settings_lock_style();
    if (st == 0) { app_lock_clock_art(fb); return true; }
    if (st == 1) return draw_wallpaper(fb);
    return false;   /* 透明书封开机无残存画面，走调用方回退图（新开机图即默认狐狸图）。 */
}

static bool draw_wallpaper(uint8_t *framebuffer) {
    if (app_settings_lock_style() != 1) return false;
    const char *path = app_settings_wallpaper_path();
    if (path[0] && ui_wallpaper_prepare_lock(framebuffer, path)) return true;
    /* v2：壁纸样式未选图 / 加载失败 → 内嵌默认图（与开机图同源，take easy 狐狸图）。 */
    return ui_draw_full_image_z(framebuffer, loading_4bpp_bin_z_start, loading_4bpp_bin_z_end);
}

static cst836u_handle_t s_tp;
static sc7a20h_handle_t s_acc;
static bool s_acc_was_on;

void app_sleep_bind(cst836u_handle_t tp, sc7a20h_handle_t acc) {
    s_tp = tp;
    s_acc = acc;
    // 没开 PM 时 IDF 不分配 CPU 保存区，浅睡里 CPU 不会掉电；这里手动申请一次。
    // Without PM, IDF never allocates CPU retention, so light sleep keeps the CPU powered; request it once here.
    esp_err_t err = sleep_cpu_configure(true);
    if (err != ESP_OK) ESP_LOGW(TAG, "CPU light-sleep power-down off: %s", esp_err_to_name(err));
}

// 触摸、加速度计都在常开 3V3 上，ESP 浅睡或 EN 拉低都不会断它们的电，必须主动送进最低功耗态。
// Touch and accelerometer sit on the always-on 3V3; neither light sleep nor EN low cuts them, so park them explicitly.
static void periph_suspend(bool host_off) {
    if (read_pico_buzzer_score_busy()) (void)read_pico_buzzer_score_stop();
    esp_err_t ioe = read_pico_ioe_sleep(host_off);
    if (ioe != ESP_OK) ESP_LOGW(TAG, "ioe sleep %s", esp_err_to_name(ioe));
    s_acc_was_on = sc7a20h_powered(s_acc);
    if (s_acc_was_on) (void)sc7a20h_power_down(s_acc);
    if (s_tp != NULL && cst836u_get_mode(s_tp) != CST836U_MODE_DEEPSLEEP) {
        esp_err_t err = cst836u_set_mode(s_tp, CST836U_MODE_DEEPSLEEP);
        if (err != ESP_OK) ESP_LOGW(TAG, "touch deepsleep %s", esp_err_to_name(err));
    }
}

// 深睡的触摸只认 TP_RST 复位；加速度计按睡前的配置重新上电。
// Deep-sleeping touch only wakes via TP_RST; the accelerometer comes back with its pre-sleep config.
static void periph_resume(void) {
    (void)read_pico_ioe_wake();
    if (s_tp != NULL && cst836u_get_mode(s_tp) == CST836U_MODE_DEEPSLEEP) {
        esp_err_t err = cst836u_wake(s_tp);
        if (err != ESP_OK) ESP_LOGW(TAG, "touch wake %s", esp_err_to_name(err));
    }
    if (s_acc_was_on) (void)sc7a20h_power_up(s_acc);
    s_acc_was_on = false;
}

static void lock_arm_ioe_wakeup(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << READ_PICO_IOE_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    read_pico_clear_ioe_int();
    gpio_wakeup_enable((gpio_num_t)READ_PICO_IOE_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

void app_lock_wait_key_idle(int timeout_ms) {
    int64_t start = esp_timer_get_time() / 1000;
    int64_t high_from = 0;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        read_pico_pmu_drain_events();
        read_pico_clear_ioe_int();
        bool held = false;
        if (read_pico_pmu_poll() == ESP_OK) {
            held = (read_pico_pmu_get()->key_state & 0x01) != 0;
        }
        bool int_high = gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) != 0;
        if (!held && int_high) {
            if (high_from == 0) high_from = esp_timer_get_time() / 1000;
            if (esp_timer_get_time() / 1000 - high_from >= 150) return;
        } else {
            high_from = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

app_wake_source_t app_last_wake_source(void) {
    return (app_wake_source_t)app_settings_last_wake();
}

static void pickup_ack(sc7a20h_handle_t acc) {
    sc7a20h_events_t ev;
    sc7a20h_read_events(acc, &ev);
    sc7a20h_ack_int(acc);
}

static bool pickup_ia(sc7a20h_handle_t acc) {
    sc7a20h_events_t ev;
    sc7a20h_aoi_src_t aoi;
    if (sc7a20h_read_events(acc, &ev) != ESP_OK) return false;
    sc7a20h_aoi_decode(ev.aoi1_src, &aoi);
    return aoi.ia;
}

// 第一次低电平不够：高通余波还会再拉高。要连续安静一段时间才进浅睡。
// A first low is not enough: high-pass ringing can rise again. Wait for a quiet stretch before light sleep.
static bool pickup_wait_quiet(sc7a20h_handle_t acc, int quiet_ms, int timeout_ms) {
    int64_t start = esp_timer_get_time() / 1000;
    int64_t low_from = 0;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        if (sc7a20h_int1_level(acc) == 0) {
            if (low_from == 0) low_from = esp_timer_get_time() / 1000;
            if (esp_timer_get_time() / 1000 - low_from >= quiet_ms) return true;
        } else {
            pickup_ack(acc);
            low_from = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

app_wake_source_t app_light_sleep_wait_timed(sc7a20h_handle_t acc, uint32_t timeout_ms) {
    if (!(book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000))) return APP_WAKE_KEY;
    bool pickup = acc != NULL && app_settings_pickup_wake();
    bool acc_armed = false;
    const int64_t deadline_us = timeout_ms
        ? esp_timer_get_time() + (int64_t)timeout_ms * 1000 : 0;
    periph_suspend(false);
    lock_arm_ioe_wakeup();
    if (pickup) {
        sc7a20h_motion_cfg_t motion = SC7A20H_MOTION_DEFAULT();
        motion.ths_mg = 350;
        motion.duration = 3;
        sc7a20h_arm_pickup_wake(acc, &motion);
        acc_armed = true;
        pickup_ack(acc);
        if (!pickup_wait_quiet(acc, 400, 2000)) {
            pickup = false;
            ESP_LOGW(TAG, "pickup INT1 stuck high, key only");
        } else {
            sc7a20h_config_light_sleep_wakeup(acc);
            if (!pickup_wait_quiet(acc, 400, 1500)) {
                gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                pickup = false;
                ESP_LOGW(TAG, "pickup INT1 re-asserted, key only");
            } else {
                // 安静窗口里若 INT1 闪过，ESP 会留下高电平唤醒挂起，关掉再打开清掉。
                // If INT1 glitched in the quiet window, ESP keeps a high-level wake pending; disable then re-enable to clear it.
                gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                sc7a20h_config_light_sleep_wakeup(acc);
                if (sc7a20h_int1_level(acc) > 0) {
                    gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                    pickup = false;
                    ESP_LOGW(TAG, "pickup INT1 high at sleep, key only");
                } else {
                    ESP_LOGI(
                        TAG, "wait key GPIO%d or pickup GPIO%d",
                        READ_PICO_IOE_INT_GPIO, (int)sc7a20h_int1_gpio(acc)
                    );
                }
            }
        }
    } else {
        ESP_LOGI(TAG, "wait key on IOE_INT GPIO%d", READ_PICO_IOE_INT_GPIO);
    }

    app_wake_source_t wake = APP_WAKE_NONE;
    int64_t ioe_low_since_us = 0;
    unsigned sleep_errors = 0;
    esp_err_t last_sleep_error = ESP_OK;
    int64_t last_sleep_error_log_ms = 0;
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        // Check the deadline before every interrupt recovery path. A stuck-low
        // IO expander must never bypass the ten-minute PMU handoff forever.
        // 每次处理中断前先检查期限，避免 IO 扩展器低电平让票根永远停住。
        if (deadline_us && now_us >= deadline_us) {
            wake = APP_WAKE_TIMEOUT;
            ESP_LOGI(TAG, "lock light sleep timeout after %u ms", (unsigned)timeout_ms);
            break;
        }
        // The PMU event is authoritative even if the IOE edge was missed.
        // Timer wakeups below provide a bounded fallback poll for that case.
        // 即使漏掉 IOE 中断，也由定时唤醒轮询 PMU 按键事件。
        if (read_pico_pmu_take_key_wakeup()) {
            wake = APP_WAKE_KEY;
            break;
        }
        read_pico_clear_ioe_int();
        if (gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) == 0) {
            if (!ioe_low_since_us) ioe_low_since_us = now_us;
            if (now_us - ioe_low_since_us >= APP_LOCK_STUCK_IRQ_US) {
                // Return to the active page instead of leaving an inert lock
                // image when a non-key IOE interrupt cannot be cleared.
                // 非按键中断一直清不掉时，退回可操作页面，避免像死机一样停在锁屏。
                ESP_LOGW(TAG, "lock IOE_INT stuck low; recover active page");
                wake = APP_WAKE_KEY;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        ioe_low_since_us = 0;
        if (pickup && sc7a20h_int1_level(acc) > 0 && pickup_ia(acc)) {
            wake = APP_WAKE_PICKUP;
            ESP_LOGI(TAG, "pickup wake");
            break;
        }
        if (pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            pickup_wait_quiet(acc, 400, 1500);
            continue;
        }
        int64_t sleep_us = APP_LOCK_KEY_POLL_US;
        if (deadline_us) {
            int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us < sleep_us) sleep_us = remaining_us;
        }
        if (sleep_us <= 0) continue;
        esp_err_t timer_error = esp_sleep_enable_timer_wakeup((uint64_t)sleep_us);
        if (timer_error != ESP_OK) {
            ESP_LOGW(TAG, "lock sleep timer rejected: %s", esp_err_to_name(timer_error));
            if (++sleep_errors >= APP_LOCK_SLEEP_ERROR_LIMIT) {
                wake = APP_WAKE_KEY;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        esp_err_t sleep_error = esp_light_sleep_start();
        int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        read_pico_clear_ioe_int();
        if (read_pico_pmu_take_key_wakeup()) {
            wake = APP_WAKE_KEY;
            break;
        }
        if (pickup && sc7a20h_int1_level(acc) > 0 && pickup_ia(acc)) {
            if (dt_ms < 80) {
                ESP_LOGW(TAG, "pickup ignored, sleep %lld ms", (long long)dt_ms);
                pickup_ack(acc);
                pickup_wait_quiet(acc, 400, 1500);
                continue;
            }
            wake = APP_WAKE_PICKUP;
            ESP_LOGI(TAG, "pickup wake");
            break;
        }
        if (pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            pickup_wait_quiet(acc, 400, 1500);
        }
        if (sleep_error != ESP_OK) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (sleep_error != last_sleep_error || now_ms - last_sleep_error_log_ms >= 30000) {
                ESP_LOGW(TAG, "lock light sleep rejected: %s, IOE_INT=%d",
                         esp_err_to_name(sleep_error),
                         gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO));
                last_sleep_error_log_ms = now_ms;
            }
            last_sleep_error = sleep_error;
            if (++sleep_errors >= APP_LOCK_SLEEP_ERROR_LIMIT) {
                ESP_LOGE(TAG, "lock light sleep repeatedly rejected; recover active page");
                wake = APP_WAKE_KEY;
                break;
            }
            // A rejected sleep must not spin at full CPU speed behind the static e-paper image.
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            last_sleep_error = ESP_OK;
            sleep_errors = 0;
        }
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    gpio_wakeup_disable((gpio_num_t)READ_PICO_IOE_INT_GPIO);
    if (acc_armed) {
        gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
        sc7a20h_events_t ev;
        sc7a20h_read_events(acc, &ev);
        sc7a20h_power_down(acc);
        sc7a20h_int1_begin(acc);
    }
    periph_resume();
    app_settings_set_last_wake((uint8_t)(wake == APP_WAKE_TIMEOUT ? APP_WAKE_NONE : wake));
    return wake;
}

app_wake_source_t app_light_sleep_wait(sc7a20h_handle_t acc) {
    return app_light_sleep_wait_timed(acc, 0);
}

esp_err_t app_enter_host_sleep(app_sleep_mode_t mode) {
    if (mode != APP_SLEEP_OFF && mode != APP_SLEEP_DEEP) return ESP_ERR_INVALID_ARG;
    // 产品页用 on_before_lock 保存并收齐后台；保留阅读内存以便交接失败仍可回到原页。
    // Product pages checkpoint/join in on_before_lock; retain reader state so a failed handoff can resume the same page.
    if (!(book_cover_jobs_quiesce(2000) && ui_avatar_quiesce(2000)) || !read_pico_transfer_pause_for_sleep()) return ESP_ERR_INVALID_STATE;
    (void)ble_pt_stop(2000);
    (void)app_sync_time_to_pmu();
    // 必须先断面板轨，再拉低 MODE，避免常开逻辑仍耗电。/ Drop panel rails before parking MODE on the always-on supply.
    pmu_selftest_prepare_powerdown();
    periph_suspend(true);
    esp_err_t err = ESP_FAIL;
    const unsigned attempts = mode == APP_SLEEP_OFF ? 1 : 3;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        err = mode == APP_SLEEP_OFF ? read_pico_pmu_power_off() : read_pico_pmu_report_sleep();
        ESP_LOGI(TAG, "%s attempt %u: %s", mode == APP_SLEEP_OFF ? "power off" : "SOFT_SLEEP",
                 attempt + 1, esp_err_to_name(err));
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(HOST_EN_DROP_WAIT_MS));
            err = ESP_ERR_TIMEOUT;
            ESP_LOGE(TAG, "EN still high after PMU ack");
            break;
        }
        if (attempt + 1 < attempts) vTaskDelay(pdMS_TO_TICKS(200));
    }
    // 失败不复位重试；恢复外设，交还锁屏/密码层，阅读恢复检查点仍由原入口处理。
    // Do not reset/retry on failure; restore peripherals to the lock/PIN caller, which retains checkpoint ownership.
    periph_resume();
    read_pico_transfer_resume_after_sleep();
    return err;
}

esp_err_t app_sync_time_to_pmu(void) {
    time_t now = time(NULL);
    if (now < 946684800 || now > 4102444799U) {
        ESP_LOGW(TAG, "skip PMU time sync: system clock invalid");
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t seconds = (uint32_t)now;
    uint8_t payload[4] = {
        (uint8_t)(seconds & 0xff),
        (uint8_t)((seconds >> 8) & 0xff),
        (uint8_t)((seconds >> 16) & 0xff),
        (uint8_t)((seconds >> 24) & 0xff),
    };
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    ESP_LOGI(TAG, "PMU time sync before power action: %s", esp_err_to_name(err));
    return err;
}

void app_restart_host(void) {
    (void)app_sync_time_to_pmu();
    pmu_selftest_prepare_powerdown();
    esp_err_t err = read_pico_pmu_action(PMU_ACTION_HOST_NORMAL_RESTART, 250, 0x5052);
    ESP_LOGI(TAG, "normal restart: %s", esp_err_to_name(err));
    if (err != ESP_OK) esp_restart();
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

void enter_lock_and_sleep(
    EpdiyHighlevelState* hl, int64_t* ignore_until_ms, sc7a20h_handle_t acc, cst836u_handle_t touch,
    bool reader_background
) {
    s_lock_feedback_visible = false;
    // Refuse to interrupt an active upload. The caller repaints its page when
    // this function returns, and the user can lock again after the transfer.
    bool radio_paused = read_pico_transfer_pause_for_sleep();
    if (!radio_paused) {
        pico_boot_clear_resume();
        ESP_LOGW(TAG, "lock postponed while transfer is busy");
        return;
    }
    uint8_t* framebuffer = epd_hl_get_framebuffer(hl);
    uint8_t lock_style = app_settings_lock_style();
    /* 计算器锁屏在没有密码时没有解锁路径（恢复备份/凭据损坏）：退回书封防死锁。 */
    if (lock_style == 3 && !lock_pin_enabled()) lock_style = 2;
    app_lock_font_t lock_font = {0};
    if (lock_pin_enabled()) app_font_begin_lock(&lock_font);
    /* 2026-10-17 三样式：0=时钟模式（替代阅读票根）、1=壁纸、2=透明书封（替代拼贴，
     * 保留当前阅读画面仅覆盖信息面板）。时钟模式永不交 PMU 深睡。 */
    if (lock_style == 2) {
        lock_cover_overlay(framebuffer);                 /* 不清屏，直接叠加面板 */
    } else if (lock_style == 3) {
        /* 计算器锁屏（用户 2026-10-19）：白底计算器画面；全点阵零字体依赖。 */
        epd_hl_set_all_white(hl);
        calc_core_set_unlock_mode(true);
        calc_core_reset();
        calc_core_paint_page(framebuffer);
    } else {
        epd_hl_set_all_white(hl);
        bool lock_drawn = lock_style == 0 ? (app_lock_clock_art(framebuffer), true)
                                          : draw_wallpaper(framebuffer);
        if (!lock_drawn) {
            ui_draw_full_image_z(framebuffer, loading_4bpp_bin_z_start, loading_4bpp_bin_z_end);
        }
    }
    enum EpdDrawError result = update_display_full(hl);
    if (result != EPD_DRAW_SUCCESS) {
        epd_poweron();
        epd_clear();
        result = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
    }
    if (lock_style == 1 && result == EPD_DRAW_SUCCESS) ui_wallpaper_cache_commit(framebuffer);

    app_lock_wait_key_idle(800);
    if (lock_style == 0) {
        /* 时钟模式：浅睡 + 每分钟局部更新点阵时间，不掉 PMU、永不关机（用户 2026-10-17）。 */
        char lock_today[40] = "";
        {
            struct tm tm_init = {0};
            if (lock_now_bj(&tm_init))
                snprintf(lock_today, sizeof(lock_today), "%04d%02d%02d",
                         tm_init.tm_year + 1900, tm_init.tm_mon + 1, tm_init.tm_mday);
        }
        for (;;) {
            app_wake_source_t wake = app_light_sleep_wait_timed(acc, lock_clock_next_tick_ms());
            if (wake != APP_WAKE_TIMEOUT) break;
            if (lock_pin_enabled()) { break; }   /* 密码启用时保持静态画面，验证交还主循环 */
            struct tm tm_chk = {0};
            bool day_flip = false;
            if (lock_now_bj(&tm_chk)) {
                char today[40];
                snprintf(today, sizeof(today), "%04d%02d%02d",
                         tm_chk.tm_year + 1900, tm_chk.tm_mon + 1, tm_chk.tm_mday);
                day_flip = lock_today[0] != 0 && strcmp(today, lock_today) != 0;
                snprintf(lock_today, sizeof(lock_today), "%s", today);
            }
            /* v2 功耗（用户 2026-10-19）：常规走时只轻量重绘点阵时钟条（跳过整页 TTF/
             * 黄历计算），DU 阈值刷（点阵纯黑白目标，无损）——扫描时间≈GL16 的 1/3，
             * 轨上电时间与 CPU 唤醒窗口同步缩短。全页 DU（用户许可）：差分裁剪后
             * 实际扫描仍只含变化像素，条外意外脏像素也会被一并收敛；跨日同全页 DU。 */
            EpdRect area = (EpdRect){0, 0, 684, 1216};
            if (day_flip) app_lock_clock_art(framebuffer);
            else lock_clock_minute_paint(framebuffer);
            guard_draw_result(hl, update_display_area_diff_with(hl, &E0470_WAVEFORM, MODE_DU, area));
        }
    } else if (lock_style == 3) {
        /* 计算器锁屏：浅睡等电源键/拿起唤醒；唤醒后计算器可用（正常计算照常），
         * 恰四位数字按＝验证 lock_pin 解锁；空闲或再按电源键回浅睡，超时深睡。 */
        (void)ble_pt_stop(2000);
        for (;;) {
            app_wake_source_t wake = app_light_sleep_wait_timed(acc, APP_LOCK_LIGHT_SLEEP_MS);
            if (wake == APP_WAKE_TIMEOUT) {
                app_enter_host_sleep(APP_SLEEP_DEEP);
                /* 深睡交接意外返回时保持锁定，稍候重试而不是忙转。 */
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (lock_calc_session(hl, touch)) break;
            /* 回浅睡前恢复整幅初始计算器（清输入与局部 DU 残影）。 */
            calc_core_reset();
            calc_core_paint_page(framebuffer);
            guard_draw_result(hl, update_display_full(hl));
        }
    } else {
        app_wake_source_t wake = APP_WAKE_KEY;
        if (lock_pin_enabled()) lock_screen_authenticate(hl, touch, acc, false);
        else wake = app_light_sleep_wait_timed(acc, APP_LOCK_LIGHT_SLEEP_MS);
        if (wake == APP_WAKE_TIMEOUT) app_enter_host_sleep(APP_SLEEP_DEEP);
    }
    pico_boot_clear_resume();
    if (lock_pin_enabled()) app_font_end_lock(&lock_font);
    read_pico_transfer_resume_after_sleep();

    // 参考帧和屏幕都归零，回到主循环后由当前页自己画一遍，不必知道是哪一页。
    // Zero the reference frame and the panel; the current page redraws after the loop resumes, without knowing which page it is.
    read_pico_pmu_drain_events();
    epd_poweron();
    /* 2026-10-17：唤醒不再 epd_clear + set_all_white（那又是一次白闪）。
     * 保留锁屏画面为 highlevel 参考帧；主循环的 display_request_full_refresh +
     * 页面呈现以单次 GC16 从锁屏图收敛到当前页。 */
    read_pico_pmu_drain_events();
    if (ignore_until_ms) {
        *ignore_until_ms = esp_timer_get_time() / 1000 + APP_LOCK_IGNORE_BOOT_MS;
    }
    ESP_LOGI(TAG, "unlocked");
}

void app_lock_boot_gate(EpdiyHighlevelState *hl, cst836u_handle_t touch, sc7a20h_handle_t acc) {
    if (!lock_pin_enabled()) return;
    // 卡挂载有界等待仅为了字体/锁屏图；没有卡仍必须验证。
    // Bound mount waiting for the face/art only; absent media still requires authentication.
    read_pico_sd_info_t info;
    for (unsigned i = 0; i < 150 && read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED; ++i)
        vTaskDelay(pdMS_TO_TICKS(20));
    app_lock_font_t saved; app_font_begin_lock(&saved);
    uint8_t *fb = epd_hl_get_framebuffer(hl);
    /* v2（用户 2026-10-19 定稿）：计算器锁屏的开机门控=计算器画面本身——
     * 与锁屏态同构（白底计算器、正常计算照常、恰四位数字按＝解锁）。
     * 全点阵零字体依赖，font_begin/end 仅对其他样式生效。 */
    if (app_settings_lock_style() == 3) {
        calc_core_set_unlock_mode(true);
        calc_core_reset();
        calc_core_paint_page(fb);
        guard_draw_result(hl, update_display_full(hl));
        /* 与 lock_screen_authenticate 的 boot 分支同构：开机门控等待不是启动失败，
         * 保留一次性恢复不提前开书——验证成功返回后由 app_main 取走恢复上次阅读。 */
        pico_boot_ready();
        pico_boot_hold_resume();
        for (;;) {
            if (lock_calc_session(hl, touch)) break;
            /* 开机门控无超时概念：会话退出（电源键/空闲）即重画初始计算器再验证。 */
            calc_core_reset();
            calc_core_paint_page(fb);
            guard_draw_result(hl, update_display_full(hl));
        }
        read_pico_pmu_drain_events();
        return;
    }
    if (!lock_screen_restore(fb)) {
        if (!app_lock_boot_art(fb)) {
            memset(fb, 255, (size_t)epd_width() * epd_height() / 2);
            ui_draw_full_image_z(fb, loading_4bpp_bin_z_start, loading_4bpp_bin_z_end);
        }
    }
    lock_screen_authenticate(hl, touch, acc, true);
    app_font_end_lock(&saved);
}
