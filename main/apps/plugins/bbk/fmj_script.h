/*
 * fmj_script.h - 剧情脚本虚拟机：变量/事件、指令解释、阻塞操作
 *
 * 墨水屏适配：原版 40ms 帧循环改为按键事件驱动。
 * 每次按键 -> fmj_script_input() -> 推进 Operate 一步或执行下一条指令。
 */
#ifndef FMJ_FMJ_SCRIPT_H
#define FMJ_FMJ_SCRIPT_H

#include "fmj.h"
#include "fmj_canvas.h"
#include "fmj_res.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 变量与事件（ScriptResources） ---------- */
extern int16_t fmj_vars[FMJ_VAR_MAX];
extern uint8_t fmj_events[FMJ_EVENT_MAX];

void fmj_vars_init_local(void);
void fmj_vars_init_global(void);
void fmj_events_init_all(void);
static inline void fmj_event_set(int n) { if (n >= 0 && n < FMJ_EVENT_MAX) fmj_events[n] = 1; }
static inline void fmj_event_clear(int n) { if (n >= 0 && n < FMJ_EVENT_MAX) fmj_events[n] = 0; }
static inline bool fmj_event_get(int n) { return n >= 0 && n < FMJ_EVENT_MAX && fmj_events[n]; }

/* ---------- 阻塞操作（Operate） ---------- */
typedef struct fmj_op fmj_op_t;
struct fmj_op {
    bool done;
    void (*key)(fmj_op_t *op, int key, bool is_up);
    void (*draw)(fmj_op_t *op, fmj_canvas_t *c);
    void (*destroy)(fmj_op_t *op);
    /* 私有数据区 */
    union {
        struct {                 /* 对话（say） */
            const uint8_t *text;
            const fmj_image_t *head;
            int head_idx;        /* 头像 PIC 资源 index（高清素材用） */
            int i_of_text, i_of_next;
            bool key_down_seen;
        } say;
        struct {                 /* 选择支（choice） */
            uint8_t *c1, *c2;     /* 补齐到等长的 GBK 串（malloc） */
            int w, bgx, bgy;
            int cur;
            bool has_select, key_down_seen;
            int addr;
        } choice;
        struct {                 /* 提示（message/gaingoods/timemsg） */
            uint8_t *text;       /* malloc 的 GBK 串 */
            bool key_down_seen;
        } msg;
        struct {                 /* 卷轴字幕（showgut） */
            uint8_t *text;
            const fmj_image_t *top, *btm;
            int top_idx, btm_idx;  /* 高清装饰条 PIC 资源 index */
            int cur_y;
        } gut;
        struct {                 /* 一次性绘制（draw once） */
            int unused;
        } once;
        struct {                 /* 菜单（menu） */
            uint8_t *items[8];   /* 最多 8 项 */
            int item_count;
            int cur;
            int rv_addr;
            bool has_select;
        } menu;
    } d;
};

/* ---------- 脚本进程 ---------- */
typedef struct fmj_proc {
    fmj_gut_t gut;
    int pc;                 /* code 内字节偏移 */
    bool running;
    fmj_op_t *op;           /* 当前阻塞操作（NULL 表示可继续执行） */
    struct fmj_proc *prev;  /* callchapter 栈 */
    /* 事件定时器（原版 timer*500ms；这里以玩家按键为节拍） */
    int timer_ticks;
    int timer_event_id;
} fmj_proc_t;

extern fmj_proc_t *fmj_proc_cur;

/* ---------- 流程控制 ---------- */
/* 开始新章节（替换当前进程；清空 NPC；重置局部变量） */
void fmj_script_start_chapter(int type, int index);
/* 调用章节（压栈） */
void fmj_script_call_chapter(int type, int index);
/* 脚本是否在运行 */
bool fmj_script_running(void);
/* 退出脚本（cmd_callback） */
void fmj_script_exit(void);
/* 触发场景事件（NPC 对话/地图事件，1-255） */
bool fmj_script_trigger_event(int event_id);
/* 地图事件触发（原版 triggerMapEvent：事件或随机战斗），由行走调用 */
void fmj_script_map_event(int x, int y);
/* 当前是否有活动操作 */
bool fmj_script_has_op(void);

/* 覆盖型等待界面：宿主据此隐藏虚拟按键、改点击屏幕任意处推进。
 * saying=对话 / gut=卷轴字幕（序幕）/ msg=黑框提示。 */
bool fmj_script_saying(void);
bool fmj_script_gut_active(void);
bool fmj_script_msg_active(void);
/* 按键输入 */
void fmj_script_input(int key, bool is_up);
/* 绘制当前操作覆盖层（脚本运行时调用；战斗时由战斗模块接管） */
void fmj_script_op_draw(fmj_canvas_t *c);
/* 释放全部脚本进程（退出游戏） */
void fmj_script_reset(void);

/* 当前操作完成后推进脚本（每帧/每输入后调用） */
void fmj_script_step(void);

/* 剧情战斗入口（cmd_enterfight），战斗结果通过事件触发返回脚本 */
typedef struct {
    int round_max;
    int monsters[3];
    int scr[3];
    int evt_rounds[3];
    int evt_nums[3];
    int loss_to;
    int win_to;
} fmj_fight_cfg_t;

/* 由 fmj_combat 实现；脚本调用 */
void fmj_combat_enter(const fmj_fight_cfg_t *cfg);
/* 随机战斗触发检查（由 fmj_script_map_event 内部使用） */
void fmj_combat_random_check(void);

/* 游戏结束（cmd_gameover / 战败），由 fmj_game 实现 */
void fmj_game_request_over(void);
/* 玩家升级提示操作（cmd_actorlayerup 简化版） */
fmj_op_t *fmj_op_levelup_msg(const char *name_gbk);
/* 菜单（cmd_menu）由脚本创建；游戏主菜单在 fmj_game 中 */

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_SCRIPT_H */
