/*
 * game_host_ui.c — 插件共享绘制原语（逻辑竖屏 684×1216，8bit 灰+抖动）。
 *
 * 全部原语无状态、无副作用（整屏强刷可安全复用）。视觉沿用 AVG 版式：
 * 对话框顶线 3px 纯黑、说话人黑底白字标签、按钮 14px 圆角、打字机折行
 * 中文=2 宽度单位。所有插件（含未来的数织/棋类）从这里拼界面，全线
 * 游戏观感一致，插件间绝不允许各画各的。
 */
#include <string.h>

#include "epdiy.h"
#include "ui_image_dither.h"
#include "ui_kit.h"

#include "game_core.h"

#define GC_INK  0x00
#define GC_HAIR 0xc2

void gc_ui_clear(uint8_t* fb) { ui_clear_page(fb); }

void gc_ui_dialog_frame(uint8_t* fb, int top) {
    ui_fill_round_rect(fb, (EpdRect){0, top, 684, 1216 - top}, 0, 0xff);
    epd_fill_rect((EpdRect){0, top + 6, 684, 3}, GC_INK, fb);
}

void gc_ui_name_tag(uint8_t* fb, int top, const char* name) {
    if (!name || !name[0]) return;
    ui_fill_round_rect(fb, (EpdRect){48, top + 20, 176, 46}, 10, GC_INK);
    ui_text_vc(fb, 136, top + 43, 22, name, EPD_DRAW_ALIGN_CENTER, true);
}

void gc_ui_button(uint8_t* fb, EpdRect r, const char* label, bool dark) {
    ui_fill_round_rect(fb, r, 10, dark ? GC_INK : 0xff);
    if (!dark) ui_draw_round_rect(fb, r, 10, GC_INK);
    ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 18, label,
               EPD_DRAW_ALIGN_CENTER, dark);
}

void gc_ui_wait_cursor(uint8_t* fb, int x, int y) {
    epd_fill_rect((EpdRect){x, y, 16, 10}, GC_INK, fb);
}

void gc_ui_title_page(uint8_t* fb, const char* game, const char* sub, bool has_continue) {
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){120, 300, 444, 3}, GC_INK, fb);
    ui_text_vc(fb, 342, 240, 46, game, EPD_DRAW_ALIGN_CENTER, false);
    if (sub && sub[0]) ui_text_vc(fb, 342, 356, 26, sub, EPD_DRAW_ALIGN_CENTER, false);
    epd_fill_rect((EpdRect){120, 414, 444, 3}, GC_INK, fb);
    ui_fill_round_rect(fb, (EpdRect){222, 620, 240, 64}, 14, GC_INK);
    ui_text_vc(fb, 342, 652, 24, "开始游戏", EPD_DRAW_ALIGN_CENTER, true);
    if (has_continue) {
        ui_fill_round_rect(fb, (EpdRect){222, 704, 240, 64}, 14, 0xff);
        ui_draw_round_rect(fb, (EpdRect){222, 704, 240, 64}, 14, GC_INK);
        ui_text_vc(fb, 342, 736, 24, "继续游戏", EPD_DRAW_ALIGN_CENTER, false);
    }
}

void gc_ui_typewrap(const char* text, int shown, int max_lines, int width_units,
                    void (*draw)(void* user, const char* line, int y), void* user, int y0, int lh) {
    if (!text || !draw) return;
    int left = shown;
    const char* p = text;
    for (int line = 0; line < max_lines && p[0] && left > 0; ++line) {
        char buf[640];
        size_t bytes = 0; int units = 0;
        while (p[bytes] && units < width_units && bytes < (size_t)left && bytes < sizeof(buf) - 4) {
            unsigned char c0 = (unsigned char)p[bytes];
            int len = c0 & 0x80 ? ((c0 & 0xe0) == 0xc0 ? 2 : ((c0 & 0xf0) == 0xe0 ? 3 : 4)) : 1;
            units += c0 > 0x7f ? 2 : 1;
            bytes += (size_t)len;
        }
        memcpy(buf, p, bytes); buf[bytes] = 0;
        draw(user, buf, y0 + line * lh);
        p += bytes; left -= (int)bytes;
    }
}

void gc_ui_blit_gray(uint8_t* fb, const uint8_t* gray, unsigned gw, unsigned gh,
                      int x, int y, unsigned area_w, unsigned area_h) {
    if (!gray || !gw || !gh) return;
    int dx = x + ((int)area_w - (int)gw) / 2;
    int dy = y + ((int)area_h - (int)gh) / 2;
    for (unsigned yy = 0; yy < gh; ++yy)
        for (unsigned xx = 0; xx < gw; ++xx)
            epd_draw_pixel(dx + xx, dy + yy,
                           ui_image_dither_gray(gray[(size_t)yy * gw + xx], dx + xx, dy + yy), fb);
}

/* 带透明蒙版 blit（avg 立绘，2026-10-18）：mask 行打包 w*ceil(w/8)，
 * bit=1 不透明绘制、bit=0 跳过（露出背景与其他立绘）；mask=NULL 时
 * 退化为整矩形 blit。g8 素材已在生成端量化到 16 档（&0xF0）——
 * **直写不再二次抖动**（2026-10-18 真机“第三个立绘糊”根因：16 档像素
 * 再过 Bayer 会把中间灰大面积抖成点阵，细密灰区观感发糊）。 */
void gc_ui_blit_gray_a(uint8_t* fb, const uint8_t* gray, unsigned gw, unsigned gh,
                       const uint8_t* mask, int x, int y,
                       unsigned area_w, unsigned area_h) {
    if (!gray || !gw || !gh) return;
    int dx = x + ((int)area_w - (int)gw) / 2;
    int dy = y + ((int)area_h - (int)gh) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    unsigned bpr = (gw + 7) / 8;
    for (unsigned yy = 0; yy < gh; ++yy) {
        const uint8_t* g_row = gray + (size_t)yy * gw;
        for (unsigned xx = 0; xx < gw; ++xx) {
            if (mask && !((mask[(size_t)yy * bpr + (xx >> 3)] >> (xx & 7)) & 1))
                continue;                          /* 透明：跳过 */
            uint8_t g = g_row[xx];
            if (g >= 0xF8) continue;               /* 纯白近似透明（伪 alpha 容错） */
            epd_draw_pixel(dx + xx, dy + yy, g, fb);   /* 已量化 16 档：直写 */
        }
    }
}

void gc_ui_missing(uint8_t* fb, const char* file, int x, int y, unsigned w, unsigned h) {
    ui_fill_round_rect(fb, (EpdRect){x + 8, y + 8, w - 16, h - 16}, 12, 0xe0);
    ui_draw_round_rect(fb, (EpdRect){x + 8, y + 8, w - 16, h - 16}, 12, GC_HAIR);
    const char* slash = strrchr(file ? file : "", '/');
    const char* base = slash ? slash + 1 : (file ? file : "");
    ui_text_vc(fb, x + (int)w / 2, y + (int)h / 2, 18, base, EPD_DRAW_ALIGN_CENTER, false);
}

void gc_ui_nopkg(uint8_t* fb, const char* game_name, gc_cat_t cat) {
    ui_clear_page(fb);
    ui_text_vc(fb, 342, 420, 34, game_name, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 520, 22, "未找到游戏资源包", EPD_DRAW_ALIGN_CENTER, false);
    char help[160];
    snprintf(help, sizeof(help), "TF 卡 games/" "%s-xxx%s", gc_cat_prefix(cat),
             cat == GC_CAT_BBK ? "" : ".zip");
    ui_text_vc(fb, 342, 586, 18, help, EPD_DRAW_ALIGN_CENTER, false);
    if (cat == GC_CAT_BBK)
        ui_text_vc(fb, 342, 626, 18, "大资源放 games/bbk-xxx/ 文件夹（目录直挂）",
                   EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 1120, 15, "长按中键退出", EPD_DRAW_ALIGN_CENTER, false);
}

/* ============ 入口包列表页（v4，用户 2026-10-18） ============
 * 恒竖屏 684×1216（竖持点选，进游戏才切横屏）：观感对齐设置页/
 * 文件管理——白底圆角卡（2px 纯黑框）+ 行内细分隔线 + 左侧两位数
 * 序号 + 右侧 chevron。行几何是绘制与命中的唯一事实源。 */

#define GCL_PORT_ROWS 8

static EpdRect list_card_rect(void) {
    return (EpdRect){36, 150, 612, GCL_PORT_ROWS * 92 + 16};
}
/* 行在卡内：卡顶 8px 装饰边 → 行 y = 158 + i*92（与命中同源）。 */
static EpdRect list_row_rect(int i) {
    return (EpdRect){36, 158 + i * 92, 612, 92};
}

/* 去分类前缀显示（avgr-gyakuten-case1 → gyakuten-case1）。 */
static const char* list_label(const char* name, gc_cat_t cat) {
    const char* prefix = gc_cat_prefix(cat);
    size_t n = strlen(prefix);
    if (!strncmp(name, prefix, n) && name[n] == '-') return name + n + 1;
    return name;
}

void gc_ui_pkg_list(uint8_t* fb, const char* title, const char* const* names,
                    int count, gc_cat_t cat) {
    /* 恒竖屏 684×1216（用户 2026-10-18）：竖持点选，进游戏后才切横屏。 */
    ui_clear_page(fb);
    const int W = 684, CX = 342;
    int shown = count < 0 ? 0 : count;
    int rows = GCL_PORT_ROWS;
    if (shown > rows) shown = rows;
    /* 标题与说明（对齐设置页 section 的字号层级）。 */
    ui_text(fb, 40, 64, 26, title, EPD_DRAW_ALIGN_LEFT, false);
    char total[96];
    if (shown > 0)
        snprintf(total, sizeof(total), "%d 个游戏 · 点选进入", count);
    else
        snprintf(total, sizeof(total), "未找到游戏资源包");
    ui_text(fb, W - 40, 64, 18, total, EPD_DRAW_ALIGN_RIGHT, false);
    /* 列表卡（白底圆角 + 2px 纯黑框，同设置页 settings_card）。 */
    EpdRect card = list_card_rect();
    card.height = (uint16_t)(shown ? shown * 92 + 16 : 150);
    ui_fill_round_rect(fb, card, 22, 0xff);
    ui_draw_control_frame(fb, card, 22, 0x00);
    if (shown <= 0) {
        char help[160];
        snprintf(help, sizeof(help), "TF 卡 games/%s-xxx%s", gc_cat_prefix(cat),
                 cat == GC_CAT_BBK ? "" : ".zip");
        ui_text(fb, 64, card.y + 56, 21, help, EPD_DRAW_ALIGN_LEFT, false);
        if (cat == GC_CAT_BBK)
            ui_text(fb, 64, card.y + 96, 18, "大资源放 games/bbk-xxx/ 文件夹（目录直挂）",
                    EPD_DRAW_ALIGN_LEFT, false);
    } else {
        for (int i = 0; i < shown; ++i) {
            EpdRect r = list_row_rect(i);
            int cy = r.y + r.height / 2;
            if (i) ui_draw_separator(fb, r.y, 86, W - 172, 0x00);
            char no[4];
            snprintf(no, sizeof(no), "%02d", i + 1);
            ui_text_vc(fb, 84, cy, 22, no, EPD_DRAW_ALIGN_CENTER, false);
            char label[48];
            const char* raw = list_label(names[i], cat);
            snprintf(label, sizeof(label), "%s", raw);
            int name_x = 116;
            int max_px = (W - 40) - (name_x + 24) - 64;
            while (label[0] && ui_text_width_px(24, label) > max_px) {
                size_t n = strlen(label) - 1;
                while (n && ((unsigned char)label[n] & 0xc0) == 0x80) --n;
                label[n] = 0;
            }
            ui_text_vc(fb, name_x, cy, 24, label, EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_icon(fb, W - 74, cy, 26, UI_ICON_CHEVRON_RIGHT, 0x00);
        }
    }
    if (count > rows) {
        char more[48];
        snprintf(more, sizeof(more), "还有 %d 个未显示", count - rows);
        ui_text(fb, 40, card.y + card.height + 26, 17, more, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_text(fb, CX, 1168, 15, "右键返回 · 长按中键退出", EPD_DRAW_ALIGN_CENTER, false);
}

int gc_ui_pkg_list_hit(int x, int y, int count) {
    int shown = count < GCL_PORT_ROWS ? count : GCL_PORT_ROWS;
    for (int i = 0; i < shown; ++i)
        if (ui_rect_hit(list_row_rect(i), x, y)) return i;
    return -1;
}

void gc_ui_nopkg_land(uint8_t* fb, const char* game_name, gc_cat_t cat) {
    /* 旋转窗口由内核（paint_plugin）负责；本函数只按横屏坐标绘制。 */
    ui_clear_page(fb);
    ui_text_vc(fb, 608, 220, 36, game_name, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 320, 24, "未找到游戏资源包", EPD_DRAW_ALIGN_CENTER, false);
    char help[160];
    snprintf(help, sizeof(help), "TF 卡 games/%s-xxx%s", gc_cat_prefix(cat),
             cat == GC_CAT_BBK ? "" : ".zip");
    ui_text_vc(fb, 608, 384, 18, help, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 648, 15, "长按中键退出", EPD_DRAW_ALIGN_CENTER, false);
}
