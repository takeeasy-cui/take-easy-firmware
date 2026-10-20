/*
 * avg_asset.h — SD 卡灰度图资加载 + PSRAM 有界 LRU 缓存。
 *
 * 解码器由宿主注入（本固件=book_image_file_grayscale，PNG 16 级灰度），
 * 缓存本体只管内存：越界逐出（LRU）、会话结束全量释放。墨水屏残影与
 * 内存上限都不由本模块操心——它只回答“这张图现在有没有灰度数据”。
 *
 * Host-injected decoder (book_image_file_grayscale in this firmware);
 * this module owns only bounded PSRAM LRU storage and lifetime.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单案件资产缓存：1 背景 + 3 立绘 + 常用证据图，越界逐出。 */
#define AVG_ASSET_SLOTS 8

typedef struct {
    char     path[160];
    uint8_t* gray;        /* PSRAM，NULL=未加载 */
    uint8_t* mask;        /* 1bit 透明蒙版（1=不透明），行打包 w*ceil(w/8)；
                            * PSRAM，可为 NULL（无蒙版=全不透明） */
    unsigned width, height;
    uint64_t last_used;
} avg_asset_slot_t;

typedef struct {
    avg_asset_slot_t slots[AVG_ASSET_SLOTS];
    uint64_t generation;
} avg_assets_t;

/// 解码回调：把 path 的图片按 (max_w, max_h) 保比例缩放成 8bit 灰度写入
/// out（容量=max_w*max_h），实际尺寸回填 *w/*h。若图片含透明，
/// 解码器分配 *mask（w*ceil(w/8) 行打包，1=不透明）并回传所有权；
/// 不透图置 NULL。宿主注入。
typedef bool (*avg_decode_fn)(const char* path, unsigned max_w, unsigned max_h,
                              uint8_t* out, uint8_t** mask, unsigned* w, unsigned* h);

void avg_assets_init(avg_assets_t* a);
void avg_assets_set_decoder(avg_decode_fn fn);

/// 取灰度数据（命中或加载+可能逐出）。失败返回 NULL（缺素材时引擎会画占位）。
/// mask 非空指针时回传蒙版（可为 NULL=全不透明），随槽缓存生命周期。
const uint8_t* avg_assets_get(avg_assets_t* a, const char* path,
                              unsigned max_w, unsigned max_h,
                              unsigned* w, unsigned* h, uint64_t now,
                              const uint8_t** mask);

/// 缓存是否刚变化（场景重建的刷新依据）：把 generation 传回比对。
uint64_t avg_assets_generation(const avg_assets_t* a);

/// 释放全部 PSRAM（退页/on_media_lost）。
void avg_assets_release(avg_assets_t* a);

#ifdef __cplusplus
}
#endif
