/*
 * avg_script.c — 解析器实现：单遍扫描 + 弱校验（引用的标签/证据在
 * 解析期查表回填索引，运行期零字符串比较）。
 *
 * Implementation: single pass, back-fills label/ev indices during parsing
 * so runtime lookups are integer compares only.
 */
#include "avg_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

/* ---------- 词法工具 ---------- */

static void copy_token(char* dst, size_t cap, const char* src, size_t len) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static void set_error(avg_script_t* s, int line, const char* msg) {
    snprintf(s->error, sizeof(s->error), "L%d: %s", line, msg);
}

/* 读取一个带引号的字符串（吃掉两端引号，支持内部中文）。 */
static const char* read_quoted(const char* p, char* out, size_t cap, size_t* len) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return NULL;
    ++p;
    const char* start = p;
    while (*p && *p != '"') ++p;
    if (*p != '"') return NULL;
    size_t n = (size_t)(p - start);
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = 0;
    if (len) *len = n;
    return p + 1;
}

/* 读取一个裸 token（到空白/结尾）。 */
static const char* read_word(const char* p, char* out, size_t cap) {
    while (*p == ' ' || *p == '\t') ++p;
    const char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    size_t n = (size_t)(p - start);
    copy_token(out, cap, start, n);
    return p;
}

static const char* read_int(const char* p, int* out) {
    while (*p == ' ' || *p == '\t') ++p;
    *out = (int)strtol(p, (char**)&p, 10);
    return p;
}

/* ---------- 产物写入 ---------- */

static avg_cmd_t* new_cmd(avg_script_t* s, int line) {
    if (s->cmd_count >= s->cmd_cap) {
        set_error(s, line, "too many commands");
        return NULL;
    }
    avg_cmd_t* c = &s->cmds[s->cmd_count++];
    memset(c, 0, sizeof(*c));
    return c;
}

static void label_ref(avg_script_t* s, avg_cmd_t* c, const char* name, int line) {
    int idx = avg_script_label(s, name);
    if (idx < 0) {
        /* 前向引用：先登记名字，扫描结束后统一回填。 */
        if (s->label_count >= AVG_MAX_LABELS) { set_error(s, line, "too many labels"); c->b = -1; return; }
        idx = s->label_count;
        copy_token(s->labels[idx].name, sizeof(s->labels[0].name), name, strlen(name));
        s->labels[idx].cmd = -1;
        s->label_count++;
    }
    c->b = idx;   /* 跳转命令把目标存 b；label 命令自身由循环写 cmd */
}

/* ---------- 主解析循环 ---------- */

void avg_script_free(avg_script_t* s) {
    if (s && s->cmds) {
        heap_caps_free(s->cmds);
        s->cmds = NULL;
    }
    if (s) { s->cmd_count = 0; s->cmd_cap = 0; }
}

bool avg_script_parse(avg_script_t* s, const char* text, size_t len) {
    memset(s, 0, sizeof(*s));
    /* 按行数估算容量（≤AVG_MAX_CMDS）：case1 八百 KB 定长 → ~110KB 实量，
     * 剩余容量给资产/字体，推理游戏期间不再挤掉字体 open。 */
    {
        size_t lines = 1;
        for (size_t i = 0; i < len; ++i)
            if (text[i] == '\n') ++lines;
        int cap = (int)lines + 8;
        if (cap > AVG_MAX_CMDS) cap = AVG_MAX_CMDS;
        s->cmds = (avg_cmd_t*)heap_caps_malloc((size_t)cap * sizeof(avg_cmd_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->cmds) {
            snprintf(s->error, sizeof(s->error), "no psram for commands");
            return false;
        }
        s->cmd_cap = cap;
    }
    char line[AVG_MAX_TEXT * 2];
    const char* p = text;
    const char* end = text + len;
    int file_line = 0;          /* 真实文件行号（报错定位用，2026-10-18） */

    while (p < end) {
        /* 取一行。 */
        const char* eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        size_t n = (size_t)(eol - p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = eol + 1 < end || *eol == '\n' ? eol + 1 : end;
        int line_no = s->cmd_count + 1; /* 兼容旧字段：命令序号 */
        ++file_line;

        /* 去注释与左右空白。 */
        for (char* q = line; *q; ++q)
            if (q[0] == '/' && q[1] == '/') { *q = 0; break; }
        char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
        size_t tl = strlen(trimmed);
        while (tl && (trimmed[tl - 1] == ' ' || trimmed[tl - 1] == '\t' || trimmed[tl - 1] == '\r')) trimmed[--tl] = 0;
        if (!tl) continue;

        /* 拆首词。 */
        char kw[12] = {0};
        const char* q = read_word(trimmed, kw, sizeof(kw));
        if (!kw[0]) continue;

        if (!strcmp(kw, "case")) {
            char title[128];
            q = read_word(q, s->case_id, sizeof(s->case_id));
            q = read_quoted(q, title, sizeof(title), NULL);
            if (!q) { set_error(s, line_no, "case needs id \"title\""); return false; }
            copy_token(s->case_title, sizeof(s->case_title), title, strlen(title));
        } else if (!strcmp(kw, "voicemap")) {
            /* 元数据：说话人→立绘映射，不产生命令。同名重复=覆盖（分章换装）。 */
            char vname[64], vfile[128];
            q = read_quoted(q, vname, sizeof(vname), NULL);
            q = read_quoted(q, vfile, sizeof(vfile), NULL);
            if (!q || !vname[0] || !vfile[0]) {
                set_error(s, line_no, "voicemap needs \"说话人\" \"文件\""); return false;
            }
            int found = -1;
            for (int i = 0; i < s->voice_map_count; ++i)
                if (!strcmp(s->voice_maps[i].name, vname)) { found = i; break; }
            if (found < 0) {
                if (s->voice_map_count >= AVG_MAX_VOICE_MAP) {
                    set_error(s, line_no, "too many voicemap"); return false;
                }
                found = s->voice_map_count++;
                copy_token(s->voice_maps[found].name, sizeof(s->voice_maps[0].name),
                           vname, strlen(vname));
            }
            copy_token(s->voice_maps[found].file, sizeof(s->voice_maps[0].file),
                       vfile, strlen(vfile));
        } else if (!strcmp(kw, "label")) {
            char name[128];
            q = read_word(q, name, sizeof(name));
            if (!name[0]) { set_error(s, line_no, "label needs a name"); return false; }
            /* 若此前有前向引用占位，补真实位置。 */
            int idx = avg_script_label(s, name);
            if (idx < 0) {
                if (s->label_count >= AVG_MAX_LABELS) { set_error(s, line_no, "too many labels"); return false; }
                idx = s->label_count++;
                copy_token(s->labels[idx].name, sizeof(s->labels[0].name), name, strlen(name));
            } else if (s->labels[idx].cmd >= 0) {
                set_error(s, line_no, "duplicate label"); return false;
            }
            s->labels[idx].cmd = s->cmd_count;
        } else if (!strcmp(kw, "jump") || !strcmp(kw, "ifset") || !strcmp(kw, "ifclear")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = !strcmp(kw, "jump") ? AVG_CMD_JUMP : (!strcmp(kw, "ifset") ? AVG_CMD_IFSET : AVG_CMD_IFCLEAR);
            char flag[12] = {0}, name[128] = {0};
            q = read_word(q, flag, sizeof(flag));
            q = read_word(q, name, sizeof(name));
            if (!name[0]) {     /* jump 没有 flag 参数 */
                copy_token(c->s1, sizeof(c->s1), flag, strlen(flag));
                label_ref(s, c, flag, line_no);
            } else {
                copy_token(c->s1, sizeof(c->s1), flag, strlen(flag));
                label_ref(s, c, name, line_no);
            }
            if (!strcmp(kw, "jump")) { /* jump: 目标在 s1 转存到 b */ }
            else if (s->labels[c->b].name[0] == 0) { set_error(s, line_no, "missing branch target"); return false; }
        } else if (!strcmp(kw, "flag")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_FLAG;
            q = read_word(q, c->s1, sizeof(c->s1));
            if (!c->s1[0]) { set_error(s, line_no, "flag needs a name"); return false; }
        } else if (!strcmp(kw, "bg")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_BG;
            q = read_word(q, c->s1, sizeof(c->s1));
            if (!c->s1[0]) { set_error(s, line_no, "bg needs a file"); return false; }
        } else if (!strcmp(kw, "char")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_CHAR;
            int slot = 0;
            q = read_int(q, &slot);
            char file[128], side[4] = {0}, focus[4] = {0};
            q = read_word(q, file, sizeof(file));
            q = read_word(q, side, sizeof(side));
            q = read_word(q, focus, sizeof(focus));
            c->a = slot >= 0 && slot <= 2 ? slot : 1;
            /* 显式按容量拼接 char/<file>（避免 strncat 截断告警与溢出歧义）。 */
            {
                const char prefix[] = "char/";
                size_t pl = sizeof(prefix) - 1;
                size_t fl = strlen(file);
                if (pl + fl >= sizeof(c->s1)) fl = sizeof(c->s1) - 1 - pl;
                memcpy(c->s1, prefix, pl);
                memcpy(c->s1 + pl, file, fl);
                c->s1[pl + fl] = 0;
            }
            c->b = (side[0] == 'L') ? AVG_SIDE_L : (side[0] == 'R') ? AVG_SIDE_R : AVG_SIDE_C;
            if (focus[0] == '1') c->b |= 0x100;
            if (!file[0]) { set_error(s, line_no, "char needs a file"); return false; }
        } else if (!strcmp(kw, "char_off")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_CHAR_OFF;
            q = read_int(q, &c->a);
        } else if (!strcmp(kw, "say")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_SAY;
            q = read_quoted(q, c->s1, sizeof(c->s1), NULL);
            q = read_quoted(q, c->s2, sizeof(c->s2), NULL);
            if (!q) { set_error(s, line_no, "say needs \"speaker\" \"text\""); return false; }
        } else if (!strcmp(kw, "think")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_THINK;
            q = read_quoted(q, c->s2, sizeof(c->s2), NULL);
            if (!q) { set_error(s, line_no, "think needs \"text\""); return false; }
        } else if (!strcmp(kw, "ev")) {
            if (s->evidence_count >= AVG_MAX_EVIDENCE) { set_error(s, line_no, "too many evidence"); return false; }
            avg_evidence_t* e = &s->evidence[s->evidence_count];
            char name[48];
            q = read_word(q, e->id, sizeof(e->id));
            q = read_quoted(q, name, sizeof(name), NULL);
            q = read_quoted(q, e->desc, sizeof(e->desc), NULL);
            if (!q || !e->id[0]) { set_error(s, line_no, "ev needs id \"name\" \"desc\""); return false; }
            copy_token(e->name, sizeof(e->name), name, strlen(name));
            e->pic[0] = 0;
            {   /* 可选第 4 参数：证物图文件名（images/ev/ 下）。 */
                char pic[48];
                const char* q2 = read_quoted(q, pic, sizeof(pic), NULL);
                if (q2) { copy_token(e->pic, sizeof(e->pic), pic, strlen(pic)); q = q2; }
            }
            s->evidence_count++;
        } else if (!strcmp(kw, "gain_ev")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            char id[24];
            c->type = AVG_CMD_GAIN_EV;
            q = read_word(q, id, sizeof(id));
            if (!id[0]) { set_error(s, line_no, "gain_ev needs an id"); return false; }
            int idx = avg_script_evidence(s, id);
            if (idx < 0) { set_error(s, line_no, "gain_ev references unknown evidence"); return false; }
            copy_token(c->s2, sizeof(c->s2), id, strlen(id));
            c->b = idx;
        } else if (!strcmp(kw, "testimony")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_TESTIMONY;
            q = read_quoted(q, c->s2, sizeof(c->s2), NULL);
            if (!q) { set_error(s, line_no, "testimony needs \"title\""); return false; }
        } else if (!strcmp(kw, "stmt")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_STMT;
            q = read_quoted(q, c->s2, sizeof(c->s2), NULL);
            if (!q) { set_error(s, line_no, "stmt needs \"text\""); return false; }
        } else if (!strcmp(kw, "press") || !strcmp(kw, "clash")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = !strcmp(kw, "press") ? AVG_CMD_PRESS_JUMP : AVG_CMD_CLASH_JUMP;
            q = read_int(q, &c->a);
            char id[24] = {0}, name[128] = {0};
            if (c->type == AVG_CMD_CLASH_JUMP) q = read_word(q, id, sizeof(id));
            q = read_word(q, name, sizeof(name));
            if (!name[0]) { set_error(s, line_no, kw); return false; }
            if (c->type == AVG_CMD_CLASH_JUMP) c->b = avg_script_evidence(s, id);
            else label_ref(s, c, name, line_no);
            if (c->type == AVG_CMD_CLASH_JUMP) c->a = c->a; /* stmt index 在 a */
            /* press: label 已在 b；clash: 需要把 name 存起来留给运行期回填。 */
            if (c->type == AVG_CMD_CLASH_JUMP) {
                /* clash 的 label 暂存 s1，运行期进入证言时回填（标签可能后置声明）。 */
                copy_token(c->s1, sizeof(c->s1), name, strlen(name));
            }
        } else if (!strcmp(kw, "ask_ev")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_ASK_EV;
            char name[128];
            q = read_word(q, name, sizeof(name));
            if (!name[0]) { set_error(s, line_no, "ask_ev needs a wrong-target label"); return false; }
            label_ref(s, c, name, line_no);
        } else if (!strcmp(kw, "end")) {
            avg_cmd_t* c = new_cmd(s, line_no);
            if (!c) return false;
            c->type = AVG_CMD_END;
            q = read_int(q, &c->a);
            c->a = c->a ? 1 : 0;
        } else {
            /* 未知命令 = 脚本被污染/格式坏（2026-10-18 实锤：上游生成脚本
             * 尾部混入裸文本“AI生成”行 → cmd#92 报错误导排查一下午）。
             * 报错直接给文件行号+原行内容，不再让人猜“命令序号”。 */
            snprintf(s->error, sizeof(s->error),
                     "file line %d: unknown command \"%.24s\"", file_line, trimmed);
            goto fail;
        }
    }

    /* 前向标签收尾：位置仍为 -1 = 引用了不存在的标签。 */
    for (int i = 0; i < s->cmd_count; ++i) {
        avg_cmd_t* c = &s->cmds[i];
        if ((c->type == AVG_CMD_JUMP || c->type == AVG_CMD_IFSET ||
             c->type == AVG_CMD_IFCLEAR || c->type == AVG_CMD_ASK_EV) && c->b < 0) {
            set_error(s, i + 1, "branch to undefined label");
            goto fail;
        }
    }
    if (!s->case_id[0]) { set_error(s, 0, "missing case header"); goto fail; }
    /* 收缩到实量：估容与实际差值直接归还 PSRAM。 */
    if (s->cmd_count > 0 && s->cmd_count < s->cmd_cap) {
        avg_cmd_t* shrunk = (avg_cmd_t*)heap_caps_realloc(
            s->cmds, (size_t)s->cmd_count * sizeof(avg_cmd_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (shrunk) { s->cmds = shrunk; s->cmd_cap = s->cmd_count; }
        /* realloc 失败保留原容量块（仅多占几 KB，不构成错误）。 */
    }
    return true;

fail:
    avg_script_free(s);
    return false;
}

bool avg_script_load_file(avg_script_t* s, const char* path) {
    memset(s, 0, sizeof(*s));
    FILE* f = fopen(path, "rb");
    if (!f) { snprintf(s->error, sizeof(s->error), "cannot open %s", path); return false; }
    char* buf = NULL;
    size_t buf_len = 0, cap = 0;
    /* 脚本文本一次性读进 PSRAM（五案完整版可达 300KB，不入 DRAM；
     * 2026-10-18 扩容：随文件长度动态分配，上限 AVG_SCRIPT_MAX=384KB）。 */
    {
        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        fseek(f, 0, SEEK_SET);
        const long AVG_SCRIPT_MAX = 384L * 1024L;
        if (flen <= 0 || flen >= AVG_SCRIPT_MAX) {
            fclose(f);
            snprintf(s->error, sizeof(s->error), "script %s too big (%ldB)", path, flen);
            return false;
        }
        buf_len = (size_t)flen;
        buf = (char*)heap_caps_malloc(buf_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) { fclose(f); snprintf(s->error, sizeof(s->error), "no psram for %s", path); return false; }
        cap = buf_len + 1;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    int err = ferror(f);
    fclose(f);
    if (err) {
        heap_caps_free(buf);
        snprintf(s->error, sizeof(s->error), "read error %s", path);
        return false;
    }
    buf[n] = 0;
    bool ok = avg_script_parse(s, buf, n);
    heap_caps_free(buf);
    if (!ok) avg_script_free(s);   /* parse 已自清；此处防御重复调用安全 */
    return ok;
}

int avg_script_label(const avg_script_t* s, const char* name) {
    for (int i = 0; i < s->label_count; ++i)
        if (!strcmp(s->labels[i].name, name)) return i;
    return -1;
}

int avg_script_evidence(const avg_script_t* s, const char* id) {
    for (int i = 0; i < s->evidence_count; ++i)
        if (!strcmp(s->evidence[i].id, id)) return i;
    return -1;
}

const avg_voice_map_t* avg_script_voice(const avg_script_t* s, const char* name) {
    if (!s || !name || !name[0]) return NULL;
    for (int i = 0; i < s->voice_map_count; ++i)
        if (!strcmp(s->voice_maps[i].name, name)) return &s->voice_maps[i];
    return NULL;
}
