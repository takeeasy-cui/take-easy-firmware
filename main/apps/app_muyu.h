/*
 * 木鱼音效（用户 2026-10-20 21f2 拍板：音效开关 + 音效选择；21g 瘦身）。
 * 21g（真机反馈连续切换死机）：曲谱（score_play 异步任务）/复音（1bit RMT）等
 * 官方 demo 音效删除，只留单音节、全部 read_pico_buzzer_tone 阻塞直调：
 *   0=关（静默） 1=笃（1800Hz/80ms，21c 起默认）
 *   2=尖锐（4000Hz/80ms） 3=低沉（1500Hz/80ms）
 * 设置存 NVS（settings：muyu_s，0..3），木鱼 app 与首页 widget 共用。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 按当前设置发声（0=关时静默）；木鱼 app 敲击与首页 widget 点击共用。 */
void app_muyu_snd_play(void);

/* 音效中文名（设置按钮显示）；snd 越界回退「关」。 */
const char *app_muyu_snd_name(uint8_t snd);

#ifdef __cplusplus
}
#endif
