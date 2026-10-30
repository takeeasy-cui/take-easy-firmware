/*
 * plugins/gtext.h — 通用文字游戏插件声明。
 */
#pragma once

#include "game_core.h"

extern const gc_plugin_t gc_plugin_gtext;

/* 文绎合体入口（gstory 2026-10-30）中键菜单桥接：存/读档导出。
 * 题库模式（quiz）无剧情档：save 返回 false。 */
bool gtext_save(gc_ctx_t* g);
bool gtext_restore(gc_ctx_t* g);
