/*
 * avg_core.c — 状态机实现。关键不变式：
 *  1. pc 永远停在"下一个待执行命令"；say/think 停在自身（重入时重放打字机）。
 *  2. 场景命令（bg/char）即时改签名，资产装载由 tick 完成，全部就绪才单次
 *     回调 scene（墨水屏不做无意义刷屏）。
 *  3. 出示判定 = 查当前证言 clash 表；错误统一跳 ask_wrong_label。
 *  4. 到达 label 即自动存档（脚本作者用 label 划剧情节拍）。
 *  5. 读档 = 恢复 pc/flags/evidence + 从脚本头重放场景命令（跳过对话），
 *     保证背景与立绘签名与存档点一致。
 */
#include "avg_core.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "avg_beep.h"

/* 当前证言面板：一段证言同时只有一份，press/present 查它。 */
static avg_testimony_t s_panel;
static bool s_panel_valid;

/* 前置声明（读档入口在定义之前使用）。 */
static uint32_t run(avg_core_t* c);
static int flag_index(avg_core_t* c, const char* name);
static void begin_say(avg_core_t* c, int idx);
static void begin_testimony(avg_core_t* c, int idx);
static void goto_label(avg_core_t* c, int label_id);

/* ---------- 拷贝工具（显式按容量收敛，避免 format-truncation） ---------- */

static void copy_fit(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void path_join_fit(char* dst, size_t cap, const char* dir, const char* file) {
    if (!dir || !file || !file[0]) { if (dst && cap) dst[0] = 0; return; }
    size_t dl = strlen(dir);
    size_t fl = strlen(file);
    if (dl == 0 || dl + fl + 1 >= cap) { dst[0] = 0; return; }
    memcpy(dst, dir, dl);
    dst[dl] = '/';
    memcpy(dst + dl + 1, file, fl);
    dst[dl + 1 + fl] = 0;
}

/* ---------- 存档（行式文本，人工可读可改） ---------- */

#define SAVE_MAGIC "AVGSAVE1"

bool avg_core_save(avg_core_t* c) {
    if (!c || !c->save_path[0] || c->mode == AVG_MODE_TITLE) return false;
    /* 证言面板期存档：恢复点写到 TESTIMONY 命令本身（2026-10-18）——
     * 面板退出后的 pc 落在威慑子例程 label 上，无法重建面板。 */
    int save_pc = (c->mode == AVG_MODE_TESTIMONY && c->testimony_cmd >= 0)
                      ? c->testimony_cmd : c->st.pc;
    char tmp[192];
    copy_fit(tmp, sizeof(tmp), c->save_path);
    size_t tl = strlen(tmp);
    if (tl + 5 >= sizeof(tmp)) return false;
    memcpy(tmp + tl, ".tmp", 5);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = true;
    ok = ok && fprintf(f, "%s\n%s\n%d\n%d\n", SAVE_MAGIC, c->script->case_id,
                       save_pc, c->t_active) > 0;
    for (int i = 0; i < c->st.flag_count && ok; ++i)
        ok = fprintf(f, "f %s\n", c->st.flag_names[i]) > 0;
    for (int i = 0; i < c->st.known_count && ok; ++i)
        ok = fprintf(f, "e %d\n", c->st.known_evidence[i]) > 0;
    if (fclose(f)) ok = false;
    if (ok) {
        remove(c->save_path);
        ok = rename(tmp, c->save_path) == 0;
    } else {
        remove(tmp);
    }
    return ok;
}

bool avg_core_has_save(const avg_core_t* c) {
    FILE* f = fopen(c->save_path, "r");
    if (!f) return false;
    char magic[16] = {0}, id[32] = {0};
    int n = fscanf(f, "%15s %31s", magic, id);
    fclose(f);
    return n == 2 && !strcmp(magic, SAVE_MAGIC) && !strcmp(id, c->script->case_id);
}

bool avg_core_start_saved(avg_core_t* c) {
    FILE* f = fopen(c->save_path, "r");
    if (!f) return false;
    char magic[16] = {0}, id[32] = {0};
    int pc = 0, active = 0;
    if (fscanf(f, "%15s %31s %d %d", magic, id, &pc, &active) != 4 ||
        strcmp(magic, SAVE_MAGIC) || strcmp(id, c->script->case_id)) { fclose(f); return false; }
    char k[4], name[24];
    int idx;
    while (fscanf(f, "%3s", k) == 1) {
        if (!strcmp(k, "f")) {
            if (fscanf(f, "%23s", name) == 1) (void)flag_index(c, name);
        } else if (!strcmp(k, "e")) {
            if (fscanf(f, "%d", &idx) == 1 && idx >= 0 && idx < c->script->evidence_count &&
                c->st.known_count < AVG_MAX_EVIDENCE) {
                bool dup = false;
                for (int i = 0; i < c->st.known_count; ++i) dup = dup || c->st.known_evidence[i] == idx;
                if (!dup) c->st.known_evidence[c->st.known_count++] = idx;
            }
        }
    }
    fclose(f);
    if (pc < 0 || pc > c->script->cmd_count) return false;

    /* 重放场景：从脚本头执行到存档 pc，跳过一切展示，恢复 bg/char/ask_ev
     * 与剧情标记。证据列表以档为准（重放后覆盖）。 */
    c->replaying = true;
    c->replay_limit = pc;
    c->st.pc = 0;
    int known = c->st.known_count;
    c->st.known_count = 0;
    run(c);
    c->st.known_count = known;
    c->replay_limit = -1;
    c->replaying = false;
    c->st.pc = pc;
    c->testimony_cmd = -1;
    s_panel_valid = false;

    /* 恢复点搜索（2026-10-18 死锁/假死修复）：存档 pc 常停在 LABEL/子例程
     * 边界（label 存档不推进 pc；证言期存档指向 TESTIMONY），原实现只认
     * SAY/THINK → mode 留在 TITLE 回标题页假死。从 pc 起向后扫到首个
     * 可恢复命令：
     *   SAY/THINK  → begin_say 台词重入；
     *   TESTIMONY  → begin_testimony 重建面板（重放中面板被跳过未建）；
     *   其余静默命令（label/gain_ev/ask_ev…）直接跨过；
     * 扫到末尾=剧末死档 → 视为读档失败。 */
    int rp = pc;
    while (rp < c->script->cmd_count) {
        avg_cmd_type_t t = c->script->cmds[rp].type;
        if (t == AVG_CMD_SAY || t == AVG_CMD_THINK || t == AVG_CMD_TESTIMONY) break;
        ++rp;
    }
    if (rp >= c->script->cmd_count) return false;
    c->st.pc = rp;
    int saved_active = c->t_active;
    const avg_cmd_t* cmd = &c->script->cmds[rp];
    if (cmd->type == AVG_CMD_TESTIMONY) {
        begin_testimony(c, rp);
        /* 读档恢复：之前全程在玩，证言视为全部解锁。 */
        c->t_unlock = s_panel.count;
        /* 存档里记录的选中行优先还原（begin_testimony 内部可能在
         * 标题比对时重置 t_active）。 */
        if (saved_active >= 0 && saved_active < s_panel.count)
            c->t_active = saved_active;
    } else {
        begin_say(c, rp);
    }
    return true;
}

/* ---------- 剧情标记 ---------- */

static int flag_index(avg_core_t* c, const char* name) {
    for (int i = 0; i < c->st.flag_count; ++i)
        if (!strcmp(c->st.flag_names[i], name)) return i;
    if (c->st.flag_count >= AVG_MAX_FLAGS) return -1;
    copy_fit(c->st.flag_names[c->st.flag_count], 12, name);
    return c->st.flag_count++;
}

static bool flag_is_set(avg_core_t* c, const char* name) {
    for (int i = 0; i < c->st.flag_count; ++i)
        if (!strcmp(c->st.flag_names[i], name)) return true;
    return false;
}

/* ---------- 对话与证言 ---------- */

static void begin_say(avg_core_t* c, int idx) {
    const avg_cmd_t* cmd = &c->script->cmds[idx];
    c->mode = AVG_MODE_DIALOG;
    c->tw.text = cmd->s2;
    c->tw.shown = 0;
    c->tw.dirty = false;
    c->tw.last_ms = c->now_ms;
    c->cur_say = idx;
    c->ops->dialog(c->ops->ctx, cmd->type == AVG_CMD_SAY ? cmd->s1 : "", cmd->s2, 0);
}

static void begin_testimony(avg_core_t* c, int idx) {
    const avg_script_t* s = c->script;
    avg_testimony_t* t = &s_panel;
    memset(t, 0, sizeof(*t));
    copy_fit(t->title, sizeof(t->title), s->cmds[idx].s2);
    for (int i = 0; i < AVG_MAX_STMTS; ++i) {
        t->stmts[i].press_label = -1;
        t->stmts[i].clash_evidence = -1;
        t->stmts[i].clash_label = -1;
    }

    int i = idx + 1;
    while (i < s->cmd_count) {
        const avg_cmd_t* cmd = &s->cmds[i];
        if (cmd->type == AVG_CMD_STMT) {
            if (t->count < AVG_MAX_STMTS)
                copy_fit(t->stmts[t->count++].text, AVG_MAX_TEXT, cmd->s2);
        } else if (cmd->type == AVG_CMD_PRESS_JUMP) {
            int si = cmd->a - 1;
            if (si >= 0 && si < t->count) t->stmts[si].press_label = cmd->b;
        } else if (cmd->type == AVG_CMD_CLASH_JUMP) {
            int si = cmd->a - 1;
            if (si >= 0 && si < t->count && cmd->b >= 0) {
                t->stmts[si].clash_evidence = cmd->b;
                t->stmts[si].clash_label = avg_script_label(s, cmd->s1);
            }
        } else break;
        ++i;
    }
    s_panel_valid = true;
    c->testimony_cmd = idx;      /* 证言期存档的恢复点（2026-10-18） */
    c->st.pc = i;                /* 越过证言声明块 */
    c->mode = AVG_MODE_TESTIMONY;
    if (strcmp(c->last_testimony_title, t->title)) {
        copy_fit(c->last_testimony_title, sizeof(c->last_testimony_title), t->title);
        c->t_active = 0;
        c->t_unlock = 1;         /* 新证言：只给第 1 条（逐条解锁 2026-10-18） */
    }
    if (c->t_unlock < 1) c->t_unlock = 1;
    if (c->t_unlock > t->count) c->t_unlock = t->count;
    if (c->t_active >= t->count) c->t_active = t->count > 0 ? t->count - 1 : 0;
    if (c->t_active >= c->t_unlock) c->t_active = c->t_unlock - 1;
    if (!c->replaying) c->ops->testimony(c->ops->ctx, t, c->t_active);
}

static void goto_label(avg_core_t* c, int label_id) {
    if (label_id >= 0 && label_id < c->script->label_count)
        c->st.pc = c->script->labels[label_id].cmd;
}

/* ---------- 命令执行 ---------- */

/* 场景签名变化：置待载标记。-1 通知下一 tick 重算张数并归零游标
 * （2026-10-18：上场景遗留的 step/done 不能带到新场景）。 */
static void mark_assets_pending(avg_core_t* c) {
    c->assets_pending = 1;
    c->asset_total = -1;
}

static uint32_t run(avg_core_t* c) {
    const avg_script_t* s = c->script;
    uint32_t dirty = AVG_DIRTY_NONE;
    while (c->st.pc < s->cmd_count &&
           (!c->replaying || c->replay_limit < 0 || c->st.pc < c->replay_limit)) {
        const avg_cmd_t* cmd = &s->cmds[c->st.pc];
        switch (cmd->type) {
        case AVG_CMD_CASE:
            ++c->st.pc;
            break;
        case AVG_CMD_LABEL:
            if (!c->replaying) avg_core_save(c);
            ++c->st.pc;
            break;
        case AVG_CMD_JUMP: {
            int target = s->labels[cmd->b].cmd;
            /* 重放禁回跳（2026-10-18 死锁修复）：威慑/错误子例程以
             * 「jump 回证言 label」收尾（如 jump t1），线性重放若执行
             * 它会在子例程↔证言间死循环挂死 UI 任务。重放只为恢复
             * bg/char/flag 状态，回跳属于交互流，一律跳过前进。 */
            if (c->replaying && target <= c->st.pc) ++c->st.pc;
            else c->st.pc = target;
            break;
        }
        case AVG_CMD_IFSET: {
            ++c->st.pc;
            if (flag_is_set(c, cmd->s1)) {
                int target = s->labels[cmd->b].cmd;
                if (!(c->replaying && target <= c->st.pc)) c->st.pc = target;
            }
            break;
        }
        case AVG_CMD_IFCLEAR: {
            ++c->st.pc;
            if (!flag_is_set(c, cmd->s1)) {
                int target = s->labels[cmd->b].cmd;
                if (!(c->replaying && target <= c->st.pc)) c->st.pc = target;
            }
            break;
        }
        case AVG_CMD_FLAG:
            (void)flag_index(c, cmd->s1);
            ++c->st.pc;
            break;
        case AVG_CMD_BG:
            copy_fit(c->bg_file, sizeof(c->bg_file), cmd->s1);
            mark_assets_pending(c);
            ++c->st.pc;
            dirty |= AVG_DIRTY_SCENE;
            break;
        case AVG_CMD_CHAR: {
            int slot = cmd->a;
            if (slot < 0 || slot > 2) slot = 0;
            copy_fit(c->char_files[slot], sizeof(c->char_files[0]), cmd->s1);
            c->char_side[slot] = (avg_side_t)(cmd->b & 0xff);
            if (cmd->b & 0x100) c->focus_slot = slot;
            mark_assets_pending(c);
            ++c->st.pc;
            dirty |= AVG_DIRTY_SCENE;
            break;
        }
        case AVG_CMD_CHAR_OFF:
            if (cmd->a >= 0 && cmd->a <= 2) c->char_files[cmd->a][0] = 0;
            mark_assets_pending(c);
            ++c->st.pc;
            dirty |= AVG_DIRTY_SCENE;
            break;
        case AVG_CMD_GAIN_EV: {
            bool dup = false;
            for (int i = 0; i < c->st.known_count; ++i) dup = dup || c->st.known_evidence[i] == cmd->b;
            if (!dup && c->st.known_count < AVG_MAX_EVIDENCE)
                c->st.known_evidence[c->st.known_count++] = cmd->b;
            ++c->st.pc;
            break;
        }
        case AVG_CMD_SAY:
        case AVG_CMD_THINK:
            if (c->replaying) { ++c->st.pc; break; }
            /* voicemap 自动跟随（2026-10-23 用户：立绘跟着说话人切换）：
             * SAY 说话人在映射表内且与当前 R 槽不同 → 原地换立绘并报
             * DIRTY_CHAR（平台层做场景区局部刷新；同说话人零开销）。 */
            if (cmd->type == AVG_CMD_SAY) {
                const avg_voice_map_t* vm = avg_script_voice(c->script, cmd->s1);
                if (vm && strcmp(vm->file, c->char_files[2]) != 0) {
                    copy_fit(c->char_files[2], sizeof(c->char_files[0]), vm->file);
                    c->char_side[2] = AVG_SIDE_R;
                    dirty |= AVG_DIRTY_CHAR;
                }
            }
            begin_say(c, c->st.pc);
            return dirty | AVG_DIRTY_DIALOG;
        case AVG_CMD_TESTIMONY:
            if (c->replaying) { ++c->st.pc; break; }
            begin_testimony(c, c->st.pc);
            avg_core_save(c);
            return AVG_DIRTY_UI;
        case AVG_CMD_PRESS_JUMP:
        case AVG_CMD_CLASH_JUMP:
        case AVG_CMD_STMT:
            ++c->st.pc;   /* 已并入证言面板；顺序流不会消费它们 */
            break;
        case AVG_CMD_ASK_EV:
            c->ask_wrong_label = cmd->b;
            ++c->st.pc;
            break;
        case AVG_CMD_END:
            if (c->replaying) { ++c->st.pc; break; }
            c->mode = AVG_MODE_VERDICT;
            if (cmd->a) remove(c->save_path);
            avg_beep_play(cmd->a ? AVG_SFX_EV_OK : AVG_SFX_EV_FAIL);
            c->ops->verdict(c->ops->ctx, cmd->a);
            return AVG_DIRTY_UI;
        }
    }
    return dirty;
}

/* ---------- 生命周期 ---------- */

bool avg_core_init(avg_core_t* c, const avg_script_t* s, const avg_render_ops_t* ops,
                   avg_assets_t* assets, const char* case_dir) {
    if (!c || !s || !ops || !assets || !case_dir || !case_dir[0]) return false;
    memset(c, 0, sizeof(*c));
    c->script = s;
    c->ops = ops;
    c->assets = assets;
    copy_fit(c->case_dir, sizeof(c->case_dir), case_dir);
    path_join_fit(c->asset_dir, sizeof(c->asset_dir), c->case_dir, "images");
    path_join_fit(c->save_path, sizeof(c->save_path), c->case_dir, "save.txt");
    c->ask_wrong_label = -1;
    c->focus_slot = -1;
    c->cur_say = -1;
    c->testimony_cmd = -1;
    for (int i = 0; i < 3; ++i) c->char_side[i] = AVG_SIDE_C;
    s_panel_valid = false;
    return true;
}

bool avg_core_ev_known(const avg_core_t* c, int ev_index) {
    if (!c || ev_index < 0 || ev_index >= c->script->evidence_count) return false;
    for (int i = 0; i < c->st.known_count; ++i)
        if (c->st.known_evidence[i] == ev_index) return true;
    return false;
}

void avg_core_show_title(avg_core_t* c) {
    c->mode = AVG_MODE_TITLE;
    s_panel_valid = false;
    c->ops->title(c->ops->ctx, c->script->case_title, avg_core_has_save(c));
}

bool avg_core_start_fresh(avg_core_t* c) {
    memset(&c->st, 0, sizeof(c->st));
    c->st.pc = 0;
    c->t_active = 0;
    c->testimony_cmd = -1;
    c->last_testimony_title[0] = 0;
    c->ask_wrong_label = -1;
    c->bg_file[0] = 0;
    for (int i = 0; i < 3; ++i) c->char_files[i][0] = 0;
    c->focus_slot = -1;
    s_panel_valid = false;
    avg_assets_release(c->assets);
    return run(c) != 0 || c->st.pc < c->script->cmd_count;
}

void avg_core_shutdown(avg_core_t* c) {
    if (c && c->assets) avg_assets_release(c->assets);
}

/* ---------- tick：资产装载 + 打字机 ---------- */

static const char* utf8_next(const char* p) {
    unsigned char c0 = (unsigned char)*p;
    if (!c0) return p;
    int n = 1;
    if ((c0 & 0xe0) == 0xc0) n = 2;
    else if ((c0 & 0xf0) == 0xe0) n = 3;
    else if ((c0 & 0xf8) == 0xf0) n = 4;
    for (int i = 1; i < n; ++i)
        if ((p[i] & 0xc0) != 0x80) { n = 1; break; }
    return p + n;
}

static uint32_t tick_typewriter(avg_core_t* c) {
    if (c->mode != AVG_MODE_DIALOG || !c->tw.text) return 0;
    if (!c->tw.text[c->tw.shown]) return 0;
    uint32_t dirty = 0;
    int64_t since = c->now_ms - c->tw.last_ms;
    while (since >= 90 && c->tw.text[c->tw.shown]) {
        const char* next = utf8_next(c->tw.text + c->tw.shown);
        c->tw.shown = (int)(next - c->tw.text);
        c->tw.last_ms += 90;
        since -= 90;
        dirty = AVG_DIRTY_TEXT;
    }
    if (dirty) c->ops->dialog(c->ops->ctx,
                              c->script->cmds[c->cur_say].type == AVG_CMD_SAY
                                  ? c->script->cmds[c->cur_say].s1 : "",
                              c->tw.text, c->tw.shown);
    return dirty;
}

/* 资产装载（2026-10-18 v3：加载期间零刷屏）。
 * 每 tick 至多解码一张（分片阻塞：喂狗间隙+长按退出可响应）；未全部
 * 就绪时返回 NONE——平台保持上一画面（点击时的“加载中”反馈由平台
 * 在 input 阶段小区域快刷完成，参考 GBA 原版 NOW LOADING 式只提示
 * 不做进度条）。游标走完（step>3）即完成：失败的槽不阻塞、以 NULL
 * 占位。全部就绪单次回调 scene，墨水屏不做无意义刷屏。 */
static void scene_set_expected(avg_core_t* c) {
    int total = c->bg_file[0] ? 1 : 0;
    for (int i = 0; i < 3; ++i)
        if (c->char_files[i][0]) ++total;
    c->asset_total = total;
    c->asset_done = 0;
    c->asset_step = 0;
}

/* 就绪回调：从缓存重取全部指针（本 tick 刚载入都在，为失败槽保 NULL）。 */
static void scene_emit(avg_core_t* c) {
    char path[288];
    unsigned bw = 0, bh = 0;
    const uint8_t* bgp = NULL;
    const uint8_t* bg_m = NULL;
    if (c->bg_file[0]) {
        path_join_fit(path, sizeof(path), c->asset_dir, c->bg_file);
        bgp = avg_assets_get(c->assets, path, 684, 830, &bw, &bh, c->now_ms, &bg_m);
    }
    const uint8_t* ch[3] = {NULL, NULL, NULL};
    const uint8_t* ch_m[3] = {NULL, NULL, NULL};
    unsigned ch_w[3] = {0, 0, 0}, ch_h[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        if (!c->char_files[k][0]) continue;
        path_join_fit(path, sizeof(path), c->asset_dir, c->char_files[k]);
        ch[k] = avg_assets_get(c->assets, path, 470, 640, &ch_w[k], &ch_h[k], c->now_ms, &ch_m[k]);
    }
    c->ops->scene(c->ops->ctx, bgp, bw, bh, ch, ch_w, ch_h, ch_m, c->char_side, c->focus_slot);
}

static uint32_t tick_assets(avg_core_t* c) {
    if (!c->assets_pending) return 0;
    if (c->asset_total < 0) scene_set_expected(c);
    if (c->asset_total == 0) { c->assets_pending = 0; return AVG_DIRTY_SCENE; }

    /* 空槽零成本连跳，直到第一个实槽（单图场景不多耗空 tick）。 */
    while (c->asset_step <= 3) {
        if (c->asset_step == 0) { if (!c->bg_file[0]) { ++c->asset_step; continue; } break; }
        if (!c->char_files[c->asset_step - 1][0]) { ++c->asset_step; continue; }
        break;
    }
    if (c->asset_step > 3) {          /* 全空/全失败：以占位直接就绪 */
        scene_emit(c);
        c->assets_pending = 0;
        return AVG_DIRTY_SCENE;
    }

    /* 本 tick 载一张：索引 0 = bg，1..3 = char slot 0..2。
     * 蒙版一同入槽（解码器填）；scene_emit 就绪后统一取齐再下发。 */
    char path[288];
    unsigned w = 0, h = 0;
    bool loaded = false;
    if (c->asset_step == 0) {
        path_join_fit(path, sizeof(path), c->asset_dir, c->bg_file);
        loaded = avg_assets_get(c->assets, path, 684, 830, &w, &h, c->now_ms, NULL) != NULL;
    } else {
        int slot = c->asset_step - 1;
        path_join_fit(path, sizeof(path), c->asset_dir, c->char_files[slot]);
        loaded = avg_assets_get(c->assets, path, 470, 640, &w, &h, c->now_ms, NULL) != NULL;
    }
    ++c->asset_step;
    if (loaded) ++c->asset_done;

    if (c->asset_step > 3) {          /* 游标走完：一次到位 */
        scene_emit(c);
        c->assets_pending = 0;
        return AVG_DIRTY_SCENE;
    }
    return AVG_DIRTY_NONE;            /* 装载中：不刷屏，屏上保持加载反馈 */
}

uint32_t avg_core_tick(avg_core_t* c, int64_t now_ms) {
    c->now_ms = now_ms;
    uint32_t dirty = tick_assets(c);
    dirty |= tick_typewriter(c);
    return dirty;
}

/* ---------- 输入 ---------- */

bool avg_core_wants_evidence(const avg_core_t* c) {
    return c->mode == AVG_MODE_TESTIMONY || c->ask_wrong_label >= 0;
}

static uint32_t input_tap(avg_core_t* c) {
    if (c->mode != AVG_MODE_DIALOG) return 0;
    if (c->tw.text && c->tw.text[c->tw.shown]) {
        c->tw.shown = (int)strlen(c->tw.text);
        const avg_cmd_t* cmd = &c->script->cmds[c->cur_say];
        c->ops->dialog(c->ops->ctx, cmd->type == AVG_CMD_SAY ? cmd->s1 : "", c->tw.text, c->tw.shown);
        return AVG_DIRTY_DIALOG;
    }
    ++c->st.pc;
    return run(c) | AVG_DIRTY_DIALOG;
}

static uint32_t press_stmt(avg_core_t* c, int idx0) {
    if (c->mode != AVG_MODE_TESTIMONY || !s_panel_valid) return 0;
    if (idx0 < 0 || idx0 >= s_panel.count) return 0;
    /* 逐条解锁（2026-10-18）：只响应已解锁行；威慑/查看第 i 条视为
     * 「达成」→ 解锁到 i+2（下一条出现）。clash 判定同门（present_evidence
     * 只匹配已解锁行）。 */
    if (idx0 >= c->t_unlock) {
        avg_beep_play(AVG_SFX_SELECT);
        return AVG_DIRTY_NONE;
    }
    c->t_active = idx0;
    if (idx0 + 2 > c->t_unlock && idx0 + 2 <= s_panel.count)
        c->t_unlock = idx0 + 2;
    const avg_stmt_t* st = &s_panel.stmts[idx0];
    if (st->press_label >= 0) {
        avg_beep_play(AVG_SFX_PRESS);
        goto_label(c, st->press_label);
        return run(c) | AVG_DIRTY_DIALOG | AVG_DIRTY_UI;
    }
    avg_beep_play(AVG_SFX_SELECT);
    return AVG_DIRTY_NONE;
}

static uint32_t present_evidence(avg_core_t* c, int ev_index) {
    if (ev_index < 0 || ev_index >= c->script->evidence_count) return 0;
    if (!avg_core_ev_known(c, ev_index)) return 0;
    if (s_panel_valid || c->ask_wrong_label >= 0) {
        /* 解锁门只限自由威慑态（2026-10-18）：
         *  - ask 态（剧情强制提示出示）匹配全表——否则玩家按提示出示
         *    必被 t_unlock 门挡住匹配失败，只有失败音没有异议画面
         *    （case1 t2: clash 目标第 2 条，进面板 t_unlock=1）；
         *  - 证言面板态只匹配已解锁行。 */
        int hit = -1;
        for (int i = 0; i < s_panel.count; ++i) {
            if (s_panel_valid && c->ask_wrong_label < 0 && i >= c->t_unlock) break;
            const avg_stmt_t* st = &s_panel.stmts[i];
            if (st->clash_evidence == ev_index && st->clash_label >= 0) {
                hit = i;
                break;
            }
        }
        if (hit >= 0) {
            avg_beep_play(AVG_SFX_OBJECTION);
            ESP_LOGI("avg_core", "clash hit stmt=%d ev=%d label=%d (unlock=%d ask=%d)",
                     hit, ev_index, s_panel.stmts[hit].clash_label, c->t_unlock,
                     c->ask_wrong_label >= 0);
            int label = s_panel.stmts[hit].clash_label;
            c->ask_wrong_label = -1;
            goto_label(c, label);
            return run(c) | AVG_DIRTY_DIALOG | AVG_DIRTY_UI;
        }
        if (c->ask_wrong_label >= 0) {
            avg_beep_play(AVG_SFX_EV_FAIL);
            int wrong = c->ask_wrong_label;
            c->ask_wrong_label = -1;
            goto_label(c, wrong);
            return run(c) | AVG_DIRTY_DIALOG | AVG_DIRTY_UI;
        }
        avg_beep_play(AVG_SFX_EV_FAIL);
        c->mode = AVG_MODE_TESTIMONY;
        c->ops->testimony(c->ops->ctx, &s_panel, c->t_active);
        return AVG_DIRTY_UI;
    }
    return 0;
}

uint32_t avg_core_input(avg_core_t* c, avg_input_t in, int arg) {
    switch (in) {
    case AVG_INPUT_TAP:
        return input_tap(c);
    case AVG_INPUT_STMT:
        return press_stmt(c, arg);
    case AVG_INPUT_OPEN_EVIDENCE: {
        if (c->mode == AVG_MODE_EVIDENCE) return 0;
        bool share = c->ask_wrong_label >= 0 || s_panel_valid;
        c->mode = AVG_MODE_EVIDENCE;
        avg_beep_play(AVG_SFX_SELECT);
        c->ops->evidence(c->ops->ctx, c->script->evidence, c->script->evidence_count,
                         0, false, share);
        return AVG_DIRTY_UI;
    }
    case AVG_INPUT_CLOSE_EVIDENCE: {
        if (c->mode != AVG_MODE_EVIDENCE) return 0;
        if (s_panel_valid) {
            c->mode = AVG_MODE_TESTIMONY;
            c->ops->testimony(c->ops->ctx, &s_panel, c->t_active);
        } else {
            c->mode = AVG_MODE_DIALOG;
            if (c->cur_say >= 0 && c->cur_say < c->script->cmd_count) {
                const avg_cmd_t* cmd = &c->script->cmds[c->cur_say];
                c->ops->dialog(c->ops->ctx,
                               cmd->type == AVG_CMD_SAY ? cmd->s1 : "",
                               c->tw.text ? c->tw.text : "", c->tw.shown);
            }
        }
        return AVG_DIRTY_UI;
    }
    case AVG_INPUT_PICK_EVIDENCE:
        if (c->mode != AVG_MODE_EVIDENCE) return 0;
        if (arg < 0 || arg >= c->script->evidence_count) return 0;
        if (!avg_core_ev_known(c, arg)) return 0;
        avg_beep_play(AVG_SFX_SELECT);
        c->ops->evidence(c->ops->ctx, c->script->evidence, c->script->evidence_count,
                         arg, true, c->ask_wrong_label >= 0 || s_panel_valid);
        return AVG_DIRTY_UI;
    case AVG_INPUT_PRESENT_EVIDENCE:
        if (c->mode != AVG_MODE_EVIDENCE) return 0;
        return present_evidence(c, arg);
    case AVG_INPUT_START:
        if (c->mode != AVG_MODE_TITLE) return 0;
        if (!avg_core_start_fresh(c)) return 0;
        return AVG_DIRTY_UI | AVG_DIRTY_SCENE | AVG_DIRTY_DIALOG;
    case AVG_INPUT_CONTINUE:
        if (c->mode != AVG_MODE_TITLE) return 0;
        if (!avg_core_start_saved(c)) return 0;
        return AVG_DIRTY_UI | AVG_DIRTY_SCENE | AVG_DIRTY_DIALOG;
    }
    return 0;
}
