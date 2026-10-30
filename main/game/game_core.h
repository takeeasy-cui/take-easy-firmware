/*
 * game_core.h — GameCore 统一游戏内核：插件接口与宿主 API。
 *
 * 设计原则（2026-10-18，v1 重构）：
 *   1. 主引擎至上：插件生命周期、运行互斥、TF 卡资源包装载、输入分发、
 *      刷新策略、共享 UI 绘制全部收在本内核；插件只实现游戏逻辑。
 *   2. 最大化复用：ZIP 访问直接复用 EPUB 的 zip_reader（只读、CRC 校验）；
 *      图片解码复用 book_image_file_*；蜂鸣复用 avg_beep/read_pico_buzzer。
 *      插件不需要的能力全部省略。
 *   3. 互斥运行：同一时刻只有一个插件实例存活；launch 时内核先 quit 卸载
 *      上一插件（含 PSRAM 归还与包卸载）。
 *
 * 资源包命名规则（TF 卡 /sdcard/games/）：
 *   推理 AVG   ：avgr-<game>-<case>.zip   （包根 = 案件目录：script.txt + images/）
 *   文字游戏   ：text-<name>.zip           （包根：story.txt）
 *   步步高 BBK ：bbk-<name>                （大资源目录直挂：/sdcard/games/bbk-<name>/
 *                                            内含 DAT.LIB/HZK16/ASC16/FMJHD.BIN）
 *   目录直挂   ：/sdcard/games/<pkg>/ 同名目录免 zip（各分类通用，开发期友好）。
 *   zip 首次挂载时整体解压到 /sdcard/.gcache/<name>/；zip 大小变化自动重解。
 *   后续分类（数织 nono-、棋类 chess-、GB gb-）沿用同一规则。
 *
 * One kernel to run them all: plugins own game logic only — lifecycle,
 * exclusive launching, package mounting, input routing, refresh policy
 * and shared painting live here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "ui_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 分类与应用入口映射 ===================== */

typedef enum {
    GC_CAT_AVGR = 0,   /* 推理 AVG（逆转裁判等；宿主引擎 avg_engine）【M6 回归位，M2 起移出入口】 */
    GC_CAT_TEXT,       /* 对话游戏（story_kernel + 对话框流渲染，text- 前缀） */
    GC_CAT_NOVEL,      /* 互动小说（story_kernel + book_layout 分页渲染，novel- 前缀） */
    GC_CAT_STORY,      /* 文绎合体入口（2026-10-30 用户）：text-+novel- 同列表，
                         挂载后按包子类型路由 gtext/gnovel 子插件 */
    GC_CAT_BBK,        /* 步步高电子词典游戏（伏魔记）【M5，暂移出入口】 */
    GC_CAT_QUIZ,       /* 习测（应知应会题库，quiz- 前缀；quiz_engine） */
} gc_cat_t;

/* 分类 → TF 卡包前缀（命名规则的唯一事实来源）。 */
const char* gc_cat_prefix(gc_cat_t cat);

/* ===================== 插件运行上下文 =====================
 * launch 时由内核填写；插件把它当自己的 this 指针保存。 */

typedef struct gc_plugin_s gc_plugin_t;

typedef struct {
    app_ctx_t*           app;       /* 宿主页面上下文（fb/hl/now_ms/请求字段） */
    const gc_plugin_t*   plugin;    /* 自己 */
    char                 pkg_name[48];    /* 包名（不含前缀与扩展名） */
    char                 pkg_dir[192];    /* 已挂载资源根目录（""=无包；paint 画引导页） */
    void*                state;     /* 插件私有状态（由插件 malloc/quit 释放） */
    char                 pkg_list[8][48]; /* 分类包名列表（入口列表页用） */
    int                  pkg_count;
    bool                 listing;   /* true=选择列表页（尚未进入哪个包） */
    /* ---- 刷新裁决（回调自行置位，内核换算为 app_redraw_t） ---- */
    EpdRect              area;      /* 本次变化区域（插件画布坐标） */
    bool                 want_full; /* true=整屏 GC16 */
    bool                 want_du;   /* true=局部区域走 DU 快刷（纯黑白 UI 态，
                                      * 如按钮翻转/对话框文字推进）；false=GL16
                                      * 定稿（灰阶画质）。仅 AREA 生效（2026-10-18） */
} gc_ctx_t;

/* ===================== 插件接口 =====================
 * 只需真正用到的方法；其余置 NULL，内核对 NULL 安全跳过。
 * state!=NULL 才会收到 input/key/tick（launch 失败只剩 paint 引导页）。 */

typedef enum {
    GC_EV_NONE = 0,          /* 无变化，不刷屏 */
    GC_EV_DIRTY,             /* g->area / want_full 描述的刷新 */
    GC_EV_PAGE,              /* 整页 APP_PAGE_REFRESH_MODE */
    GC_EV_QUIT,              /* 退出游戏回应用页 */
} gc_ev_t;

struct gc_plugin_s {
    const char* id;          /* 固定标识：avgr / text / bbk */
    const char* name;        /* 应用入口显示名（应用页 tile 标签） */
    gc_cat_t    cat;         /* 该插件服务的分类（决定扫包前缀与入口） */
    bool        landscape;   /* true=横屏插件：画布 1216×684，内核包旋转窗口与坐标互换 */
    bool        middle_menu; /* true=中短键下发给插件（文绎弹存读档菜单）；
                                * false 维持内核默认：中短键=整屏强刷 */

    /* 进插件页时创建游戏会话。pkg_dir 已挂载（""=无包，插件画引导页）。
     * title_arg：入口直传参数（NULL=默认）。返回 false=会话失败（state 置 NULL）。 */
    bool (*launch)(gc_ctx_t* g, const char* title_arg);
    /* 退页/切插件/TF 失效：释放全部 PSRAM 与文件句柄（含 state → NULL）。 */
    void (*quit)(gc_ctx_t* g);
    /* 纯绘制（强刷复用，禁副作用）。任何时候都必须可调。 */
    void (*paint)(gc_ctx_t* g, uint8_t* fb);
    /* 触摸手势（已识别手势直通）。 */
    gc_ev_t (*input)(gc_ctx_t* g, const ui_gesture_event_t* ev);
    /* 三键（中键长按=退出由内核统一处理，短按下发给插件）。 */
    gc_ev_t (*key)(gc_ctx_t* g, int key);
    /* 周期驱动（打字机/动画/节流自动存档）。now_ms 在 g->app。 */
    gc_ev_t (*tick)(gc_ctx_t* g);
};

/* ===================== 宿主 API（薄壳 APP 调） ===================== */

/// 启动插件（分类内最新可用包）。互斥：先 quit 在跑插件。返回 launch 结果。
bool gc_launch(const gc_plugin_t* p, const char* title_arg, app_ctx_t* app);

/// 进入插件分类页：扫包并停在列表页（新入口流程）。
void gc_open_list(const gc_plugin_t* p, app_ctx_t* app);

/// 列表页选择某个包并启动（内核内部使用；命中测试见 gc_ui_pkg_list_hit）。
app_redraw_t gc_pick_package(int index);

/// 当前在跑插件与上下文（无则 NULL）。
const gc_plugin_t* gc_active(void);

/// 退出清理：quit 插件 + 卸载包。幂等。
void gc_shutdown(void);

/* —— 薄壳桥接（app 页把 app_desc_t 回调直接接到内核） —— */
void         gc_app_render(app_ctx_t* ctx, uint8_t* fb);
app_redraw_t gc_app_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev);
app_redraw_t gc_app_key(app_ctx_t* ctx, int key);
app_redraw_t gc_app_key_long(app_ctx_t* ctx, int key);   /* 中长按=退出（恒语义） */
app_redraw_t gc_app_tick(app_ctx_t* ctx);
app_redraw_t gc_app_enter(const gc_plugin_t* p, const char* title, app_ctx_t* ctx);
EpdRect      gc_app_area(app_ctx_t* ctx);
/// 自定义分区（app_desc_t.present）：AREA 走 GL16 定稿刷（游戏 16 级灰画面
/// 用 DU 动态刷灰阶全糊）；返回 true=已自刷。非 AREA 返回 false 走宿主默认。
bool         gc_app_present(app_ctx_t* ctx, app_redraw_t redraw);
void         gc_app_exit(app_ctx_t* ctx);
void         gc_app_media_lost(app_ctx_t* ctx);

/* ===================== 资源包 API ===================== */

/// 列出分类包名（zip 去扩展名；目录原名）。kinds/kind_slots 可空：逐包写入
/// 子类型（GC_CAT_STORY 列表的 游戏/小说 徽标用）。返回包数。
int gc_pkg_scan(gc_cat_t cat, char* out, size_t capacity, int slots,
                gc_cat_t* kinds, int kind_slots);

/// 当前挂载包的子类型（文绎：GC_CAT_TEXT/GC_CAT_NOVEL；mount 时记录）。
gc_cat_t gc_pkg_mounted_kind(void);

/// 挂载分类内第 index 个包：返回资源根目录（zip→缓存目录；目录→游戏目录）；
/// 失败 NULL。内核 launch 已自动调用（index 0），一般无需再碰。
const char* gc_pkg_mount(gc_cat_t cat, int index);

/// 卸载当前挂载的包（保留缓存目录加速下次进入）。
void gc_pkg_unmount(void);

/* ===================== 共享 UI（game_host_ui.c） =====================
 * 插件通用墨水屏绘制原语（无状态可复用）；逻辑竖屏 684×1216，8bit 灰+抖动。 */

void gc_ui_clear(uint8_t* fb);
void gc_ui_dialog_frame(uint8_t* fb, int top);                    /* 对话框底板+顶线 */
void gc_ui_name_tag(uint8_t* fb, int top, const char* name);      /* 说话人黑标签 */
void gc_ui_button(uint8_t* fb, EpdRect r, const char* label, bool dark);
void gc_ui_wait_cursor(uint8_t* fb, int x, int y);                /* ▼ 继续光标 */
void gc_ui_title_page(uint8_t* fb, const char* game, const char* sub, bool has_continue);
/// 打字机安全折行：text 前 shown 字节按每行 width_units（中文=2）回调 draw(line,y)。
void gc_ui_typewrap(const char* text, int shown, int max_lines, int width_units,
                    void (*draw)(void* user, const char* line, int y), void* user, int y0, int lh);
/// 只测折行数不绘制（动态布局用）：返回按宽度折行后的总行数（skip_lines 跳过前 N 行）。
int  gc_ui_typewrap_count(const char* text, int width_units, int skip_lines);
/// 灰度图抖动居中贴入区域。
void gc_ui_blit_gray(uint8_t* fb, const uint8_t* gray, unsigned gw, unsigned gh,
                     int x, int y, unsigned area_w, unsigned area_h);
/// 带透明蒙版灰度贴入（avg 立绘）：mask 行打包 bit=1 不透明；NULL=全不透明。
void gc_ui_blit_gray_a(uint8_t* fb, const uint8_t* gray, unsigned gw, unsigned gh,
                       const uint8_t* mask, int x, int y,
                       unsigned area_w, unsigned area_h);
/// 缺素材占位（灰框 + 文件名）。
void gc_ui_missing(uint8_t* fb, const char* file, int x, int y, unsigned w, unsigned h);
/// 资源包缺失引导页（竖屏插件）。
void gc_ui_nopkg(uint8_t* fb, const char* game_name, gc_cat_t cat);
/// 资源包缺失引导页（横屏插件：内核在旋转窗口内直绘）。
void gc_ui_nopkg_land(uint8_t* fb, const char* game_name, gc_cat_t cat);
/// 入口包列表页（恒竖屏 684×1216：竖持点选；进游戏后才切旋转）。
/// kinds 可空（仅 GC_CAT_STORY 传）：逐项子类型，驱动 游戏/小说 徽标。
void gc_ui_pkg_list(uint8_t* fb, const char* title, const char* const* names,
                    int count, gc_cat_t cat, const gc_cat_t* kinds);
/// 列表页命中测试（竖屏坐标；返回行号，-1=未命中）。
int gc_ui_pkg_list_hit(int x, int y, int count);

#ifdef __cplusplus
}
#endif
