/*
 * avg_types.h — PyWright 风格文字推理 AVG 引擎的核心数据结构。
 *
 * 引擎核心不依赖任何 UI/显示头文件：绘制通过 avg_render 回调接口由
 * 平台层注入，音频走 avg_beep（read_pico_buzzer_tone, LEDC），图片解码
 * 由平台层注入。移植新固件 = 重写一个适配层文件，引擎零改动。
 *
 * Core data structures for the PyWright-style adventure engine. The core
 * has zero UI/graphics includes: painting goes through the avg_render
 * callback table, sound through avg_beep (LEDC tone), image decoding is
 * injected by the host. Porting = one adapter file, engine untouched.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 容量上限（单案件内存模型） ===================== */
#define AVG_MAX_TEXT      640      /* 单条对话/证言文本字节 */
#define AVG_MAX_CMDS      4096     /* 单案件命令数（PSRAM 动态分配+实量收缩，此为守卫上限；4096=逆转裁判两章合并包实需 3704+余量） */
#define AVG_MAX_LABELS    256      /* 标签（跳转目标）数（两章合并实需 240） */
#define AVG_MAX_EVIDENCE  24       /* 案件定义证据数（两章合并实需 20；证物簿 UI 按行数动态分页无写死） */
#define AVG_MAX_STMTS     12       /* 单段证言条目数 */
#define AVG_MAX_FLAGS     32       /* 剧情标记数 */
#define AVG_MAX_VOICE_MAP 16       /* 说话人→立绘映射表（voicemap 元数据） */
#define AVG_SAVE_BYTES    2048     /* 存档行缓冲 */

/* 立绘槽位：0=左 1=中 2=右（与绘制回调的 side 参数一致）。 */
typedef enum { AVG_SIDE_L = 0, AVG_SIDE_C = 1, AVG_SIDE_R = 2 } avg_side_t;

/* 证据（证物簿条目）。描述为长文本，脚本内静态存储；
 * 运行期“已获得”由 core 的 known 列表管理（存档内容），不写回脚本表。 */
typedef struct {
    char id[24];
    char name[48];
    char desc[AVG_MAX_TEXT];
    char pic[48];    /* 可选图文件名（images/ev/ 下，缺省首字占位，2026-10-18） */
} avg_evidence_t;

/* 证言条目：庭审面板一行。press/clash 由脚本声明式注册，
 * 引擎只查表执行，不写死任何案件逻辑。 */
typedef struct {
    char text[AVG_MAX_TEXT];
    int  press_label;      /* 威慑本条 → 跳转标签，-1=无追问 */
    int  clash_evidence;   /* 与该证据索引矛盾 → 跳 clash_label，-1=无 */
    int  clash_label;
} avg_stmt_t;

/* 证言段（一次证人发言的完整面板）。 */
typedef struct {
    char title[96];
    avg_stmt_t stmts[AVG_MAX_STMTS];
    int count;
} avg_testimony_t;

/* ===================== 脚本命令 =====================
 * 解析期把文本行变成定长命令；运行期状态机顺序消费，不再做字符串解析。 */
typedef enum {
    AVG_CMD_CASE = 0,      /* 案件元数据：id + 标题 */
    AVG_CMD_LABEL,         /* name */
    AVG_CMD_JUMP,          /* name */
    AVG_CMD_IFSET,         /* flag name, else 落空继续 */
    AVG_CMD_IFCLEAR,       /* flag name 的反义 */
    AVG_CMD_FLAG,          /* flag name（置位） */
    AVG_CMD_BG,            /* file */
    AVG_CMD_CHAR,          /* slot, file, side, focus */
    AVG_CMD_CHAR_OFF,      /* slot */
    AVG_CMD_SAY,           /* speaker, text */
    AVG_CMD_THINK,         /* text */
    AVG_CMD_GAIN_EV,       /* evidence id */
    AVG_CMD_TESTIMONY,     /* title（开始证言；stmt/press/clash 子命令跟随其后） */
    AVG_CMD_STMT,          /* text（属于当前证言） */
    AVG_CMD_PRESS_JUMP,    /* stmt index, label（威慑第 i 条的跳转） */
    AVG_CMD_CLASH_JUMP,    /* stmt index, evidence id, label（矛盾判定） */
    AVG_CMD_ASK_EV,        /* 进入强制出示模式，label_wrong */
    AVG_CMD_END,           /* 1=无罪 0=有罪（判决页） */
} avg_cmd_type_t;

typedef struct {
    avg_cmd_type_t type;
    int    a;                      /* slot / stmt index / verdict */
    int    b;                      /* label id / focus / evidence index */
    char   s1[128];                /* label 名 / 文件 / 说话人 */
    char   s2[AVG_MAX_TEXT];       /* 文本 / 证据 id */
} avg_cmd_t;

/* 标签名 → 命令索引。 */
typedef struct { char name[128]; int cmd; } avg_label_t;

/* ===================== 运行期状态（存档内容） ===================== */

typedef struct {
    int  pc;                       /* 下一条命令索引 —— 存档核心字段 */
    char flag_names[AVG_MAX_FLAGS][12];
    int  flag_count;
    int  known_evidence[AVG_MAX_EVIDENCE];  /* 已获得的证据索引 */
    int  known_count;
} avg_state_t;

typedef enum {
    AVG_MODE_TITLE = 0,   /* 标题页：开始 / 继续 */
    AVG_MODE_DIALOG,      /* 对话推进 */
    AVG_MODE_TESTIMONY,   /* 法庭证言面板（威慑/指认） */
    AVG_MODE_EVIDENCE,    /* 证物簿浮层 */
    AVG_MODE_VERDICT,     /* 判决页 */
} avg_mode_t;

/* 打字机状态。 */
typedef struct {
    const char* text;     /* 当前整句（指向命令缓冲） */
    int  shown;           /* 已显示字节（按 UTF-8 整字符推进由平台层负责） */
    int64_t last_ms;
    bool dirty;           /* 本次 tick 是否新增字符（刷新裁决用） */
} avg_typewriter_t;

#ifdef __cplusplus
}
#endif
