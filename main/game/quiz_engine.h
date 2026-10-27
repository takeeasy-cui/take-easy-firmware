/*
 * quiz_engine.h — 题库闯关引擎（text- 包内 quiz.txt，2026-10-27）。
 *
 * 与叙事脚本（story.txt）并列的第二种内容形态，同走对话游戏入口：
 *   - 题库 quiz.txt：* title 行 + 逐题块（Q/A/B/C/D/R/E 行）
 *   - 随机出题（每次练习洗牌）、连对 3 次毕业（不再出现）、
 *     跳过按钮（永不再出）、错题集重练（错过且未掌握的题）
 *   - 进度 .quizprog 持久化（每题 streak/skipped/wronged）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QZ_MENU = 0,   /* 主菜单 */
    QZ_ASK,        /* 答题 */
    QZ_FEED,       /* 答案反馈 */
    QZ_DONE,       /* 本轮结束 */
} quiz_phase_t;

typedef struct quiz_engine_s quiz_engine_t;

/// 解析题库并读进度。失败（文件缺失/无有效题目）返回 NULL。
quiz_engine_t* quiz_load(const char* quiz_path, const char* prog_path);
void quiz_free(quiz_engine_t* q);

/// 整页绘制（竖屏 684×1216）。
void quiz_paint(quiz_engine_t* q, uint8_t* fb);

/// 点按处理；返回 true=界面变化需重刷。
bool quiz_tap(quiz_engine_t* q, int x, int y);

/// 中键快捷（反馈页=下一题，菜单=随机练习，结算=返回菜单；答题页不响应防误触）。
/// 返回 true=界面变化需重刷。
bool quiz_middle_key(quiz_engine_t* q);

/// 包标题（列表显示名用；无则空串）。
void quiz_get_title(const quiz_engine_t* q, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
