/*
 * avgr.c — 推理 AVG 插件（逆转裁判系列）。avgr_ 前缀插件体系第一员。
 *
 * 纯适配层：引擎在 components/avg_engine（零改动复用）；本文件只做
 * ① avg_render_ops 六类界面绘制快照 ② TAP/键 → avg_input_t ③ dirty →
 * GameCore 刷新申报。观感与原 app_avg 完全一致（冻结）。
 *
 * 资源包：avgr-<game>-<case>.zip，包根=案件目录（script.txt + images/）。
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "avg_asset.h"
#include "avg_beep.h"
#include "avg_core.h"
#include "avg_script.h"
#include "book_cover.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"

/* pkg_dir(192)+文件名的拼接触发保守截断告警：缓冲实测足够（192 对齐内核包路径）。 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "game_core.h"

/* ===================== 布局（横屏 1216×684，v3 全面加大 2026-10-18） =====================
 * 用户反馈：文字/按钮全部加大；证言底行不再被面板框线挡住。
 * 底栏统一 y=612..688 外框内（存档左/提示中/证物右），面板与对话框
 * 主区都收在 y≤604，与底栏物理隔离（挡线问题根治）。 */
#define AVGR_DIALOG_TOP    404
#define AVGR_TEXT_X        48
#define AVGR_TEXT_Y0       508
#define AVGR_TEXT_LH       54
#define AVGR_TEXT_PX       32
#define AVGR_PANEL_TOP     56
#define AVGR_PANEL_BOTTOM  600
#define AVGR_STMT_Y0       152
#define AVGR_STMT_LH       58
/* 底栏（y612..668，高 56）：存档 / 提示 / 证物 三段。 */
#define AVGR_BAR_Y         612
#define AVGR_BTN_H         56
#define AVGR_EV_BTN        ((EpdRect){1086, AVGR_BAR_Y, 110, AVGR_BTN_H})
#define AVGR_MENU_BTN      ((EpdRect){20, AVGR_BAR_Y, 110, AVGR_BTN_H})
#define AVGR_SFX_BTN       ((EpdRect){1008, 18, 188, 60})   /* 标题页音效开关 */
#define AVGR_GRID_X0       52
#define AVGR_GRID_Y0       156
#define AVGR_CELL_W        272
#define AVGR_CELL_H        140
#define AVGR_COLS          4
/* 证物详情卡（v3 上紧凑下扩容）：列表只留两行，详情卡 y452..688 高 236。 */
#define AVGR_DETAIL_Y      470
#define AVGR_DETAIL_H      (684 + 4 - AVGR_DETAIL_Y)

#define AVGR_INK  0x00
#define AVGR_DIM  0x8a
#define AVGR_HAIR 0xc2

/* ===================== 插件私有状态（state） ===================== */
typedef struct {
    avg_script_t* script;         /* ~800KB PSRAM */
    avg_assets_t  assets;
    avg_core_t    core;
    const uint8_t* ev_pic;        /* 当前详情证物图（g8 含蒙版） */
    unsigned ev_pic_w, ev_pic_h;
    const uint8_t* ev_pic_mask;
    bool          loaded;
    bool          running;
    bool          loading;        /* 开始/继续后资产解码中：屏保持“加载中”按钮态 */
    int           loading_btn;    /* 哪个按钮在加载：1=开始 2=继续（用户要点哪按钮哪变） */
    uint64_t      asset_gen;      /* 资产代次：变化后执行“用完即释放” */

    /* 绘制快照（ops 记录，render 统一画——冻结模式）。 */
    const uint8_t* bg;  unsigned bg_w, bg_h;
    const uint8_t* ch[3]; unsigned chw[3], chh[3]; int ch_side[3];
    const uint8_t* ch_mask[3];   /* 立绘 1bit 透明蒙版（无 alpha 时 NULL） */
    int focus;
    const avg_testimony_t* panel; int panel_active;
    int sel_known, sel_ev;
    bool ask_mode;
    int verdict_good;
    bool has_save;
    bool save_flash;         /* 存档反馈停驻：按钮态已存档，~700ms 后回标题 */
    int64_t save_flash_until;
    char title[128], cur_name[48], cur_text[AVG_MAX_TEXT];
    int shown;
    char script_path[192];
    char sfx_path[192];     /* 音效开关持久化（launch 时读，切换时写） */
    /* 场景帧缓存（性能优化 2026-10-18，S3 当前设备流畅度）：
     * 纯对话推进不重画上半屏场景（~70 万像素/次的 blit 白画），
     * 只有 ops_scene 报新场景或非 DIALOG 画面污染 fb 后才全画。 */
    int  scene_gen;         /* 引擎场景代次（ops_scene 回调递增） */
    int  painted_gen;       /* fb 上场景代次 */
    bool fb_scene_ok;       /* fb 场景区有效；离开 DIALOG 画面即失效 */
    avg_render_ops_t ops;   /* 会话生命周期：launch 栈版本返回后悬空，
                              * 点「开始」走 ops 回调即 InstrFetchProhibited
                              * （2026-10-18 backtrace 实锸）。 */
} avgr_state_t;

/*
 * 用户 2026-10-18 五项验收标准（针对本插件）：
 *  1. 图片尽量还原：素材进包，缺失自动占位；
 *  2. 声音仅事件提示音（avg_beep 全表 ≤3 音符短音，无 BGM，符合）；
 *  3. 刷新翻页式局部：打字机逐字刷新取消（内部推进但不刷屏），
 *     paint 画整句——每句/每次切换只一次局部刷新；
 *  4. 存档写 SD 独立目录 /sdcard/.gsaves/<pkg>/（不进 .gcache，
 *     zip 更新重解缓存不会丢档；多游戏互不干扰）；
 *  5. 内存用完即释放：场景代次变化后，非当前场景（1 bg+3 char）图
 *     不再驻留 PSRAM，立即归还。
 */

static int side_clamp(int s) { return s < 0 || s > 2 ? 1 : s; }
static void trim_assets(avgr_state_t* s);   /* 前向：ops_scene 场景切换时调用 */

/* ===================== 图片解码注入（book_image 管线，冻结复用） =====================
 * v3 横屏（用户 2026-10-18 真机反馈“人物后面大量空白”）：
 *  - bg：cover 满铺场景区 1216×410（grayscale 内部等比放大+中心裁切，
 *    目标像素 498K ≤引擎预留 684×830=568K）；
 *  - char：contain 满槽高（等比、不裁切，底对齐由 paint 决定），
 *    ≤368×410=151K ≤预留 470×640=301K；
 *  - 两者输出均重新量化 16 档：管线 bilinear 会把源图 16 档插出
 *    中间灰（缩放后量化的同一教训，2026-10-17）。 */
static void quantize16(uint8_t* px, size_t n) {
    for (size_t i = 0; i < n; ++i) px[i] = (uint8_t)(px[i] & 0xF0);
}

/* ===================== g8 裸格式直读（场景切换提速+立绘透明） =====================
 * 头 10B：“EVG8\x01”(5) + u16le w + u16le h + u8 flags(bit0=有蒙版)，
 * 数据 w*h 字节 16 级灰 + [蒙版 h*ceil(w/8) 行打包，bit=1 不透明]。
 * 预处理脚本已烘焙到设备目标尺寸，设备端只 fread（~30ms vs PNG 500ms）。 */
static bool avgr_g8_load(const char* path, unsigned max_w, unsigned max_h,
                         uint8_t* out, uint8_t** mask, unsigned* w, unsigned* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, "EVG8\x01", 5) != 0) {
        fclose(f);
        return false;
    }
    unsigned gw = hdr[5] | ((unsigned)hdr[6] << 8);
    unsigned gh = hdr[7] | ((unsigned)hdr[8] << 8);
    bool has_mask = (hdr[9] & 1) != 0;
    /* 尺寸校验用面积制（2026-10-18 背景不显示根因）：背景 g8 烘焙为
     * 1216×410，宽 > 引擎槽 max_w=684，但面积 498K ≤ 预留 568K——
     * 单独的 gw>max_w 判断把合法满铺背景直接拒成缺图。容量按面积
     * 预留（buf=max_w*max_h），就按面积校验。 */
    if (!gw || !gh || (uint64_t)gw * gh > (uint64_t)max_w * max_h) {
        fclose(f);
        return false;
    }
    if (fread(out, 1, (size_t)gw * gh, f) != (size_t)gw * gh) {
        fclose(f);
        return false;
    }
    uint8_t* m = NULL;
    if (has_mask) {
        size_t bytes = (size_t)gh * ((gw + 7) / 8);
        m = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m || fread(m, 1, bytes, f) != bytes) {
            if (m) heap_caps_free(m);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    *w = gw;
    *h = gh;
    if (mask) *mask = m;
    else if (m) heap_caps_free(m);
    return true;
}

static bool avgr_image_decode(const char* path, unsigned max_w, unsigned max_h,
                              uint8_t* out, uint8_t** mask, unsigned* w, unsigned* h) {
    if (mask) *mask = NULL;
    /* g8 变体探测（2026-10-18）：脚本引用 .png，打包工具伴生同名 .g8
     * —— 有 g8 走快路径（fread 直读+蒙版），无则退回 PNG 兼容路径。 */
    char g8path[296];
    const char* ext = strrchr(path, '.');
    if (ext && !strcmp(ext, ".png") && (size_t)(ext - path) + 3 < sizeof(g8path)) {
        snprintf(g8path, sizeof(g8path), "%.*s.g8", (int)(ext - path), path);
        FILE* probe = fopen(g8path, "rb");
        if (probe) {
            fclose(probe);
            path = g8path;
            ext = ".g8";
        }
    }
    if (ext && !strcmp(ext, ".g8"))
        return avgr_g8_load(path, max_w, max_h, out, mask, w, h);
    unsigned ow = 0, oh = 0;
    if (!book_image_file_dimensions(path, true, &ow, &oh) || !ow || !oh) return false;
    bool is_bg = strstr(path, "/bg/") != NULL;
    bool is_char = strstr(path, "/char/") != NULL;
    unsigned fw, fh;
    if (is_bg) { fw = 1216; fh = 410; }
    else if (is_char) {
        fw = 368; fh = 410;
        unsigned fit_h = (unsigned)((uint64_t)oh * fw / ow);
        if (fit_h > fh || fit_h == 0) { fw = (unsigned)((uint64_t)ow * fh / oh); }
        else fh = fit_h;
    } else { fw = max_w; fh = max_h; }
    if (!fw || !fh || (uint64_t)fw * fh > (uint64_t)max_w * max_h) return false;
    if (!book_image_file_grayscale(path, true, fw, fh, out)) return false;
    if (is_bg || is_char) quantize16(out, (size_t)fw * fh);
    *w = fw; *h = fh;
    return true;
}

/* ===================== ops：引擎 → 状态记录（冻结） ===================== */
static void ops_scene(void* ctx, const uint8_t* bg, unsigned bw, unsigned bh,
                      const uint8_t* ch[3], const unsigned chw[3], const unsigned chh[3],
                      const uint8_t* ch_mask[3],
                      const avg_side_t side[3], int focus) {
    avgr_state_t* s = (avgr_state_t*)ctx;
    s->bg = bg; s->bg_w = bw; s->bg_h = bh;
    for (int i = 0; i < 3; ++i) {
        s->ch[i] = ch[i]; s->chw[i] = chw[i]; s->chh[i] = chh[i];
        s->ch_mask[i] = ch_mask ? ch_mask[i] : NULL;
        s->ch_side[i] = side_clamp((int)side[i]);
    }
    s->focus = focus;
    s->scene_gen++;   /* 新场景内容：下次 paint 必须全画（帧缓存失效） */
    trim_assets(s);   /* 场景签名变化 → 回收非本场景图（时机修复 2026-10-18：
                        * 原来跟着 generation 跑，证物图一加载就把立绘/
                        * 其它证物踢了；现在只随场景切换，且 LRU 淘汰后
                        * paint 实时重取不悬空） */
}
static void ops_dialog(void* ctx, const char* name, const char* text, int shown) {
    avgr_state_t* s = (avgr_state_t*)ctx;
    snprintf(s->cur_name, sizeof(s->cur_name), "%s", name ? name : "");
    snprintf(s->cur_text, sizeof(s->cur_text), "%s", text ? text : "");
    s->shown = shown;
}
static void ops_testimony(void* ctx, const avg_testimony_t* t, int active) {
    ((avgr_state_t*)ctx)->panel = t; ((avgr_state_t*)ctx)->panel_active = active;
}
static void ops_evidence(void* ctx, const avg_evidence_t* ev, int count,
                          int selected, bool detail, bool ask_mode) {
    avgr_state_t* s = (avgr_state_t*)ctx;
    s->sel_known = selected;
    s->sel_ev = detail ? selected : -1;
    s->ask_mode = ask_mode;
    /* 详情图预取（回调中副作用 OK，paint 保持纯画）：LRU 命中后零成本。 */
    s->ev_pic = NULL;
    s->ev_pic_mask = NULL;
    s->ev_pic_w = s->ev_pic_h = 0;
    if (detail && selected >= 0 && selected < count && s->core.asset_dir[0]) {
        char evp[288];
        snprintf(evp, sizeof(evp), "%s/ev/%.20s.g8", s->core.asset_dir,
                 s->script->evidence[selected].id);
        s->ev_pic = avg_assets_get(&s->assets, evp, 160, 160,
                                   &s->ev_pic_w, &s->ev_pic_h,
                                   s->core.now_ms, &s->ev_pic_mask);
    }
}
static void ops_verdict(void* ctx, int good)    { ((avgr_state_t*)ctx)->verdict_good = good; }
static void ops_title(void* ctx, const char* t, bool has_save) {
    avgr_state_t* s = (avgr_state_t*)ctx;
    s->has_save = has_save; s->running = false;
    snprintf(s->title, sizeof(s->title), "%s", t ? t : "");
}

/* ===================== paint（六类界面，冻结版观感） ===================== */
/* 资产路径多级回退（2026-10-23）：剧本只写文件名（如 bg_court.png），
 * 实际包结构五花八门（平铺 / images/ / images/bg|char/）。逐候选探测，
 * .g8 伴生优先后 .png；全部未命中时回退到旧拼接路径（保持缺图提示行为）。 */
static bool avgr_asset_path(const avgr_state_t* s, const char* name, char* out, size_t cap) {
    static const char* const k_sub[] = { "", "images", "images/bg", "images/char", "bg", "char" };
    for (unsigned i = 0; i < sizeof(k_sub) / sizeof(k_sub[0]); ++i) {
        if (k_sub[i][0]) snprintf(out, cap, "%s/%s/%.190s", s->core.asset_dir, k_sub[i], name);
        else snprintf(out, cap, "%s/%.190s", s->core.asset_dir, name);
        char* ext = strrchr(out, '.');
        if (ext && !strcmp(ext, ".png")) {          /* .g8 伴生变体优先 */
            memcpy(ext, ".g8", 4);
            FILE* p = fopen(out, "rb");
            if (p) { fclose(p); return true; }
            memcpy(ext, ".png", 5);
        }
        FILE* p = fopen(out, "rb");
        if (p) { fclose(p); return true; }
    }
    snprintf(out, cap, "%s/%.190s", s->core.asset_dir, name);
    return false;
}

static void paint_scene(uint8_t* fb, avgr_state_t* s) {
    /* 悬空指针根治（2026-10-18 真机"出示证据后立绘全糊"）：证物簿开箱
     * 加载 4 张 ev 图会把 8 槽 LRU 挤爆——旧立绘 slot 被 free，而快照
     * 指针 s->ch[i]/s->bg 仍指已释放 PSRAM，重画=垃圾数据。
     * 对策：paint 不再信任 ops_scene 快照指针，按 core 内文件名实时
     * 从 LRU 重取（命中缓存零成本；被逐出则重加载或占位，永不悬空）。 */
    char full[288];
    ui_fill_round_rect(fb, (EpdRect){0, 0, 1216, AVGR_DIALOG_TOP}, 0, 0xff);
    const uint8_t* bg = NULL;
    unsigned bw = 0, bh = 0;
    if (s->core.bg_file[0]) {
        avgr_asset_path(s, s->core.bg_file, full, sizeof(full));
        bg = avg_assets_get(&s->assets, full, 684, 830, &bw, &bh, s->core.now_ms, NULL);
    }
    if (bg) gc_ui_blit_gray_a(fb, bg, bw, bh, NULL, 0, 0, 1216, AVGR_DIALOG_TOP);
    else if (s->core.bg_file[0])
        gc_ui_missing(fb, s->core.bg_file, 0, 0, 1216, AVGR_DIALOG_TOP);
    /* 横屏三槽位：L/C/R 各占约三分之一场景区，底边对齐对话框顶。 */
    static const int k_x[3] = {16, 424, 832};
    static const int k_w[3] = {368, 368, 368};
    for (int i = 0; i < 3; ++i) {
        if (!s->core.char_files[i][0]) continue;
        avgr_asset_path(s, s->core.char_files[i], full, sizeof(full));
        unsigned cw = 0, ch_h = 0;
        const uint8_t* cmask = NULL;
        const uint8_t* cg = avg_assets_get(&s->assets, full, 470, 640, &cw, &ch_h,
                                           s->core.now_ms, &cmask);
        if (!cg) continue;
        int x = k_x[side_clamp((int)s->core.char_side[i])], w = k_w[side_clamp((int)s->core.char_side[i])];
        int y = AVGR_DIALOG_TOP - (int)ch_h;
        if (y < 0) y = 0;
        gc_ui_blit_gray_a(fb, cg, cw, ch_h, cmask, x, y,
                          (unsigned)w, (unsigned)(AVGR_DIALOG_TOP - y));
    }
}
static void paint_ev_btn(uint8_t* fb) { gc_ui_button(fb, AVGR_EV_BTN, "证物", false); }
static void paint_save_btn(uint8_t* fb, const avgr_state_t* s) {
    /* 存档按钮（2026-10-18 菜单改名）：点击后反馈态「已存档」（DU 小刷），
     * 停驻 ~700ms 后整屏回标题。参考 GBA 逆裁判 SAVE 提示/PyWright 存档弹层。 */
    gc_ui_button(fb, AVGR_MENU_BTN, (s && s->save_flash) ? "已存档" : "存档", false);
}
/* 底栏提示行（证言页）：居中于存档/证物两按钮之间，永不与面板框线
 * 重叠（挡线问题根治 2026-10-18：面板收窄到 y≤604，底栏 y≥612）。 */
static void paint_bar_hint(uint8_t* fb, const char* text) {
    ui_text_vc(fb, 608, AVGR_BAR_Y + AVGR_BTN_H / 2, 20, text,
               EPD_DRAW_ALIGN_CENTER, false);
}
static void draw_line_cb(void* user, const char* line, int y) {
    ui_text_vc((uint8_t*)user, AVGR_TEXT_X, y, AVGR_TEXT_PX, line, EPD_DRAW_ALIGN_LEFT, false);
}
static void paint_dialog(uint8_t* fb, avgr_state_t* s) {
    /* v3（2026-10-18 加大）：对话框底板 404..604，底栏 612..668 独立。
     * 名牌 26px、正文 32px、行距 54。 */
    ui_fill_round_rect(fb, (EpdRect){0, AVGR_DIALOG_TOP, 1216, 684 - AVGR_DIALOG_TOP}, 0, 0xff);
    epd_fill_rect((EpdRect){0, AVGR_DIALOG_TOP + 6, 1216, 3}, AVGR_INK, fb);
    if (s->cur_name[0]) {
        ui_fill_round_rect(fb, (EpdRect){44, AVGR_DIALOG_TOP + 24, 196, 56}, 12, AVGR_INK);
        ui_text_vc(fb, 142, AVGR_DIALOG_TOP + 52, 26, s->cur_name,
                   EPD_DRAW_ALIGN_CENTER, true);
    }
    epd_fill_rect((EpdRect){AVGR_TEXT_X, AVGR_DIALOG_TOP + 92, 92, 3}, AVGR_HAIR, fb);
    /* 整句直出；行宽 70 单位（=35 中文字≈1120px 满 1216 宽，2026-10-18
     * “右半空白”根因：竖屏时代 34 单位遗到横屏只占半幅）。 */
    gc_ui_typewrap(s->cur_text, (int)strlen(s->cur_text), 3, 70, draw_line_cb,
                   fb, AVGR_TEXT_Y0, AVGR_TEXT_LH);
    if (s->cur_text[0]) gc_ui_wait_cursor(fb, 1150, AVGR_DIALOG_TOP + 216);
    paint_save_btn(fb, s);
    paint_ev_btn(fb);
}
static void paint_testimony(uint8_t* fb, avgr_state_t* s) {
    if (!s->panel) return;
    /* v3（2026-10-18）：面板收窄到 y600，语句 25px/行距 56；提示行独立
     * 底栏区，不再压面板底框线（“最下面说明被线挡住”根治）。 */
    EpdRect panel = {24, AVGR_PANEL_TOP, 1168, AVGR_PANEL_BOTTOM - AVGR_PANEL_TOP};
    ui_fill_round_rect(fb, panel, 18, 0xff);
    ui_draw_round_rect(fb, panel, 18, AVGR_INK);
    ui_text_vc(fb, 52, AVGR_PANEL_TOP + 44, 30, s->panel->title, EPD_DRAW_ALIGN_LEFT, false);
    epd_fill_rect((EpdRect){52, AVGR_PANEL_TOP + 82, 1112, 3}, AVGR_HAIR, fb);
    for (int i = 0; i < s->panel->count; ++i) {
        int y = AVGR_STMT_Y0 + i * AVGR_STMT_LH;
        if (y > AVGR_PANEL_BOTTOM - 22) break;
        if (i >= s->core.t_unlock) {
            /* 未解锁：深灰底 + 白字反转（v2 2026-10-18：浅灰底上白字看
             * 不清）；字号同步加大。锁行不可点，提示“尚未提及”。 */
            ui_fill_round_rect(fb, (EpdRect){40, y - 30, 1136, AVGR_STMT_LH - 2}, 8, 0x70);
            ui_text_vc(fb, 82, y - 1, 25, "？？？ ——该内容尚未提及——",
                       EPD_DRAW_ALIGN_LEFT, true);
            continue;
        }
        if (i == s->panel_active) {
            ui_fill_round_rect(fb, (EpdRect){40, y - 30, 1136, AVGR_STMT_LH - 2}, 8, 0xe0);
            epd_fill_circle(58, y - 3, 7, AVGR_INK, fb);
        }
        char line[AVG_MAX_TEXT];
        snprintf(line, sizeof(line), "%d. %s", i + 1, s->panel->stmts[i].text);
        while (ui_text_width_px(28, line) > 1030) {
            size_t n = strlen(line) - 1;
            while (n && ((unsigned char)line[n] & 0xc0) == 0x80) --n;
            line[n] = 0;
        }
        ui_text_vc(fb, 82, y - 1, 28, line, EPD_DRAW_ALIGN_LEFT, false);
    }
    paint_bar_hint(fb, "点按证言可威慑 · 「存档」存档回标题 · 「证物」出示证据");
    paint_save_btn(fb, s);
    paint_ev_btn(fb);
}
static void paint_evidence(uint8_t* fb, avgr_state_t* s) {
    /* v3（2026-10-18 用户“上面空白太大/下面太小/显示不全”）：
     * 列表两行紧凑（y124..434），详情卡扩到 y452..680（高 228）——
     * 名称 34px + 描述 24px×4 行，全部完整显示；缩略/详情图均 16 档直写。 */
    ui_clear_page(fb);
    ui_text_vc(fb, 52, 96, 36, "证物簿", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 140, 48, 1124, 0xa0);
    /* 关闭按钮上提（2026-10-18 遮挡修复）：原 y84..136 与首行格子 y124+
     * 重叠；提到 y40..92（与标题同行区），格子 y156 起完全不碰。 */
    gc_ui_button(fb, (EpdRect){1104, 40, 92, 52}, "关闭", false);
    int known = 0;
    for (int i = 0; i < s->script->evidence_count; ++i) {
        if (!avg_core_ev_known(&s->core, i)) continue;
        int col = known % AVGR_COLS, row = known / AVGR_COLS;
        int x = AVGR_GRID_X0 + col * (AVGR_CELL_W + 8);
        int y = AVGR_GRID_Y0 + row * (AVGR_CELL_H + 10);
        if (y + AVGR_CELL_H > AVGR_DETAIL_Y - 12) break;  /* 与命中同界：底留详情卡 */
        EpdRect cell = {x, y, AVGR_CELL_W, AVGR_CELL_H};
        bool sel = known == s->sel_known;
        ui_fill_round_rect(fb, cell, 12, sel ? 0xe0 : 0xff);
        ui_draw_round_rect(fb, cell, 12, sel ? AVGR_INK : AVGR_HAIR);
        /* 证物缩略图（预取同路径），缺图退回首字占位。 */
        char evp[288];
        snprintf(evp, sizeof(evp), "%s/ev/%.20s.g8", s->core.asset_dir,
                 s->script->evidence[i].id);
        unsigned pw = 0, ph = 0;
        const uint8_t* pic = avg_assets_get(&s->assets, evp, 160, 160,
                                            &pw, &ph, s->core.now_ms, NULL);
        if (pic) {
            EpdRect picr = {x + (AVGR_CELL_W - 100) / 2, y + 8, 100, 100};
            ui_fill_round_rect(fb, picr, 10, 0xf6);
            gc_ui_blit_gray_a(fb, pic, pw, ph, NULL, picr.x, picr.y, 100, 100);
        } else {
            ui_fill_round_rect(fb, (EpdRect){x + (AVGR_CELL_W - 92) / 2, y + 8, 92, 92}, 10, 0xf0);
            char first[5];
            snprintf(first, sizeof(first), "%s", s->script->evidence[i].name);
            unsigned char c0 = (unsigned char)first[0];
            if (c0 & 0x80) {
                if ((c0 & 0xe0) == 0xc0) first[2] = 0;
                else if ((c0 & 0xf0) == 0xe0) first[3] = 0;
                else first[4] = 0;
            } else first[1] = 0;
            ui_text_vc(fb, x + AVGR_CELL_W / 2, y + 54, 36, first, EPD_DRAW_ALIGN_CENTER, false);
        }
        char name[64];
        snprintf(name, sizeof(name), "%s", s->script->evidence[i].name);
        while (ui_text_width_px(18, name) > AVGR_CELL_W - 14) {
            size_t n = strlen(name) - 1;
            while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
            name[n] = 0;
        }
        ui_text_vc(fb, x + AVGR_CELL_W / 2, y + AVGR_CELL_H - 16, 18, name,
                   EPD_DRAW_ALIGN_CENTER, false);
        known++;
    }
    if (!known) ui_text_vc(fb, 608, 300, 24, "暂时没有证物", EPD_DRAW_ALIGN_CENTER, false);
    if (s->sel_ev >= 0 && s->sel_ev < s->script->evidence_count) {
        const avg_evidence_t* ev = &s->script->evidence[s->sel_ev];
        /* 详情卡 v3：高 228（y452..680），左图 168 + 名称 34px + 描述 24px×4 行
         * （完整显示，ask 态末行避让出示按钮）。 */
        EpdRect detail = {36, AVGR_DETAIL_Y, 1144, AVGR_DETAIL_H};
        ui_fill_round_rect(fb, detail, 16, 0xe0);
        ui_draw_round_rect(fb, detail, 16, AVGR_INK);
        if (s->ev_pic) {
            EpdRect picr = {56, AVGR_DETAIL_Y + (AVGR_DETAIL_H - 176) / 2, 176, 176};
            ui_fill_round_rect(fb, picr, 10, 0xf6);
            gc_ui_blit_gray_a(fb, s->ev_pic, s->ev_pic_w, s->ev_pic_h, NULL,
                              picr.x, picr.y, 176, 176);
        }
        int tx = 268;
        ui_text_vc(fb, tx, AVGR_DETAIL_Y + 38, 34, ev->name, EPD_DRAW_ALIGN_LEFT, false);
        epd_fill_rect((EpdRect){tx, AVGR_DETAIL_Y + 66, 96, 3}, AVGR_HAIR, fb);
        const char* p = ev->desc;
        for (int ln = 0; ln < 4 && p[0]; ++ln) {
            char line[AVG_MAX_TEXT];
            /* 行宽满幅（2026-10-18“下面对话框只用一半”同根）：详情文本区
             * tx=268..1160 ≈ 892px ÷ 24px = 37 字 = 74 单位；末行避让出示钮。 */
            int wu_max = (s->ask_mode && ln == 3) ? 44 : 74;
            size_t take = 0; int wu = 0;
            while (p[take] && wu < wu_max) {
                unsigned char c1 = (unsigned char)p[take];
                take += c1 & 0x80 ? ((c1 & 0xe0) == 0xc0 ? 2 : ((c1 & 0xf0) == 0xe0 ? 3 : 4)) : 1;
                wu += c1 > 0x7f ? 2 : 1;
            }
            memcpy(line, p, take); line[take] = 0;
            ui_text_vc(fb, tx, AVGR_DETAIL_Y + 106 + ln * 34, 24, line,
                       EPD_DRAW_ALIGN_LEFT, false);
            p += take;
        }
        if (s->ask_mode)
            gc_ui_button(fb, (EpdRect){984, AVGR_DETAIL_Y + AVGR_DETAIL_H - 84, 188, 68},
                         "出示证据", true);
    }
}
static void paint_verdict(uint8_t* fb, avgr_state_t* s) {
    ui_clear_page(fb);
    ui_text_vc(fb, 608, 250, 88, s->verdict_good ? "无罪！" : "有罪…", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 390, 28,
               s->verdict_good ? "—— 第一章 · 最初的逆转 · 完 ——" : "—— 辩护失败 ——",
               EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 640, 20, "点按屏幕返回标题页", EPD_DRAW_ALIGN_CENTER, false);
}
static void paint_title(uint8_t* fb, avgr_state_t* s) {
    /* v3（2026-10-18 加大）：标题 52/副标 30，双按钮 292×84、字 28；
     * loading 态白底细框「加载中……」（GBA NOW LOADING 式，谁在加载谁变）。 */
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){120, 84, 976, 3}, AVGR_INK, fb);
    ui_text_vc(fb, 608, 148, 52, "逆转裁判", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 250, 30, s->title[0] ? s->title : "最初的逆转",
               EPD_DRAW_ALIGN_CENTER, false);
    epd_fill_rect((EpdRect){120, 306, 976, 3}, AVGR_INK, fb);
    if (s->loading && s->loading_btn == 1) {
        ui_fill_round_rect(fb, (EpdRect){462, 362, 292, 84}, 16, 0xff);
        ui_draw_round_rect(fb, (EpdRect){462, 362, 292, 84}, 16, AVGR_HAIR);
        ui_text_vc(fb, 608, 404, 26, "加载中……", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        ui_fill_round_rect(fb, (EpdRect){462, 362, 292, 84}, 16, AVGR_INK);
        ui_text_vc(fb, 608, 404, 28, "开始游戏", EPD_DRAW_ALIGN_CENTER, true);
    }
    if (s->has_save) {
        if (s->loading && s->loading_btn == 2) {
            ui_fill_round_rect(fb, (EpdRect){462, 466, 292, 84}, 16, 0xff);
            ui_draw_round_rect(fb, (EpdRect){462, 466, 292, 84}, 16, AVGR_HAIR);
            ui_text_vc(fb, 608, 508, 26, "加载中……", EPD_DRAW_ALIGN_CENTER, false);
        } else {
            ui_fill_round_rect(fb, (EpdRect){462, 466, 292, 84}, 16, 0xff);
            ui_draw_round_rect(fb, (EpdRect){462, 466, 292, 84}, 16, AVGR_INK);
            ui_text_vc(fb, 608, 508, 28, "继续游戏", EPD_DRAW_ALIGN_CENTER, false);
        }
    }
    /* 右上角音效开关（2026-10-18 用户需求）：随标题页常态显示。 */
    ui_fill_round_rect(fb, AVGR_SFX_BTN, 12, 0xff);
    ui_draw_round_rect(fb, AVGR_SFX_BTN, 12, AVGR_DIM);
    ui_text_vc(fb, AVGR_SFX_BTN.x + AVGR_SFX_BTN.width / 2,
               AVGR_SFX_BTN.y + AVGR_SFX_BTN.height / 2, 24,
               avg_beep_enabled() ? "音效：开" : "音效：关",
               EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 648, 20, "长按中键退出 · 游戏中右键返回", EPD_DRAW_ALIGN_CENTER, false);
}
/* 脚本加载失败引导（含 TF 卡包路径提示）。 */
static void paint_noscript(uint8_t* fb, avgr_state_t* s) {
    ui_clear_page(fb);
    ui_text_vc(fb, 608, 300, 30, "未找到游戏脚本", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 368, 15, s->script_path, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 608, 420, 18, "把 avgr-*.zip 放到 TF 卡 games 文件夹后重新进入",
               EPD_DRAW_ALIGN_CENTER, false);
}

static void paint(gc_ctx_t* g, uint8_t* fb) {
    avgr_state_t* s = (avgr_state_t*)g->state;
    if (!s) { gc_ui_nopkg_land(fb, "逆转裁判", GC_CAT_AVGR); return; }
    if (!s->loaded) { paint_noscript(fb, s); return; }
    /* 加载中（点击开始/继续后）：保持标题页+按钮已转加载态——此时
     * run 已推进到首句（mode=DIALOG），必须拦在 mode 分发之前。 */
    if (s->loading) { paint_title(fb, s); s->fb_scene_ok = false; return; }
    switch (s->core.mode) {
    case AVG_MODE_TITLE:    paint_title(fb, s);    s->fb_scene_ok = false; return;
    case AVG_MODE_EVIDENCE: paint_evidence(fb, s); s->fb_scene_ok = false; return;
    case AVG_MODE_VERDICT:  paint_verdict(fb, s);  s->fb_scene_ok = false; return;
    default: break;
    }
    /* 帧缓存快路径（性能优化 2026-10-18，S3 当前设备）：纯对话推进
     * （want_du=DU 快刷档）且 fb 场景区仍是本场景（代次一致、未被其他
     * 画面污染）→ 跳过场景 blit 与清屏（~70 万像素/次），只重画对话框。 */
    if (s->core.mode == AVG_MODE_DIALOG && g->want_du &&
        s->fb_scene_ok && s->painted_gen == s->scene_gen) {
        paint_dialog(fb, s);
        return;
    }
    paint_scene(fb, s);
    s->painted_gen = s->scene_gen;
    s->fb_scene_ok = true;
    if (s->core.mode == AVG_MODE_TESTIMONY) {
        paint_testimony(fb, s);
        s->fb_scene_ok = false;   /* 面板横跨场景区：回 DIALOG 首帧需全画 */
    } else {
        paint_dialog(fb, s);
    }
}

/* ===================== dirty → GameCore 申报 =====================
 * 残留修复（2026-10-18 真机）：界面级变化（SCENE/UI 位，含一切模式切换
 * 与证言/证物进出）一律整屏；只有纯对话句推进缩到对话框区。
 * 区域精确修复（2026-10-18 真机「带上一小条图案」）：原 -30 上探把
 * 对话框上方的场景画也带进刷新区（一道 30px 条纹闪烁），改为只刷
 * 对话框本体（y=410..684）。框内纯黑白文本/按钮 → DU 快刷（又快又净），
 * 灰阶画面仍走 GL16。 */
static gc_ev_t apply(avgr_state_t* s, gc_ctx_t* g, uint32_t dirty) {
    if (!s->loaded || dirty == AVG_DIRTY_NONE) return GC_EV_NONE;
    g->want_du = false;                    /* 先复位（2026-10-18 实锤：整屏分支
                                              * 残留上帧 DU 档，出示证据后整屏巨变走
                                              * 2值DU刷——灰阶全糊视觉上“没反应”） */
    if ((dirty & (AVG_DIRTY_SCENE | AVG_DIRTY_UI)) || s->core.mode != AVG_MODE_DIALOG)
        g->area = (EpdRect){0, 0, 1216, 684};
    else {
        g->area = (EpdRect){0, AVGR_DIALOG_TOP, 1216, 684 - AVGR_DIALOG_TOP};
        g->want_du = true;    /* 对话框纯黑白 → DU 快刷 */
    }
    g->want_full = false;
    return GC_EV_DIRTY;
}

/* 刷新收尾（2026-10-18 v3）：场景签名刚变、资产还在解码（pending）时不推
 * 中间帧——否则先闪一格缺素材占位、解码完再刷一次，墨水屏两次刷新都白费。
 * 吞掉本次刷新，tick 在全部就绪时统一返回 DIRTY_SCENE 一次到位。
 * （标题开始/继续走 start_or_continue，不经此过滤。） */
static gc_ev_t finish(avgr_state_t* s, gc_ctx_t* g, uint32_t dirty) {
    if (s->core.assets_pending && (dirty & AVG_DIRTY_SCENE)) return GC_EV_NONE;
    return apply(s, g, dirty);
}

/* ===================== 输入 ===================== */

/* 标题页音效开关（2026-10-18 用户需求）：点击切换并写 SD 持久化
 * （/sdcard/.gsaves/<pkg>/sfx.txt，一行 "1"/"0"；写失败不影响本次切换）。
 * 切到开时播一声确认音（关时静默——正在关的时候响一声语义不对）。 */
static gc_ev_t toggle_sfx(avgr_state_t* s, gc_ctx_t* g) {
    bool on = !avg_beep_enabled();
    avg_beep_set_enabled(on);
    if (on) avg_beep_play(AVG_SFX_SELECT);
    if (s->sfx_path[0]) {
        FILE* f = fopen(s->sfx_path, "w");
        if (f) { fputs(on ? "1\n" : "0\n", f); fclose(f); }
    }
    g->area = (EpdRect){AVGR_SFX_BTN.x - 10, AVGR_SFX_BTN.y - 10,
                        AVGR_SFX_BTN.width + 20, AVGR_SFX_BTN.height + 20};
    g->want_du = true;    /* 开关文字翻转：纯黑白小区域 DU 快刷 */
    g->want_full = false;
    return GC_EV_DIRTY;
}

/* 「存档」= 存档并回标题页（2026-10-18 重命名+两级反馈，参考 GBA 逆裁判
 * SAVE 提示 / PyWright 存档弹层 / 伏魔记「保存进度」页，均先给正在保存的
 * 可见反馈再跳走；SD 本地写入很快，反馈以按钮态呈现足够）：
 *  ① 蜂鸣存档音（零显示成本）；
 *  ② 按钮原地转「已存档」，只 DU 小刷一次（~300ms）；写 SD 同步完成；
 *  ③ 停驻 ~700ms 让反馈可感知，tick 到点后整屏回标题（有档可继续）。 */
static gc_ev_t save_game(avgr_state_t* s, gc_ctx_t* g) {
    if (s->core.mode == AVG_MODE_TITLE || s->core.mode == AVG_MODE_VERDICT)
        return GC_EV_NONE;
    avg_beep_play(AVG_SFX_SAVE);
    if (!avg_core_save(&s->core)) {          /* 写失败：错误音，留在原地 */
        avg_beep_play(AVG_SFX_EV_FAIL);
        return GC_EV_NONE;
    }
    s->save_flash = true;
    s->save_flash_until = (int64_t)g->app->now_ms + 700;
    g->area = (EpdRect){AVGR_MENU_BTN.x - 12, AVGR_MENU_BTN.y - 12,
                        AVGR_MENU_BTN.width + 24, AVGR_MENU_BTN.height + 24};
    g->want_du = true;
    g->want_full = false;
    return GC_EV_DIRTY;
}

/* 标题「开始/继续游戏」点击反馈（2026-10-18 用户需求，参考 GBA 原版
 * NOW LOADING + PyWright 确认音，最省资源版）：
 *  ① 立即蜂鸣确认（LEDC PWM，零显示成本）——“点到了”；
 *  ② 按钮原地转「加载中……」，只小区域 DU 快刷一次（~300ms）——“在干活”；
 *  ③ 解码期间（2~8s）屏幕完全静止，就绪后场景整屏一次到位。
 * 全程只 2 次刷新，无进度条逐级刷屏。 */
static gc_ev_t start_or_continue(avgr_state_t* s, gc_ctx_t* g, avg_input_t in) {
    avg_beep_play(AVG_SFX_SELECT);                   /* ① 点击成功：短确认音 */
    s->running = true;
    s->loading_btn = (in == AVG_INPUT_CONTINUE) ? 2 : 1;   /* 谁在加载谁变 */
    uint32_t dirty = avg_core_input(&s->core, in, 0);
    if (!dirty) {                                    /* 读档失败/启动失败：错误音反馈 */
        s->running = false;
        s->loading_btn = 0;
        avg_beep_play(AVG_SFX_EV_FAIL);
        return GC_EV_NONE;
    }
    if (s->core.assets_pending) {                    /* ② 按钮转加载态（小刷） */
        s->loading = true;
        g->area = (EpdRect){452, 352, 312, 208};     /* 开始+继续按钮带边距（v3 几何） */
        g->want_du = true;                           /* 按钮态翻转：DU 小刷 */
        g->want_full = false;
        return GC_EV_DIRTY;
    }
    return apply(s, g, dirty);                       /* 无图场景：直接进 */
}

static gc_ev_t input(gc_ctx_t* g, const ui_gesture_event_t* ev) {
    avgr_state_t* s = (avgr_state_t*)g->state;
    if (ev->type != UI_GESTURE_TAP || !s->loaded) return GC_EV_NONE;
    if (s->loading) return GC_EV_NONE;   /* 解码中吞点按：防乱点推进剧情 */
    int x = ev->x0, y = ev->y0;
    switch (s->core.mode) {
    case AVG_MODE_TITLE:
        if (ui_rect_hit(AVGR_SFX_BTN, x, y))
            return toggle_sfx(s, g);
        if (x >= 462 && x <= 754 && y >= 362 && y <= 446)
            return start_or_continue(s, g, AVG_INPUT_START);
        if (s->has_save && x >= 462 && x <= 754 && y >= 466 && y <= 550)
            return start_or_continue(s, g, AVG_INPUT_CONTINUE);
        return GC_EV_NONE;
    case AVG_MODE_DIALOG:
        if (ui_rect_hit(AVGR_MENU_BTN, x, y))
            return save_game(s, g);
        if (ui_rect_hit(AVGR_EV_BTN, x, y))
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_OPEN_EVIDENCE, 0));
        return finish(s, g, avg_core_input(&s->core, AVG_INPUT_TAP, 0));
    case AVG_MODE_TESTIMONY:
        if (ui_rect_hit(AVGR_MENU_BTN, x, y))
            return save_game(s, g);
        if (ui_rect_hit(AVGR_EV_BTN, x, y))
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_OPEN_EVIDENCE, 0));
        if (s->panel)
            for (int i = 0; i < s->panel->count; ++i) {
                if (i >= s->core.t_unlock) break;   /* 未解锁行不命中（引擎同门） */
                int sy = AVGR_STMT_Y0 + i * AVGR_STMT_LH;
                if (y >= sy - 32 && y <= sy + 24 && x >= 30 && x <= 1186)
                    return finish(s, g, avg_core_input(&s->core, AVG_INPUT_STMT, i));
            }
        return GC_EV_NONE;
    case AVG_MODE_EVIDENCE:
        if (x >= 1104 && x <= 1196 && y >= 40 && y <= 92)
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_CLOSE_EVIDENCE, 0));
        if (s->ask_mode && s->sel_ev >= 0 &&
            x >= 984 && x <= 1172 && y >= 596 && y <= 664) {
            int ev_idx = s->sel_ev; s->sel_ev = -1;
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_PRESENT_EVIDENCE, ev_idx));
        }
        {
            int known = 0;
            for (int i = 0; i < s->script->evidence_count; ++i) {
                if (!avg_core_ev_known(&s->core, i)) continue;
                int col = known % AVGR_COLS, row = known / AVGR_COLS;
                int cx = AVGR_GRID_X0 + col * (AVGR_CELL_W + 8);
                int cy = AVGR_GRID_Y0 + row * (AVGR_CELL_H + 10);
                if (cy + AVGR_CELL_H > AVGR_DETAIL_Y - 12) break;  /* 与绘制同界 */
                if (x >= cx && x <= cx + AVGR_CELL_W && y >= cy && y <= cy + AVGR_CELL_H)
                    return finish(s, g, avg_core_input(&s->core, AVG_INPUT_PICK_EVIDENCE, i));
                known++;
            }
        }
        return GC_EV_NONE;
    case AVG_MODE_VERDICT:
        avg_core_show_title(&s->core);
        s->running = false;
        return apply(s, g, AVG_DIRTY_UI);
    }
    return GC_EV_NONE;
}

static gc_ev_t key(gc_ctx_t* g, int k) {
    avgr_state_t* s = (avgr_state_t*)g->state;
    if (!s->loaded || s->loading) return GC_EV_NONE;
    /* 左键（KEY_1）：推进/开关浮层。 */
    if (k == UI_KEY_1) {
        if (s->core.mode == AVG_MODE_DIALOG)
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_TAP, 0));
        if (s->core.mode == AVG_MODE_TESTIMONY)
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_OPEN_EVIDENCE, 0));
        if (s->core.mode == AVG_MODE_EVIDENCE)
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_CLOSE_EVIDENCE, 0));
        return GC_EV_NONE;
    }
    /* 游戏内右键=返回/取消（2026-10-18 用户：误触退出太容易，应返回上一级）：
     * 证物簿→关闭（取消）；判决页→回标题；标题页→退出游戏（无损失）；
     * 对话/证言中右键无操作（退出只留长按中键，避免丢进度）。 */
    if (k == UI_KEY_3) {
        if (s->core.mode == AVG_MODE_EVIDENCE)
            return finish(s, g, avg_core_input(&s->core, AVG_INPUT_CLOSE_EVIDENCE, 0));
        if (s->core.mode == AVG_MODE_VERDICT) {
            avg_core_show_title(&s->core);
            s->running = false;
            return apply(s, g, AVG_DIRTY_UI);
        }
        if (s->core.mode == AVG_MODE_TITLE) return GC_EV_QUIT;
        return GC_EV_NONE;
    }
    return GC_EV_NONE;
}

/* ============ 内存：用完即释放（用户 2026-10-18） ============
 * 场景代次变化（装载/逐出）后，把不属于当前场景（1 bg + 3 char）的
 * 缓存图立即释放；当前场景图保留供重绘，退页 quit 全量释放。 */
static void trim_assets(avgr_state_t* s) {
    const char* keep[4] = {0};
    if (s->core.bg_file[0]) keep[0] = s->core.bg_file;
    for (int i = 0; i < 3; ++i)
        if (s->core.char_files[i][0]) keep[i + 1] = s->core.char_files[i];
    for (int i = 0; i < AVG_ASSET_SLOTS; ++i) {
        avg_asset_slot_t* slot = &s->assets.slots[i];
        if (!slot->gray || !slot->path[0]) continue;
        const char* base = strrchr(slot->path, '/');
        base = base ? base + 1 : slot->path;
        bool hit = false;
        for (int k = 0; k < 4 && !hit; ++k)
            if (keep[k] && !strcmp(base, keep[k])) hit = true;
        if (!hit) {
            heap_caps_free(slot->gray);
            slot->gray = NULL;
            if (slot->mask) {
                heap_caps_free(slot->mask);
                slot->mask = NULL;
            }
            slot->path[0] = 0;
        }
    }
}

/* ===================== tick：解码驱动（装载期间零刷屏） =====================
 * 资产 pending 时（标题开始后/场景切换后）：保持加载反馈画面不动，
 * 逐 tick 解码一张；就绪的那一 tick 引擎返回 DIRTY_SCENE，这里统一
 * 清 loading 并一次推屏到位（打字机逐字仍屏蔽：整句直出）。 */
static gc_ev_t tick(gc_ctx_t* g) {
    avgr_state_t* s = (avgr_state_t*)g->state;
    if (!s->loaded || !s->running) return GC_EV_NONE;
    /* 存档反馈停驻：到点整屏回标题；停驻期间不推进剧情。 */
    if (s->save_flash) {
        if ((int64_t)g->app->now_ms >= s->save_flash_until) {
            s->save_flash = false;
            s->running = false;
            avg_core_show_title(&s->core);    /* ops_title 回调更新 has_save */
            return apply(s, g, AVG_DIRTY_UI);  /* 整屏回标题 */
        }
        return GC_EV_NONE;
    }
    uint32_t dirty = avg_core_tick(&s->core, g->app->now_ms);
    /* 解码中：屏上保持「加载中」反馈，不推任何中间帧。 */
    if (s->core.assets_pending) return GC_EV_NONE;
    s->loading = false;    /* 就绪（或本无图）：此后画面可以正常推进 */
    s->loading_btn = 0;    /* 按钮加载态解除（标题或直接进场景） */
    /* 无动态画面：打字机逐字刷新取消，只认句子级/场景级变化。 */
    dirty &= ~(uint32_t)AVG_DIRTY_TEXT;
    if (dirty == AVG_DIRTY_NONE) return GC_EV_NONE;
    return apply(s, g, dirty);
}

/* ===================== 生命周期 ===================== */
static avg_render_ops_t* k_ops_for(avgr_state_t* s) {
    /* ops 存进 state（PSRAM，会话生命周期）；返回栈/static 地址都会悬空。 */
    s->ops.ctx = s;
    s->ops.scene = ops_scene; s->ops.dialog = ops_dialog; s->ops.testimony = ops_testimony;
    s->ops.evidence = ops_evidence; s->ops.verdict = ops_verdict; s->ops.title = ops_title;
    return &s->ops;
}

static bool launch(gc_ctx_t* g, const char* title_arg) {
    (void)title_arg;   /* 单案件包：案件名即包名，无需外部参数。 */
    avgr_state_t* s = (avgr_state_t*)calloc(1, sizeof(avgr_state_t));
    if (!s) return false;
    g->state = s;
    s->script = (avg_script_t*)heap_caps_malloc(sizeof(avg_script_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->script) return false;
    avg_assets_init(&s->assets);
    avg_assets_set_decoder(avgr_image_decode);
    if (!g->pkg_dir[0]) return false;   /* 无包：paint 走引导页 */
    snprintf(s->script_path, sizeof(s->script_path), "%s/script.txt", g->pkg_dir);
    if (!avg_script_load_file(s->script, s->script_path)) {
        /* 解析器把原因写进 script->error；静默失败会浪费整轮串口排查（2026-10-18）。 */
        ESP_LOGE("avgr", "script load failed: %s [%s]", s->script_path, s->script->error);
        return false;
    }
    if (!avg_core_init(&s->core, s->script, k_ops_for(s), &s->assets, g->pkg_dir)) {
        ESP_LOGE("avgr", "core init failed");
        return false;
    }
    /* V2（用户 2026-10-18）：存档写 SD 独立目录（每游戏互不干扰），
     * 不随 zip 更新清缓存丢失。save_path 为公开字段直接覆写。 */
    (void)mkdir("/sdcard/.gsaves", 0775);
    char save_dir[192];
    snprintf(save_dir, sizeof(save_dir), "/sdcard/.gsaves/%.40s", g->pkg_name);
    (void)mkdir(save_dir, 0775);
    snprintf(s->core.save_path, sizeof(s->core.save_path), "%s/save.txt", save_dir);
    /* 音效开关持久化：sfx.txt 一行 "1"/"0"，无文件=默认开。 */
    snprintf(s->sfx_path, sizeof(s->sfx_path), "%s/sfx.txt", save_dir);
    {
        FILE* f = fopen(s->sfx_path, "r");
        if (f) { avg_beep_set_enabled(fgetc(f) == '1'); fclose(f); }
        else avg_beep_set_enabled(true);
    }
    avg_core_show_title(&s->core);
    s->loaded = true;
    return true;
}

static void quit(gc_ctx_t* g) {
    avgr_state_t* s = (avgr_state_t*)g->state;
    if (!s) return;
    avg_beep_stop();
    if (s->loaded) avg_core_shutdown(&s->core);   /* 释放资产 PSRAM */
    if (s->script) {
        avg_script_free(s->script);   /* cmds 实量数组（2026-10-18 实量化） */
        heap_caps_free(s->script);
    }
    free(s);
    g->state = NULL;
}

const gc_plugin_t gc_plugin_avgr = {
    .id = "avgr",
    .name = "推理游戏",
    .cat = GC_CAT_AVGR,
    .landscape = true,          /* 横屏插件（GBA 版式）：内核包旋转窗口与坐标互换 */
    .launch = launch,
    .quit = quit,
    .paint = paint,
    .input = input,
    .key = key,
    .tick = tick,
};
