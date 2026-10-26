/*
 * story_kernel.h — Story Engine v1 共用内核（M1 定稿 2026-10-26，docs/local/story-engine-spec-v1.md）。
 *
 * 文本包（/sdcard/games/text-*=对话游戏 / novel-*=互动小说）story.txt 的
 * 逐行解释器：加载、变量/标志、label 表、副作用（bg/char/title）、选项、
 * 存档序列化。渲染（GameFront=对话框流 / NovelFront=book_layout 分页）
 * 由插件实现；内核不碰屏幕。
 *
 * 脚本语法（gtext 风格一行一令，* 前缀；# 注释；UTF-8；宽容未知命令）：
 *   * title 文本                     包标题（novel 章节标题也用它在流内出扉页行）
 *   * bg 名|none                     背景（渲染器解析路径；none=清除）
 *   * char 槽(0-2) 名|none L|C|R [聚焦0|1]   立绘上屏/清槽
 *   * clear                          清立绘不清背景
 *   > 正文                          段落/旁白（继承最近名牌）
 *   * name 名                        粘性名牌（后续 > 的说话人；say 也更新它）
 *   * say 名 正文                    带名牌对话
 *   * hr                             分隔线（渲染为一句居中破折号）
 *   * label 名                       跳转锚点（≤512）
 *   * goto 名                        无条件跳
 *   * ifgoto 名 op 值 名             条件跳（op==,!=,>,<,>=,<=；值可数字或变量）
 *   * ifngoto 名 op 值 名            条件取反跳
 *   * if 名 g 名 / * ifnot 名 g 名   布尔标志跳（gtext 旧语法，兼容）
 *   * set 名 = 数字|名|名 ± 名|数字   整数变量赋值（未声明隐式建槽，≤64）
 *   * set 名                          无 '=' 视为布尔标志置位（gtext 旧语法）
 *   * choice 文案->标签|文案->标签…  直跳式选项（2..6 项；选后跳目标 label）
 *   * choice a|b|c…                  旧式标志选项（选后置 flag c0..c5 顺序继续）
 *   * ending 文案                    结局登记 + 终页
 *   * end                            无名结局
 *
 * One interpreter for both fronts: it never touches the framebuffer; plugins
 * own rendering and call sk_step until a pause event arrives.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SK_MAX_VARS    64
#define SK_MAX_FLAGS   64
#define SK_MAX_LABELS  512
#define SK_MAX_CHOICES 6
#define SK_TEXT_MAX    640
#define SK_NAME_MAX    24
#define SK_SCRIPT_MAX  (2 * 1024 * 1024)   /* story.txt 整读上限 */

typedef enum {
    SK_EV_NONE = 0,   /* 行被内核消化（副作用），无暂停——调用方继续 step */
    SK_EV_TEXT,       /* 一句正文（> 或 * say 或 * hr） */
    SK_EV_CHOICE,     /* 选项集：等调用方选完后 sk_choose() */
    SK_EV_END,        /* * ending 文案（text 可空 = * end） */
    SK_EV_EOF,        /* 脚本走完（无结局命令） */
} sk_event_t;

typedef struct {
    sk_event_t type;
    char name[48];                          /* TEXT：说话人（空=旁白） */
    char text[SK_TEXT_MAX];                 /* TEXT 正文 / END 结局文案 */
    int  choice_count;                     /* CHOICE：2..6 */
    char choice_text[SK_MAX_CHOICES][96];  /* CHOICE：选项文案（渲染用） */
} sk_out_t;

typedef struct {
    char name[SK_NAME_MAX];
    int  line;                             /* 0 基行号 */
} sk_label_t;

typedef struct {
    /* 脚本整读（PSRAM；sk_load 分配，sk_free 释放） */
    char*  text;
    size_t len;
    int    lines;
    int32_t* offs;                          /* 每行起始偏移表（PSRAM，load 建一次）：
                                           * 行访问 O(1)，且就地截断换行不影响寻址 */
    int    pc;                             /* 下一执行行（0 基）；>=lines 为终态 */

    /* 变量与标志（命名表线性查找；未声明变量读出 0） */
    char    vnames[SK_MAX_VARS][SK_NAME_MAX];
    int16_t vvals[SK_MAX_VARS];
    int     vcount;
    char    fnames[SK_MAX_FLAGS][SK_NAME_MAX];
    int     fcount;

    /* label 表（load 时一次扫描） */
    sk_label_t labels[SK_MAX_LABELS];
    int        lcount;

    /* 副作用状态：渲染器消费后自行清 dirty */
    char  title[96];                        /* * title 最新值 */
    char  bg[96];                          /* ""=无；"none"=显式清除 */
    bool  bg_dirty;
    char  chars[3][96];                    /* 立绘槽：""=空；"none"=清槽请求 */
    bool  char_dirty;
    uint8_t char_pos[3];                   /* 0=L 1=C 2=R */
    bool  char_focus[3];

    /* 选项上下文（CHOICE 暂停时有效） */
    bool choice_direct;                    /* true=直跳式 */
    char choice_target[SK_MAX_CHOICES][SK_NAME_MAX];

    char last_name[48];                    /* 粘性名牌 */
} sk_state_t;

/* ===================== 生命周期 ===================== */

/// 整读脚本并建 label 表。失败 false（文件缺失/超限/空）。state 须先清零。
bool sk_load(sk_state_t* sk, const char* path);
/// 释放脚本缓冲并复位运行态（保留 state 本体，由拥有者管理）。
void sk_free(sk_state_t* sk);

/// 重开：pc/变量/标志/名牌/背景/立绘复位，脚本与 label 表保留（结局页重玩用）。
void sk_restart(sk_state_t* sk);

/// 从当前 pc 解释一行。事件见 sk_event_t；副作用就地更新 sk 字段。
/// TEXT/CHOICE/END/EOF 即暂停点（该行已消费，pc 指向下一行）。
sk_event_t sk_step(sk_state_t* sk, sk_out_t* out);

/// 选择第 index 项（0 基）：直跳式跳目标 label；旧式置 flag c{index} 顺序继续。
/// 须在 SK_EV_CHOICE 暂停时调用；越界忽略。
void sk_choose(sk_state_t* sk, int index);

/* ===================== 变量 / 标志 / label ===================== */

int  sk_var_get(const sk_state_t* sk, const char* name);        /* 未声明=0 */
void sk_var_set(sk_state_t* sk, const char* name, int16_t val); /* 隐式建槽 */
bool sk_flag_has(const sk_state_t* sk, const char* name);
void sk_flag_set(sk_state_t* sk, const char* name);
int  sk_find_label(const sk_state_t* sk, const char* name);     /* -1=无 */

/* ===================== 存档序列化（单行文本，向后兼容 gtext 旧 .gtsave） =====================
 * 格式："<pc> [P<页码>] [名=值 …] [标志名 …]"
 * 旧 gtext 存档 "pc f1 f2"（裸词）自动按标志解析。extra_page 由 NovelFront
 * 使用（-1=不存页码）；restore 回读后由调用方决定如何使用。 */
size_t sk_save(const sk_state_t* sk, int extra_page, char* buf, size_t cap);
bool   sk_restore(sk_state_t* sk, const char* line, int* extra_page);

#ifdef __cplusplus
}
#endif
