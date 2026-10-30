/*
 * fmj.h - 伏魔记引擎公共定义（移植自步步高 BBRPG / fmj.kt Kotlin 版）
 *
 * 适配 ESP32-S3 墨水屏阅读器（kiikoread 固件）：
 * - 原版画面 160x96 单色，数据来自 TF 卡 /sdcard/fmj/ 下原版资源文件
 * - 墨水屏刷新慢，引擎改为事件驱动步进模型：一次按键 = 一步逻辑 = 最多一次刷屏
 * - 大文件（DAT.LIB 557KB / HZK16 267KB）一次性载入 PSRAM
 */
#ifndef FMJ_FMJ_H
#define FMJ_FMJ_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 原版常量 ---------- */
#define FMJ_SCREEN_W   160
#define FMJ_SCREEN_H   96
#define FMJ_TILE_W     16
#define FMJ_TILE_H     16
/* 可视地图块数（原版：160/16-1 = 9 列，96/16 = 6 行） */
#define FMJ_MAP_VIEW_W 9
#define FMJ_MAP_VIEW_H 6
#define FMJ_MAP_LEFT_OFFSET 8

/* 虚拟按键（对应原版 Global.KEY_*） */
#define FMJ_KEY_UP        1
#define FMJ_KEY_DOWN      2
#define FMJ_KEY_LEFT      3
#define FMJ_KEY_RIGHT     4
#define FMJ_KEY_PAGEUP    5
#define FMJ_KEY_PAGEDOWN  6
#define FMJ_KEY_ENTER     7
#define FMJ_KEY_CANCEL    8

/* 像素灰度（画布 8bit 灰度，墨量：0=白 255=黑，16 级灰度供抗锯齿用） */
#define FMJ_WHITE 0
#define FMJ_BLACK 255
#define FMJ_GRAY_AA 160   /* 抗锯齿中值灰 */
#define FMJ_GRAY_FRAME 96 /* 边框外晕灰 */

/* 资源类型号（DAT.LIB ResType） */
#define FMJ_RT_GUT 1   /* 剧情脚本 */
#define FMJ_RT_MAP 2   /* 地图 */
#define FMJ_RT_ARS 3   /* 角色 */
#define FMJ_RT_MRS 4   /* 魔法 */
#define FMJ_RT_SRS 5   /* 特效 */
#define FMJ_RT_GRS 6   /* 道具 */
#define FMJ_RT_TIL 7   /* tile 图块 */
#define FMJ_RT_ACP 8   /* 角色行走图 */
#define FMJ_RT_GDP 9   /* 道具图片 */
#define FMJ_RT_GGJ 10  /* 特效图片 */
#define FMJ_RT_PIC 11  /* 杂类图片 */
#define FMJ_RT_MLR 12  /* 链资源 */

/* 方向（资源内编码，Direction.kt） */
#define FMJ_DIR_NORTH 1
#define FMJ_DIR_EAST  2
#define FMJ_DIR_SOUTH 3
#define FMJ_DIR_WEST  4

/* NPC 状态 */
#define FMJ_ST_STOP       0
#define FMJ_ST_FORCE_MOVE 1
#define FMJ_ST_WALKING    2
#define FMJ_ST_PAUSE      3
#define FMJ_ST_ACTIVE     4

/* 全局事件/变量规模（ScriptResources） */
#define FMJ_EVENT_MAX 2401
#define FMJ_VAR_MAX   240

/* 装备类型号（GRS type） */
#define FMJ_EQ_HEAD   1
#define FMJ_EQ_BODY   2
#define FMJ_EQ_WRIST  3
#define FMJ_EQ_FOOT   4
#define FMJ_EQ_SHIELD 5
#define FMJ_EQ_WEAPON 7
#define FMJ_EQ_DECOR  6

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_H */
