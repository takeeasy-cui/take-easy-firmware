/*
 * fmj_script.c - 剧情脚本虚拟机实现
 *
 * 指令集与 fmj.kt ScriptVM.kt 完全对应（0-79）。
 * 时间型操作（滚动字幕、延时提示）改为按键步进，适配墨水屏。
 */
#include "fmj_script.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "fmj_font.h"
#include "fmj_scene.h"
#include "fmj_screen.h"

/* ---------- 变量与事件 ---------- */
int16_t fmj_vars[FMJ_VAR_MAX];
uint8_t fmj_events[FMJ_EVENT_MAX];
fmj_proc_t *fmj_proc_cur = NULL;

void fmj_vars_init_local(void) { memset(&fmj_vars[200], 0, sizeof(int16_t) * 40); }
void fmj_vars_init_global(void) { memset(&fmj_vars[0], 0, sizeof(int16_t) * 200); }
void fmj_events_init_all(void) { memset(fmj_events, 0, sizeof(fmj_events)); }

/* ---------- 操作构造 ---------- */
static fmj_op_t *op_new(void) {
    fmj_op_t *op = calloc(1, sizeof(fmj_op_t));
    return op;
}

static void op_free(fmj_op_t *op) {
    if (!op) return;
    if (op->destroy) op->destroy(op);
    free(op);
}

static void op_finish(fmj_op_t *op) { op->done = true; }

/* ---------- say：对话（灰度分层边框：外晕灰 + 内深墨 + 头像斜线切角） ---------- */
static void say_draw(fmj_op_t *op, fmj_canvas_t *c) {
    if (op->d.say.head) {
        fmj_canvas_draw_rect_fill(c, 9, 50, 150, 95, FMJ_WHITE);
        fmj_canvas_draw_rect_layered(c, 11, 52, 148, 93);
        /* 头像区斜线切角：黑主线 + 灰晕 */
        for (int k = 0; k <= 6; ++k) {
            fmj_canvas_set_px(c, 38 + k, 50 + k, FMJ_GRAY_AA);
            fmj_canvas_set_px(c, 39 + k, 51 + k, FMJ_BLACK);
        }
        for (int x = 46; x <= 149; ++x) fmj_canvas_set_px(c, x, 57, FMJ_BLACK);
        /* 头像：原版 PIC 点阵（复古像素路线） */
        if (op->d.say.head)
            fmj_canvas_draw_image(c, op->d.say.head, 1, 14, 47, false);
        op->d.say.i_of_next = fmj_draw_text_rect(c, op->d.say.text, op->d.say.i_of_text,
                                                 46, 59, 145, 75, FMJ_BLACK, FMJ_WHITE);
        op->d.say.i_of_next = fmj_draw_text_rect(c, op->d.say.text, op->d.say.i_of_next,
                                                 14, 76, 145, 93, FMJ_BLACK, FMJ_WHITE);
    } else {
        fmj_canvas_draw_rect_fill(c, 9, 55, 150, 95, FMJ_WHITE);
        fmj_canvas_draw_rect_layered(c, 11, 57, 148, 93);
        op->d.say.i_of_next = fmj_draw_text_rect(c, op->d.say.text, op->d.say.i_of_text,
                                                 14, 59, 145, 75, FMJ_BLACK, FMJ_WHITE);
        op->d.say.i_of_next = fmj_draw_text_rect(c, op->d.say.text, op->d.say.i_of_next,
                                                 14, 76, 145, 93, FMJ_BLACK, FMJ_WHITE);
    }
}

static void say_key(fmj_op_t *op, int key, bool is_up) {
    if (is_up) {
        if (op->d.say.key_down_seen) {
            /* 已显示到末尾则结束，否则等待下一次按键翻屏 */
            int total = fmj_cstr_len(op->d.say.text, 0);
            if (op->d.say.i_of_next >= total - 1) op_finish(op);
            op->d.say.key_down_seen = false;
        }
    } else {
        op->d.say.key_down_seen = true;
        /* 按下即翻页：文本游标推进到下一屏 */
        if (op->d.say.i_of_next > op->d.say.i_of_text) {
            op->d.say.i_of_text = op->d.say.i_of_next;
        }
    }
}

/* ---------- message：黑框提示（框体绘制见 fmj_draw_msg_box） ---------- */
static void msg_key(fmj_op_t *op, int key, bool is_up) {
    if (is_up && op->d.msg.key_down_seen) op_finish(op);
    if (!is_up) op->d.msg.key_down_seen = true;
}

static void msg_draw(fmj_op_t *op, fmj_canvas_t *c) {
    fmj_draw_msg_box(c, op->d.msg.text);
}

static void msg_destroy(fmj_op_t *op) {
    if (op->d.msg.text) { heap_caps_free(op->d.msg.text); op->d.msg.text = NULL; }
}

static fmj_op_t *op_msg_new(const uint8_t *text) {
    fmj_op_t *op = op_new();
    if (!op) return NULL;
    int len = fmj_cstr_len(text, 0) + 1;
    op->d.msg.text = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!op->d.msg.text) { free(op); return NULL; }
    memcpy(op->d.msg.text, text, len);
    op->key = msg_key;
    op->draw = msg_draw;
    op->destroy = msg_destroy;
    return op;
}

/* ---------- choice：选择支 ---------- */
static void choice_draw(fmj_op_t *op, fmj_canvas_t *c) {
    fmj_canvas_draw_rect_fill(c, op->d.choice.bgx, op->d.choice.bgy,
                             op->d.choice.bgx + op->d.choice.w - 1,
                             op->d.choice.bgy + 16 * 2 + 5, FMJ_WHITE);
    fmj_canvas_draw_rect_layered(c, op->d.choice.bgx + 1, op->d.choice.bgy + 1,
                                 op->d.choice.bgx + op->d.choice.w - 2,
                                 op->d.choice.bgy + 16 * 2 + 4);
    if (op->d.choice.cur == 0) {
        fmj_draw_text(c, op->d.choice.c1, op->d.choice.bgx + 3, op->d.choice.bgy + 3, FMJ_WHITE, FMJ_BLACK);
        fmj_draw_text(c, op->d.choice.c2, op->d.choice.bgx + 3, op->d.choice.bgy + 3 + 16, FMJ_BLACK, FMJ_WHITE);
    } else {
        fmj_draw_text(c, op->d.choice.c1, op->d.choice.bgx + 3, op->d.choice.bgy + 3, FMJ_BLACK, FMJ_WHITE);
        fmj_draw_text(c, op->d.choice.c2, op->d.choice.bgx + 3, op->d.choice.bgy + 3 + 16, FMJ_WHITE, FMJ_BLACK);
    }
}

static void choice_key(fmj_op_t *op, int key, bool is_up) {
    if (!is_up) {
        if (key == FMJ_KEY_DOWN || key == FMJ_KEY_UP || key == FMJ_KEY_LEFT || key == FMJ_KEY_RIGHT)
            op->d.choice.cur = 1 - op->d.choice.cur;
        op->d.choice.key_down_seen = key;
    } else {
        if (key == FMJ_KEY_ENTER && op->d.choice.key_down_seen == key) {
            op->d.choice.has_select = true;
            op_finish(op);
        }
        op->d.choice.key_down_seen = 0;
    }
}

static void choice_destroy(fmj_op_t *op) {
    if (op->d.choice.c1) heap_caps_free(op->d.choice.c1);
    if (op->d.choice.c2) heap_caps_free(op->d.choice.c2);
}

/* ---------- showgut：卷轴字幕 ---------- */
static void gut_draw(fmj_op_t *op, fmj_canvas_t *c) {
    fmj_canvas_clear(c, FMJ_WHITE);
    int y0 = op->d.gut.top ? op->d.gut.top->height : 0;
    int y1 = op->d.gut.btm ? FMJ_SCREEN_H - op->d.gut.btm->height : FMJ_SCREEN_H;
    if (op->d.gut.top) fmj_canvas_draw_image(c, op->d.gut.top, 1, 0, 0, false);
    if (op->d.gut.btm) fmj_canvas_draw_image(c, op->d.gut.btm, 1, 0, FMJ_SCREEN_H - op->d.gut.btm->height, false);
    fmj_draw_text_scroll(c, op->d.gut.text, op->d.gut.cur_y, y0, y1, FMJ_BLACK, FMJ_WHITE);
}

static void gut_key(fmj_op_t *op, int key, bool is_up) {
    if (is_up) {
        if (key == FMJ_KEY_CANCEL) { op_finish(op); return; }
        int y0 = op->d.gut.top ? op->d.gut.top->height : 0;
        if (op->d.gut.cur_y <= y0 - 16 * 4) { op_finish(op); return; }
        op->d.gut.cur_y -= 16;
        if (op->d.gut.cur_y < y0) {
            /* 再向上滚一次确认结束：直接完成 */
            op_finish(op);
        }
    }
}

static void gut_destroy(fmj_op_t *op) {
    if (op->d.gut.text) heap_caps_free(op->d.gut.text);
}

/* ---------- menu：菜单 ---------- */
static void menu_draw(fmj_op_t *op, fmj_canvas_t *c) {
    int w = 0;
    for (int i = 0; i < op->d.menu.item_count; ++i) {
        int tw = fmj_text_width(op->d.menu.items[i]);
        if (tw > w) w = tw;
    }
    w += 12;
    int h = op->d.menu.item_count * 16 + 6;
    int x = FMJ_SCREEN_W - w - 4, y = 4;
    fmj_canvas_draw_rect_fill(c, x, y, x + w - 1, y + h - 1, FMJ_WHITE);
    fmj_canvas_draw_rect_layered(c, x + 1, y + 1, x + w - 2, y + h - 2);
    for (int i = 0; i < op->d.menu.item_count; ++i) {
        if (i == op->d.menu.cur)
            fmj_draw_text(c, op->d.menu.items[i], x + 6, y + 3 + i * 16, FMJ_WHITE, FMJ_BLACK);
        else
            fmj_draw_text(c, op->d.menu.items[i], x + 6, y + 3 + i * 16, FMJ_BLACK, FMJ_WHITE);
    }
    /* 三角光标（灰度柔化） */
    int ty = y + 3 + op->d.menu.cur * 16 + 1;
    for (int i = 0; i < 7; ++i)
        for (int j = i; j <= 13 - i; ++j)
            fmj_canvas_set_px(c, x + 1 + i, ty + j, FMJ_BLACK);
}

static void menu_key(fmj_op_t *op, int key, bool is_up) {
    if (is_up) return;
    if (key == FMJ_KEY_DOWN) op->d.menu.cur = (op->d.menu.cur + 1) % op->d.menu.item_count;
    else if (key == FMJ_KEY_UP) op->d.menu.cur = (op->d.menu.cur - 1 + op->d.menu.item_count) % op->d.menu.item_count;
    else if (key == FMJ_KEY_ENTER) {
        fmj_vars[op->d.menu.rv_addr] = (int16_t)op->d.menu.cur;
        op->d.menu.has_select = true;
        op_finish(op);
    }
}

static void menu_destroy(fmj_op_t *op) {
    for (int i = 0; i < 8; ++i) {
        if (op->d.menu.items[i]) { heap_caps_free(op->d.menu.items[i]); op->d.menu.items[i] = NULL; }
    }
}

/* ---------- 进程管理 ---------- */
static void proc_free_chain(fmj_proc_t *p) {
    while (p) {
        fmj_proc_t *prev = p->prev;
        if (p->op) { op_free(p->op); p->op = NULL; }
        free(p);
        p = prev;
    }
}

void fmj_script_reset(void) {
    proc_free_chain(fmj_proc_cur);
    fmj_proc_cur = NULL;
}

static fmj_proc_t *proc_new(int type, int index) {
    fmj_proc_t *p = calloc(1, sizeof(fmj_proc_t));
    if (!p) return NULL;
    if (fmj_res_gut(&p->gut, type, index) != 0) { free(p); return NULL; }
    p->pc = 0;
    p->running = true;
    return p;
}

void fmj_script_start_chapter(int type, int index) {
    proc_free_chain(fmj_proc_cur);
    fmj_scene_delete_all_npc();
    fmj_vars_init_local();
    fmj_proc_cur = proc_new(type, index);
}

void fmj_script_call_chapter(int type, int index) {
    fmj_proc_t *p = proc_new(type, index);
    if (!p) return;
    if (fmj_proc_cur) {
        p->prev = fmj_proc_cur;
        if (fmj_proc_cur->op) { op_free(fmj_proc_cur->op); fmj_proc_cur->op = NULL; }
    }
    fmj_proc_cur = p;
}

bool fmj_script_running(void) {
    return fmj_proc_cur && fmj_proc_cur->running;
}

void fmj_script_exit(void) {
    if (fmj_proc_cur) fmj_proc_cur->running = false;
}

bool fmj_script_has_op(void) {
    return fmj_proc_cur && fmj_proc_cur->running && fmj_proc_cur->op;
}

/* 覆盖型界面判定：按当前活动的 op 的 key 回调区分类型。 */
bool fmj_script_saying(void) {
    return fmj_proc_cur && fmj_proc_cur->op && fmj_proc_cur->op->key == say_key;
}
bool fmj_script_gut_active(void) {
    return fmj_proc_cur && fmj_proc_cur->op && fmj_proc_cur->op->key == gut_key;
}
bool fmj_script_msg_active(void) {
    return fmj_proc_cur && fmj_proc_cur->op && fmj_proc_cur->op->key == msg_key;
}

void fmj_script_op_draw(fmj_canvas_t *c) {
    if (fmj_proc_cur && fmj_proc_cur->op && fmj_proc_cur->op->draw)
        fmj_proc_cur->op->draw(fmj_proc_cur->op, c);
}

/* 场景事件触发（事件号 1..num_scene_event） */
bool fmj_script_trigger_event(int event_id) {
    if (!fmj_proc_cur) return false;
    fmj_proc_t *p = fmj_proc_cur;
    if (event_id < 1 || event_id > p->gut.num_scene_event) {
        p->running = false;
        return false;
    }
    int addr = fmj_u16(p->gut.events, (event_id - 1) * 2);
    if (addr == 0) { p->running = false; return false; }
    p->pc = addr - p->gut.header_cnt;
    if (p->pc < 0) p->pc = 0;
    if (p->op) { op_free(p->op); p->op = NULL; }
    p->running = true;
    return true;
}

/* goto 地址（相对资源头） */
static void proc_goto(fmj_proc_t *p, int addr) {
    p->pc = addr - p->gut.header_cnt;
    if (p->pc < 0) p->pc = 0;
    p->running = true;
}

/* 地图事件（NPC 事件由 scene_action 触发；这里处理地图格事件与随机战斗） */
void fmj_script_map_event(int x, int y) {
    if (!fmj_map_loaded) return;
    int ev = fmj_map_event_num(&fmj_cur_map, x, y);
    if (ev > 0) {
        fmj_script_trigger_event(ev + 40);
        return;
    }
    /* 无地图事件：随机战斗检查 */
    if (fmj_script_running()) fmj_combat_random_check();
}

/* ---------- 升级提示（cmd_actorlayerup 简化） ---------- */
typedef struct {
    fmj_op_t base;
    uint8_t text[32];
} levelup_op_t;

static void levelup_draw(fmj_op_t *op, fmj_canvas_t *c) {
    levelup_op_t *lo = (levelup_op_t *)op;
    fmj_draw_msg_box(c, lo->text);
}

static void levelup_key(fmj_op_t *op, int key, bool is_up) {
    if (is_up) op_finish(op);
}

fmj_op_t *fmj_op_levelup_msg(const char *name_gbk) {
    levelup_op_t *lo = calloc(1, sizeof(levelup_op_t));
    if (!lo) return NULL;
    /* 文案："XX 修行提升"（GBK） */
    static const uint8_t suffix[] = {0xD0, 0xDE, 0xD0, 0xD0, 0xCC, 0xFD, 0xCC, 0xE3, 0xC9, 0xFD, 0}; /* 修行提升 */
    int n = fmj_cstr_len((const uint8_t *)name_gbk, 0);
    if (n > 18) n = 18;
    memcpy(lo->text, name_gbk, n);
    memcpy(lo->text + n, suffix, sizeof(suffix));
    lo->base.key = levelup_key;
    lo->base.draw = levelup_draw;
    return &lo->base;
}

/* ==========================================================================
 * 指令解释器
 * ========================================================================== */

/* 由 fmj_game.c 提供 */
extern int fmj_game_save(void);
/* 由 fmj_combat.c 提供 */
extern void fmj_combat_set_random(const int *monsters, int count, int scrb, int scrl, int scrr);

/* GRS 物品名（GBK，指向 DAT.LIB 内部） */
static const uint8_t *goods_name(int type, int index) {
    int off = fmj_datlib_offset(FMJ_RT_GRS, type, index);
    if (off < 0) return (const uint8_t *)"\0";
    return fmj_datlib_buffer() + off + 6;
}

/* MRS 法术名（GBK） */
static const uint8_t *magic_name(int type, int index) {
    int off = fmj_datlib_offset(FMJ_RT_MRS, type, index);
    if (off < 0) return (const uint8_t *)"\0";
    return fmj_datlib_buffer() + off + 6;
}

/* 复制 GBK 串到 PSRAM */
static uint8_t *dup_gbk(const uint8_t *src, int len_incl_nul) {
    uint8_t *p = heap_caps_malloc(len_incl_nul, MALLOC_CAP_SPIRAM);
    if (p) memcpy(p, src, len_incl_nul);
    return p;
}

void fmj_script_input(int key, bool is_up) {
    if (fmj_proc_cur && fmj_proc_cur->op && fmj_proc_cur->op->key) {
        fmj_proc_cur->op->key(fmj_proc_cur->op, key, is_up);
        fmj_script_step();
    }
}

void fmj_script_step(void) {
    fmj_proc_t *p = fmj_proc_cur;
    if (!p) return;
    if (p->op) {
        if (p->op->done) {
            op_free(p->op);
            p->op = NULL;
        } else {
            return;
        }
    }
    if (!p->running) return;

    const uint8_t *code = p->gut.code;
    int len = p->gut.code_len;

    while (p->running && !p->op && p->pc < len) {
        int pc = p->pc;
        int cmd = code[pc++];
        switch (cmd) {
        case 0:  /* music：跳过 */
            pc += 4;
            break;
        case 1: { /* loadmap */
            int type = fmj_u16(code, pc);
            int index = fmj_u16(code, pc + 2);
            int x = fmj_u16(code, pc + 4);
            int y = fmj_u16(code, pc + 6);
            fmj_scene_load_map(type, index, x, y);
            pc += 8;
            break;
        }
        case 2: { /* createactor */
            int actor = fmj_u16(code, pc);
            int x = fmj_u16(code, pc + 2);
            int y = fmj_u16(code, pc + 4);
            fmj_scene_create_actor(actor, x, y);
            pc += 6;
            break;
        }
        case 3: { /* deletenpc */
            fmj_scene_delete_npc(fmj_u16(code, pc));
            pc += 2;
            break;
        }
        case 6: { /* move：NPC 瞬移到目标（墨水屏不做行走动画） */
            int id = fmj_u16(code, pc);
            int dx = fmj_u16(code, pc + 2);
            int dy = fmj_u16(code, pc + 4);
            fmj_actor_t *npc = fmj_scene_npc(id);
            if (npc && npc->kind != FMJ_K_EMPTY) {
                npc->map_x = (int16_t)dx;
                npc->map_y = (int16_t)dy;
            }
            pc += 6;
            break;
        }
        case 9:  /* callback */
            p->running = false;
            break;
        case 10: { /* goto */
            proc_goto(p, fmj_u16(code, pc));
            pc = p->pc;
            break;
        }
        case 11: { /* if：事件标志非零跳转 */
            int va = fmj_u16(code, pc);
            int addr = fmj_u16(code, pc + 2);
            if (fmj_event_get(va)) {
                proc_goto(p, addr);
                pc = p->pc;
            } else {
                pc += 4;
            }
            break;
        }
        case 12: { /* set 变量 */
            int id = fmj_u16(code, pc);
            int v = fmj_u16(code, pc + 2);
            if (id < FMJ_VAR_MAX) fmj_vars[id] = (int16_t)v;
            pc += 4;
            break;
        }
        case 13: { /* say 对话 */
            int pic = fmj_u16(code, pc);
            int tlen = fmj_cstr_len(code, pc + 2) + 1;
            fmj_op_t *op = op_new();
            if (op) {
                op->d.say.text = code + pc + 2;
                op->d.say.head = (pic > 0) ? fmj_res_image(FMJ_RT_PIC, 1, pic) : NULL;
                op->d.say.head_idx = pic;
                op->d.say.i_of_text = 0;
                op->d.say.i_of_next = 0;
                op->d.say.key_down_seen = false;
                op->key = say_key;
                op->draw = say_draw;
                p->op = op;
            }
            pc += 2 + tlen;
            break;
        }
        case 14: { /* startchapter */
            int type = fmj_u16(code, pc);
            int index = fmj_u16(code, pc + 2);
            p->pc = pc + 4;
            fmj_script_start_chapter(type, index);
            return; /* 当前进程已被替换 */
        }
        case 16: { /* screens：设置地图屏幕位置 */
            int x = fmj_u16(code, pc);
            int y = fmj_u16(code, pc + 2);
            fmj_map_scr_x = x;
            fmj_map_scr_y = y;
            /* 2026-10-30：脚本给的视口越界 → tile 区画不满露白，钳制进地图 */
            fmj_scene_camera_clamp(&fmj_map_scr_x, &fmj_map_scr_y);
            pc += 4;
            break;
        }
        case 20: /* gameover */
            p->running = false;
            fmj_game_request_over();
            break;
        case 21: { /* ifcmp：变量相等跳转 */
            int id = fmj_u16(code, pc);
            int other = fmj_u16(code, pc + 2);
            int addr = fmj_u16(code, pc + 4);
            if (id < FMJ_VAR_MAX && fmj_vars[id] == (int16_t)other) {
                proc_goto(p, addr);
                pc = p->pc;
            } else {
                pc += 6;
            }
            break;
        }
        case 22: { /* add */
            int va = fmj_u16(code, pc);
            int v = fmj_u16(code, pc + 2);
            if (va < FMJ_VAR_MAX) fmj_vars[va] = (int16_t)(fmj_vars[va] + v);
            pc += 4;
            break;
        }
        case 23: { /* sub */
            int va = fmj_u16(code, pc);
            int v = fmj_u16(code, pc + 2);
            if (va < FMJ_VAR_MAX) fmj_vars[va] = (int16_t)(fmj_vars[va] - v);
            pc += 4;
            break;
        }
        case 24: /* setcontrolid */
            fmj_scene_set_control(fmj_u16(code, pc));
            pc += 2;
            break;
        case 26: fmj_event_set(fmj_u16(code, pc)); pc += 2; break;
        case 27: fmj_event_clear(fmj_u16(code, pc)); pc += 2; break;
        case 28: { /* buy：商店（首版提示） */
            int tlen = fmj_cstr_len(code, pc) + 1;
            static const uint8_t hint[] = {0xC9, 0xCC, 0xB5, 0xEA, 0xB9, 0xA6, 0xC4, 0xDC, 0xD5, 0xFD, 0xD4, 0xDA, 0xBF, 0xAA, 0xB7, 0xA2, 0xD6, 0xD0, 0}; /* 商店功能正在开发中 */
            p->op = op_msg_new(hint);
            pc += tlen;
            break;
        }
        case 29: { /* facetoface：让角色相对而视 */
            int id0 = fmj_u16(code, pc);
            int id1 = fmj_u16(code, pc + 2);
            fmj_actor_t *c1 = id0 == 0 ? (fmj_player_count ? &fmj_players[fmj_player_ctrl] : NULL) : fmj_scene_npc(id0);
            fmj_actor_t *c2 = id1 == 0 ? (fmj_player_count ? &fmj_players[fmj_player_ctrl] : NULL) : fmj_scene_npc(id1);
            if (c1 && c2 && c2->kind != FMJ_K_EMPTY) {
                if (c1->map_x > c2->map_x) c2->direction = FMJ_DIR_EAST;
                else if (c1->map_x < c2->map_x) c2->direction = FMJ_DIR_WEST;
                else if (c1->map_y > c2->map_y) c2->direction = FMJ_DIR_SOUTH;
                else if (c1->map_y < c2->map_y) c2->direction = FMJ_DIR_NORTH;
            }
            pc += 4;
            break;
        }
        case 30: /* movie：特效动画，跳过 */
            pc += 10;
            break;
        case 31: { /* choice 选择支 */
            int l1 = fmj_cstr_len(code, pc) + 1;
            int l2 = fmj_cstr_len(code, pc + l1) + 1;
            int addr = fmj_u16(code, pc + l1 + l2);
            int w1 = fmj_text_width(code + pc);
            int w2 = fmj_text_width(code + pc + l1);
            int w = (w1 > w2 ? w1 : w2) - 8 + 6;
            fmj_op_t *op = op_new();
            if (op) {
                int maxl = l1 > l2 ? l1 : l2;
                op->d.choice.c1 = dup_gbk(code + pc, l1);
                op->d.choice.c2 = dup_gbk(code + pc + l1, l2);
                /* 短串补空格至等长 */
                uint8_t *shorter = (l1 < l2) ? op->d.choice.c1 : op->d.choice.c2;
                int sl = (l1 < l2) ? l1 : l2;
                for (int i = sl - 1; i < maxl - 1; ++i) shorter[i] = ' ';
                shorter[maxl - 1] = 0;
                op->d.choice.w = w;
                op->d.choice.bgx = (FMJ_SCREEN_W - w) / 2;
                op->d.choice.bgy = (FMJ_SCREEN_H - (16 * 2 + 6)) / 2;
                op->d.choice.cur = 0;
                op->d.choice.addr = addr;
                op->d.choice.key_down_seen = 0;
                op->key = choice_key;
                op->draw = choice_draw;
                op->destroy = choice_destroy;
                p->op = op;
            }
            pc += l1 + l2 + 2;
            break;
        }
        case 32: { /* createbox */
            int id = fmj_u16(code, pc);
            int box = fmj_u16(code, pc + 2);
            int x = fmj_u16(code, pc + 4);
            int y = fmj_u16(code, pc + 6);
            fmj_scene_create_box(id, box, x, y);
            pc += 8;
            break;
        }
        case 33: fmj_scene_delete_npc(fmj_u16(code, pc)); pc += 2; break; /* deletebox */
        case 34: { /* gaingoods：获得物品 */
            int gtype = fmj_u16(code, pc);
            int gindex = fmj_u16(code, pc + 2);
            fmj_goods_add(gtype, gindex, 1);
            /* 拼接 "获得:物品名"（GBK） */
            const uint8_t *gname = goods_name(gtype, gindex);
            static const uint8_t pre[] = {0xBB, 0xF1, 0xB5, 0xC3, 0x3A, 0}; /* 获得: */
            int pl = 5, gl = fmj_cstr_len(gname, 0);
            uint8_t *msg = heap_caps_malloc(pl + gl + 1, MALLOC_CAP_SPIRAM);
            if (msg) {
                memcpy(msg, pre, pl);
                memcpy(msg + pl, gname, gl + 1);
                fmj_op_t *op = op_msg_new(msg);
                if (op) {
                    op->d.msg.key_down_seen = true; /* 1 秒后自动结束（近似：松开任意键结束） */
                    p->op = op;
                }
                heap_caps_free(msg);
            }
            pc += 4;
            break;
        }
        case 35: { /* initfight：随机战斗池 */
            int monsters[8];
            for (int i = 0; i < 8; ++i) monsters[i] = fmj_u16(code, pc + i * 2);
            int scrb = fmj_u16(code, pc + 16);
            int scrl = fmj_u16(code, pc + 18);
            int scrr = fmj_u16(code, pc + 20);
            fmj_fight_enabled = true;
            fmj_combat_set_random(monsters, 8, scrb, scrl, scrr);
            pc += 22;
            break;
        }
        case 36: fmj_fight_enabled = true; break;  /* fightenable */
        case 37: fmj_fight_enabled = false; break; /* fightdisenable */
        case 38: { /* createnpc */
            int id = fmj_u16(code, pc);
            int res = fmj_u16(code, pc + 2);
            int x = fmj_u16(code, pc + 4);
            int y = fmj_u16(code, pc + 6);
            fmj_scene_create_npc(id, res, x, y);
            pc += 8;
            break;
        }
        case 39: { /* enterfight：剧情战斗 */
            fmj_fight_cfg_t cfg;
            cfg.round_max = fmj_u16(code, pc);
            for (int i = 0; i < 3; ++i) {
                cfg.monsters[i] = fmj_u16(code, pc + 2 + i * 2);
                cfg.scr[i] = fmj_u16(code, pc + 8 + i * 2);
                cfg.evt_rounds[i] = fmj_u16(code, pc + 14 + i * 2);
                cfg.evt_nums[i] = fmj_u16(code, pc + 20 + i * 2);
            }
            cfg.loss_to = fmj_u16(code, pc + 26);
            cfg.win_to = fmj_u16(code, pc + 28);
            p->pc = pc + 30;
            p->running = false; /* 战斗接管，结束后按事件回来 */
            fmj_combat_enter(&cfg);
            return;
        }
        case 40: fmj_scene_delete_actor(fmj_u16(code, pc)); pc += 2; break;
        case 41: fmj_money += fmj_s32(code, pc); pc += 4; break;  /* gainmoney */
        case 42: fmj_money -= fmj_s32(code, pc); pc += 4; break;  /* usemoney */
        case 43: fmj_money = fmj_s32(code, pc); pc += 4; break;  /* setmoney */
        case 44: { /* learnmagic */
            int actor = fmj_u16(code, pc);
            int mtype = fmj_u16(code, pc + 2);
            int mindex = fmj_u16(code, pc + 4);
            fmj_actor_t *pl = fmj_scene_get_player(actor);
            if (pl && pl->lm_count < 8) {
                pl->lm_type[pl->lm_count] = (uint8_t)mtype;
                pl->lm_index[pl->lm_count] = (uint8_t)mindex;
                ++pl->lm_count;
            }
            pc += 6;
            /* 学会魔法提示：名字 + "学会了此法术"（近似原版提示） */
            const uint8_t *mname = magic_name(mtype, mindex);
            static const uint8_t sfx[] = {0xD1, 0xA7, 0xBB, 0xE1, 0xC1, 0xCB, 0xB7, 0xA8, 0xCA, 0xF5, 0}; /* 学会了法术 */
            int nl = fmj_cstr_len(mname, 0);
            uint8_t *msg = heap_caps_malloc(nl + sizeof(sfx), MALLOC_CAP_SPIRAM);
            if (msg) {
                memcpy(msg, mname, nl);
                memcpy(msg + nl, sfx, sizeof(sfx));
                p->op = op_msg_new(msg);
                heap_caps_free(msg);
            }
            break;
        }
        case 45: { /* sale：卖（首版提示） */
            static const uint8_t hint[] = {0xC9, 0xCC, 0xB5, 0xEA, 0xB9, 0xA6, 0xC4, 0xDC, 0xD5, 0xFD, 0xD4, 0xDA, 0xBF, 0xAA, 0xB7, 0xA2, 0xD6, 0xD0, 0}; /* 商店功能正在开发中 */
            p->op = op_msg_new(hint);
            break;
        }
        case 46: { /* npcmovemod */
            int id = fmj_u16(code, pc);
            int st = fmj_u16(code, pc + 2);
            fmj_actor_t *npc = fmj_scene_npc(id);
            if (npc && st <= 4) npc->state = (uint8_t)st;
            pc += 4;
            break;
        }
        case 47: { /* message */
            int tlen = fmj_cstr_len(code, pc) + 1;
            p->op = op_msg_new(code + pc);
            pc += tlen;
            break;
        }
        case 48: { /* deletegoods（失败跳转；地址复用 start+2，与原版一致） */
            int gtype = fmj_u16(code, pc);
            int gindex = fmj_u16(code, pc + 2);
            int addr = fmj_u16(code, pc + 2);
            if (!fmj_goods_remove(gtype, gindex)) {
                proc_goto(p, addr);
                pc = p->pc;
            } else {
                pc += 6;
            }
            break;
        }
        case 49: { /* resumeactorhp */
            int id = fmj_u16(code, pc);
            int v = fmj_u16(code, pc + 2);
            fmj_actor_t *pl = fmj_scene_get_player(id);
            if (pl && pl->max_hp > 0) pl->hp = (int16_t)(pl->max_hp * v / 100);
            pc += 4;
            break;
        }
        case 50: { /* actorlayerup：升级 */
            int actor = fmj_u16(code, pc);
            int to = fmj_u16(code, pc + 2);
            fmj_actor_t *pl = fmj_scene_get_player(actor);
            if (pl && to > pl->level) {
                /* 简化升级：属性按 10% 提升，回满 */
                int dl = to - pl->level;
                pl->max_hp = (int16_t)(pl->max_hp + 10 * dl);
                pl->hp = pl->max_hp;
                pl->max_mp = (int16_t)(pl->max_mp + 5 * dl);
                pl->mp = pl->max_mp;
                pl->attack = (int16_t)(pl->attack + 2 * dl);
                pl->defend = (int16_t)(pl->defend + 1 * dl);
                pl->level = (uint8_t)to;
                p->op = fmj_op_levelup_msg(pl->name);
            }
            pc += 4;
            break;
        }
        case 51: fmj_scene_box_open(fmj_u16(code, pc)); pc += 2; break;
        case 52: fmj_scene_delete_all_npc(); break; /* delallnpc */
        case 53: { /* npcstep */
            int id = fmj_u16(code, pc);
            int faceto = fmj_u16(code, pc + 2);
            int step = fmj_u16(code, pc + 4);
            static const uint8_t dmap[4] = {FMJ_DIR_NORTH, FMJ_DIR_EAST, FMJ_DIR_SOUTH, FMJ_DIR_WEST};
            uint8_t d = faceto < 4 ? dmap[faceto] : FMJ_DIR_SOUTH;
            fmj_actor_t *a = id == 0 ? (fmj_player_count ? &fmj_players[fmj_player_ctrl] : NULL) : fmj_scene_npc(id);
            if (a && a->kind != FMJ_K_EMPTY) {
                a->direction = d;
                a->step = (uint8_t)(step & 3);
            }
            pc += 6;
            break;
        }
        case 54: { /* setscenename */
            int tlen = fmj_cstr_len(code, pc) + 1;
            int n = tlen - 1;
            if (n > 15) n = 15;
            memcpy(fmj_scene_name, code + pc, (size_t)n);
            fmj_scene_name[n] = 0;
            pc += tlen;
            break;
        }
        case 55: { /* showscenename：场景名提示 */
            fmj_op_t *op = op_msg_new((const uint8_t *)fmj_scene_name);
            if (op) {
                op->d.msg.key_down_seen = true;
                p->op = op;
            }
            break;
        }
        case 56: break; /* showscreen：场景由主循环统一重绘 */
        case 57: { /* usegoods（失败跳转） */
            int gtype = fmj_u16(code, pc);
            int gindex = fmj_u16(code, pc + 2);
            int addr = fmj_u16(code, pc + 4);
            if (!fmj_goods_remove(gtype, gindex)) {
                proc_goto(p, addr);
                pc = p->pc;
            } else {
                pc += 6;
            }
            break;
        }
        case 58: { /* attribtest */
            int actor = fmj_u16(code, pc);
            int at = fmj_u16(code, pc + 2);
            int value = fmj_u16(code, pc + 4);
            int addr1 = fmj_u16(code, pc + 6);
            int addr2 = fmj_u16(code, pc + 8);
            fmj_actor_t *pl = fmj_scene_get_player(actor);
            int cur = 0;
            if (pl) {
                switch (at) {
                case 0: cur = pl->level; break;
                case 1: cur = pl->attack; break;
                case 2: cur = pl->defend; break;
                case 3: cur = pl->speed; break;
                case 4: cur = pl->hp; break;
                case 5: cur = pl->mp; break;
                case 6: cur = pl->exp; break;
                case 7: cur = pl->lingli; break;
                case 8: cur = pl->luck; break;
                case 23: cur = pl->max_hp; break;
                case 24: cur = pl->max_mp; break;
                default: cur = 0; break;
                }
            }
            if (cur < value) { proc_goto(p, addr1); pc = p->pc; }
            else if (cur > value) { proc_goto(p, addr2); pc = p->pc; }
            else pc += 10;
            break;
        }
        case 59: { /* attribset */
            int actor = fmj_u16(code, pc);
            int at = fmj_u16(code, pc + 2);
            int v = fmj_u16(code, pc + 4);
            fmj_actor_t *pl = fmj_scene_get_player(actor);
            if (pl) {
                switch (at) {
                case 0: pl->level = (uint8_t)v; break;
                case 1: pl->attack = (int16_t)v; break;
                case 2: pl->defend = (int16_t)v; break;
                case 3: pl->speed = (uint8_t)v; break;
                case 4: pl->hp = (int16_t)v; break;
                case 5: pl->mp = (int16_t)v; break;
                case 6: pl->exp = (int16_t)v; break;
                case 7: pl->lingli = (uint8_t)v; break;
                case 8: pl->luck = (uint8_t)v; break;
                case 15: pl->max_hp = (int16_t)v; break;
                case 16: pl->max_mp = (int16_t)v; break;
                default: break;
                }
            }
            pc += 6;
            break;
        }
        case 60: { /* attribadd */
            int actor = fmj_u16(code, pc);
            int at = fmj_u16(code, pc + 2);
            int v = fmj_u16(code, pc + 4);
            fmj_actor_t *pl = fmj_scene_get_player(actor);
            if (pl) {
                switch (at) {
                case 0: pl->level = (uint8_t)(pl->level + v); break;
                case 1: pl->attack = (int16_t)(pl->attack + v); break;
                case 2: pl->defend = (int16_t)(pl->defend + v); break;
                case 3: pl->speed = (uint8_t)(pl->speed + v); break;
                case 4: pl->hp = (int16_t)(pl->hp + v); break;
                case 5: pl->mp = (int16_t)(pl->mp + v); break;
                case 6: pl->exp = (int16_t)(pl->exp + v); break;
                case 7: pl->lingli = (uint8_t)(pl->lingli + v); break;
                case 8: pl->luck = (uint8_t)(pl->luck + v); break;
                case 10: pl->max_hp = (int16_t)(pl->max_hp + v); break;
                case 11: pl->max_mp = (int16_t)(pl->max_mp + v); break;
                default: break;
                }
            }
            pc += 6;
            break;
        }
        case 61: { /* showgut：卷轴字幕 */
            int top = fmj_u16(code, pc);
            int btm = fmj_u16(code, pc + 2);
            int tlen = fmj_cstr_len(code, pc + 4) + 1;
            fmj_op_t *op = op_new();
            if (op) {
                op->d.gut.text = dup_gbk(code + pc + 4, tlen);
                op->d.gut.top = top > 0 ? fmj_res_image(FMJ_RT_PIC, 5, top) : NULL;
                op->d.gut.btm = btm > 0 ? fmj_res_image(FMJ_RT_PIC, 5, btm) : NULL;
                op->d.gut.top_idx = top;
                op->d.gut.btm_idx = btm;
                op->d.gut.cur_y = op->d.gut.btm ? FMJ_SCREEN_H - op->d.gut.btm->height : FMJ_SCREEN_H;
                op->key = gut_key;
                op->draw = gut_draw;
                op->destroy = gut_destroy;
                p->op = op;
            }
            pc += 4 + tlen;
            break;
        }
        case 62: { /* usegoodsnum */
            int gtype = fmj_u16(code, pc);
            int gindex = fmj_u16(code, pc + 2);
            int num = fmj_u16(code, pc + 4);
            int addr = fmj_u16(code, pc + 6);
            if (!fmj_goods_use_num(gtype, gindex, num)) {
                proc_goto(p, addr);
                pc = p->pc;
            } else {
                pc += 8;
            }
            break;
        }
        case 63: { /* randrade：概率跳转 */
            int rate = fmj_u16(code, pc);
            int addr = fmj_u16(code, pc + 2);
            if (rand() % 1000 <= rate) {
                proc_goto(p, addr);
                pc = p->pc;
            } else {
                pc += 4;
            }
            break;
        }
        case 64: { /* menu */
            int rv = fmj_u16(code, pc);
            int tlen = fmj_cstr_len(code, pc + 2) + 1;
            fmj_op_t *op = op_new();
            if (op) {
                /* 空格分隔的选项 */
                int count = 0;
                int i = 0;
                const uint8_t *s = code + pc + 2;
                while (s[i] != 0 && count < 8) {
                    int start = i;
                    while (s[i] != 0 && s[i] != ' ') ++i;
                    int l = i - start;
                    uint8_t *item = heap_caps_malloc(l + 1, MALLOC_CAP_SPIRAM);
                    if (item) {
                        memcpy(item, s + start, l);
                        item[l] = 0;
                        op->d.menu.items[count++] = item;
                    }
                    while (s[i] == ' ') ++i;
                }
                op->d.menu.item_count = count;
                op->d.menu.cur = 0;
                op->d.menu.rv_addr = rv;
                op->key = menu_key;
                op->draw = menu_draw;
                op->destroy = menu_destroy;
                p->op = op;
            }
            /* 原版 len = cstr.size + 3，参数区实际 2+1+cstr，多出 1 字节按原版跳过 */
            pc += 2 + tlen + 1;
            break;
        }
        case 65: { /* testmoney */
            int money = fmj_s32(code, pc);
            int addr = fmj_u16(code, pc + 4);
            if (fmj_money < money) {
                proc_goto(p, addr);
                pc = p->pc;
            } else {
                pc += 6;
            }
            break;
        }
        case 66: { /* callchapter */
            int type = fmj_u16(code, pc);
            int index = fmj_u16(code, pc + 2);
            p->pc = pc + 4;
            fmj_script_call_chapter(type, index);
            return;
        }
        case 67: { /* discmp */
            int id = fmj_u16(code, pc);
            int num = fmj_u16(code, pc + 2);
            int addr_lt = fmj_u16(code, pc + 4);
            int addr_gt = fmj_u16(code, pc + 6);
            int16_t v = id < FMJ_VAR_MAX ? fmj_vars[id] : 0;
            if (v < (int16_t)num) { proc_goto(p, addr_lt); pc = p->pc; }
            else if (v > (int16_t)num) { proc_goto(p, addr_gt); pc = p->pc; }
            else pc += 8;
            break;
        }
        case 68: { /* return：回到上一级脚本 */
            fmj_proc_t *prev = p->prev;
            p->prev = NULL;
            proc_free_chain(p);
            fmj_proc_cur = prev;
            p = prev;
            if (!p) return;
            return;
        }
        case 69: { /* timemsg：延时提示（按键跳过） */
            int tlen = fmj_cstr_len(code, pc + 2) + 1;
            p->op = op_msg_new(code + pc + 2);
            pc += 2 + tlen;
            break;
        }
        case 70: fmj_disable_save = true; break;
        case 71: fmj_disable_save = false; break;
        case 72: { /* gamesave */
            int r = fmj_game_save();
            static const uint8_t okmsg[] = {0xB4, 0xE6, 0xB5, 0xB5, 0xB3, 0xC9, 0xB9, 0xA6, 0}; /* 存档成功 */
            static const uint8_t failmsg[] = {0xB4, 0xE6, 0xB5, 0xB5, 0xCA, 0xA7, 0xB0, 0xDC, 0}; /* 存档失败 */
            p->op = op_msg_new(r == 0 ? okmsg : failmsg);
            break;
        }
        case 73: { /* seteventtimer */
            p->timer_event_id = fmj_u16(code, pc);
            p->timer_ticks = fmj_u16(code, pc + 2);
            pc += 4;
            break;
        }
        case 74: case 75: break; /* showpos 开关 */
        case 76: { /* setto：变量赋值 */
            int from = fmj_u16(code, pc);
            int to = fmj_u16(code, pc + 2);
            if (from < FMJ_VAR_MAX && to < FMJ_VAR_MAX) fmj_vars[to] = fmj_vars[from];
            pc += 4;
            break;
        }
        case 77: { /* testgoodsnum */
            int gtype = fmj_u16(code, pc);
            int gindex = fmj_u16(code, pc + 2);
            int num = fmj_u16(code, pc + 4);
            int addr_eq = fmj_u16(code, pc + 6);
            int addr_gt = fmj_u16(code, pc + 8);
            int have = fmj_goods_num(gtype, gindex);
            if (have == num) { proc_goto(p, addr_eq); pc = p->pc; }
            else if (have > num) { proc_goto(p, addr_gt); pc = p->pc; }
            else pc += 10;
            break;
        }
        case 78: case 79: pc += 2; break; /* fightmiss / armstoss：忽略 */
        default:
            /* 未知指令：终止脚本避免死循环 */
            p->running = false;
            return;
        }
        p->pc = pc;
    }
    if (p->pc >= len) p->running = false;
}
