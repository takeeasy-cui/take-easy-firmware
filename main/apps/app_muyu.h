/*
 * 木鱼音效（用户 2026-10-20 21f2 拍板：音效开关 + 音效选择）。
 * 音效集对齐官方 demo 固件「TF 卡与蜂鸣器」页（app_sd.c 同源 API）：
 *   1=笃（单音 tone 1800Hz/80ms，21c 起的默认「笃」声）
 *   2=曲谱（read_pico_buzzer_score_play，官方「曲谱 Score」）
 *   3=复音（PROBE_1BIT，官方 1bit 多声部）
 *   4=尖锐（PROBE_4K，官方 4k）
 *   5=低沉（PROBE_A_15K，官方 1.5k 经典方波）
 *   0=关（静默）
 * 设置存 NVS（settings：muyu_snd/s），木鱼 app 与首页 widget 共用。
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
