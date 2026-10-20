/*
 * Nothing 风格 UI 设计系统实现。仅绘制，无副作用；几何参数逐项对应设计稿。
 */
#include "ui_nothing.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_avatar.h"
#include "ui_kit.h"

/* ================= 点阵字模（5×7，与设计稿 GLYPH 一致） ================= */
static const char k_glyph[24][7][6] = {
    /* 0 */ {"01110","10001","10011","10101","11001","10001","01110"},
    /* 1 */ {"00100","01100","00100","00100","00100","00100","01110"},
    /* 2 */ {"01110","10001","00001","00110","01000","10000","11111"},
    /* 3 */ {"11110","00001","00001","01110","00001","00001","11110"},
    /* 4 */ {"00010","00110","01010","10010","11111","00010","00010"},
    /* 5 */ {"11111","10000","11110","00001","00001","10001","01110"},
    /* 6 */ {"00110","01000","10000","11110","10001","10001","01110"},
    /* 7 */ {"11111","00001","00010","00100","01000","01000","01000"},
    /* 8 */ {"01110","10001","10001","01110","10001","10001","01110"},
    /* 9 */ {"01110","10001","10001","01111","00001","00010","01100"},
    /* : */ {"00000","00110","00110","00000","00110","00110","00000"},
    /* % */ {"11001","11010","00010","00100","01000","01011","10011"},
    /* / */ {"00001","00010","00010","00100","01000","01000","10000"},
    /* ° */ {"01110","10001","10001","01110","00000","00000","00000"},
    /* + */ {"00000","00100","00100","11111","00100","00100","00000"},
    /* − */ {"00000","00000","00000","11111","00000","00000","00000"},
    /* × */ {"10001","01010","00100","01010","10001","00000","00000"},
    /* ÷ */ {"00000","00100","00000","11111","00000","00100","00000"},
    /* = */ {"00000","00000","11111","00000","11111","00000","00000"},
    /* . */ {"00000","00000","00000","00000","00000","00110","00110"},
    /* C */ {"01110","10001","10000","10000","10000","10001","01110"},
    /* ± */ {"00000","00100","00100","11111","00100","00100","11111"},
    /* ⌫ */ {"00000","00100","01000","11111","01000","00100","00000"},
    /* 空 */ {"00000","00000","00000","00000","00000","00000","00000"},
};
static int glyph_index(unsigned char c) {
    switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return c - '0';
        case ':': return 10;
        case '%': return 11;
        case '/': return 12;
        case '+': return 14;
        case '-': return 15;
        case '=': return 18;
        case '.': return 19;
        case 'C': return 20;
        default: return 23;
    }
}
/* 多字节运算符（× ÷ − ± ⌫）→ 点阵槽位与字节宽。返回 0 表示未知字符。 */
static int glyph_index_bytes(const unsigned char* s, int* adv) {
    if (s[0] == 0xC2 && s[1] == 0xB0) { *adv = 2; return 13; }  /* ° */
    if (s[0] == 0xC2 && s[1] == 0xB1) { *adv = 2; return 21; }  /* ± */
    if (s[0] == 0xC3 && s[1] == 0x97) { *adv = 2; return 16; }  /* × */
    if (s[0] == 0xC3 && s[1] == 0xB7) { *adv = 2; return 17; }  /* ÷ */
    if (s[0] == 0xE2 && s[1] == 0x88 && s[2] == 0x92) { *adv = 3; return 15; }  /* − */
    if (s[0] == 0xE2 && s[1] == 0x8C && s[2] == 0xAB) { *adv = 3; return 22; }  /* ⌫ */
    *adv = 1;
    return glyph_index(s[0]);
}

int nothing_dot_text_w(const char* str, int pitch) {
    int n = 0;
    const unsigned char* s = (const unsigned char*)str;
    while (*s) {
        int adv;
        (void)glyph_index_bytes(s, &adv);
        s += adv;
        ++n;
    }
    return n ? (n * 5 + n - 1) * pitch : 0;
}

int nothing_dot_text(uint8_t* fb, int x, int y_top, const char* str,
                     int dot_r, int pitch, uint8_t color) {
    int cx = x;
    const unsigned char* s = (const unsigned char*)str;
    while (*s) {
        int adv;
        int gi = glyph_index_bytes(s, &adv);
        for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
            if (k_glyph[gi][gy][gx] == '1')
                epd_fill_circle(cx + gx * pitch + pitch / 2,
                                y_top + gy * pitch + pitch / 2, dot_r, color, fb);
        }
        cx += 6 * pitch;
        s += adv;
    }
    return cx - pitch;
}

/* 横屏版（v1.8 时钟锁屏）：逻辑横屏 (x∈[0,1216), y∈[0,684))，物理像素 (683-y, x)。
 * 字模与竖屏共用；圆点在 90° 旋转下不变，仅圆心做映射。 */
int nothing_dot_text_rot(uint8_t* fb, int x, int y_top, const char* str,
                         int dot_r, int pitch, uint8_t color) {
    int cx = x;
    const unsigned char* s = (const unsigned char*)str;
    while (*s) {
        int adv;
        int gi = glyph_index_bytes(s, &adv);
        for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
            if (k_glyph[gi][gy][gx] == '1')
                epd_fill_circle(683 - (y_top + gy * pitch + pitch / 2),
                                cx + gx * pitch + pitch / 2, dot_r, color, fb);
        }
        cx += 6 * pitch;
        s += adv;
    }
    return cx - pitch;
}

/* ================= 粗线几何 ================= */
void nothing_thick_line(uint8_t* fb, int x0, int y0, int x1, int y1,
                        int w, uint8_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int dist = (int)sqrtf((float)(dx * dx + dy * dy));
    int steps = dist > 0 ? dist : 1;
    for (int i = 0; i <= steps; ++i) {
        int x = x0 + (int)((int64_t)dx * i / steps);
        int y = y0 + (int)((int64_t)dy * i / steps);
        epd_fill_circle(x, y, w / 2, color, fb);
    }
}

void nothing_thick_poly(uint8_t* fb, const int* pts, int n, int w, uint8_t color) {
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        nothing_thick_line(fb, pts[i * 2], pts[i * 2 + 1],
                           pts[j * 2], pts[j * 2 + 1], w, color);
    }
}

/* 圆弧（角度制，参数采样）。仅内部使用。 */
static void arc_deg(uint8_t* fb, int cx, int cy, int r,
                    int a0, int a1, int w, uint8_t color) {
    int radius = w / 2;
    epd_fill_circle(cx + (int)(cosf(a0 * 0.01745f) * r),
                    cy + (int)(sinf(a0 * 0.01745f) * r), radius, color, fb);
    for (int a = a0; a <= a1; a += 8) {
        int x = cx + (int)(cosf(a * 0.01745f) * r);
        int y = cy + (int)(sinf(a * 0.01745f) * r);
        epd_fill_circle(x, y, radius, color, fb);
    }
}

/* ================= 状态栏 ================= */

extern const uint8_t fox_avatar_bin_start[] asm("_binary_fox_avatar_bin_start");

static EXT_RAM_BSS_ATTR uint8_t s_status_avatar[82 * 82];
static EXT_RAM_BSS_ATTR uint8_t s_fox_avatar_gray[82 * 82];
static bool s_fox_avatar_expanded;

// 默认身份头像：内嵌 4bpp 狐狸（epdiy 约定 15=白 0=黑，高半字节在前）首用展开为 8bpp 灰度。
// Default identity avatar: lazily expand the embedded 4bpp fox into 8-bit gray.
const uint8_t* nothing_default_avatar82(void) {
    if (!s_fox_avatar_expanded) {
        for (int i = 0; i < 82 * 82 / 2; ++i) {
            uint8_t b = fox_avatar_bin_start[i];
            s_fox_avatar_gray[i * 2]     = (uint8_t)((b >> 4) * 17);
            s_fox_avatar_gray[i * 2 + 1] = (uint8_t)((b & 0x0f) * 17);
        }
        s_fox_avatar_expanded = true;
    }
    return s_fox_avatar_gray;
}

/* 身份头像：设置的自定义头像（ui_avatar 82 槽，与设置页同槽不互斥），无则默认狐狸。
 * request/poll 对同路径为廉价 no-op（READY 不重启解码），渲染路径直调安全。 */
static const uint8_t* status_avatar_source(void) {
    const char* path = app_settings_avatar_path();
    if (path[0]) {
        ui_avatar_request(path, 82);
        if (ui_avatar_poll(path, 82, s_status_avatar) == UI_AVATAR_READY) return s_status_avatar;
    }
    return nothing_default_avatar82();
}

bool nothing_statusbar_avatar_pending(void) {
    const char* path = app_settings_avatar_path();
    if (!path[0]) return false;
    ui_avatar_request(path, 82);
    return ui_avatar_poll(path, 82, s_status_avatar) == UI_AVATAR_PENDING;
}

/* 身份头像位：38x38 圆角（r=8，82 源最近邻采样），垂直在 40px 状态栏内居中；
 * 快刷模式下与设置页头像一致转稳定点阵。用户 2026-10-17：34→38 再放大。 */
void nothing_blit_status_avatar(uint8_t* fb, int x, int y) {
    const uint8_t* src = status_avatar_source();
    for (int r = 0; r < 38; ++r) {
        for (int c = 0; c < 38; ++c) {
            int dx = c < 8 ? 8 - c : c >= 30 ? c - 29 : 0;
            int dy = r < 8 ? 8 - r : r >= 30 ? r - 29 : 0;
            if (dx * dx + dy * dy > 64) continue;
            unsigned sx = (unsigned)c * 82 / 38, sy = (unsigned)r * 82 / 38;
            epd_draw_pixel(x + c, y + r, src[sy * 82 + sx], fb);
        }
    }
    if (app_settings_main_fast_refresh()) ui_image_bw_rect(fb, (EpdRect){x, y, 38, 38});
}

void nothing_draw_statusbar(uint8_t* fb) {
    /* 电池（rc95 软件油量表）：右侧外框 + 容量条 + 右耳 + 百分比，24px 固定字号。
     * 先算占位，设备名宽度预算以电池左沿为界。 */
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    char pct[8] = "--";
    int gauge = pmu_battery_percent(pmu);
    unsigned soc = 100;
    if (gauge >= 0) {
        soc = (unsigned)gauge;
        if (soc > 100) soc = 100;
        snprintf(pct, sizeof(pct), "%u%%", soc);
    }
    int text_w = ui_text_fixed_width_px(24, pct);
    int bx = 684 - NX_MARGIN - text_w - 56;
    int by = NX_STATUS_Y + 8;
    ui_draw_round_rect(fb, (EpdRect){bx, by, 40, 24}, 5, NX8_INK);
    epd_fill_rect((EpdRect){bx + 42, by + 7, 5, 10}, NX8_INK, fb);
    int fill_w = (int)((40 - 6) * soc / 100);
    epd_fill_rect((EpdRect){bx + 3, by + 3, fill_w < 0 ? 0 : fill_w, 18}, NX8_INK, fb);
    ui_text_fixed(fb, 684 - NX_MARGIN, NX_STATUS_Y + 8, 24, pct, EPD_DRAW_ALIGN_RIGHT, false);
    /* 左上身份：设置的头像（34px，栏内最大合理档）+ 设备名（28px，同理由 24 放大）。
     * 名称为设置值（默认 take-easy），超宽按码点截断，不越过电池。 */
    nothing_blit_status_avatar(fb, NX_MARGIN, NX_STATUS_Y + 1);
    char name[64];
    snprintf(name, sizeof(name), "%s", app_settings_device_name());
    if (!name[0]) snprintf(name, sizeof(name), "take-easy");
    int x = NX_MARGIN + 34 + 12;
    int budget = bx - 16 - x;
    while (*name && ui_text_fixed_width_px(28, name) > budget) {
        size_t n = strlen(name) - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
        name[n] = 0;
    }
    if (name[0]) ui_text_fixed(fb, x, NX_STATUS_Y + 5, 28, name, EPD_DRAW_ALIGN_LEFT, false);
    /* 状态栏下墨色 hairline（设计稿 2px ink） */
    epd_fill_rect((EpdRect){0, NX_STATUS_Y + NX_STATUS_H + 2, 684, 2}, NX8_INK, fb);
}

/* ================= 底部导航 ================= */
/* 盘内图标：以盘心 (cx, cy) 画。 */
static void icon_home(uint8_t* fb, int cx, int cy, uint8_t ink, uint8_t hole) {
    /* 2026-10-17：主页盘直接引用头像原图（圆盘裁剪，最近邻采样 82 源），
     * 点阵化效果经真机确认不佳已删除；激活态由盘外框强调，头像本身不变。 */
    (void)ink; (void)hole;
    const uint8_t* src = status_avatar_source();
    const int r = 29;
    const int size = r * 2;
    for (int row = -r; row < r; ++row) {
        for (int col = -r; col < r; ++col) {
            if (col * col + row * row > r * r) continue;   /* 圆形裁剪 */
            unsigned sx = (unsigned)(col + r) * 82 / size;
            unsigned sy = (unsigned)(row + r) * 82 / size;
            epd_draw_pixel(cx + col, cy + row, src[sy * 82 + sx], fb);
        }
    }
    if (app_settings_main_fast_refresh())
        ui_image_bw_rect(fb, (EpdRect){cx - r, cy - r, size, size});
}

static void icon_library(uint8_t* fb, int cx, int cy, uint8_t ink, uint8_t hole) {
    (void)hole;
    int x0 = cx - 27, y0 = cy - 27;
    /* 上排两本高书 */
    ui_fill_round_rect(fb, (EpdRect){x0 + 10, y0 + 7, 14, 20}, 4, ink);
    ui_fill_round_rect(fb, (EpdRect){x0 + 29, y0 + 10, 14, 18}, 4, ink);
    /* 下排三本矮书 */
    ui_fill_round_rect(fb, (EpdRect){x0 + 7, y0 + 32, 13, 14}, 4, ink);
    ui_fill_round_rect(fb, (EpdRect){x0 + 25, y0 + 32, 13, 14}, 4, ink);
    ui_fill_round_rect(fb, (EpdRect){x0 + 42, y0 + 32, 7, 14}, 3, ink);
}

static void icon_apps(uint8_t* fb, int cx, int cy, uint8_t ink, uint8_t hole) {
    int d = 10; /* 半对角长 ×1.2 */
    nothing_thick_line(fb, cx - d, cy - d, cx + d, cy + d, 13, ink);
    nothing_thick_line(fb, cx - d, cy + d, cx + d, cy - d, 13, ink);
    /* 右下白点（贴边，选中态为 steel） */
    epd_fill_circle(cx + 8, cy + 8, 5, hole, fb);
}

static void icon_settings(uint8_t* fb, int cx, int cy, uint8_t ink, uint8_t hole) {
    /* 螺母：六边形粗描边 + 中心孔（坐标 ×1.2） */
    const int pts[] = {0, -23, 20, -11, 20, 11, 0, 23, -20, 11, -20, -11};
    int poly[12];
    for (int i = 0; i < 6; ++i) { poly[i * 2] = cx + pts[i * 2]; poly[i * 2 + 1] = cy + pts[i * 2 + 1]; }
    nothing_thick_poly(fb, poly, 6, 7, ink);
    epd_fill_circle(cx, cy, 8, ink, fb);
    epd_fill_circle(cx, cy, 6, hole, fb);
}

void nothing_draw_nav(uint8_t* fb, int active) {
    /* v1（用户 2026-10-17）：全线 2px 纯黑——顶部分割线与四盘边框统一。 */
    epd_fill_rect((EpdRect){0, NX_NAV_TOP, 684, 2}, NX8_INK, fb);
    const int cell = 684 / 4;
    for (int i = 0; i < 4; ++i) {
        int cx = cell * i + cell / 2;
        int cy = NX_NAV_TOP + NX_NAV_H / 2;
        bool on = i == active;
        EpdRect disc = {cx - NX_DISC / 2, cy - NX_DISC / 2, NX_DISC, NX_DISC};
        if (on) {
            if (i == 0) {   /* 主页盘：白底头像 + 黑边 */
                ui_fill_round_rect(fb, disc, NX_DISC_R, NX8_PAPER);
                ui_draw_control_frame(fb, disc, NX_DISC_R, NX8_INK);
            } else ui_fill_round_rect(fb, disc, NX_DISC_R, NX8_INK);
        } else ui_draw_control_frame(fb, disc, NX_DISC_R, NX8_INK);
        uint8_t ink = (on && i != 0) ? NX8_PAPER : NX8_INK;
        uint8_t hole = on ? NX8_STEEL : NX8_PAPER;
        switch (i) {
            case 0: icon_home(fb, cx, cy, ink, hole); break;
            case 1: icon_library(fb, cx, cy, ink, hole); break;
            case 2: icon_apps(fb, cx, cy, ink, hole); break;
            default: icon_settings(fb, cx, cy, ink, hole); break;
        }
    }
}

int nothing_nav_hit(uint16_t x, uint16_t y) {
    if (y < NX_NAV_TOP + 2 || x >= 684) return -1;
    return (int)(x / (684 / 4));
}

/* ================= 点阵天气图样（Feather sun 公版几何） ================= */
void nothing_draw_sun(uint8_t* fb, int x, int y, int size) {
    int s = size / 24;
    if (s < 1) s = 1;
    int cx = x + size / 2, cy = y + size / 2;
    int w = 2 * s; /* 设计稿线宽 1.9 ≈ 2vb */
    /* 中心盘：外圈 ink、内圈 paper 得到粗环 */
    epd_fill_circle(cx, cy, 5 * s, NX8_INK, fb);
    epd_fill_circle(cx, cy, 5 * s - w, NX8_PAPER, fb);
    /* 8 条放射线（r 9s→11s，正四向 + 对角） */
    static const int v8[8][2] = {{1000, 0}, {707, 707}, {0, 1000}, {-707, 707},
                                 {-1000, 0}, {-707, -707}, {0, -1000}, {707, -707}};
    for (int k = 0; k < 8; ++k) {
        int ax = cx + v8[k][0] * 9 * s / 1000;
        int ay = cy + v8[k][1] * 9 * s / 1000;
        int bx = cx + v8[k][0] * 11 * s / 1000;
        int by = cy + v8[k][1] * 11 * s / 1000;
        nothing_thick_line(fb, ax, ay, bx, by, w, NX8_INK);
    }
}

/* ================= 实况天气图标族（用户 2026-10-19）=================
 * 统一 size 见方（建议 120），Feather 公版造型几何化：多云=日+云、阴=云、
 * 雾=三横条、雨=云+斜短线、雪=云+小圆点。线宽随 size 缩放（s=size/24）。 */
static void wx_cloud(uint8_t* fb, int cx, int cy, int s, int w) {
    /* 云底：三圆并排 + 底部矩形封平。 */
    epd_fill_circle(cx - 3 * s, cy, 2 * s, NX8_INK, fb);
    epd_fill_circle(cx, cy - s, 3 * s, NX8_INK, fb);
    epd_fill_circle(cx + 3 * s, cy, 2 * s, NX8_INK, fb);
    /* 内缩挖空成描边云。 */
    epd_fill_circle(cx - 3 * s, cy, 2 * s - w, NX8_PAPER, fb);
    epd_fill_circle(cx, cy - s, 3 * s - w, NX8_PAPER, fb);
    epd_fill_circle(cx + 3 * s, cy, 2 * s - w, NX8_PAPER, fb);
    /* 底边封口（内外双线）。 */
    nothing_thick_line(fb, cx - 4 * s, cy, cx + 4 * s, cy, w, NX8_INK);
    nothing_thick_line(fb, cx - 3 * s + 1, cy + w, cx + 3 * s - 1, cy + w, 1, NX8_PAPER);
}

void nothing_draw_weather(uint8_t* fb, int x, int y, int size, int code) {
    int s = size / 24;
    if (s < 1) s = 1;
    int w = 2 * s;
    int cx = x + size / 2, cy = y + size / 2;
    switch (code) {
        case 1: { /* 多云：小日盘右上 + 云左下 */
            int sx = cx + 3 * s, sy = cy - 3 * s, rs = 2 * s;
            epd_fill_circle(sx, sy, rs, NX8_INK, fb);
            epd_fill_circle(sx, sy, rs - w, NX8_PAPER, fb);
            static const int v8[8][2] = {{1000, 0}, {707, 707}, {0, 1000}, {-707, 707},
                                         {-1000, 0}, {-707, -707}, {0, -1000}, {707, -707}};
            for (int k = 0; k < 8; ++k) {
                int ax = sx + v8[k][0] * (rs + s) / 1000;
                int ay = sy + v8[k][1] * (rs + s) / 1000;
                int bx = sx + v8[k][0] * (rs + 2 * s) / 1000;
                int by = sy + v8[k][1] * (rs + 2 * s) / 1000;
                nothing_thick_line(fb, ax, ay, bx, by, w, NX8_INK);
            }
            wx_cloud(fb, cx - s, cy + 2 * s, s, w);
            break;
        }
        case 2: /* 阴：单云居中 */
            wx_cloud(fb, cx, cy, s + 1, w);
            break;
        case 3: { /* 雾：云 + 下方两条横杠 */
            wx_cloud(fb, cx, cy - 2 * s, s, w);
            nothing_thick_line(fb, cx - 4 * s, cy + 3 * s, cx + 4 * s, cy + 3 * s, w, NX8_INK);
            nothing_thick_line(fb, cx - 3 * s, cy + 5 * s, cx + 3 * s, cy + 5 * s, w, NX8_INK);
            break;
        }
        case 4: { /* 雨：云 + 三条斜短线 */
            wx_cloud(fb, cx, cy - 2 * s, s, w);
            for (int k = -1; k <= 1; ++k)
                nothing_thick_line(fb, cx + k * 3 * s, cy + 2 * s,
                                   cx + k * 3 * s - s, cy + 5 * s, w, NX8_INK);
            break;
        }
        case 5: { /* 雪：云 + 三列小圆点 */
            wx_cloud(fb, cx, cy - 2 * s, s, w);
            for (int k = -1; k <= 1; ++k) {
                epd_fill_circle(cx + k * 3 * s, cy + 3 * s, s, NX8_INK, fb);
                epd_fill_circle(cx + k * 3 * s - s / 2, cy + 5 * s, s, NX8_INK, fb);
            }
            break;
        }
        case 0: /* 晴：原太阳 */
        default:
            nothing_draw_sun(fb, x, y, size);
            break;
    }
}

/* ================= 书卡 ================= */
EpdRect nothing_draw_book_card(uint8_t* fb, EpdRect rect, const char* name,
                                unsigned pct, bool has_pct, bool cover_dark,
                                bool focused) {
    uint8_t edge = NX8_INK;   /* v1：线条统一 2px 纯黑，不再按焦点区分灰度 */
    EpdRect inner = {rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4};
    /* 粗外框：先实心圆角块再内缩铺纸 */
    ui_fill_round_rect(fb, rect, NX_R_CARD, edge);
    ui_fill_round_rect(fb, inner, NX_R_CARD - 2, NX8_PAPER);
    /* 封面色块：上半部分（遮挡区以上）整块填底色 */
    int scrim_h = 130;
    EpdRect cover = {inner.x, inner.y, inner.width, inner.height - scrim_h};
    ui_fill_round_rect(fb, cover, NX_R_CARD - 2, cover_dark ? NX8_INK : NX8_GRID);
    if (!cover_dark) { /* 灰底卡片再描一圈墨色细边增强对比 */
        ui_draw_round_rect(fb, cover, NX_R_CARD - 2, NX8_INK);
    }
    /* 书名居中于封面（30px，超宽截断） */
    char line[96];
    snprintf(line, sizeof(line), "%s", name);
    int px = 30;
    while (line[0] && ttf_text_width_px(px, line) > cover.width - 24 && px > 18) --px;
    if (line[0] && ttf_text_width_px(px, line) > cover.width - 24) {
        /* 截断到 UTF8 边界 */
        size_t n = strlen(line) - 1;
        while (n && ((unsigned char)line[n] & 0xc0) == 0x80) --n;
        line[n] = 0;
    }
    ui_text_vc(fb, cover.x + cover.width / 2, cover.y + cover.height / 2, px,
               line, EPD_DRAW_ALIGN_CENTER, cover_dark);
    /* 遮挡区：浅灰底 + 6px 网点 + 顶边分隔 */
    EpdRect scrim = {inner.x, inner.y + inner.height - scrim_h, inner.width, scrim_h};
    ui_fill_round_rect(fb, scrim, NX_R_CARD - 2, NX8_SCRIM);
    epd_fill_rect((EpdRect){scrim.x, scrim.y, scrim.width, 2}, NX8_INK, fb);
    for (int gy = scrim.y + 10; gy < scrim.y + scrim_h - 10; gy += 6) {
        for (int gx = scrim.x + 10; gx < scrim.x + scrim.width - 10; gx += 6) {
            if (((gx / 6) + (gy / 6)) % 2 == 0) epd_draw_pixel(gx, gy, NX8_STEEL, fb);
        }
    }
    char small[96];
    snprintf(small, sizeof(small), "%s", name);
    int spx = 19;
    while (small[0] && ttf_text_width_px(spx, small) > scrim.width - 32) {
        size_t n = strlen(small) - 1;
        while (n && ((unsigned char)small[n] & 0xc0) == 0x80) --n;
        small[n] = 0;
    }
    ui_text(fb, scrim.x + 16, scrim.y + 12, 19, small, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, scrim.x + 16, scrim.y + 48, 13,
            has_pct ? "上次阅读到这里" : "尚未开始阅读", EPD_DRAW_ALIGN_LEFT, false);
    /* 进度行：左点阵数字 + 右百分比 */
    char num[8];
    snprintf(num, sizeof(num), "%u", pct);
    nothing_dot_text(fb, scrim.x + 16, scrim.y + 78, num, 2.4 > 2 ? 2 : 2, 5, NX8_INK);
    char pct_txt[10];
    snprintf(pct_txt, sizeof(pct_txt), "%u%%", pct);
    ui_text(fb, scrim.x + scrim.width - 16, scrim.y + 78, 11, pct_txt, EPD_DRAW_ALIGN_RIGHT, false);
    return scrim;
}

/* ================= 应用盘 / chips / 虚线框 ================= */
static void app_icon_files(uint8_t* fb, int cx, int cy, uint8_t ink) {
    int w = 5, x0 = cx - 19, y0 = cy - 20, x1 = cx + 19, y1 = cy + 20;
    nothing_thick_line(fb, x0 + 3, y0, x1 - 8, y0, w, ink);
    nothing_thick_line(fb, x1 - 8, y0, x1, y0 + 8, w, ink);
    nothing_thick_line(fb, x1, y0 + 8, x1, y1, w, ink);
    nothing_thick_line(fb, x1, y1, x0, y1, w, ink);
    nothing_thick_line(fb, x0, y1, x0, y0, w, ink);
    nothing_thick_line(fb, x1 - 8, y0, x1 - 8, y0 + 8, w, ink);
    nothing_thick_line(fb, x0 + 7, y0 + 12, x1 - 7, y0 + 12, w, ink);
    nothing_thick_line(fb, x0 + 7, y0 + 19, x1 - 7, y0 + 19, w, ink);
    nothing_thick_line(fb, x0 + 7, y0 + 26, x0 + 17, y0 + 26, w, ink);
}

static void app_icon_transfer(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 信号弧 + 上下小天线（近似设计稿传输造型） */
    arc_deg(fb, cx, cy + 2, 14, 200, 340, 5, ink);
    arc_deg(fb, cx, cy + 2, 8, 200, 340, 5, ink);
    nothing_thick_line(fb, cx - 2, cy - 2, cx - 2, cy + 10, 5, ink);
    nothing_thick_line(fb, cx + 2, cy - 2, cx + 2, cy + 10, 5, ink);
}

static void app_icon_weread(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 实心云：两圆 + 底部连接块 */
    epd_fill_circle(cx - 8, cy - 4, 8, ink, fb);
    epd_fill_circle(cx + 6, cy + 1, 11, ink, fb);
    epd_fill_rect((EpdRect){cx - 8, cy - 4, 22, 15}, ink, fb);
}

static void app_icon_calc(uint8_t* fb, int cx, int cy, uint8_t ink) {
    /* 计算器：圆角外框 + 顶部显示条 + 右下小方块 */
    EpdRect body = {cx - 17, cy - 21, 34, 42};
    ui_fill_round_rect(fb, body, 5, ink);
    ui_fill_round_rect(fb, (EpdRect){body.x + 2, body.y + 2, body.width - 4, body.height - 4}, 3, NX8_PAPER);
    ui_fill_round_rect(fb, (EpdRect){body.x + 5, body.y + 5, 24, 7}, 2, ink);
    ui_fill_round_rect(fb, (EpdRect){body.x + 5, body.y + 26, 7, 7}, 2, ink);
}

void nothing_draw_app_disc(uint8_t* fb, int x, int y, int icon) {
    EpdRect disc = {x, y, NX_DISC, NX_DISC};
    ui_draw_round_rect(fb, disc, NX_DISC_R, NX8_GRID);
    int cx = x + NX_DISC / 2, cy = y + NX_DISC / 2;
    switch (icon) {
        case 0: app_icon_files(fb, cx, cy, NX8_INK); break;
        case 1: app_icon_transfer(fb, cx, cy, NX8_INK); break;
        case 2: app_icon_weread(fb, cx, cy, NX8_INK); break;
        default: app_icon_calc(fb, cx, cy, NX8_INK); break;
    }
}

void nothing_draw_chip(uint8_t* fb, EpdRect rect, const char* label, bool on) {
    int r = rect.height / 2;
    ui_fill_round_rect(fb, rect, r, on ? NX8_INK : NX8_GRID);
    ui_fill_round_rect(fb, (EpdRect){rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4},
                       r - 2, NX8_PAPER);
    ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2, 15,
               label, EPD_DRAW_ALIGN_CENTER, false);
    if (on) ui_fill_round_rect(fb, (EpdRect){rect.x + rect.width - 9, rect.y - 1, 10, 10}, 3, NX8_STEEL);
}

void nothing_draw_dashed_box(uint8_t* fb, EpdRect rect, const char* label) {
    const int seg = 8, gap = 6;
    for (int x = rect.x; x < rect.x + rect.width - seg; x += seg + gap) {
        epd_fill_rect((EpdRect){x, rect.y, seg, 2}, NX8_GRID, fb);
        epd_fill_rect((EpdRect){x, rect.y + rect.height - 2, seg, 2}, NX8_GRID, fb);
    }
    for (int y = rect.y; y < rect.y + rect.height - seg; y += seg + gap) {
        epd_fill_rect((EpdRect){rect.x, y, 2, seg}, NX8_GRID, fb);
        epd_fill_rect((EpdRect){rect.x + rect.width - 2, y, 2, seg}, NX8_GRID, fb);
    }
    ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2, 15,
               label, EPD_DRAW_ALIGN_CENTER, false);
}
