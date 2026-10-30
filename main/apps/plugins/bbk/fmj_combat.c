/*
 * fmj_combat.c - 回合制战斗实现（经典简化版）
 *
 * 流程：玩家依次选动作（攻击/法术/物品/逃跑）→ 按速度执行 → 消息逐条按键浏览 → 判定胜负。
 * 剧情战斗结束后触发 winto/lossto 事件；随机战斗胜利结算经验金钱。
 */
#include "fmj_combat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fmj_font.h"
#include "fmj_scene.h"
#include "fmj_screen.h"

/* ---------- 文案（GBK） ---------- */
static const uint8_t G_ATTACK[] = {0xB9, 0xA5, 0xBB, 0xF7, 0};           /* 攻击 */
static const uint8_t G_MAGIC[] = {0xB7, 0xA8, 0xCA, 0xF5, 0};           /* 法术 */
static const uint8_t G_ITEM[] = {0xCE, 0xEF, 0xC6, 0xB7, 0};             /* 物品 */
static const uint8_t G_FLEE[] = {0xCC, 0xD3, 0xC5, 0xDC, 0};             /* 逃跑 */
static const uint8_t G_HIT[] = {0xCA, 0xDC, 0xB5, 0xBD, 0};             /* 受到 */
static const uint8_t G_DMG[] = {0xB5, 0xE3, 0xC9, 0xCB, 0xBA, 0xA6, 0}; /* 点伤害 */
static const uint8_t G_DEAD[] = {0xC9, 0xF7, 0xC2, 0xE4, 0};            /* 阵亡 */
static const uint8_t G_WIN[] = {0xD5, 0xBD, 0xB6, 0xB7, 0xCA, 0xA4, 0xC0, 0xFB, 0};   /* 战斗胜利 */
static const uint8_t G_LOSE[] = {0xD5, 0xBD, 0xB6, 0xB7, 0xCA, 0xA7, 0xB0, 0xDC, 0};   /* 战斗失败 */
static const uint8_t G_GET[] = {0xBB, 0xF1, 0xB5, 0xC3, 0};             /* 获得 */
static const uint8_t G_EXP[] = {0xBE, 0xAD, 0xD1, 0xE9, 0};             /* 经验 */
static const uint8_t G_MONEY[] = {0xBD, 0xF0, 0xC7, 0xAE, 0};            /* 金钱 */
static const uint8_t G_MP[] = {0xD5, 0xE6, 0xC6, 0xF8, 0};              /* 真气 */
static const uint8_t G_FLEE_OK[] = {0xCC, 0xD3, 0xC5, 0xDC, 0xB3, 0xC9, 0xB9, 0xA6, 0}; /* 逃跑成功 */

/* ---------- 状态 ---------- */
enum {
    FMJ_CB_NONE = 0,
    FMJ_CB_ACTION,   /* 选动作 */
    FMJ_CB_TARGET,   /* 选目标 */
    FMJ_CB_MAGIC,    /* 选法术 */
    FMJ_CB_ITEM,     /* 选物品 */
    FMJ_CB_MSG,      /* 浏览消息 */
    FMJ_CB_OVER,     /* 结束动画 */
};

#define FMJ_CB_MSG_N 8
#define FMJ_CB_MON_MAX 3

typedef struct {
    int state;
    fmj_actor_t mons[FMJ_CB_MON_MAX];
    int mon_count;
    int scr[3];               /* 背景三图 */
    bool is_story;
    int win_to, loss_to;
    int round_cnt;
    int cur_player;           /* 当前选动作的玩家 */
    int cur_menu;             /* 菜单光标 */
    int cur_target;           /* 目标光标 */
    int cur_magic, cur_item;
    /* 消息队列 */
    uint8_t msgs[FMJ_CB_MSG_N][48];
    int msg_count, msg_show;
    /* 回合计划 */
    uint8_t plan_kind[FMJ_PLAYERS_MAX];  /* 0=攻击 1=法术 2=物品 3=逃跑 */
    int plan_arg[FMJ_PLAYERS_MAX];       /* 法术/物品下标 */
    int plan_target[FMJ_PLAYERS_MAX];
    int win_money, win_exp;
    bool fled;                 /* 逃跑标记：退出战斗不算胜负 */
} fmj_cb_t;

static fmj_cb_t cb;

/* 随机战斗池 */
static int s_pool[8];
static int s_pool_count = 0;
static int s_scr[3] = {1, 1, 1};

void fmj_combat_set_random(const int *monsters, int count, int scrb, int scrl, int scrr) {
    s_pool_count = 0;
    for (int i = 0; i < count && i < 8; ++i) {
        if (monsters[i] > 0) s_pool[s_pool_count++] = monsters[i];
    }
    s_scr[0] = scrb;
    s_scr[1] = scrl;
    s_scr[2] = scrr;
}

/* ---------- 消息 ---------- */
static void cb_msg(const uint8_t *a, const uint8_t *b, const uint8_t *c, int num) {
    if (cb.msg_count >= FMJ_CB_MSG_N) return;
    uint8_t *m = cb.msgs[cb.msg_count++];
    int off = 0;
    if (a) { int l = fmj_cstr_len(a, 0); memcpy(m + off, a, l); off += l; }
    if (b) { int l = fmj_cstr_len(b, 0); memcpy(m + off, b, l); off += l; }
    if (num >= 0) { off += snprintf((char *)(m + off), 8, "%d", num); }
    if (c) { int l = fmj_cstr_len(c, 0); memcpy(m + off, c, l); off += l; }
    m[off] = 0;
}

static void cb_msg_str(const uint8_t *s) {
    if (cb.msg_count >= FMJ_CB_MSG_N) return;
    int l = fmj_cstr_len(s, 0);
    if (l > 47) l = 47;
    memcpy(cb.msgs[cb.msg_count], s, l);
    cb.msgs[cb.msg_count][l] = 0;
    ++cb.msg_count;
}

/* ---------- 战斗建立 ---------- */
static void cb_start(bool story, const int *mons, int mon_count, const int *scr,
                     int win_to, int loss_to, int round_max) {
    memset(&cb, 0, sizeof(cb));
    cb.state = FMJ_CB_ACTION;
    cb.is_story = story;
    cb.win_to = win_to;
    cb.loss_to = loss_to;
    for (int i = 0; i < 3; ++i) cb.scr[i] = scr ? scr[i] : 1;
    cb.mon_count = 0;
    for (int i = 0; i < mon_count && i < FMJ_CB_MON_MAX; ++i) {
        if (mons[i] <= 0) continue;
        fmj_actor_t *m = &cb.mons[cb.mon_count];
        if (fmj_actor_load(m, FMJ_K_MONSTER, 3, mons[i]) == 0) {
            m->hp = m->max_hp;
            ++cb.mon_count;
            cb.win_money += m->money;
            cb.win_exp += m->m_exp;
        }
    }
    /* 玩家血量保底 */
    for (int i = 0; i < fmj_player_count; ++i) {
        if (fmj_players[i].hp <= 0) fmj_players[i].hp = 1;
    }
    cb.cur_player = 0;
}

void fmj_combat_enter(const fmj_fight_cfg_t *cfg) {
    cb_start(true, cfg->monsters, 3, cfg->scr, cfg->win_to, cfg->loss_to, cfg->round_max);
}

void fmj_combat_random_check(void) {
    if (!fmj_fight_enabled || s_pool_count == 0) return;
    if (rand() % 20 != 0) return;
    /* 1-3 只随机怪物 */
    int count = 1 + rand() % 3;
    int mons[3] = {0};
    for (int i = 0; i < count; ++i) mons[i] = s_pool[rand() % s_pool_count];
    cb_start(false, mons, 3, s_scr, 0, 0, 0);
}

bool fmj_combat_active(void) { return cb.state != FMJ_CB_NONE; }

bool fmj_combat_msg_active(void) {
    return cb.state == FMJ_CB_MSG && cb.msg_show < cb.msg_count;
}

/* ---------- 伤害结算 ---------- */
static fmj_actor_t *alive_monster(void) {
    for (int i = 0; i < cb.mon_count; ++i)
        if (cb.mons[i].hp > 0) return &cb.mons[i];
    return NULL;
}

static int calc_damage(const fmj_actor_t *src, const fmj_actor_t *dst) {
    int d = src->attack - dst->defend;
    if (d < 1) d = 1;
    d += rand() % 5 - 2;
    if (d < 1) d = 1;
    return d;
}

static bool any_player_alive(void) {
    for (int i = 0; i < fmj_player_count; ++i)
        if (fmj_players[i].hp > 0) return true;
    return false;
}

static void cb_finish(bool win) {
    cb.state = FMJ_CB_NONE;
    if (cb.fled) return; /* 逃跑：直接回场景 */
    if (cb.is_story) {
        fmj_script_trigger_event(win ? cb.win_to : cb.loss_to);
    } else if (!win) {
        fmj_game_request_over();
    } else {
        /* 随机战斗胜利：经验金钱结算 */
        fmj_money += cb.win_money;
        for (int i = 0; i < fmj_player_count; ++i) {
            fmj_players[i].exp = (int16_t)(fmj_players[i].exp + cb.win_exp);
        }
    }
}

/* 回合执行：所有玩家计划完成后调用 */
static void cb_execute_round(void) {
    /* 玩家动作（按速度快慢，简化为顺序） */
    for (int i = 0; i < fmj_player_count; ++i) {
        fmj_actor_t *p = &fmj_players[i];
        if (p->hp <= 0) continue;
        int kind = cb.plan_kind[i];
        if (kind == 3) { /* 逃跑 */
            cb.fled = true;
            cb_msg_str(G_FLEE_OK);
            cb.state = FMJ_CB_MSG;
            return; /* 直接结束回合进入消息 */
        }
        fmj_actor_t *m = &cb.mons[cb.plan_target[i]];
        if (m->hp <= 0) m = alive_monster();
        if (!m) break;
        if (kind == 0) { /* 攻击 */
            int dmg = calc_damage(p, m);
            m->hp = (int16_t)(m->hp - dmg);
            cb_msg((const uint8_t *)p->name, G_HIT, G_DMG, dmg);
            if (m->hp <= 0) cb_msg((const uint8_t *)m->name, G_DEAD, NULL, -1);
        } else if (kind == 1) { /* 法术：取链/自学列表中第 plan_arg 个 */
            int mtype, mindex;
            int total = p->magic_chain_learn_num + p->lm_count;
            int k = cb.plan_arg[i];
            if (k < p->magic_chain_learn_num) {
                /* 从 MLR 链取第 k 个 */
                int off = fmj_datlib_offset(FMJ_RT_MLR, 1, p->magic_chain);
                mtype = mindex = 0;
                if (off >= 0) {
                    const uint8_t *buf = fmj_datlib_buffer();
                    int cnt = buf[off + 2] & 0xFF;
                    if (k < cnt) {
                        mtype = buf[off + 3 + k * 2];
                        mindex = buf[off + 3 + k * 2 + 1];
                    }
                }
            } else {
                mtype = p->lm_type[k - p->magic_chain_learn_num];
                mindex = p->lm_index[k - p->magic_chain_learn_num];
            }
            /* 耗 MP + 伤害 = 灵力加成 */
            int moff = fmj_datlib_offset(FMJ_RT_MRS, mtype, mindex);
            int cost = 0;
            if (moff >= 0) cost = fmj_datlib_buffer()[moff + 4];
            if (p->mp < cost) {
                cb_msg_str(G_MP); /* 真气不足（简化） */
            } else {
                p->mp = (int16_t)(p->mp - cost);
                int dmg = p->lingli / 2 + 5 + rand() % 4;
                m->hp = (int16_t)(m->hp - dmg);
                cb_msg((const uint8_t *)p->name, G_HIT, G_DMG, dmg);
                if (m->hp <= 0) cb_msg((const uint8_t *)m->name, G_DEAD, NULL, -1);
            }
        } else if (kind == 2) { /* 物品：恢复类，简单回满 HP */
            if (fmj_goods_count > 0) {
                fmj_goods_ent_t *g = &fmj_goods[cb.plan_arg[i] % fmj_goods_count];
                /* 判定是否药品：type 9-13 视为药品 */
                if (g->type >= 9 && g->type <= 13) {
                    p->hp = p->max_hp;
                    fmj_goods_remove(g->type, g->index);
                    cb_msg_str(G_GET);
                } else {
                    /* 非药品按投掷：对怪物造成固定伤害 */
                    int dmg = 10 + rand() % 6;
                    m->hp = (int16_t)(m->hp - dmg);
                    fmj_goods_remove(g->type, g->index);
                    cb_msg((const uint8_t *)p->name, G_HIT, G_DMG, dmg);
                    if (m->hp <= 0) cb_msg((const uint8_t *)m->name, G_DEAD, NULL, -1);
                }
            }
        }
    }
    /* 怪物还击 */
    if (alive_monster()) {
        for (int i = 0; i < cb.mon_count; ++i) {
            fmj_actor_t *m = &cb.mons[i];
            if (m->hp <= 0) continue;
            /* 攻击随机存活玩家 */
            int alive[FMJ_PLAYERS_MAX], n = 0;
            for (int j = 0; j < fmj_player_count; ++j)
                if (fmj_players[j].hp > 0) alive[n++] = j;
            if (n == 0) break;
            fmj_actor_t *p = &fmj_players[alive[rand() % n]];
            int dmg = calc_damage(m, p);
            p->hp = (int16_t)(p->hp - dmg);
            cb_msg((const uint8_t *)m->name, G_HIT, G_DMG, dmg);
            if (p->hp <= 0) cb_msg((const uint8_t *)p->name, G_DEAD, NULL, -1);
        }
    }
    /* 判定 */
    if (!alive_monster()) {
        cb_msg_str(G_WIN);
        if (cb.win_money > 0) cb_msg(G_GET, G_MONEY, NULL, cb.win_money);
        if (cb.win_exp > 0) cb_msg(G_GET, G_EXP, NULL, cb.win_exp);
        cb.state = FMJ_CB_MSG;
        cb.msg_show = 0;
        return;
    }
    if (!any_player_alive()) {
        cb_msg_str(G_LOSE);
        cb.state = FMJ_CB_MSG;
        cb.msg_show = 0;
        return;
    }
    /* 新回合 */
    ++cb.round_cnt;
    memset(cb.plan_kind, 0, sizeof(cb.plan_kind));
    cb.cur_player = 0;
    cb.state = FMJ_CB_ACTION;
    cb.cur_menu = 0;
}

/* ---------- 输入 ---------- */
void fmj_combat_input(int key, bool is_up) {
    if (!is_up) return; /* 墨水屏只在抬起时响应，防抖 */
    switch (cb.state) {
    case FMJ_CB_ACTION: {
        if (key == FMJ_KEY_RIGHT || key == FMJ_KEY_DOWN) cb.cur_menu = (cb.cur_menu + 1) % 4;
        else if (key == FMJ_KEY_LEFT || key == FMJ_KEY_UP) cb.cur_menu = (cb.cur_menu + 3) % 4;
        else if (key == FMJ_KEY_ENTER) {
            switch (cb.cur_menu) {
            case 0: /* 攻击 */
                cb.state = FMJ_CB_TARGET;
                cb.cur_target = 0;
                break;
            case 1: /* 法术 */
                cb.state = FMJ_CB_MAGIC;
                cb.cur_magic = 0;
                break;
            case 2: /* 物品 */
                if (fmj_goods_count == 0) break;
                cb.state = FMJ_CB_ITEM;
                cb.cur_item = 0;
                break;
            case 3: /* 逃跑 */
                cb.plan_kind[cb.cur_player] = 3;
                cb.cur_player++;
                if (cb.cur_player >= fmj_player_count) cb_execute_round();
                break;
            }
        }
        break;
    }
    case FMJ_CB_TARGET:
        if (key == FMJ_KEY_RIGHT || key == FMJ_KEY_DOWN) cb.cur_target = (cb.cur_target + 1) % cb.mon_count;
        else if (key == FMJ_KEY_LEFT || key == FMJ_KEY_UP) cb.cur_target = (cb.cur_target + cb.mon_count - 1) % cb.mon_count;
        else if (key == FMJ_KEY_CANCEL) { cb.state = FMJ_CB_ACTION; break; }
        else if (key == FMJ_KEY_ENTER) {
            if (cb.mons[cb.cur_target].hp > 0) {
                cb.plan_kind[cb.cur_player] = 0;
                cb.plan_target[cb.cur_player] = cb.cur_target;
                cb.cur_player++;
                if (cb.cur_player >= fmj_player_count) cb_execute_round();
                else cb.state = FMJ_CB_ACTION;
            }
        }
        break;
    case FMJ_CB_MAGIC: {
        fmj_actor_t *p = &fmj_players[cb.cur_player < fmj_player_count ? cb.cur_player : fmj_player_ctrl];
        int total = p->magic_chain_learn_num + p->lm_count;
        if (total == 0) { cb.state = FMJ_CB_ACTION; break; }
        if (key == FMJ_KEY_DOWN) cb.cur_magic = (cb.cur_magic + 1) % total;
        else if (key == FMJ_KEY_UP) cb.cur_magic = (cb.cur_magic + total - 1) % total;
        else if (key == FMJ_KEY_CANCEL) cb.state = FMJ_CB_ACTION;
        else if (key == FMJ_KEY_ENTER) {
            cb.plan_kind[cb.cur_player] = 1;
            cb.plan_arg[cb.cur_player] = cb.cur_magic;
            cb.plan_target[cb.cur_player] = cb.cur_target;
            cb.state = FMJ_CB_TARGET; /* 法术也要选目标 */
            cb.cur_target = 0;
        }
        break;
    }
    case FMJ_CB_ITEM:
        if (key == FMJ_KEY_DOWN) cb.cur_item = (cb.cur_item + 1) % fmj_goods_count;
        else if (key == FMJ_KEY_UP) cb.cur_item = (cb.cur_item + fmj_goods_count - 1) % fmj_goods_count;
        else if (key == FMJ_KEY_CANCEL) cb.state = FMJ_CB_ACTION;
        else if (key == FMJ_KEY_ENTER) {
            cb.plan_kind[cb.cur_player] = 2;
            cb.plan_arg[cb.cur_player] = cb.cur_item;
            cb.cur_player++;
            if (cb.cur_player >= fmj_player_count) cb_execute_round();
            else cb.state = FMJ_CB_ACTION;
        }
        break;
    case FMJ_CB_MSG:
        if (key == FMJ_KEY_ENTER || key == FMJ_KEY_CANCEL || key == FMJ_KEY_PAGEDOWN) {
            ++cb.msg_show;
            if (cb.msg_show >= cb.msg_count) {
                if (cb.fled) { cb_finish(false); break; }
                bool win = alive_monster() == NULL;
                if (cb.msg_count > 0 && memcmp(cb.msgs[cb.msg_count - 1], G_WIN, sizeof(G_WIN) - 1) == 0) win = true;
                if (cb.msg_count > 0 && memcmp(cb.msgs[cb.msg_count - 1], G_LOSE, sizeof(G_LOSE) - 1) == 0) win = false;
                cb_finish(win);
            }
        }
        break;
    default:
        break;
    }
}

/* ---------- 绘制 ---------- */
static void draw_menu_item(fmj_canvas_t *c, const uint8_t *label, int x, int y, bool sel) {
    if (sel) fmj_draw_text(c, label, x, y, FMJ_WHITE, FMJ_BLACK);
    else fmj_draw_text(c, label, x, y, FMJ_BLACK, FMJ_WHITE);
}

void fmj_combat_draw(fmj_canvas_t *c) {
    fmj_canvas_clear(c, FMJ_WHITE);
    /* 背景：原版 PIC 三图拼合（上/左下/右下） */
    const fmj_image_t *bg = fmj_res_image(FMJ_RT_PIC, 4, cb.scr[0]);
    if (bg) fmj_canvas_draw_image(c, bg, 1, 0, 0, false);
    const fmj_image_t *bgl = fmj_res_image(FMJ_RT_PIC, 4, cb.scr[1]);
    if (bgl) fmj_canvas_draw_image(c, bgl, 1, 0, FMJ_SCREEN_H - bgl->height, false);
    const fmj_image_t *bgr = fmj_res_image(FMJ_RT_PIC, 4, cb.scr[2]);
    if (bgr) fmj_canvas_draw_image(c, bgr, 1, FMJ_SCREEN_W - bgr->width, 0, false);

    /* 怪物（战斗大图 ACP fight_pic） */
    static const int mon_pos[3][2] = {{12, 25}, {44, 14}, {82, 11}};
    for (int i = 0; i < cb.mon_count; ++i) {
        fmj_actor_t *m = &cb.mons[i];
        if (m->hp <= 0) continue;
        const fmj_image_t *img = fmj_res_image(FMJ_RT_ACP, m->fight_pic, m->index);
        if (img) fmj_canvas_draw_image(c, img, 1, mon_pos[i][0], mon_pos[i][1], false);
        /* 目标选择光标 */
        if (cb.state == FMJ_CB_TARGET && cb.cur_target == i) {
            fmj_canvas_draw_rect_stroke(c, mon_pos[i][0] - 2, mon_pos[i][1] - 2,
                                        mon_pos[i][0] + 46, mon_pos[i][1] + 46, FMJ_BLACK);
        }
    }

    /* 底部状态区：灰度分层分隔线（上灰晕 + 下主墨） */
    fmj_canvas_draw_rect_fill(c, 0, 96 - 24, 159, 95, FMJ_WHITE);
    fmj_canvas_draw_line(c, 0, 96 - 26, 159, 96 - 26, FMJ_GRAY_FRAME);
    fmj_canvas_draw_line(c, 0, 96 - 25, 159, 96 - 25, FMJ_BLACK);

    fmj_actor_t *p = &fmj_players[fmj_player_ctrl];
    if (fmj_player_count > 0) {
        char line[32];
        snprintf(line, sizeof(line), "%d/%d %d/%d", p->hp, p->max_hp, p->mp, p->max_mp);
        fmj_draw_text(c, (const uint8_t *)p->name, 2, 96 - 22, FMJ_BLACK, FMJ_WHITE);
        fmj_draw_text(c, (const uint8_t *)line, 2, 96 - 8, FMJ_BLACK, FMJ_WHITE);
    }

    if (cb.state == FMJ_CB_ACTION || cb.state == FMJ_CB_TARGET) {
        draw_menu_item(c, G_ATTACK, 62, 96 - 22, cb.cur_menu == 0);
        draw_menu_item(c, G_MAGIC, 62, 96 - 8, cb.cur_menu == 1);
        draw_menu_item(c, G_ITEM, 110, 96 - 22, cb.cur_menu == 2);
        draw_menu_item(c, G_FLEE, 110, 96 - 8, cb.cur_menu == 3);
    } else if (cb.state == FMJ_CB_MAGIC) {
        fmj_actor_t *pl = &fmj_players[cb.cur_player < fmj_player_count ? cb.cur_player : fmj_player_ctrl];
        int total = pl->magic_chain_learn_num + pl->lm_count;
        for (int k = 0; k < total && k < 4; ++k) {
            int mtype, mindex;
            if (k < pl->magic_chain_learn_num) {
                int off = fmj_datlib_offset(FMJ_RT_MLR, 1, pl->magic_chain);
                mtype = mindex = 0;
                if (off >= 0) {
                    const uint8_t *buf = fmj_datlib_buffer();
                    int cnt = buf[off + 2] & 0xFF;
                    if (k < cnt) { mtype = buf[off + 3 + k * 2]; mindex = buf[off + 3 + k * 2 + 1]; }
                }
            } else {
                mtype = pl->lm_type[k - pl->magic_chain_learn_num];
                mindex = pl->lm_index[k - pl->magic_chain_learn_num];
            }
            const uint8_t *mname = (const uint8_t *)"\0";
            int moff = fmj_datlib_offset(FMJ_RT_MRS, mtype, mindex);
            if (moff >= 0) mname = fmj_datlib_buffer() + moff + 6;
            fmj_canvas_draw_rect_fill(c, 58, 96 - 24, 159, 95, FMJ_WHITE);
            fmj_draw_text(c, mname, 62, 96 - 22 + (k % 4) * 14, k == cb.cur_magic ? FMJ_WHITE : FMJ_BLACK,
                          k == cb.cur_magic ? FMJ_BLACK : FMJ_WHITE);
        }
    } else if (cb.state == FMJ_CB_ITEM) {
        fmj_canvas_draw_rect_fill(c, 58, 96 - 24, 159, 95, FMJ_WHITE);
        for (int k = 0; k < fmj_goods_count && k < 4; ++k) {
            const uint8_t *gname = (const uint8_t *)"\0";
            int off = fmj_datlib_offset(FMJ_RT_GRS, fmj_goods[k].type, fmj_goods[k].index);
            if (off >= 0) gname = fmj_datlib_buffer() + off + 6;
            fmj_draw_text(c, gname, 62, 96 - 22 + (k % 4) * 14, k == cb.cur_item ? FMJ_WHITE : FMJ_BLACK,
                          k == cb.cur_item ? FMJ_BLACK : FMJ_WHITE);
        }
    } else if (cb.state == FMJ_CB_MSG && cb.msg_show < cb.msg_count) {
        fmj_draw_msg_box(c, cb.msgs[cb.msg_show]);
    }
}
