/*
 * fmj_scene.h - 主游戏场景：角色、地图、行走与绘制
 */
#ifndef FMJ_FMJ_SCENE_H
#define FMJ_FMJ_SCENE_H

#include "fmj.h"
#include "fmj_canvas.h"
#include "fmj_res.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FMJ_NPC_SLOTS 41   /* 操作号 1-40，0 为空 */
#define FMJ_PLAYERS_MAX 4
#define FMJ_GOODS_MAX 64

/* 角色种类 */
#define FMJ_K_EMPTY  0
#define FMJ_K_PLAYER 1
#define FMJ_K_NPC    2
#define FMJ_K_MONSTER 3
#define FMJ_K_OBJ    4

typedef struct {
    uint8_t kind;      /* FMJ_K_* */
    uint8_t type;      /* 资源 type 字段 */
    uint8_t index;     /* 资源 index 字段 */
    uint8_t walk_type; /* 行走图 ACP type（玩家=type，NPC=2，对象=4） */
    uint8_t walk_index;
    char name[16];     /* GBK 字节 */
    int16_t map_x, map_y;
    uint8_t direction; /* FMJ_DIR_* */
    uint8_t step;
    uint8_t state;     /* FMJ_ST_* */
    uint8_t delay;
    uint8_t level;
    int16_t max_hp, hp, max_mp, mp;
    int16_t attack, defend;
    uint8_t speed, lingli, luck;
    int16_t exp;
    uint8_t magic_chain;
    uint8_t magic_chain_learn_num;
    uint8_t equip_type[8]; /* 装备槽 type，0=无（index 与 type 对应） */
    uint8_t equip_index[8];
    /* 怪物专用 */
    int16_t money, m_exp;
    uint8_t drop_type, drop_index, drop_num;
    uint8_t atbuff;
    uint8_t fight_pic; /* 战斗大图 ACP type */
    uint8_t drawn;      /* 绘制排序临时标记 */
    /* 自学法术（cmd_learnmagic），最多 8 个 */
    uint8_t lm_type[8], lm_index[8];
    int lm_count;
} fmj_actor_t;

/* ---------- 全局游戏状态 ---------- */
extern fmj_actor_t fmj_players[FMJ_PLAYERS_MAX];
extern int fmj_player_count;      /* 已创建主角数 */
extern int fmj_player_ctrl;      /* 当前控制的主角下标 */
extern fmj_actor_t fmj_npcs[FMJ_NPC_SLOTS];
extern fmj_map_t fmj_cur_map;
extern bool fmj_map_loaded;
extern int fmj_map_scr_x, fmj_map_scr_y;
extern char fmj_scene_name[16];  /* GBK */

typedef struct { uint8_t type, index, num; } fmj_goods_ent_t;
extern fmj_goods_ent_t fmj_goods[FMJ_GOODS_MAX];
extern int fmj_goods_count;
extern int fmj_money;
extern bool fmj_disable_save;

/* 随机战斗开关与参数（cmd_initfight） */
extern bool fmj_fight_enabled;
extern int fmj_fight_scr[3]; /* 三档遇怪率 */

/* ---------- 解析 ---------- */
/* 相机视口钳制（供 loadmap/screens 指令与走动跟随共用）：小图贴原点，大图 clamp 边界 */
void fmj_scene_camera_clamp(int *scr_x, int *scr_y);
/* 从 ARS 资源解析角色到 a（kind 指定）。成功返回 0。 */
int fmj_actor_load(fmj_actor_t *a, uint8_t kind, int type, int index);

/* ---------- 场景 ---------- */
/* 载入地图并设置屏幕位置 */
void fmj_scene_load_map(int type, int index, int scr_x, int scr_y);
/* 创建主角（id 附加到 playerList） */
void fmj_scene_create_actor(int actor_id, int x, int y);
void fmj_scene_delete_actor(int actor_id);
fmj_actor_t *fmj_scene_get_player(int actor_id);
void fmj_scene_set_control(int actor_id);
/* 创建/删除 NPC（操作号 1-40） */
fmj_actor_t *fmj_scene_create_npc(int id, int res_id, int x, int y);
void fmj_scene_delete_npc(int id);
void fmj_scene_delete_all_npc(void);
fmj_actor_t *fmj_scene_npc(int id);
/* 宝箱（与 NPC 共用槽） */
fmj_actor_t *fmj_scene_create_box(int id, int box_id, int x, int y);
void fmj_scene_box_open(int id);

/* 地图 (x,y) 是否可走（考虑 NPC 占位） */
bool fmj_scene_can_walk(int x, int y);
/* 玩家行走一步（方向键）。返回 true 表示触发/移动需要刷屏 */
bool fmj_scene_walk(int dir);
/* 确认键：触发面前 NPC 对话 / 地图事件 / 随机战斗 */
void fmj_scene_action(void);

/* ---------- 绘制 ---------- */
void fmj_scene_draw(fmj_canvas_t *c);
/* 单个行走精灵 */
void fmj_actor_draw_walk(fmj_canvas_t *c, const fmj_actor_t *a, int scr_x, int scr_y);

/* 查询地图 (x,y) 处 NPC 的操作号（0=无） */
int fmj_scene_npc_at(int x, int y);

/* 物品清单操作（返回 false 表示已满/不存在） */
bool fmj_goods_add(int type, int index, int num);
bool fmj_goods_remove(int type, int index);
int fmj_goods_num(int type, int index);
bool fmj_goods_use_num(int type, int index, int num);

/* 资源方向值转换（1-4，非法回南方） */
int fmj_dir_from(int v);
/* NPC 巡逻推进一步（每次玩家移动后调用一次） */
void fmj_scene_tick_npcs(void);

#ifdef __cplusplus
}
#endif

#endif /* FMJ_FMJ_SCENE_H */
