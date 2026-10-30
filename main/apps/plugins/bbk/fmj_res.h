/*
 * fmj_res.h - DAT.LIB 资源解析（图片 / 地图 / 剧情 / 特效）
 */
#ifndef FMJ_FMJ_RES_H
#define FMJ_FMJ_RES_H

#include "fmj.h"
#include "fmj_canvas.h"
#include "fmj_datlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 解码图片资源（TIL/ACP/GDP/GGJ/PIC 等）。内部带小型缓存，
 * 重复获取同一资源直接返回缓存项。失败返回 NULL。
 */
const fmj_image_t *fmj_res_image(int res_type, int type, int index);

/* 释放全部图片缓存（退出游戏时调用） */
void fmj_res_image_flush(void);

/* 图片资源的原始字节数（用于 SRS 连续读取） */
int fmj_res_image_bytes_count(int res_type, int type, int index);

/* ---------- 地图（ResMap） ---------- */
typedef struct {
    int type;
    int index;
    int til_index;          /* 使用的 TIL 资源索引 */
    int width;
    int height;
    const uint8_t *data;   /* w*h*2，指向 DAT.LIB 内部缓冲 */
    char name[16];         /* GBK 原始字节 */
} fmj_map_t;

/* 解析地图。out 由调用者提供（栈上即可），data 指向 PSRAM 缓冲。成功返回 0。 */
int fmj_res_map(fmj_map_t *out, int type, int index);

bool fmj_map_can_walk(const fmj_map_t *m, int x, int y);
/* 主角行走限制（原版 canPlayerWalk：边缘 4/4/3/2 格不可走） */
bool fmj_map_can_player_walk(const fmj_map_t *m, int x, int y);
int fmj_map_event_num(const fmj_map_t *m, int x, int y);
int fmj_map_tile_index(const fmj_map_t *m, int x, int y);

/* ---------- 剧情（ResGut） ---------- */
typedef struct {
    int type;
    int index;
    int num_scene_event;
    const uint8_t *events; /* num_scene_event*2 字节，指向 DAT.LIB */
    const uint8_t *code;   /* 脚本字节码 */
    int code_len;
    int header_cnt;        /* events*2+3，地址换算用 */
} fmj_gut_t;

int fmj_res_gut(fmj_gut_t *out, int type, int index);

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_RES_H */
