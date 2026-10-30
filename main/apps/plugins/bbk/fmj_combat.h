/*
 * fmj_combat.h - 回合制战斗
 */
#ifndef FMJ_FMJ_COMBAT_H
#define FMJ_FMJ_COMBAT_H

#include "fmj.h"
#include "fmj_canvas.h"
#include "fmj_script.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设置随机战斗池（cmd_initfight） */
void fmj_combat_set_random(const int *monsters, int count, int scrb, int scrl, int scrr);

/* 剧情战斗（cmd_enterfight） */
void fmj_combat_enter(const fmj_fight_cfg_t *cfg);

/* 行走遇怪检查（20% 概率，原版 COMBAT_PROBABILITY） */
void fmj_combat_random_check(void);

/* 战斗是否进行中 */
bool fmj_combat_active(void);

/* 战斗消息浏览态：宿主据此隐藏虚拟按键、改点击屏幕推进 */
bool fmj_combat_msg_active(void);

/* 战斗输入处理（active 时由主循环调用） */
void fmj_combat_input(int key, bool is_up);

/* 战斗绘制（active 时由主循环调用） */
void fmj_combat_draw(fmj_canvas_t *c);

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_COMBAT_H */
