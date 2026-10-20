/*
 * avg_core.h — 剧情状态机：插件化引擎核心，零 UI 依赖。
 *
 * 平台层职责（见 app_avg.c）：实现 avg_render_ops_t 回调表、把触摸/按键
 * 翻译成 avg_input_t、每 tick 调 avg_core_tick 驱动打字机与资产加载、按
 * 返回的 dirty 位决定墨水屏局部刷新区域。换平台 = 重写这一层。
 *
 * 本模块职责：解读脚本（say/think/testimony/press/clash/ask_ev/branch/
 * gain_ev/end），维护模式与进度，证据/证言的判定查表，存档与读档。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "avg_asset.h"
#include "avg_script.h"
#include "avg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 平台渲染回调（全部由适配层实现，引擎只调用） ---------- */
typedef struct avg_render_ops_s {
    void* ctx;
    /* 场景：背景 + 三槽立绘（缺素材传 NULL，平台画占位）。focus=说话槽或-1。
     * ch_mask：立绘 1bit 蒙版（1=不透明，行打包），PNG 无 alpha 时 NULL。 */
    void (*scene)(void* ctx, const uint8_t* bg, unsigned bw, unsigned bh,
                  const uint8_t* ch[3], const unsigned w[3], const unsigned h[3],
                  const uint8_t* ch_mask[3],
                  const avg_side_t side[3], int focus);
    /* 对话框：shown=打字机已显示字节数（UTF-8 安全截断）；name 空=内心独白。 */
    void (*dialog)(void* ctx, const char* name, const char* text, int shown);
    /* 证言面板：active=当前待威慑条（-1 无）。 */
    void (*testimony)(void* ctx, const avg_testimony_t* t, int active);
    /* 证物簿：selected=列表选中（详情态），detail=true 显示描述与出示按钮。 */
    void (*evidence)(void* ctx, const avg_evidence_t* ev, int count,
                     int selected, bool detail, bool ask_mode);
    /* 判决页与标题页。 */
    void (*verdict)(void* ctx, int good);
    void (*title)(void* ctx, const char* case_title, bool has_save);
} avg_render_ops_t;

/* ---------- 输入（平台翻译后的语义动作） ---------- */
typedef enum {
    AVG_INPUT_TAP = 0,
    AVG_INPUT_STMT,              /* arg = 证言条目序号（0 基）：威慑该条 */
    AVG_INPUT_OPEN_EVIDENCE,     /* 打开证物簿 */
    AVG_INPUT_CLOSE_EVIDENCE,
    AVG_INPUT_PICK_EVIDENCE,     /* arg = 证据索引：查看详情 */
    AVG_INPUT_PRESENT_EVIDENCE,  /* arg = 证据索引：出示（ask 态或证言中） */
    AVG_INPUT_START,             /* 标题：从头开始 */
    AVG_INPUT_CONTINUE,          /* 标题：读档 */
} avg_input_t;

/* ---------- dirty 位（tick/input 返回，平台合并刷新区用） ---------- */
enum {
    AVG_DIRTY_NONE    = 0,
    AVG_DIRTY_DIALOG  = 1 << 0,  /* 对话框变化（新句/完成） */
    AVG_DIRTY_TEXT    = 1 << 1,  /* 打字机新增字符（低频跟随即可） */
    AVG_DIRTY_SCENE   = 1 << 2,  /* 背景/立绘变化（建议整屏或场景区） */
    AVG_DIRTY_UI      = 1 << 3,  /* 证言/证据/标题/判决界面变化 */
};

typedef struct {
    const avg_script_t* script;
    const avg_render_ops_t* ops;
    avg_assets_t* assets;
    char save_path[160];
    char asset_dir[192];        /* /sdcard/games/<case>/images */
    char case_dir[192];

    /* 进度与剧情状态。 */
    avg_mode_t mode;
    avg_state_t st;             /* pc/flags/known —— 存档内容 */
    int cur_say;                /* say 命令索引（重绘对话用） */
    int ask_wrong_label;        /* 出示错误跳转（-1=未开启） */
    char last_testimony_title[96];
    int  t_active;              /* 最近威慑到的条目 */
    int  t_unlock;              /* 证言已解锁条数（初始 1，威慑到第 i 条后解锁到
                                  * i+2——逐条给出，2026-10-18 用户要求对齐
                                  * GBA 原版：一次性全列出等于把矛盾摊开剧透）；
                                  * 同标题重回面板保留进度。 */
    avg_typewriter_t tw;

    /* 场景签名（变化时重建场景并预载资产）。 */
    char bg_file[128];
    char char_files[3][128];
    avg_side_t char_side[3];
    int  focus_slot;
    int  testimony_cmd;    /* 当前证言面板的 TESTIMONY 命令索引（-1=无）；
                            * TESTIMONY 模式存档以它为恢复点（2026-10-18） */
    int  assets_pending;
    /* 逐张装载游标（2026-10-18 v3）：step=0..3 指向待载槽（0=bg, 1..3=char），
     * 走过 3 即完成；total=-1 待重算。装载期间 tick 返回 NONE——反馈由平台
     * 在 input 阶段一次小区域刷新给出（GBA 原版 NOW LOADING 式，零中间刷屏）。 */
    int  asset_total;
    int  asset_done;
    int  asset_step;          /* 内部游标：0=bg, 1..3=char slot */
    bool scene_dirty;
    bool replaying;           /* 读档时静默重放：跳过一切展示回调 */
    int  replay_limit;        /* 重放截止 pc（-1 无限制） */
    uint64_t now_ms;
} avg_core_t;

/* ---------- 生命周期 ---------- */

bool avg_core_init(avg_core_t* c, const avg_script_t* s, const avg_render_ops_t* ops,
                   avg_assets_t* assets, const char* case_dir);
void avg_core_show_title(avg_core_t* c);     /* 进入标题页（含继续判定） */
bool avg_core_start_fresh(avg_core_t* c);
bool avg_core_start_saved(avg_core_t* c);
bool avg_core_save(avg_core_t* c);
bool avg_core_has_save(const avg_core_t* c);
/// 证据是否已在证物簿（运行状态，读档自动恢复）。
bool avg_core_ev_known(const avg_core_t* c, int ev_index);
void avg_core_shutdown(avg_core_t* c);       /* 释放全部 PSRAM（app 退页） */

/* ---------- 驱动 ---------- */

uint32_t avg_core_tick(avg_core_t* c, int64_t now_ms);   /* 打字机/资产装载 */
uint32_t avg_core_input(avg_core_t* c, avg_input_t in, int arg);

/* 当前是否等待玩家在证言/证据界面的操作（用于点按命中区分）。 */
bool avg_core_wants_evidence(const avg_core_t* c);

#ifdef __cplusplus
}
#endif
