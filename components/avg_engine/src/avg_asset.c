/*
 * avg_asset.c — 有界 LRU：每槽独立 PSRAM 块，逐出即 free（含 1bit 蒙版）。
 */
#include "avg_asset.h"

#include <string.h>

#include "esp_heap_caps.h"

static avg_decode_fn s_decode;

void avg_assets_set_decoder(avg_decode_fn fn) { s_decode = fn; }

void avg_assets_init(avg_assets_t* a) {
    memset(a, 0, sizeof(*a));
}

uint64_t avg_assets_generation(const avg_assets_t* a) { return a->generation; }

void avg_assets_release(avg_assets_t* a) {
    for (int i = 0; i < AVG_ASSET_SLOTS; ++i) {
        if (a->slots[i].gray) {
            heap_caps_free(a->slots[i].gray);
            a->slots[i].gray = NULL;
        }
        if (a->slots[i].mask) {
            heap_caps_free(a->slots[i].mask);
            a->slots[i].mask = NULL;
        }
        a->slots[i].path[0] = 0;
    }
    a->generation++;
}

const uint8_t* avg_assets_get(avg_assets_t* a, const char* path,
                              unsigned max_w, unsigned max_h,
                              unsigned* w, unsigned* h, uint64_t now,
                              const uint8_t** mask) {
    if (mask) *mask = NULL;
    if (!path || !path[0] || !s_decode) return NULL;

    int hit = -1;
    for (int i = 0; i < AVG_ASSET_SLOTS; ++i)
        if (a->slots[i].gray && !strcmp(a->slots[i].path, path)) { hit = i; break; }

    if (hit >= 0) {
        a->slots[hit].last_used = now;
        if (w) *w = a->slots[hit].width;
        if (h) *h = a->slots[hit].height;
        if (mask) *mask = a->slots[hit].mask;
        return a->slots[hit].gray;
    }

    /* 找空槽或最久未用槽。 */
    int victim = -1;
    for (int i = 0; i < AVG_ASSET_SLOTS; ++i)
        if (!a->slots[i].gray) { victim = i; break; }
    if (victim < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < AVG_ASSET_SLOTS; ++i)
            if (a->slots[i].last_used < oldest) { oldest = a->slots[i].last_used; victim = i; }
    }
    if (victim < 0 || victim >= AVG_ASSET_SLOTS) return NULL;

    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)max_w * max_h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;
    uint8_t* dmask = NULL;
    unsigned ow = 0, oh = 0;
    /* 2026-10-28：偶发解码失败重试一次（真机实测"背景时有时无"：SD 读抖动/
     * LRU 逐出与刷屏并发时一次性失败会被渲染端画占位框，重试即消除）。
     * decode 失败时 buf 半途数据不置信，重试前零填。 */
    bool ok = s_decode(path, max_w, max_h, buf, &dmask, &ow, &oh);
    if (!ok && (dmask == NULL)) {
        memset(buf, 0, (size_t)max_w * max_h);
        ok = s_decode(path, max_w, max_h, buf, &dmask, &ow, &oh);
    }
    if (!ok || !ow || !oh) {
        heap_caps_free(buf);
        if (dmask) heap_caps_free(dmask);
        return NULL;
    }

    if (a->slots[victim].gray) heap_caps_free(a->slots[victim].gray);
    if (a->slots[victim].mask) heap_caps_free(a->slots[victim].mask);
    avg_asset_slot_t* s = &a->slots[victim];
    s->gray = buf;
    s->mask = dmask;
    s->width = ow;
    s->height = oh;
    s->last_used = now;
    strlcpy(s->path, path, sizeof(s->path));
    a->generation++;
    if (w) *w = ow;
    if (h) *h = oh;
    if (mask) *mask = dmask;
    return buf;
}
