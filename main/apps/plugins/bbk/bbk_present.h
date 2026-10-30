/*
 * bbk_present.h - FMJ 画布差分输出层（take-easy BBK 插件专用）
 *
 * 原版点阵路线：160x96 画布 → 6x 整数放大 → 横屏游戏区 (0,54)。
 * 差分：与 shadow（上次已输出画布）比较得脏 bbox（16px tile 对齐），
 * 只放大 blit 变化区 + 对角十字抗锯齿；无变化不申报刷新。
 * 申报坐标系：GameCore 物理竖屏 EpdRect（横屏→竖屏旋转由本层换算）。
 */
#ifndef BBK_PRESENT_H
#define BBK_PRESENT_H

#include "epdiy.h"
#include "fmj_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化（分配 shadow，PSRAM）/ 释放。返回 0 成功。 */
int  bbk_present_init(void);
void bbk_present_deinit(void);
/* 强制下帧全刷（进入游戏/跳屏后由宿主调用）。 */
void bbk_present_reset(void);

/* 画布 → fb：
 *   变化区 6x blit（含 AA）→ *out_area（物理竖屏坐标）→ 返回 true；
 *   零变化 → 返回 false（调用方不申报刷新）。
 *   全量（reset 后首次）→ *out_full = true。 */
bool bbk_present(const fmj_canvas_t *c, uint8_t *fb,
                 EpdRect *out_area, bool *out_full);

#ifdef __cplusplus
}
#endif

#endif /* BBK_PRESENT_H */
