/*
 * gnovel.h — 互动小说插件（novel-<name> 包，story.txt 同一内核）。
 */
#pragma once

#include "game_core.h"

extern const gc_plugin_t gc_plugin_gnovel;

/* 文绎合体入口（gstory 2026-10-30）中键菜单桥接：存/读档导出。 */
bool gnovel_save(gc_ctx_t* g);
bool gnovel_restore(gc_ctx_t* g);
