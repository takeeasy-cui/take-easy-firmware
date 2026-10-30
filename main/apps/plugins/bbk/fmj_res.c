/*
 * fmj_res.c - 资源解析实现
 *
 * ResImage 原始格式（与 fmj.kt ResImage.kt 一致）：
 *   header 6 字节：type, index, width, height, number, mode(mode==2 透明)
 *   mode=1 不透明：1bit/px（1=黑），行尾字节对齐
 *   mode=2 透明：  2bit/px（bit1 透明位，bit0 黑白），行尾字节对齐，
 *                 且每行结束后若当前字节偏移为奇数则跳 1 字节
 */
#include "fmj_res.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

/* ---------- 图片缓存（固定槽数环形覆盖） ---------- */
#define FMJ_IMG_CACHE_N 128

typedef struct {
    int key;                 /* 0 = 空 */
    fmj_image_t img;
} fmj_cache_ent_t;

static fmj_cache_ent_t s_cache[FMJ_IMG_CACHE_N];
static int s_cache_next = 0;

static void cache_free(int slot) {
    if (s_cache[slot].key && s_cache[slot].img.px) {
        heap_caps_free(s_cache[slot].img.px);
    }
    memset(&s_cache[slot], 0, sizeof(fmj_cache_ent_t));
}

void fmj_res_image_flush(void) {
    for (int i = 0; i < FMJ_IMG_CACHE_N; ++i) cache_free(i);
    s_cache_next = 0;
}

static fmj_cache_ent_t *cache_find(int key) {
    for (int i = 0; i < FMJ_IMG_CACHE_N; ++i) {
        if (s_cache[i].key == key) return &s_cache[i];
    }
    return NULL;
}

static fmj_cache_ent_t *cache_alloc(int key) {
    fmj_cache_ent_t *e = &s_cache[s_cache_next];
    cache_free(s_cache_next);
    s_cache_next = (s_cache_next + 1) % FMJ_IMG_CACHE_N;
    e->key = key;
    return e;
}

/* 解码一张图片到 out（px 已分配） */
static int decode_image(fmj_image_t *img, const uint8_t *buf, int off) {
    int width = buf[off + 2] & 0xFF;
    int height = buf[off + 3] & 0xFF;
    int number = buf[off + 4] & 0xFF;
    int mode = buf[off + 5] & 0xFF;
    if (width <= 0 || height <= 0 || number <= 0) return -1;

    size_t bpr = (size_t)((width * 2 + 7) / 8);
    size_t total = bpr * (size_t)height * (size_t)number;
    uint8_t *px = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!px) return -2;
    memset(px, 0, total);

    img->width = width;
    img->height = height;
    img->number = number;
    img->bytes_per_row = (int)bpr;
    img->px = px;

    const uint8_t *src = buf + off + 6;
    if (mode == 2) {
        /* 透明：2bit/px，行字节对齐 + 偶字节对齐；源偏移跨帧连续推进 */
        int src_of = 0;
        for (int f = 0; f < number; ++f) {
            for (int y = 0; y < height; ++y) {
                uint8_t *dst = px + ((size_t)f * height + y) * bpr;
                int cnt = 0;
                for (int x = 0; x < width; ++x) {
                    int v = (src[src_of] << cnt) & 0xC0;
                    int out;
                    if (v & 0x80) out = 2;          /* 透明 */
                    else out = (v & 0x40) ? 1 : 0;  /* 黑 / 白 */
                    int shift = 6 - ((x & 3) << 1);
                    dst[x >> 2] |= (uint8_t)(out << shift);
                    cnt += 2;
                    if (cnt >= 8) { cnt = 0; ++src_of; }
                }
                if (cnt != 0) { cnt = 0; ++src_of; }
                if (src_of & 1) ++src_of;
            }
        }
    } else {
        /* 不透明：1bit/px */
        int row_bytes = (width + 7) / 8;
        for (int f = 0; f < number; ++f) {
            const uint8_t *fs = buf + off + 6 + (size_t)f * row_bytes * height;
            for (int y = 0; y < height; ++y) {
                const uint8_t *line = fs + (size_t)y * row_bytes;
                uint8_t *dst = px + ((size_t)f * height + y) * bpr;
                for (int x = 0; x < width; ++x) {
                    int bit = (line[x >> 3] >> (7 - (x & 7))) & 1;
                    int shift = 6 - ((x & 3) << 1);
                    dst[x >> 2] |= (uint8_t)(bit << shift);
                }
            }
        }
    }
    return 0;
}

const fmj_image_t *fmj_res_image(int res_type, int type, int index) {
    int key = (res_type << 12) | (type << 8) | index;
    fmj_cache_ent_t *e = cache_find(key);
    if (e) return &e->img;

    int off = fmj_datlib_offset(res_type, type, index);
    if (off < 0) return NULL;
    const uint8_t *buf = fmj_datlib_buffer();
    if (!buf || off + 6 > (int)fmj_datlib_size()) return NULL;

    int width = buf[off + 2] & 0xFF;
    int height = buf[off + 3] & 0xFF;
    int number = buf[off + 4] & 0xFF;
    int mode = buf[off + 5] & 0xFF;
    if (width <= 0 || height <= 0 || number <= 0) return NULL;
    if (mode != 1 && mode != 2) return NULL;
    /* 校验数据长度 */
    size_t need = (size_t)number * height *
                  ((mode == 2 ? width * 2 : width) + 7) / 8 + 6;
    if ((size_t)off + need > fmj_datlib_size()) return NULL;

    e = cache_alloc(key);
    if (decode_image(&e->img, buf, off) != 0) {
        cache_free((int)(e - s_cache));
        return NULL;
    }
    return &e->img;
}

int fmj_res_image_bytes_count(int res_type, int type, int index) {
    int off = fmj_datlib_offset(res_type, type, index);
    if (off < 0) return -1;
    const uint8_t *buf = fmj_datlib_buffer();
    if (!buf) return -1;
    int width = buf[off + 2] & 0xFF;
    int height = buf[off + 3] & 0xFF;
    int number = buf[off + 4] & 0xFF;
    int mode = buf[off + 5] & 0xFF;
    if (width <= 0 || height <= 0 || number <= 0) return -1;
    size_t len = (size_t)number * ((width * (mode == 2 ? 2 : 1) + 7) / 8) * height * mode;
    return (int)len + 6;
}

/* ---------- 地图 ---------- */
int fmj_res_map(fmj_map_t *out, int type, int index) {
    int off = fmj_datlib_offset(FMJ_RT_MAP, type, index);
    if (off < 0) return -1;
    const uint8_t *buf = fmj_datlib_buffer();
    if (!buf) return -2;

    memset(out, 0, sizeof(*out));
    out->type = buf[off] & 0xFF;
    out->index = buf[off + 1] & 0xFF;
    out->til_index = buf[off + 2];

    int n = fmj_cstr_len(buf, off + 3);
    if (n > 15) n = 15;
    memcpy(out->name, buf + off + 3, (size_t)n);
    out->name[n] = 0;

    out->width = buf[off + 0x10] & 0xFF;
    out->height = buf[off + 0x11] & 0xFF;
    if (out->width <= 0 || out->height <= 0) return -3;

    size_t len = (size_t)out->width * out->height * 2;
    if ((size_t)off + 0x12 + len > fmj_datlib_size()) return -4;
    out->data = buf + off + 0x12;
    return 0;
}

bool fmj_map_can_walk(const fmj_map_t *m, int x, int y) {
    if (!m || x < 0 || x >= m->width || y < 0 || y >= m->height) return false;
    int i = y * m->width + x;
    return (m->data[i * 2] & 0x80) != 0;
}

bool fmj_map_can_player_walk(const fmj_map_t *m, int x, int y) {
    if (!m) return false;
    if (x < 4 || x >= m->width - 4 || y < 3 || y >= m->height - 2) return false;
    return fmj_map_can_walk(m, x, y);
}

int fmj_map_event_num(const fmj_map_t *m, int x, int y) {
    if (!m || x < 0 || x >= m->width || y < 0 || y >= m->height) return -1;
    int i = y * m->width + x;
    return m->data[i * 2 + 1] & 0xFF;
}

int fmj_map_tile_index(const fmj_map_t *m, int x, int y) {
    if (!m || x < 0 || x >= m->width || y < 0 || y >= m->height) return 0;
    int i = y * m->width + x;
    return m->data[i * 2] & 0x7F;
}

/* ---------- 剧情 ---------- */
int fmj_res_gut(fmj_gut_t *out, int type, int index) {
    int off = fmj_datlib_offset(FMJ_RT_GUT, type, index);
    if (off < 0) return -1;
    const uint8_t *buf = fmj_datlib_buffer();
    if (!buf) return -2;

    memset(out, 0, sizeof(*out));
    out->type = buf[off] & 0xFF;
    out->index = buf[off + 1] & 0xFF;
    int length = fmj_u16(buf, off + 0x18);
    int nse = buf[off + 0x1a] & 0xFF;
    out->num_scene_event = nse;
    out->events = buf + off + 0x1b;
    int code_len = length - nse * 2 - 3;
    if (code_len <= 0) return -3;
    if ((size_t)off + 0x1b + (size_t)nse * 2 + (size_t)code_len > fmj_datlib_size())
        return -4;
    out->code = buf + off + 0x1b + nse * 2;
    out->code_len = code_len;
    out->header_cnt = nse * 2 + 3;
    return 0;
}
