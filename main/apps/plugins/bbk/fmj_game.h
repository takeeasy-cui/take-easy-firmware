/*
 * fmj_game.h - 游戏总控：标题、流程状态机、主菜单、NVS 存档
 */
#ifndef FMJ_FMJ_GAME_H
#define FMJ_FMJ_GAME_H

#include "fmj.h"
#include "fmj_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化全部数据（DAT.LIB、字库已由宿主加载后调用） */
void fmj_game_start(void);
/* 释放游戏状态（退出应用） */
void fmj_game_stop(void);

/* 游戏是否在运行（app 页面活跃期间） */
bool fmj_game_active(void);

/* 每次触摸/按键事件输入；key 为 FMJ_KEY_*，is_up 表示抬起事件 */
void fmj_game_input(int key, bool is_up);

/* 绘制当前画面到 160x96 画布 */
void fmj_game_draw(fmj_canvas_t *c);

/* 当前场景的整屏背景类型（供宿主贴 rt=20 整图）：
 * 0 = 无整图（tile 地图自绘） 1 = 标题背景 2 = 战斗背景 */
int fmj_game_scene_bg(void);

/* 状态页/物品页全屏覆盖界面：宿主据此隐藏虚拟按键、点击屏幕返回 */
bool fmj_game_menu_overlay(void);

/* 游戏结束请求（脚本/战斗调用） */
void fmj_game_request_over(void);

/* NVS 存档；返回 0 成功 */
int fmj_game_save(void);
/* 是否存在存档 */
bool fmj_game_has_save(void);

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_GAME_H */
