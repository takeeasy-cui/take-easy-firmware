/*
 * story_kernel.c — Story Engine v1 共用内核实现。
 *
 * 见 story_kernel.h 头注释（语法与语义）。实现要点：
 *  - 脚本整读 PSRAM，行访问就地截断（text 可变内存，沿 gtext 做法）；
 *  - label 表 load 时一次扫描，跳转 O(表长)；
 *  - 未知 * 命令：串口一行日志后继续（宽容，试玩半成品包是常态）；
 *  - 变量/标志未声明即建槽，读未声明变量=0（容忍脚本试错）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "story_kernel.h"

/* ===================== 行访问与命令匹配 ===================== */

static char* line_at(sk_state_t* sk, int index) {
    if (index < 0 || index >= sk->lines) return NULL;
    return sk->text + sk->offs[index];       /* 偏移表 O(1)：就地截断不影响寻址 */
}

/* 全词匹配：行以 "* cmd" 开头且 cmd 后是空白或行尾（防 "* ifgoto" 误配 "* if"）。
 * 返回 true 时 arg 指向参数（已跳前导空格，行尾已就地截断）。 */
static bool match(char* l, const char* cmd, char** arg) {
    size_t n = strlen(cmd);
    if (strncmp(l, cmd, n) != 0) return false;
    char c = l[n];
    if (c != ' ' && c != '\n' && c != 0) return false;
    char* a = l + n;
    while (*a == ' ') ++a;
    char* nl = strchr(a, '\n');
    if (nl) *nl = 0;                       /* 就地截断（offs 表寻址不受影响） */
    char* r = strchr(a, '\r');
    if (r) *r = 0;
    *arg = a;
    return true;
}

static void copy_trim(char* dst, size_t cap, const char* src, size_t n) {
    size_t take = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, take);
    dst[take] = 0;
}

/* ===================== 变量 / 标志 / label ===================== */

int sk_var_get(const sk_state_t* sk, const char* name) {
    for (int i = 0; i < sk->vcount; ++i)
        if (!strncmp(sk->vnames[i], name, SK_NAME_MAX)) return sk->vvals[i];
    return 0;
}

void sk_var_set(sk_state_t* sk, const char* name, int16_t val) {
    for (int i = 0; i < sk->vcount; ++i)
        if (!strncmp(sk->vnames[i], name, SK_NAME_MAX)) { sk->vvals[i] = val; return; }
    if (sk->vcount >= SK_MAX_VARS) return;              /* 满：丢弃（日志由 step 打） */
    copy_trim(sk->vnames[sk->vcount], SK_NAME_MAX, name, strlen(name));
    sk->vvals[sk->vcount] = val;
    ++sk->vcount;
}

bool sk_flag_has(const sk_state_t* sk, const char* name) {
    for (int i = 0; i < sk->fcount; ++i)
        if (!strncmp(sk->fnames[i], name, SK_NAME_MAX)) return true;
    return false;
}

void sk_flag_set(sk_state_t* sk, const char* name) {
    if (sk_flag_has(sk, name)) return;
    if (sk->fcount >= SK_MAX_FLAGS) return;
    copy_trim(sk->fnames[sk->fcount], SK_NAME_MAX, name, strlen(name));
    ++sk->fcount;
}

int sk_find_label(const sk_state_t* sk, const char* name) {
    for (int i = 0; i < sk->lcount; ++i)
        if (!strncmp(sk->labels[i].name, name, SK_NAME_MAX)) return sk->labels[i].line;
    return -1;
}

static void goto_label(sk_state_t* sk, const char* name) {
    int t = sk_find_label(sk, name);
    if (t >= 0) sk->pc = t;
    else printf("SK: label not found '%s'\n", name);
}

/* 操作数：纯数字取字面，否则查变量（未声明=0）。 */
static int parse_val(const sk_state_t* sk, const char* tok) {
    if (!*tok) return 0;
    char* end = NULL;
    long v = strtol(tok, &end, 10);
    if (end && *end == 0) return (int)v;
    return sk_var_get(sk, tok);
}

static bool cmp_op(int l, const char* op, int r) {
    if (!strcmp(op, "==")) return l == r;
    if (!strcmp(op, "!=")) return l != r;
    if (!strcmp(op, ">"))  return l > r;
    if (!strcmp(op, "<"))  return l < r;
    if (!strcmp(op, ">=")) return l >= r;
    if (!strcmp(op, "<=")) return l <= r;
    return false;
}

/* ===================== 复合条件（cchoice/iftext/ifc 共用，2026-10-30 檐下谣） =====================
 * 条件串：原子 ‘+’ 连接（全部满足才过）。原子形如
 *   f:名            标志已置
 *   !f:名           标志未置
 *   v:名OP值         整数变量比较（OP ∈ ==,!=,>=,<=,>,<）
 * 空条件串由调用方处理；原子不认识 → 判不过（保守）。 */
static bool eval_cond(const sk_state_t* sk, const char* cond) {
    char work[192];
    if (strlen(cond) >= sizeof(work)) return false;
    strcpy(work, cond);
    char* save = NULL;
    bool any = false;
    for (char* at = strtok_r(work, "+", &save); at; at = strtok_r(NULL, "+", &save)) {
        any = true;
        while (*at == ' ') ++at;
        if (at[0] == '!' && at[1] == 'f' && at[2] == ':') {
            if (sk_flag_has(sk, at + 3)) return false;
        } else if (at[0] == 'f' && at[1] == ':') {
            if (!sk_flag_has(sk, at + 2)) return false;
        } else if (at[0] == 'v' && at[1] == ':') {
            const char* p = at + 2, * q = p;
            while (*q && *q != '=' && *q != '!' && *q != '>' && *q != '<') ++q;
            if (!*q || q == p) return false;
            size_t nn = (size_t)(q - p);
            char name[SK_NAME_MAX];
            if (nn >= SK_NAME_MAX) return false;
            memcpy(name, p, nn); name[nn] = 0;
            int oplen = 1;
            if ((q[0] == '=' && q[1] == '=') || q[0] == '!' ||
                ((q[0] == '>' || q[0] == '<') && q[1] == '=')) oplen = 2;
            char op[3] = {q[0], (char)(oplen == 2 ? q[1] : 0), 0};
            if (!cmp_op(sk_var_get(sk, name), op, atoi(q + oplen))) return false;
        } else {
            return false;
        }
    }
    return any;
}

/* ===================== 加载 / 释放 ===================== */

bool sk_load(sk_state_t* sk, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > SK_SCRIPT_MAX) { fclose(f); return false; }
    sk->text = (char*)heap_caps_malloc((size_t)n + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sk->text) { fclose(f); return false; }
    size_t got = fread(sk->text, 1, (size_t)n, f);
    fclose(f);
    sk->text[got] = '\n';
    sk->text[got + 1] = 0;
    sk->len = got + 1;
    sk->lines = 0;
    for (size_t i = 0; i < sk->len; ++i)
        if (sk->text[i] == '\n') ++sk->lines;
    /* 行偏移表（O(1) 行访问；就地截断换行不会破坏寻址——根治 gtext 旧 O(n) 走链错位）。 */
    sk->offs = (int32_t*)heap_caps_calloc((size_t)sk->lines + 1, sizeof(int32_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sk->offs) {
        heap_caps_free(sk->text);
        sk->text = NULL;
        return false;
    }
    {
        int li = 0;
        sk->offs[li++] = 0;
        for (size_t i = 0; i < sk->len; ++i)
            if (sk->text[i] == '\n' && li < sk->lines) sk->offs[li++] = (int32_t)(i + 1);
    }
    /* label 表一次扫描（就地截断行尾，偏移表不受影响）。 */
    sk->lcount = 0;
    for (int i = 0; i < sk->lines && sk->lcount < SK_MAX_LABELS; ++i) {
        char* l = line_at(sk, i);
        char* arg;
        if (l && match(l, "* label", &arg) && *arg)
            copy_trim(sk->labels[sk->lcount].name, SK_NAME_MAX, arg, strlen(arg)),
            sk->labels[sk->lcount++].line = i;
    }
    sk->pc = 0;
    return true;
}

void sk_free(sk_state_t* sk) {
    if (sk->text) heap_caps_free(sk->text);
    if (sk->offs) heap_caps_free(sk->offs);
    sk->text = NULL;
    sk->offs = NULL;
    sk->len = 0;
    sk->lines = 0;
    sk->pc = 0;
    sk->lcount = 0;
    sk->vcount = 0;
    sk->fcount = 0;
    sk->title[0] = 0;
    sk->bg[0] = 0;
    sk->bg_dirty = false;
    sk->char_dirty = false;
    sk->st_count = 0;
    sk->status[0] = 0;
    sk->status_dirty = false;
    sk->last_name[0] = 0;
}

/* ===================== set / 条件跳 ===================== */

static void rebuild_status(sk_state_t* sk);   /* 前向：exec_set 联动国势栏 */

static void exec_set(sk_state_t* sk, char* arg) {
    char* eq = strchr(arg, '=');
    if (!eq) {                                      /* 旧式：* set 标志 */
        if (*arg) sk_flag_set(sk, arg);
        return;
    }
    *eq = 0;
    char name[SK_NAME_MAX];
    copy_trim(name, sizeof(name), arg, (size_t)(eq - arg));
    char* rhs = eq + 1;
    while (*rhs == ' ') ++rhs;
    char t1[32] = {0}, t2[32] = {0};
    char op = 0;
    int fields = sscanf(rhs, "%31s %c %31s", t1, &op, t2);
    int v;
    if (fields >= 1) {
        v = parse_val(sk, t1);
        if (fields == 3) {
            int r = parse_val(sk, t2);
            if (op == '+') v += r;
            else if (op == '-') v -= r;
            else if (op == '*') v *= r;
            else if (op == '/') v = r ? v / r : 0;
        }
    } else v = 0;
    sk_var_set(sk, name, (int16_t)v);
    rebuild_status(sk);                       /* 国势栏随变量联动（未声明=no-op） */
}

static void exec_ifgoto(sk_state_t* sk, char* arg, bool invert) {
    char name[SK_NAME_MAX] = {0}, op[8] = {0}, val[32] = {0}, target[SK_NAME_MAX] = {0};
    if (sscanf(arg, "%23s %7s %31s %23s", name, op, val, target) != 4) return;
    bool hit = cmp_op(sk_var_get(sk, name), op, parse_val(sk, val));
    if (hit != invert) goto_label(sk, target);
}

static void exec_if_flag(sk_state_t* sk, char* arg, bool invert) {
    /* 旧语法：* if 标志 g 目标（'g' 字面分隔）。 */
    char name[SK_NAME_MAX] = {0}, g[4] = {0}, target[SK_NAME_MAX] = {0};
    if (sscanf(arg, "%23s %3s %23s", name, g, target) != 3) return;
    bool hit = sk_flag_has(sk, name);
    if ((hit && !invert) || (!hit && invert)) goto_label(sk, target);
}

/* ===================== 选项 ===================== */

/* 国势栏重展开（* status 声明后，任何 * set 都会刷 status[]；未声明无开销）。 */
static void rebuild_status(sk_state_t* sk) {
    if (sk->st_count <= 0) return;
    size_t n = 0;
    for (int i = 0; i < sk->st_count; ++i) {
        if (i && n + 1 < sizeof(sk->status)) sk->status[n++] = ' ';
        n += (size_t)snprintf(sk->status + n, sizeof(sk->status) - n, "%s%d",
                              sk->st_label[i], sk_var_get(sk, sk->st_var[i]));
        if (n >= sizeof(sk->status) - 1) { n = sizeof(sk->status) - 1; break; }
    }
    sk->status[n] = 0;
    sk->status_dirty = true;
}

static void exec_status(sk_state_t* sk, char* arg) {
    sk->st_count = 0;
    while (*arg && sk->st_count < 6) {
        while (*arg == ' ') ++arg;              /* 跳空白 */
        if (!*arg) break;
        char* tok = arg;
        char* sp = strchr(arg, ' ');
        size_t len = sp ? (size_t)(sp - arg) : strlen(arg);
        if (sp) arg = sp + 1; else arg += len;
        /* tok：标签=变量 */
        char* eq = memchr(tok, '=', len);
        if (eq && eq - tok > 0 && eq - tok < (long)sizeof(sk->st_label[0])) {
            copy_trim(sk->st_label[sk->st_count], sizeof(sk->st_label[0]),
                      tok, (size_t)(eq - tok));
            size_t vn = len - (size_t)(eq - tok) - 1;
            if (vn > 0 && vn < SK_NAME_MAX)
                copy_trim(sk->st_var[sk->st_count], SK_NAME_MAX, eq + 1, vn),
                ++sk->st_count;
        }
    }
    rebuild_status(sk);
}

static void parse_choice(sk_state_t* sk, char* arg, sk_out_t* out) {
    sk->choice_direct = strstr(arg, "->") != NULL;
    sk->choice_target[0][0] = 0;
    for (int i = 0; i < SK_MAX_CHOICES; ++i) {   /* 锁定态清零（老 choice 无锁） */
        sk->choice_locked[i] = false;
        sk->choice_hint[i][0] = 0;
    }
    out->choice_count = 0;
    char* tok = arg;
    while (*tok && out->choice_count < SK_MAX_CHOICES) {
        char* bar = strchr(tok, '|');
        size_t n = bar ? (size_t)(bar - tok) : strlen(tok);
        if (n) {
            char seg[192];
            copy_trim(seg, sizeof(seg), tok, n);
            if (sk->choice_direct) {
                char* arrow = strstr(seg, "->");
                if (arrow) {
                    *arrow = 0;
                    copy_trim(sk->choice_target[out->choice_count], SK_NAME_MAX,
                              arrow + 2, strlen(arrow + 2));
                    copy_trim(out->choice_text[out->choice_count],
                              sizeof(out->choice_text[0]), seg, strlen(seg));
                    ++out->choice_count;
                } else {
                    printf("SK: choice seg missing '->': '%s'\n", seg);
                }
            } else {
                copy_trim(out->choice_text[out->choice_count],
                          sizeof(out->choice_text[0]), seg, strlen(seg));
                ++out->choice_count;
            }
        }
        if (!bar) break;
        tok = bar + 1;
    }
    if (out->choice_count == 0) out->choice_count = 1, strcpy(out->choice_text[0], "继续");
}

/* cchoice（条件选项，2026-10-30 檐下谣）：项 = [条件]文案->标签 或 [g:条件]文案->标签@提示。
 *   无 [ ]：恒显；
 *   [f:…/v:…/!f:…+]：条件不过 → 隐藏；
 *   [g:条件]：条件不过 → 灰显锁定项（保留位置，禁止选中，点提示出提示语）；
 *             条件过 → 正常可选项。条件过时 @后缀的提示语被忽略。 */
static void parse_cchoice(sk_state_t* sk, char* arg, sk_out_t* out) {
    sk->choice_direct = true;
    out->choice_count = 0;
    for (int i = 0; i < SK_MAX_CHOICES; ++i) {
        sk->choice_locked[i] = false;
        sk->choice_hint[i][0] = 0;
        sk->choice_target[i][0] = 0;
    }
    char* tok = arg;
    while (*tok && out->choice_count < SK_MAX_CHOICES) {
        char* bar = strchr(tok, '|');
        size_t n = bar ? (size_t)(bar - tok) : strlen(tok);
        if (n) {
            char seg[384];
            size_t take = n < sizeof(seg) - 1 ? n : sizeof(seg) - 1;
            memcpy(seg, tok, take); seg[take] = 0;
            while (*seg == ' ') memmove(seg, seg + 1, strlen(seg));   /* 去前导空格 */
            bool lock_mode = false;
            char cond[192] = {0};
            char* body = seg;
            if (seg[0] == '[') {
                char* close = strchr(seg, ']');
                if (close) {
                    *close = 0;
                    const char* c = seg + 1;
                    if (c[0] == 'g' && c[1] == ':') { lock_mode = true; c += 2; }
                    copy_trim(cond, sizeof(cond), c, strlen(c));
                    body = close + 1;
                    while (*body == ' ') ++body;
                }
            }
            bool pass = cond[0] ? eval_cond(sk, cond) : true;
            bool show_ok = pass;                     /* 正常可选项 */
            bool show_lock = !pass && lock_mode;     /* 灰显锁定项 */
            if (show_ok || show_lock) {
                char* arrow = strstr(body, "->");
                if (arrow) {
                    const char* hint = NULL;
                    *arrow = 0;
                    char* tail = arrow + 2;
                    if (show_lock) {
                        const char* at = strchr(tail, '@');
                        if (at) hint = at + 1;
                    } else {
                        char* at = strchr(tail, '@');
                        if (at) *at = 0;             /* 已解锁：提示语忽略 */
                    }
                    copy_trim(sk->choice_target[out->choice_count], SK_NAME_MAX,
                              tail, strlen(tail));
                    if (show_lock) sk->choice_locked[out->choice_count] = true;
                    if (hint) {
                        while (*hint == ' ') ++hint;
                        copy_trim(sk->choice_hint[out->choice_count],
                                  sizeof(sk->choice_hint[0]), hint, strlen(hint));
                    } else if (show_lock) {
                        strcpy(sk->choice_hint[out->choice_count], "条件未满足");
                    }
                    copy_trim(out->choice_text[out->choice_count],
                              sizeof(out->choice_text[0]), body, strlen(body));
                    ++out->choice_count;
                } else {
                    printf("SK: cchoice seg missing '->': '%s'\n", body);
                }
            }                                         /* 不满足且非锁：隐藏 */
        }
        if (!bar) break;
        tok = bar + 1;
    }
    if (out->choice_count == 0) { out->choice_count = 1; strcpy(out->choice_text[0], "继续"); }
}

void sk_choose(sk_state_t* sk, int index) {
    if (index < 0 || index >= SK_MAX_CHOICES) return;
    if (sk->choice_locked[index]) return;     /* cchoice 锁定项：不响应（防漏侧） */
    if (sk->choice_direct) {
        if (sk->choice_target[index][0]) goto_label(sk, sk->choice_target[index]);
    } else {
        char mark[8];
        snprintf(mark, sizeof(mark), "c%d", index);
        sk_flag_set(sk, mark);               /* 顺序继续（choice 行已消费） */
    }
}

bool sk_choice_is_locked(const sk_state_t* sk, int index) {
    return index >= 0 && index < SK_MAX_CHOICES && sk->choice_locked[index];
}

const char* sk_choice_hint(const sk_state_t* sk, int index) {
    return (index >= 0 && index < SK_MAX_CHOICES) ? sk->choice_hint[index] : "";
}

void sk_restart(sk_state_t* sk) {
    sk->pc = 0;
    sk->vcount = 0;
    sk->fcount = 0;
    sk->last_name[0] = 0;
    sk->bg[0] = 0;
    sk->bg_dirty = true;
    sk->chars[0][0] = sk->chars[1][0] = sk->chars[2][0] = 0;
    sk->char_dirty = true;
    sk->st_count = 0;
    sk->status[0] = 0;
    sk->status_dirty = false;
}

/* ===================== 步进 ===================== */

sk_event_t sk_step(sk_state_t* sk, sk_out_t* out) {
    if (sk->pc >= sk->lines) { out->type = SK_EV_EOF; out->text[0] = 0; return SK_EV_EOF; }
    char* l = line_at(sk, sk->pc);
    if (!l) { out->type = SK_EV_EOF; out->text[0] = 0; return SK_EV_EOF; }

    char* arg = NULL;
    ++sk->pc;                               /* 默认本行已消费（跳转会改写 pc） */

    if (*l == 0 || *l == '#' || *l == '\n' || *l == '\r') return SK_EV_NONE;

    if (*l == '>') {                        /* 正文：继承粘性名牌 */
        /* 2026-10-28 修复：就地截断本行行尾（与 match 同规）。text 缓冲行间只有
         * '\n' 没有 '\0'，不截断时 strlen(l+1) 会一路数到文件尾——一句旁白把
         * 后续十几行（混着星号命令原文）整段吸进 out->text，真机表现为
         * “对话全堆积 + 命令标记原样上屏”。offs 表按原始偏移寻址，截断无副作用。 */
        char* nl = strchr(l, '\n');
        if (nl) *nl = 0;
        char* cr = strchr(l, '\r');
        if (cr) *cr = 0;
        out->type = SK_EV_TEXT;
        copy_trim(out->name, sizeof(out->name), sk->last_name, strlen(sk->last_name));
        copy_trim(out->text, SK_TEXT_MAX, l + 1, strlen(l + 1));
        return SK_EV_TEXT;
    }

    if (match(l, "* say", &arg)) {
        const char* sp = strchr(arg, ' ');
        if (sp) {
            /* 2026-10-28 重放保真修复：原 *sp=0 就地破坏脚本缓冲——launch 的 restore
             * 重放或结局重开第二次执行本行时，空格已永久变 \0，strchr 找不到，
             * 名/正文拆分失效（真机实锤：“* say 王生 姑娘…”只显示“王生”，正文全丢；
             * 名牌三连“王生/女郎/王生”即三句对话各丢正文）。按长度拷贝零缓冲修改，
             * gtext 存档重放同受益。 */
            copy_trim(out->name, sizeof(out->name), arg, (size_t)(sp - arg));
            const char* t = sp + 1;
            while (*t == ' ') ++t;
            copy_trim(out->text, SK_TEXT_MAX, t, strlen(t));
        } else {                            /* * say 文本（无名，罕见） */
            out->name[0] = 0;
            copy_trim(out->text, SK_TEXT_MAX, arg, strlen(arg));
        }
        copy_trim(sk->last_name, sizeof(sk->last_name), out->name, strlen(out->name));
        out->type = SK_EV_TEXT;
        return SK_EV_TEXT;
    }

    if (match(l, "* name", &arg)) {
        copy_trim(sk->last_name, sizeof(sk->last_name), arg, strlen(arg));
        return SK_EV_NONE;
    }
    if (match(l, "* title", &arg)) {
        copy_trim(sk->title, sizeof(sk->title), arg, strlen(arg));
        return SK_EV_NONE;
    }
    if (match(l, "* bg", &arg)) {
        bool none = !strcmp(arg, "none");
        copy_trim(sk->bg, sizeof(sk->bg), none ? "" : arg, none ? 0 : strlen(arg));
        sk->bg_dirty = true;
        return SK_EV_NONE;
    }
    if (match(l, "* char", &arg)) {
        int slot = 0, focus = 0;
        char file[96] = {0}, pos_c = 'C';
        if (sscanf(arg, "%d %95s %c %d", &slot, file, &pos_c, &focus) >= 2 &&
            slot >= 0 && slot < 3) {
            bool none = !strcmp(file, "none");
            copy_trim(sk->chars[slot], sizeof(sk->chars[slot]),
                      none ? "" : file, none ? 0 : strlen(file));
            sk->char_pos[slot] = (pos_c == 'L') ? 0 : (pos_c == 'R') ? 2 : 1;
            sk->char_focus[slot] = focus != 0;
            sk->char_dirty = true;
        }
        return SK_EV_NONE;
    }
    if (match(l, "* clear", &arg)) {
        (void)arg;
        for (int i = 0; i < 3; ++i) sk->chars[i][0] = 0;
        sk->char_dirty = true;
        return SK_EV_NONE;
    }
    if (match(l, "* hr", &arg)) {           /* 分隔线 → 一句正文 */
        (void)arg;
        out->type = SK_EV_TEXT;
        out->name[0] = 0;
        strcpy(out->text, "· · · · · ·");
        return SK_EV_TEXT;
    }
    if (match(l, "* label", &arg)) return SK_EV_NONE;   /* 锚点行（表已建） */
    if (match(l, "* goto", &arg)) { goto_label(sk, arg); return SK_EV_NONE; }
    if (match(l, "* ifgoto", &arg)) { exec_ifgoto(sk, arg, false); return SK_EV_NONE; }
    if (match(l, "* ifngoto", &arg)) { exec_ifgoto(sk, arg, true); return SK_EV_NONE; }
    if (match(l, "* ifnot", &arg)) { exec_if_flag(sk, arg, true); return SK_EV_NONE; }
    if (match(l, "* iftext", &arg)) {           /* 条件插句：不过整行跳过 */
        char* sp = strchr(arg, ' ');
        if (sp) {
            char cond[192];
            copy_trim(cond, sizeof(cond), arg, (size_t)(sp - arg));
            if (cond[0] && eval_cond(sk, cond)) {
                const char* t = sp + 1;
                while (*t == ' ') ++t;
                out->type = SK_EV_TEXT;
                copy_trim(out->name, sizeof(out->name), sk->last_name, strlen(sk->last_name));
                copy_trim(out->text, SK_TEXT_MAX, t, strlen(t));
                return SK_EV_TEXT;
            }
        }
        return SK_EV_NONE;
    }
    if (match(l, "* ifc", &arg)) {              /* 复合条件跳（原子同 cchoice） */
        char cond[192] = {0}, target[SK_NAME_MAX] = {0};
        if (sscanf(arg, "%191s %23s", cond, target) == 2 && eval_cond(sk, cond))
            goto_label(sk, target);
        return SK_EV_NONE;
    }
    if (match(l, "* if", &arg)) { exec_if_flag(sk, arg, false); return SK_EV_NONE; }
    if (match(l, "* set", &arg)) { exec_set(sk, arg); return SK_EV_NONE; }
    if (match(l, "* status", &arg)) { exec_status(sk, arg); return SK_EV_NONE; }
    if (match(l, "* choice", &arg)) {
        out->name[0] = 0;                    /* 选项页不带名牌（防残留上一句说话人） */
        parse_choice(sk, arg, out);
        out->type = SK_EV_CHOICE;
        return SK_EV_CHOICE;
    }
    if (match(l, "* cchoice", &arg)) {       /* 条件选项（檐下谣） */
        out->name[0] = 0;
        parse_cchoice(sk, arg, out);
        out->type = SK_EV_CHOICE;
        return SK_EV_CHOICE;
    }
    if (match(l, "* ending", &arg)) {
        out->type = SK_EV_END;
        copy_trim(out->text, SK_TEXT_MAX, arg, strlen(arg));
        sk->pc = sk->lines + 1;            /* 终态 */
        return SK_EV_END;
    }
    if (match(l, "* end", &arg)) {
        (void)arg;
        out->type = SK_EV_END;
        out->text[0] = 0;
        sk->pc = sk->lines + 1;
        return SK_EV_END;
    }

    if (*l == '*') {                        /* 未知命令：日志后宽容继续 */
        char unk[64];
        copy_trim(unk, sizeof(unk), l, strlen(l));
        printf("SK: unknown cmd '%s'\n", unk);
    }
    return SK_EV_NONE;
}

/* ===================== 存档序列化 ===================== */

size_t sk_save(const sk_state_t* sk, int extra_page, char* buf, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "%d", sk->pc);
    if (extra_page >= 0) n += (size_t)snprintf(buf + n, cap - n, " P%d", extra_page);
    for (int i = 0; i < sk->vcount && n + 2 < cap; ++i)
        n += (size_t)snprintf(buf + n, cap - n, " %s=%d", sk->vnames[i], sk->vvals[i]);
    for (int i = 0; i < sk->fcount && n + 2 < cap; ++i)
        n += (size_t)snprintf(buf + n, cap - n, " %s", sk->fnames[i]);
    /* 副作用持久化（2026-10-28）：重进存档即恢复场景，不再白屏。资源名单 token
     * 无空白，与旧格式兼容（旧存档无这些 token=保持旧行为）。 */
    if (sk->bg[0] && n + 100 < cap)
        n += (size_t)snprintf(buf + n, cap - n, " BG=%s", sk->bg);
    for (int i = 0; i < 3; ++i)
        if (sk->chars[i][0] && n + 110 < cap)
            n += (size_t)snprintf(buf + n, cap - n, " C%d=%s:%d", i, sk->chars[i],
                                  (int)sk->char_pos[i]);
    if (sk->st_count > 0 && n + 144 < cap) {
        n += (size_t)snprintf(buf + n, cap - n, " ST=");
        for (int i = 0; i < sk->st_count && n + 24 < cap; ++i)
            n += (size_t)snprintf(buf + n, cap - n, "%s%s:%s", i ? "," : "",
                                  sk->st_label[i], sk->st_var[i]);
    }
    return n;
}

bool sk_restore(sk_state_t* sk, const char* line, int* extra_page) {
    if (extra_page) *extra_page = -1;
    int pc = -1;
    char tok[96];
    const char* p = line;
    if (sscanf(p, "%d", &pc) != 1 || pc < 0) return false;
    sk->pc = pc;
    bool bg_touched = false, char_touched = false, st_touched = false;
    while (*p && *p == ' ') ++p;
    while (*p && *p != ' ') ++p;            /* 跳过 pc token */
    while (sscanf(p, "%95s", tok) == 1) {
        if (tok[0] == 'P' && (tok[1] >= '0' && tok[1] <= '9')) {
            if (extra_page) *extra_page = atoi(tok + 1);
        } else if (!strncmp(tok, "BG=", 3)) {          /* 背景副作用 */
            copy_trim(sk->bg, sizeof(sk->bg), tok + 3, strlen(tok + 3));
            bg_touched = true;
        } else if (!strncmp(tok, "C", 1) && tok[1] >= '0' && tok[1] <= '2' && tok[2] == '=') {
            int slot = tok[1] - '0';                   /* 立绘槽：C0=名:位 */
            char* colon = strchr(tok + 3, ':');
            if (colon) {
                *colon = 0;
                copy_trim(sk->chars[slot], sizeof(sk->chars[slot]),
                          tok + 3, strlen(tok + 3));
                sk->char_pos[slot] = (uint8_t)(atoi(colon + 1) & 3);
                char_touched = true;
            }
        } else if (!strncmp(tok, "ST=", 3)) {          /* 国势栏声明 */
            sk->st_count = 0;
            char* seg = tok + 3;
            while (*seg && sk->st_count < 6) {
                char* comma = strchr(seg, ',');
                size_t len = comma ? (size_t)(comma - seg) : strlen(seg);
                char* colon = memchr(seg, ':', len);
                if (colon && colon - seg > 0 && colon - seg < (long)sizeof(sk->st_label[0])) {
                    copy_trim(sk->st_label[sk->st_count], sizeof(sk->st_label[0]),
                              seg, (size_t)(colon - seg));
                    size_t vn = len - (size_t)(colon - seg) - 1;
                    if (vn > 0 && vn < SK_NAME_MAX)
                        copy_trim(sk->st_var[sk->st_count], SK_NAME_MAX, colon + 1, vn),
                        ++sk->st_count;
                }
                if (!comma) break;
                seg = comma + 1;
            }
            st_touched = true;
        } else if (strchr(tok, '=')) {
            char* eq = strchr(tok, '=');
            *eq = 0;
            sk_var_set(sk, tok, (int16_t)atoi(eq + 1));
        } else {
            sk_flag_set(sk, tok);           /* 旧 gtext 裸词=标志 */
        }
        p = strchr(p, ' ');
        if (!p) break;
        while (*p == ' ') ++p;
    }
    if (bg_touched) sk->bg_dirty = true;
    if (char_touched) sk->char_dirty = true;
    if (st_touched) rebuild_status(sk);      /* 变量已回填：展开即当前值 */
    return true;
}
