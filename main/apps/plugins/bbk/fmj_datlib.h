/*
 * fmj_datlib.h - DAT.LIB 资源库加载与索引
 */
#ifndef FMJ_FMJ_DATLIB_H
#define FMJ_FMJ_DATLIB_H

#include "fmj.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 从 TF 卡加载 DAT.LIB（约 557KB，载入 PSRAM）。
 * path 形如 "/sdcard/fmj/DAT.LIB"。成功返回 0。
 */
int fmj_datlib_init(const char *path);
void fmj_datlib_deinit(void);

/* DAT.LIB 整个文件缓冲（在 PSRAM 中），仅数据层内部使用 */
const uint8_t *fmj_datlib_buffer(void);
size_t fmj_datlib_size(void);

/*
 * 查资源偏移。返回 -1 表示不存在。
 * resType 1-12；type、index 与原版一致。
 */
int fmj_datlib_offset(int res_type, int type, int index);

/* ---------- 底层字节读取工具（小端） ---------- */
static inline int fmj_u8(const uint8_t *b, int off) { return b[off] & 0xFF; }
static inline int fmj_i8(const uint8_t *b, int off) { return (int)(int8_t)b[off]; }
/* 2 字节无符号 */
static inline int fmj_u16(const uint8_t *b, int off) {
    return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8);
}
/* 1 字节有符号（bit7 符号位，原版 get1ByteSInt） */
static inline int fmj_s8(const uint8_t *b, int off) {
    int v = b[off] & 0x7F;
    return (b[off] & 0x80) ? -v : v;
}
/* 2 字节有符号（bit15 符号位，原版 get2BytesSInt） */
static inline int fmj_s16(const uint8_t *b, int off) {
    int v = (b[off] & 0xFF) | ((b[off + 1] & 0x7F) << 8);
    return (b[off + 1] & 0x80) ? -v : v;
}
/* 4 字节有符号 */
static inline int fmj_s32(const uint8_t *b, int off) {
    return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8) |
           ((b[off + 2] & 0xFF) << 16) | ((int8_t)b[off + 3] << 24);
}

/* GBK 字节串长度（到 0 结尾） */
static inline int fmj_cstr_len(const uint8_t *b, int off) {
    int i = 0;
    while (b[off + i] != 0) ++i;
    return i;
}

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_DATLIB_H */
