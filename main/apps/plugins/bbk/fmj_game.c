/*
 * fmj_game.c - 游戏总控实现
 */
#include "fmj_game.h"

#include <string.h>
#include <stdio.h>

#include "nvs.h"

#include "fmj_combat.h"
#include "fmj_font.h"
#include "fmj_scene.h"
#include "fmj_script.h"

/* ---------- 文案（GBK） ---------- */
static const uint8_t G_TITLE[] = {0xB7, 0xFC, 0xC4, 0xA7, 0xBC, 0xC7, 0};              /* 伏魔记 */
static const uint8_t G_NEW[] = {0xD0, 0xC2, 0xB5, 0xC4, 0xC0, 0xFA, 0xB3, 0xCC, 0};  /* 新的历程 */
static const uint8_t G_LOAD[] = {0xBC, 0xCC, 0xD0, 0xF8, 0xC7, 0xB0, 0xD4, 0xB5, 0}; /* 继续前缘 */
static const uint8_t G_OVER[] = {0xD3, 0xCE, 0xCF, 0xB7, 0xBD, 0xE1, 0xCA, 0xF8, 0}; /* 游戏结束 */
static const uint8_t G_PRESS[] = {0xB0, 0xB4, 0xC8, 0xCE, 0xD2, 0xE2, 0xBC, 0xFC, 0xB7, 0xB5, 0xBB, 0xD8, 0}; /* 按任意键返回 */
static const uint8_t G_STATE[] = {0xD7, 0xB4, 0xCC, 0xAC, 0};                        /* 状态 */
static const uint8_t G_GOODS[] = {0xCE, 0xEF, 0xC6, 0xB7, 0};                         /* 物品 */
static const uint8_t G_SAVE[] = {0xB4, 0xE6, 0xB5, 0xB5, 0};                         /* 存档 */
static const uint8_t G_QUIT[] = {0xB7, 0xB5, 0xBB, 0xD8, 0xB1, 0xEA, 0xCC, 0xE2, 0}; /* 返回标题 */
static const uint8_t G_LEVEL[] = {0xB5, 0xC8, 0xBC, 0xB6, 0};                        /* 等级 */
static const uint8_t G_EXPW[] = {0xBE, 0xAD, 0xD1, 0xE9, 0};                         /* 经验 */
static const uint8_t G_MONEYS[] = {0xBD, 0xF0, 0xC7, 0xAE, 0};                       /* 金钱 */
static const uint8_t G_HP[] = {0xC9, 0xFA, 0xC3, 0xFC, 0};                           /* 生命 */
static const uint8_t G_ATK[] = {0xB9, 0xA5, 0xBB, 0xF7, 0};                           /* 攻击 */
static const uint8_t G_DEF[] = {0xB7, 0xC0, 0xD3, 0xF9, 0};                           /* 防御 */
static const uint8_t G_EMPTYBAG[] = {0xC9, 0xCF, 0xB5, 0xDA, 0xD7, 0xF3, 0xB7, 0xE2, 0}; /* 尚未拾取 */

/* ---------- 状态机 ---------- */
enum {
    FMJ_G_TITLE = 0,   /* 标题菜单 */
    FMJ_G_PLAYING,     /* 主游戏 */
    FMJ_G_MENU,        /* 游戏主菜单（覆盖在场景上） */
    FMJ_G_MENU_STATE,  /* 状态页 */
    FMJ_G_MENU_GOODS,  /* 物品页 */
    FMJ_G_OVER_SCREEN, /* 游戏结束画面 */
};

static int s_state = FMJ_G_TITLE;
static int s_title_sel = 0;
static int s_menu_sel = 0;
static int s_any_key = 0;          /* 结束画面按任意键 */
static bool s_running = false;

/* 存档镜像（NVS blob） */
#define FMJ_SAVE_MAGIC 0x464D4A31 /* "FMJ1" */
typedef struct {
    uint32_t magic;
    int32_t script_type, script_index;
    int32_t map_type, map_index, map_scr_x, map_scr_y;
    int32_t player_count, money, goods_count;
    fmj_actor_t players[FMJ_PLAYERS_MAX];
    fmj_goods_ent_t goods[FMJ_GOODS_MAX];
    char scene_name[16];
    int16_t vars[FMJ_VAR_MAX];
    uint8_t events_bits[(FMJ_EVENT_MAX + 7) / 8];
} fmj_save_t;

/* ---------- 新游戏 / 读档 ---------- */
static void start_new_game(void) {
    fmj_vars_init_global();
    fmj_vars_init_local();
    fmj_events_init_all();
    fmj_player_count = 0;
    fmj_player_ctrl = 0;
    fmj_goods_count = 0;
    fmj_money = 0;
    fmj_fight_enabled = false;
    memset(fmj_scene_name, 0, sizeof(fmj_scene_name));
    fmj_script_start_chapter(1, 1);
}

static void enter_playing(void) {
    s_state = FMJ_G_PLAYING;
    fmj_script_step(); /* 推进到第一个操作 */
}

bool fmj_game_active(void) { return s_running; }

void fmj_game_start(void) {
    s_state = FMJ_G_TITLE;
    s_title_sel = 0;
    s_running = true;
}

void fmj_game_stop(void) {
    s_running = false;
    fmj_script_reset();
}

void fmj_game_request_over(void) {
    s_state = FMJ_G_OVER_SCREEN;
    s_any_key = 0;
}

/* ---------- 存档 ---------- */
int fmj_game_save(void) {
    if (fmj_disable_save || !fmj_proc_cur) return -1;
    fmj_save_t sv;
    memset(&sv, 0, sizeof(sv));
    sv.magic = FMJ_SAVE_MAGIC;
    sv.script_type = fmj_proc_cur->gut.type;
    sv.script_index = fmj_proc_cur->gut.index;
    sv.map_type = fmj_cur_map.type;
    sv.map_index = fmj_cur_map.index;
    sv.map_scr_x = fmj_map_scr_x;
    sv.map_scr_y = fmj_map_scr_y;
    sv.player_count = fmj_player_count;
    sv.money = fmj_money;
    sv.goods_count = fmj_goods_count;
    memcpy(sv.players, fmj_players, sizeof(fmj_players));
    memcpy(sv.goods, fmj_goods, sizeof(fmj_goods));
    memcpy(sv.scene_name, fmj_scene_name, sizeof(sv.scene_name));
    memcpy(sv.vars, fmj_vars, sizeof(sv.vars));
    for (int i = 0; i < FMJ_EVENT_MAX; ++i) {
        if (fmj_events[i]) sv.events_bits[i >> 3] |= (uint8_t)(1 << (i & 7));
    }
    nvs_handle_t h;
    if (nvs_open("te_fmj", NVS_READWRITE, &h) != ESP_OK) return -2;
    esp_err_t err = nvs_set_blob(h, "slot1", &sv, sizeof(sv));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -3;
}

bool fmj_game_has_save(void) {
    nvs_handle_t h;
    if (nvs_open("te_fmj", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = nvs_get_blob(h, "slot1", NULL, &len) == ESP_OK && len == sizeof(fmj_save_t);
    nvs_close(h);
    return ok;
}

static bool load_save(void) {
    nvs_handle_t h;
    fmj_save_t sv;
    if (nvs_open("te_fmj", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(sv);
    esp_err_t err = nvs_get_blob(h, "slot1", &sv, &len);
    nvs_close(h);
    if (err != ESP_OK || sv.magic != FMJ_SAVE_MAGIC || len != sizeof(sv)) return false;

    fmj_vars_init_global();
    fmj_vars_init_local();
    fmj_events_init_all();
    fmj_player_count = 0;
    fmj_player_ctrl = 0;
    fmj_goods_count = 0;
    fmj_money = sv.money;
    fmj_goods_count = sv.goods_count > FMJ_GOODS_MAX ? FMJ_GOODS_MAX : (int)sv.goods_count;
    memcpy(fmj_players, sv.players, sizeof(fmj_players));
    memcpy(fmj_goods, sv.goods, sizeof(fmj_goods));
    memcpy(fmj_scene_name, sv.scene_name, sizeof(fmj_scene_name));
    memcpy(fmj_vars, sv.vars, sizeof(fmj_vars));
    for (int i = 0; i < FMJ_EVENT_MAX; ++i) {
        fmj_events[i] = (sv.events_bits[i >> 3] >> (i & 7)) & 1;
    }
    fmj_player_count = sv.player_count > FMJ_PLAYERS_MAX ? FMJ_PLAYERS_MAX : (int)sv.player_count;
    /* 恢复地图与章节（从章节开头重入，事件标志保证剧情正确跳过） */
    fmj_scene_load_map(sv.map_type, sv.map_index, sv.map_scr_x, sv.map_scr_y);
    fmj_script_start_chapter(sv.script_type, sv.script_index);
    return true;
}

/* ---------- 输入路由 ---------- */
void fmj_game_input(int key, bool is_up) {
    if (!s_running) return;

    switch (s_state) {
    case FMJ_G_TITLE:
        if (is_up) {
            if (key == FMJ_KEY_UP || key == FMJ_KEY_DOWN) s_title_sel = 1 - s_title_sel;
            else if (key == FMJ_KEY_ENTER) {
                if (s_title_sel == 0) {
                    start_new_game();
                    enter_playing();
                } else {
                    if (load_save()) enter_playing();
                    /* 无存档或失败：留在标题 */
                }
            }
        }
        break;
    case FMJ_G_PLAYING: {
        if (fmj_combat_active()) {
            fmj_combat_input(key, is_up);
            break;
        }
        if (fmj_script_running()) {
            if (fmj_script_has_op()) {
                fmj_script_input(key, is_up);
            } else if (is_up) {
                fmj_script_step();
            }
            break;
        }
        /* 场景操作 */
        if (is_up) {
            switch (key) {
            case FMJ_KEY_LEFT: case FMJ_KEY_RIGHT:
            case FMJ_KEY_UP: case FMJ_KEY_DOWN: {
                int dir = key == FMJ_KEY_LEFT ? FMJ_DIR_WEST :
                          key == FMJ_KEY_RIGHT ? FMJ_DIR_EAST :
                          key == FMJ_KEY_UP ? FMJ_DIR_NORTH : FMJ_DIR_SOUTH;
                (void)fmj_scene_walk(dir);
                if (fmj_script_running() && !fmj_script_has_op()) fmj_script_step();
                break;
            }
            case FMJ_KEY_ENTER:
                fmj_scene_action();
                if (fmj_script_running() && !fmj_script_has_op()) fmj_script_step();
                break;
            case FMJ_KEY_CANCEL:
                s_state = FMJ_G_MENU;
                s_menu_sel = 0;
                break;
            default:
                break;
            }
        }
        break;
    }
    case FMJ_G_MENU:
        if (is_up) {
            if (key == FMJ_KEY_UP) s_menu_sel = (s_menu_sel + 3) % 4;
            else if (key == FMJ_KEY_DOWN) s_menu_sel = (s_menu_sel + 1) % 4;
            else if (key == FMJ_KEY_CANCEL) s_state = FMJ_G_PLAYING;
            else if (key == FMJ_KEY_ENTER) {
                if (s_menu_sel == 0) s_state = FMJ_G_MENU_STATE;
                else if (s_menu_sel == 1) s_state = FMJ_G_MENU_GOODS;
                else if (s_menu_sel == 2) { (void)fmj_game_save(); }
                else s_state = FMJ_G_TITLE;
            }
        }
        break;
    case FMJ_G_MENU_STATE:
    case FMJ_G_MENU_GOODS:
        if (is_up && (key == FMJ_KEY_CANCEL || key == FMJ_KEY_ENTER)) s_state = FMJ_G_MENU;
        break;
    case FMJ_G_OVER_SCREEN:
        if (is_up) {
            s_any_key = 1;
            s_state = FMJ_G_TITLE;
            s_title_sel = 0;
        }
        break;
    default:
        break;
    }
}

/* ---------- 绘制 ---------- */
static void draw_centered(fmj_canvas_t *c, const uint8_t *t, int y, int scale, bool inverse) {
    int w = fmj_text_width(t) * scale;
    int x = (FMJ_SCREEN_W - w) / 2;
    fmj_draw_text_scaled(c, t, x, y, scale,
                         inverse ? FMJ_WHITE : FMJ_BLACK, inverse ? FMJ_BLACK : FMJ_WHITE);
}

/* 标题按钮：选中=白底黑字，未选=黑底白字（灰度分层边框） */
static void title_button(fmj_canvas_t *c, const uint8_t *t, int y, bool sel) {
    int w = fmj_text_width(t);
    int x0 = (FMJ_SCREEN_W - w - 14) / 2;
    int x1 = x0 + w + 14;
    fmj_canvas_draw_rect_fill(c, x0, y, x1, y + 17, sel ? FMJ_WHITE : FMJ_BLACK);
    fmj_canvas_draw_rect_layered(c, x0, y, x1, y + 17);
    fmj_draw_text(c, t, (FMJ_SCREEN_W - w) / 2, y + 1,
                  sel ? FMJ_BLACK : FMJ_WHITE, sel ? FMJ_WHITE : FMJ_BLACK);
}

int fmj_game_scene_bg(void) {
    if (s_state == FMJ_G_TITLE) return 1;
    if (s_state == FMJ_G_PLAYING && fmj_combat_active()) return 2;
    return 0;
}

bool fmj_game_menu_overlay(void) {
    return s_state == FMJ_G_MENU_STATE || s_state == FMJ_G_MENU_GOODS;
}

void fmj_game_draw(fmj_canvas_t *c) {
    switch (s_state) {
    case FMJ_G_TITLE:
        fmj_canvas_clear_transparent(c); /* 透出 rt=20 标题背景整图 */
        /* 大字：scale 3 + 灰度投影（横屏 ×7 后 336px 高） */
        {
            int w = fmj_text_width(G_TITLE) * 3;
            int x = (FMJ_SCREEN_W - w) / 2;
            fmj_draw_text_scaled(c, G_TITLE, x + 1, 5, 3, FMJ_GRAY_AA, FMJ_WHITE);
            fmj_draw_text_scaled(c, G_TITLE, x, 4, 3, FMJ_BLACK, FMJ_WHITE);
        }
        title_button(c, G_NEW, 58, s_title_sel == 0);
        title_button(c, G_LOAD, 79, s_title_sel == 1);
        break;
    case FMJ_G_PLAYING:
        if (fmj_combat_active()) {
            fmj_combat_draw(c);
        } else if (fmj_script_running() && fmj_script_has_op()) {
            /* 覆盖型操作（对话等）先画场景 */
            fmj_scene_draw(c);
            fmj_script_op_draw(c);
        } else {
            fmj_scene_draw(c);
        }
        break;
    case FMJ_G_MENU: {
        fmj_scene_draw(c);
        /* 半屏菜单：灰度分层边框 */
        fmj_canvas_draw_rect_fill(c, 24, 12, 135, 84, FMJ_WHITE);
        fmj_canvas_draw_rect_layered(c, 26, 14, 133, 82);
        static const uint8_t *items[4] = {G_STATE, G_GOODS, G_SAVE, G_QUIT};
        for (int i = 0; i < 4; ++i) {
            if (s_menu_sel == i) fmj_draw_text(c, items[i], 32, 18 + i * 16, FMJ_WHITE, FMJ_BLACK);
            else fmj_draw_text(c, items[i], 32, 18 + i * 16, FMJ_BLACK, FMJ_WHITE);
        }
        break;
    }
    case FMJ_G_MENU_STATE: {
        fmj_canvas_clear(c, FMJ_WHITE);
        fmj_canvas_draw_rect_layered(c, 3, 3, 156, 92);
        if (fmj_player_count > 0) {
            const fmj_actor_t *p = &fmj_players[fmj_player_ctrl];
            char line[20];
            fmj_draw_text(c, (const uint8_t *)p->name, 10, 8, FMJ_BLACK, FMJ_WHITE);
            fmj_draw_text(c, G_LEVEL, 10, 28, FMJ_BLACK, FMJ_WHITE);
            snprintf(line, sizeof(line), "%d", p->level);
            fmj_draw_text(c, (const uint8_t *)line, 42, 28, FMJ_BLACK, FMJ_WHITE);
            fmj_draw_text(c, G_HP, 10, 46, FMJ_BLACK, FMJ_WHITE);
            snprintf(line, sizeof(line), "%d/%d", p->hp, p->max_hp);
            fmj_draw_text(c, (const uint8_t *)line, 42, 46, FMJ_BLACK, FMJ_WHITE);
            fmj_draw_text(c, G_ATK, 10, 64, FMJ_BLACK, FMJ_WHITE);
            snprintf(line, sizeof(line), "%d", p->attack);
            fmj_draw_text(c, (const uint8_t *)line, 42, 64, FMJ_BLACK, FMJ_WHITE);
            fmj_draw_text(c, G_DEF, 80, 64, FMJ_BLACK, FMJ_WHITE);
            snprintf(line, sizeof(line), "%d", p->defend);
            fmj_draw_text(c, (const uint8_t *)line, 112, 64, FMJ_BLACK, FMJ_WHITE);
            fmj_draw_text(c, G_EXPW, 80, 46, FMJ_BLACK, FMJ_WHITE);
            snprintf(line, sizeof(line), "%d", p->exp);
            fmj_draw_text(c, (const uint8_t *)line, 112, 46, FMJ_BLACK, FMJ_WHITE);
            fmj_draw_text(c, G_MONEYS, 80, 28, FMJ_BLACK, FMJ_WHITE);
            snprintf(line, sizeof(line), "%d", fmj_money);
            fmj_draw_text(c, (const uint8_t *)line, 112, 28, FMJ_BLACK, FMJ_WHITE);
        }
        break;
    }
    case FMJ_G_MENU_GOODS: {
        fmj_canvas_clear(c, FMJ_WHITE);
        fmj_canvas_draw_rect_layered(c, 3, 3, 156, 92);
        if (fmj_goods_count == 0) {
            fmj_draw_text(c, G_EMPTYBAG, 12, 12, FMJ_BLACK, FMJ_WHITE);
        } else {
            for (int i = 0; i < fmj_goods_count && i < 5; ++i) {
                int off = fmj_datlib_offset(FMJ_RT_GRS, fmj_goods[i].type, fmj_goods[i].index);
                const uint8_t *gname = (const uint8_t *)"\0";
                if (off >= 0) gname = fmj_datlib_buffer() + off + 6;
                char num[8];
                snprintf(num, sizeof(num), "x%d", fmj_goods[i].num);
                fmj_draw_text(c, gname, 10, 8 + i * 17, FMJ_BLACK, FMJ_WHITE);
                fmj_draw_text(c, (const uint8_t *)num, 130, 8 + i * 17, FMJ_BLACK, FMJ_WHITE);
            }
        }
        break;
    }
    case FMJ_G_OVER_SCREEN:
        fmj_canvas_clear(c, FMJ_WHITE);
        draw_centered(c, G_OVER, 30, 2, false);
        draw_centered(c, G_PRESS, 62, 1, false);
        break;
    default:
        fmj_canvas_clear(c, FMJ_WHITE);
        break;
    }
}
