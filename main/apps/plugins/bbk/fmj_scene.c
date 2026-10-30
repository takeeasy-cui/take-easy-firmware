/*
 * fmj_scene.c - 主游戏场景实现
 */
#include "fmj_scene.h"

#include <string.h>
#include <stdlib.h>

#include "fmj_font.h"
#include "fmj_screen.h"
#include "fmj_script.h"

/* ---------- 全局状态 ---------- */
fmj_actor_t fmj_players[FMJ_PLAYERS_MAX];
int fmj_player_count = 0;
int fmj_player_ctrl = 0;
fmj_actor_t fmj_npcs[FMJ_NPC_SLOTS];
fmj_map_t fmj_cur_map;
bool fmj_map_loaded = false;
int fmj_map_scr_x = 0, fmj_map_scr_y = 0;
char fmj_scene_name[16] = {0};

fmj_goods_ent_t fmj_goods[FMJ_GOODS_MAX];
int fmj_goods_count = 0;
int fmj_money = 0;
bool fmj_disable_save = false;

bool fmj_fight_enabled = false;
int fmj_fight_scr[3] = {0, 0, 0};

/* 绘制排序用的主角标记 */
static bool fmj_player_drawn_flag = false;

/* ---------- 工具 ---------- */
static void copy_gbk_name(char *dst, const uint8_t *buf, int off) {
    int n = fmj_cstr_len(buf, off);
    if (n > 15) n = 15;
    memcpy(dst, buf + off, (size_t)n);
    dst[n] = 0;
}

static int walk_dir_dx(int dir) {
    if (dir == FMJ_DIR_WEST) return -1;
    if (dir == FMJ_DIR_EAST) return 1;
    return 0;
}
static int walk_dir_dy(int dir) {
    if (dir == FMJ_DIR_NORTH) return -1;
    if (dir == FMJ_DIR_SOUTH) return 1;
    return 0;
}

/* 行走精灵绘制偏移：North→1, East→4, South→7, West→10，步幅 [0,1,2,1] */
static const int k_walk_offset[5] = {0, 1, 4, 7, 10};
static const uint8_t k_walk_step[4] = {0, 1, 2, 1};

void fmj_actor_draw_walk(fmj_canvas_t *c, const fmj_actor_t *a, int scr_x, int scr_y) {
    if (a->kind == FMJ_K_EMPTY) return;
    int sx = (a->map_x - scr_x) * FMJ_TILE_W + FMJ_MAP_LEFT_OFFSET;
    int frame = k_walk_offset[a->direction <= 4 ? a->direction : 3] + k_walk_step[a->step & 3];
    /* 原版 2bit 点阵绘制（复古像素路线；整数放大与差分在输出层 bbk_present） */
    const fmj_image_t *img = fmj_res_image(FMJ_RT_ACP, a->walk_type, a->walk_index);
    if (!img) return;
    int sy = (a->map_y - scr_y) * FMJ_TILE_H + (FMJ_TILE_H - img->height);
    /* 仅裁整体越界（fmj_canvas_draw_image 有逐像素裁剪）；旧版粗判
     * sx >= SCREEN_W - TILE_W 会把视口最右一列的角色直接画没 */
    if (sx + img->width <= 0 || sx >= FMJ_SCREEN_W ||
        sy + img->height <= 0 || sy >= FMJ_SCREEN_H) return;
    fmj_canvas_draw_image(c, img, frame + 1, sx, sy, false);
}

/* ---------- 角色解析 ---------- */
int fmj_actor_load(fmj_actor_t *a, uint8_t kind, int type, int index) {
    int off = fmj_datlib_offset(FMJ_RT_ARS, type, index);
    if (off < 0) return -1;
    const uint8_t *buf = fmj_datlib_buffer();
    if (!buf) return -2;

    memset(a, 0, sizeof(*a));
    a->kind = kind;
    a->type = buf[off] & 0xFF;
    a->index = buf[off + 1] & 0xFF;

    if (kind == FMJ_K_PLAYER) {
        /* Player.setData */
        a->walk_type = a->type; /* WalkingSprite(type, buf[off+0x16]) */
        a->walk_index = buf[off + 0x16] & 0xFF;
        a->fight_pic = 0;      /* FightingSprite(PIC, index)，战斗大图走 PIC 分支 */
        a->direction = buf[off + 2] & 0xFF ? fmj_dir_from(buf[off + 2]) : FMJ_DIR_SOUTH;
        a->step = buf[off + 3] & 0xFF;
        a->map_x = buf[off + 5] & 0xFF;
        a->map_y = buf[off + 6] & 0xFF;
        a->magic_chain = buf[off + 0x17] & 0xFF;
        a->magic_chain_learn_num = buf[off + 9] & 0xFF;
        copy_gbk_name(a->name, buf, off + 0x0a);
        a->level = buf[off + 0x20] & 0xFF;
        a->max_hp = fmj_u16(buf, off + 0x26);
        a->hp = fmj_u16(buf, off + 0x28);
        a->max_mp = fmj_u16(buf, off + 0x2a);
        a->mp = fmj_u16(buf, off + 0x2c);
        a->attack = fmj_u16(buf, off + 0x2e);
        a->defend = fmj_u16(buf, off + 0x30);
        a->speed = buf[off + 0x36] & 0xFF;
        a->lingli = buf[off + 0x37] & 0xFF;
        a->luck = buf[off + 0x38] & 0xFF;
        a->exp = fmj_u16(buf, off + 0x32);
        /* 装备槽 0..7 对应 sEquipTypes = {6,6,5,3,7,2,4,1}，
         * 资源内偏移：slot0=0x1e slot1=0x1f slot2=0x1b slot3=0x1d
         *            slot4=0x1c slot5=0x19 slot6=0x1a slot7=0x18 */
        static const uint8_t eq_off[8] = {0x1e, 0x1f, 0x1b, 0x1d, 0x1c, 0x19, 0x1a, 0x18};
        static const uint8_t eq_type[8] = {6, 6, 5, 3, 7, 2, 4, 1};
        for (int i = 0; i < 8; ++i) {
            uint8_t t = buf[off + eq_off[i]] & 0xFF;
            if (t != 0) {
                a->equip_type[i] = eq_type[i];
                a->equip_index[i] = t;
            }
        }
    } else if (kind == FMJ_K_NPC || kind == FMJ_K_OBJ) {
        /* NPC.setData / SceneObj.setData */
        a->state = buf[off + 4] & 0xFF;
        if (a->state > 4) a->state = 0;
        copy_gbk_name(a->name, buf, off + 9);
        a->delay = buf[off + 0x15] & 0xFF;
        if (a->delay == 0) a->state = FMJ_ST_STOP;
        a->walk_type = (kind == FMJ_K_NPC) ? 2 : 4;
        a->walk_index = buf[off + 0x16] & 0xFF;
        a->direction = kind == FMJ_K_OBJ ? FMJ_DIR_NORTH : fmj_dir_from(buf[off + 2]);
        a->step = buf[off + 3] & 0xFF;
    } else if (kind == FMJ_K_MONSTER) {
        /* Monster.setData */
        a->magic_chain = buf[off + 0x2f] & 0xFF;
        a->magic_chain_learn_num = buf[off + 2] & 0xFF;
        a->atbuff = buf[off + 4] & 0xFF;
        copy_gbk_name(a->name, buf, off + 6);
        a->level = buf[off + 0x12] & 0xFF;
        a->max_hp = fmj_u16(buf, off + 0x18);
        a->hp = fmj_u16(buf, off + 0x1a);
        a->max_mp = fmj_u16(buf, off + 0x1c);
        a->mp = fmj_u16(buf, off + 0x1e);
        a->attack = fmj_u16(buf, off + 0x20);
        a->defend = fmj_u16(buf, off + 0x22);
        a->speed = buf[off + 0x13] & 0xFF;
        a->lingli = buf[off + 0x14] & 0xFF;
        a->luck = buf[off + 0x16] & 0xFF;
        a->money = fmj_u16(buf, off + 0x24);
        a->m_exp = fmj_u16(buf, off + 0x26);
        a->drop_type = buf[off + 0x2b] & 0xFF;
        a->drop_index = buf[off + 0x2c] & 0xFF;
        a->drop_num = buf[off + 0x2d] & 0xFF;
        a->fight_pic = buf[off + 0x2e] & 0xFF; /* 战斗大图 ACP type */
    }
    return 0;
}

/* 资源方向值转换（非法值回南方） */
int fmj_dir_from(int v) {
    if (v < 1 || v > 4) return FMJ_DIR_SOUTH;
    return v;
}

/* 相机视口钳制：视口落在地图内（小于视口的小图贴原点，两侧留白为原版行为）。
 * 防脚本 loadmap/screens 给的坐标越界 → 视口越出地图 → tile 循环画不满 →
 * 大块白屏/白条（2026-10-30 真机实测“画面移动有空白条”根因之一）。 */
void fmj_scene_camera_clamp(int *scr_x, int *scr_y) {
    int vw = FMJ_MAP_VIEW_W, vh = FMJ_MAP_VIEW_H;
    int mw = fmj_cur_map.width, mh = fmj_cur_map.height;
    if (mw > vw) {
        if (*scr_x < 0) *scr_x = 0;
        if (*scr_x > mw - vw) *scr_x = mw - vw;
    } else {
        *scr_x = 0;
    }
    if (mh > vh) {
        if (*scr_y < 0) *scr_y = 0;
        if (*scr_y > mh - vh) *scr_y = mh - vh;
    } else {
        *scr_y = 0;
    }
}

/* 相机跟随（2026-10-30 真机反馈：大地图必须随走动滚动，否则走出视口即
 * “不能换场景”、玩家消失、tile 区露白）。原版是每步滚 1 格的逐像素滚屏；
 * 墨水屏适配为“出带跳滚”：玩家越出中央带（视口 9×6，列[3,5]行[2,3]）
 * 才把相机一次跳到玩家居中（列 4 / 行 2）——屏内走动零背景刷（差分只推
 * 玩家小区域），出带跳滚一次全刷（差分自动发现全画布变 → 整游戏区一次推屏）。 */
static void camera_follow(const fmj_actor_t *p) {
    if (!fmj_map_loaded) return;
    int tx = fmj_map_scr_x, ty = fmj_map_scr_y;
    int cx = p->map_x - fmj_map_scr_x;   /* 玩家在视口中的列/行 */
    int cy = p->map_y - fmj_map_scr_y;
    if (cx < 3 || cx > FMJ_MAP_VIEW_W - 4)
        tx = p->map_x - (FMJ_MAP_VIEW_W - 1) / 2;    /* 回中央列（9→列4） */
    if (cy < 2 || cy > FMJ_MAP_VIEW_H - 3)
        ty = p->map_y - (FMJ_MAP_VIEW_H / 2 - 1);    /* 回中央行（6→行2） */
    fmj_scene_camera_clamp(&tx, &ty);
    fmj_map_scr_x = tx;
    fmj_map_scr_y = ty;
}

void fmj_scene_load_map(int type, int index, int scr_x, int scr_y) {
    fmj_res_map(&fmj_cur_map, type, index);
    fmj_map_loaded = true;
    fmj_map_scr_x = scr_x;
    fmj_map_scr_y = scr_y;
    fmj_scene_camera_clamp(&fmj_map_scr_x, &fmj_map_scr_y);
}

void fmj_scene_create_actor(int actor_id, int x, int y) {
    if (fmj_player_count >= FMJ_PLAYERS_MAX) return;
    fmj_actor_t *a = &fmj_players[fmj_player_count];
    if (fmj_actor_load(a, FMJ_K_PLAYER, 1, actor_id) != 0) return;
    a->map_x = x + fmj_map_scr_x;
    a->map_y = y + fmj_map_scr_y;
    ++fmj_player_count;
}

void fmj_scene_delete_actor(int actor_id) {
    for (int i = 0; i < fmj_player_count; ++i) {
        if (fmj_players[i].index == actor_id) {
            memmove(&fmj_players[i], &fmj_players[i + 1],
                    sizeof(fmj_actor_t) * (fmj_player_count - 1 - i));
            --fmj_player_count;
            if (fmj_player_ctrl >= fmj_player_count) fmj_player_ctrl = 0;
            break;
        }
    }
}

fmj_actor_t *fmj_scene_get_player(int actor_id) {
    for (int i = 0; i < fmj_player_count; ++i) {
        if (fmj_players[i].index == actor_id) return &fmj_players[i];
    }
    return NULL;
}

void fmj_scene_set_control(int actor_id) {
    for (int i = 0; i < fmj_player_count; ++i) {
        if (fmj_players[i].index == actor_id) {
            fmj_actor_t t = fmj_players[i];
            memmove(&fmj_players[1], &fmj_players[0], sizeof(fmj_actor_t) * i);
            fmj_players[0] = t;
            fmj_player_ctrl = 0;
            return;
        }
    }
}

fmj_actor_t *fmj_scene_create_npc(int id, int res_id, int x, int y) {
    if (id < 1 || id >= FMJ_NPC_SLOTS) return NULL;
    fmj_actor_t *a = &fmj_npcs[id];
    if (fmj_actor_load(a, FMJ_K_NPC, 2, res_id) != 0) return NULL;
    a->map_x = x;
    a->map_y = y;
    return a;
}

void fmj_scene_delete_npc(int id) {
    if (id >= 1 && id < FMJ_NPC_SLOTS) memset(&fmj_npcs[id], 0, sizeof(fmj_actor_t));
}

void fmj_scene_delete_all_npc(void) {
    memset(fmj_npcs, 0, sizeof(fmj_npcs));
}

fmj_actor_t *fmj_scene_npc(int id) {
    if (id < 0 || id >= FMJ_NPC_SLOTS) return NULL;
    return &fmj_npcs[id];
}

fmj_actor_t *fmj_scene_create_box(int id, int box_id, int x, int y) {
    if (id < 1 || id >= FMJ_NPC_SLOTS) return NULL;
    fmj_actor_t *a = &fmj_npcs[id];
    if (fmj_actor_load(a, FMJ_K_OBJ, 4, box_id) != 0) return NULL;
    a->map_x = x;
    a->map_y = y;
    return a;
}

void fmj_scene_box_open(int id) {
    if (id >= 1 && id < FMJ_NPC_SLOTS) fmj_npcs[id].step = 1;
}

int fmj_scene_npc_at(int x, int y) {
    for (int i = 1; i < FMJ_NPC_SLOTS; ++i) {
        if (fmj_npcs[i].kind != FMJ_K_EMPTY &&
            fmj_npcs[i].map_x == x && fmj_npcs[i].map_y == y) return i;
    }
    /* 主角占位也算阻挡 */
    return 0;
}

bool fmj_scene_can_walk(int x, int y) {
    if (!fmj_map_can_walk(&fmj_cur_map, x, y)) return false;
    if (fmj_scene_npc_at(x, y) != 0) return false;
    if (fmj_player_count > 0) {
        const fmj_actor_t *p = &fmj_players[fmj_player_ctrl];
        if (p->map_x == x && p->map_y == y) return false;
    }
    return true;
}

/* NPC 巡逻一步（事件驱动：每帧 tick 调用一次概率推进） */
static void npc_step_walk(fmj_actor_t *a) {
    int nx = a->map_x + walk_dir_dx(a->direction);
    int ny = a->map_y + walk_dir_dy(a->direction);
    if (fmj_scene_can_walk(nx, ny)) {
        a->map_x = nx;
        a->map_y = ny;
        a->step = (a->step + 1) & 3;
    } else {
        /* 换方向：N→E→S→W 循环 */
        a->direction = a->direction % 4 + 1;
    }
}

void fmj_scene_tick_npcs(void) {
    for (int i = 1; i < FMJ_NPC_SLOTS; ++i) {
        fmj_actor_t *a = &fmj_npcs[i];
        if (a->kind != FMJ_K_NPC) continue;
        if (a->state == FMJ_ST_WALKING || a->state == FMJ_ST_FORCE_MOVE) {
            int r = rand() % 5;
            if (r == 0) a->state = FMJ_ST_PAUSE;
            else npc_step_walk(a);
        }
    }
}

/* ---------- 行走与事件 ---------- */
/* 走一步；返回是否需要刷屏 */
bool fmj_scene_walk(int dir) {
    fmj_actor_t *p = &fmj_players[fmj_player_ctrl];
    if (fmj_player_count == 0) return false;
    bool moved = false;

    int nx = p->map_x + walk_dir_dx(dir);
    int ny = p->map_y + walk_dir_dy(dir);

    /* 目标格子的地图事件先触发（原版 triggerMapEvent） */
    fmj_script_map_event(nx, ny);

    bool can = fmj_map_can_player_walk(&fmj_cur_map, nx, ny) && fmj_scene_npc_at(nx, ny) == 0;
    if (can) {
        p->direction = dir;
        p->map_x = nx;
        p->map_y = ny;
        p->step = (p->step + 1) & 3;
        moved = true;
    } else {
        p->direction = dir;
        p->step = (p->step + 1) & 3;
        moved = true; /* 原地踏步也要换帧 */
    }
    /* 地图随走动滚动（2026-10-30 真机反馈核心项）：真实位移后相机跟带 */
    if (can) camera_follow(p);
    fmj_scene_tick_npcs();
    return moved;
}

void fmj_scene_action(void) {
    fmj_actor_t *p = &fmj_players[fmj_player_ctrl];
    if (fmj_player_count == 0) return;
    int x = p->map_x + walk_dir_dx(p->direction);
    int y = p->map_y + walk_dir_dy(p->direction);

    int npc_id = fmj_scene_npc_at(x, y);
    if (npc_id != 0) {
        fmj_script_trigger_event(npc_id);
        return;
    }
    fmj_script_map_event(x, y);
}

/* ---------- 绘制 ---------- */
void fmj_scene_draw(fmj_canvas_t *c) {
    fmj_canvas_clear(c, FMJ_WHITE);

    if (fmj_map_loaded) {
        int max_y = FMJ_MAP_VIEW_H < fmj_cur_map.height - fmj_map_scr_y
                        ? FMJ_MAP_VIEW_H : fmj_cur_map.height - fmj_map_scr_y;
        int max_x = FMJ_MAP_VIEW_W < fmj_cur_map.width - fmj_map_scr_x
                        ? FMJ_MAP_VIEW_W : fmj_cur_map.width - fmj_map_scr_x;
        const fmj_image_t *til = fmj_res_image(FMJ_RT_TIL, 1, fmj_cur_map.til_index);
        if (til) {
            for (int y = 0; y < max_y; ++y) {
                for (int x = 0; x < max_x; ++x) {
                    int tile = fmj_map_tile_index(&fmj_cur_map, fmj_map_scr_x + x, fmj_map_scr_y + y);
                    fmj_canvas_draw_image(c, til, tile + 1,
                                          x * FMJ_TILE_W + FMJ_MAP_LEFT_OFFSET, y * FMJ_TILE_H, false);
                }
            }
        }
    }

    /* 主角位置 */
    int py = -1;
    if (fmj_player_count > 0) py = fmj_players[fmj_player_ctrl].map_y;

    /* 按 y 从大到小绘制，保证遮挡关系 */
    for (int order = 0; order < FMJ_NPC_SLOTS + 1; ++order) {
        /* 找当前未绘制的最大 y 的角色 */
        int best = -1;
        int best_y = -1;
        for (int i = 1; i < FMJ_NPC_SLOTS; ++i) {
            if (fmj_npcs[i].kind == FMJ_K_EMPTY || fmj_npcs[i].drawn) continue;
            if (fmj_npcs[i].map_y > best_y) { best_y = fmj_npcs[i].map_y; best = i; }
        }
        bool draw_player = false;
        if (!fmj_player_drawn_flag && py > best_y && fmj_player_count > 0) {
            draw_player = true;
        }
        if (draw_player) {
            fmj_actor_draw_walk(c, &fmj_players[fmj_player_ctrl], fmj_map_scr_x, fmj_map_scr_y);
            fmj_player_drawn_flag = true;
        } else if (best >= 0) {
            fmj_actor_draw_walk(c, &fmj_npcs[best], fmj_map_scr_x, fmj_map_scr_y);
            fmj_npcs[best].drawn = true;
        } else break;
    }
    /* 清除绘制标记 */
    fmj_player_drawn_flag = false;
    for (int i = 1; i < FMJ_NPC_SLOTS; ++i) fmj_npcs[i].drawn = false;

    /* 两侧留白（原版 drawSideFrame：8px 竖线） */
    for (int y = 0; y < FMJ_SCREEN_H; ++y) {
        for (int i = 0; i < 4; ++i) {
            fmj_canvas_set_px(c, i * 2, y, FMJ_BLACK);
            fmj_canvas_set_px(c, FMJ_SCREEN_W - 8 + i * 2, y, FMJ_BLACK);
        }
    }
}

/* ---------- 物品清单 ---------- */
bool fmj_goods_add(int type, int index, int num) {
    for (int i = 0; i < fmj_goods_count; ++i) {
        if (fmj_goods[i].type == type && fmj_goods[i].index == index) {
            int n = fmj_goods[i].num + num;
            if (n > 99) n = 99;
            fmj_goods[i].num = (uint8_t)n;
            return true;
        }
    }
    if (fmj_goods_count >= FMJ_GOODS_MAX) return false;
    fmj_goods[fmj_goods_count].type = (uint8_t)type;
    fmj_goods[fmj_goods_count].index = (uint8_t)index;
    fmj_goods[fmj_goods_count].num = (uint8_t)num;
    ++fmj_goods_count;
    return true;
}

bool fmj_goods_remove(int type, int index) {
    for (int i = 0; i < fmj_goods_count; ++i) {
        if (fmj_goods[i].type == type && fmj_goods[i].index == index) {
            if (fmj_goods[i].num > 1) { --fmj_goods[i].num; return true; }
            memmove(&fmj_goods[i], &fmj_goods[i + 1],
                    sizeof(fmj_goods_ent_t) * (fmj_goods_count - 1 - i));
            --fmj_goods_count;
            return true;
        }
    }
    return false;
}

int fmj_goods_num(int type, int index) {
    for (int i = 0; i < fmj_goods_count; ++i) {
        if (fmj_goods[i].type == type && fmj_goods[i].index == index) return fmj_goods[i].num;
    }
    return 0;
}

bool fmj_goods_use_num(int type, int index, int num) {
    int have = fmj_goods_num(type, index);
    if (have < num) return false;
    for (int i = 0; i < fmj_goods_count; ++i) {
        if (fmj_goods[i].type == type && fmj_goods[i].index == index) {
            fmj_goods[i].num = (uint8_t)(have - num);
            if (fmj_goods[i].num == 0) return fmj_goods_remove(type, index);
            return true;
        }
    }
    return false;
}
