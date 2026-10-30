/*
 * fmj_datlib.c - DAT.LIB 资源库加载与索引
 *
 * 原版 DAT.LIB 结构（与 fmj.kt DatLib.kt 一致）：
 * - 0x0000-0x0F  文件头
 * - 0x0010 起    资源索引表：每项 3 字节 (resType, type, index)，resType=0xFF 结束
 * - 0x2000 起    偏移表：每项 3 字节 (block, low, high)，
 *                offset = block * 0x4000 + (high<<8 | low)
 * 资源数据紧随其后，首个资源头 6 字节为 type/index/…，由各资源解析器处理。
 */
#include "fmj_datlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define FMJ_HT_SIZE 2048  /* 开放寻址哈希槽数（资源总数约 1244） */
#define FMJ_HT_KEY_EMPTY (-1)

typedef struct {
    int key;   /* (resType<<16)|(type<<8)|index */
    int value; /* 文件内偏移 */
} fmj_ht_entry_t;

static uint8_t *s_buf = NULL;
static size_t s_size = 0;
static fmj_ht_entry_t *s_ht = NULL;

static inline int fmj_key(int rt, int type, int index) {
    return (rt << 16) | (type << 8) | index;
}

static void ht_put(int key, int value) {
    int h = key & (FMJ_HT_SIZE - 1);
    while (s_ht[h].key != FMJ_HT_KEY_EMPTY) {
        h = (h + 1) & (FMJ_HT_SIZE - 1);
    }
    s_ht[h].key = key;
    s_ht[h].value = value;
}

int fmj_datlib_init(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
    long sz = ftell(f);
    if (sz < 0x2100) { fclose(f); return -3; }
    rewind(f);

    s_buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!s_buf) { fclose(f); return -4; }
    if (fread(s_buf, 1, (size_t)sz, f) != (size_t)sz) {
        heap_caps_free(s_buf); s_buf = NULL; fclose(f); return -5;
    }
    fclose(f);
    s_size = (size_t)sz;

    s_ht = heap_caps_malloc(sizeof(fmj_ht_entry_t) * FMJ_HT_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_ht) { fmj_datlib_deinit(); return -6; }
    for (int i = 0; i < FMJ_HT_SIZE; ++i) s_ht[i].key = FMJ_HT_KEY_EMPTY;

    /* 解析索引表与偏移表（原版 getAllResOffset） */
    int i = 0x10;
    int j = 0x2000;
    int count = 0;
    while (i < (int)s_size && s_buf[i] != 0xFF) {
        int rt = s_buf[i];
        int type = s_buf[i + 1];
        int index = s_buf[i + 2] & 0xFF;
        int block = s_buf[j];
        int low = s_buf[j + 1];
        int high = s_buf[j + 2];
        int off = block * 0x4000 + ((high << 8) | low);
        if (off > 0 && off < (int)s_size) {
            ht_put(fmj_key(rt, type, index), off);
            ++count;
        }
        i += 3;
        j += 3;
    }
    return count > 0 ? 0 : -7;
}

void fmj_datlib_deinit(void) {
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
    if (s_ht) { heap_caps_free(s_ht); s_ht = NULL; }
    s_size = 0;
}

const uint8_t *fmj_datlib_buffer(void) { return s_buf; }
size_t fmj_datlib_size(void) { return s_size; }

int fmj_datlib_offset(int res_type, int type, int index) {
    if (!s_ht) return -1;
    int key = fmj_key(res_type, type, index);
    int h = key & (FMJ_HT_SIZE - 1);
    for (int probe = 0; probe < FMJ_HT_SIZE; ++probe) {
        if (s_ht[h].key == FMJ_HT_KEY_EMPTY) break;
        if (s_ht[h].key == key) return s_ht[h].value;
        h = (h + 1) & (FMJ_HT_SIZE - 1);
    }
    return -1;
}
