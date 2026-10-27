/*
 * quiz_engine.c — 题库闯关引擎实现。
 *
 * 题库格式（UTF-8，一行一令，// 注释）：
 *   * title 2026应知应会大闯关       标题（列表显示名）
 *   Q 题干
 *   A 选项一 … D 选项四
 *   R C / R AB / R 对            正确项：单字母=单选，多字母=多选（全选对才得分），
 *                                   R 行写“对”或选项为 对/错 = 判断题（两项渲染）
 *   E 解析
 * 进度文件 .quizprog：首行题数，其后每行 "idx streak skipped wronged"。
 *
 * 题型（2026-10-27 用户需求：判断题 + 多选题）：
 *   - 判断题：R 为“对/错/T/F/√/×”或 A/B 选项恰为“对/错”文本 → 渲染 两项；
 *   - 多选题：R 为 2-4 个字母（如 R ABD）→ 勾选模式，选完[确认答题]；
 *   - 单选题：R 单字母（原行为）。
 *   布局自适应：题干 1-6 行、选项文字换行逐项下排、解析超 13 行翻页，
 *   不再固定 4 行题干/8 行解析（实测截断修复）。
 *
 * 规则（2026-10-27 用户需求）：
 *   - 每次练习从可出题池（未跳过且未毕业）随机洗牌出题；
 *   - 选项顺序每题随机乱序（比对按原题字母基准，反馈字母翻译为屏面字母）；
 *   - 连续答对 3 次的题毕业，不再出现；答错清零并进错题集；
 *   - 答题页 [跳过此题] 按钮：该题永不再出；
 *   - 菜单 [错题重练]：只从错过的未毕业题随机出。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "epdiy.h"
#include "esp_random.h"

#include "ui_kit.h"
#include "ui_nothing.h"

#include "game_core.h"
#include "quiz_engine.h"

#define QZ_MAX_Q          512
#define QZ_TEXT           256   /* 题干/选项拷贝上限（B） */
#define QZ_EX             512   /* 解析拷贝上限（B） */
#define QZ_MASTER_STREAK  3
#define QZ_MASTER_DESC    "连续答对3次毕业"

/* 按钮几何（paint 与 tap 命中共用；逻辑竖屏 684×1216）。 */
#define QZ_BX   48
#define QZ_BW   588
#define QZ_BH   64
#define M_Y_PRACTICE  600
#define M_Y_WRONG     692
#define M_Y_RESET     784
#define A_OPT_X 54
#define A_OPT_W 576
#define A_OPT_H 56
#define A_ROW_BOTTOM  1080     /* 跳过/菜单行钉底：短题不随内容上跳 */
#define A_SKIP_X 48
#define A_SKIP_W 356
#define A_MENU_X 436
#define A_MENU_W 200
#define F_NEXT_X 48
#define F_NEXT_W 400
#define F_MENU_X 476
#define F_MENU_W 160
#define F_ROW_Y 932
#define D_MENU_X 142
#define D_MENU_W 400
#define D_MENU_Y 800
#define D_RESET_Y 900

typedef struct {
    int     l_q, l_a, l_b, l_c, l_d, l_e;   /* 行号；-1=缺 */
    char    correct[5];                      /* 单选:"C\0"；多选:"ABD\0"；判断:"对"/"错" */
    uint8_t multi;                          /* 1=多选（2-4 个正确项） */
    uint8_t judge;                          /* 1=判断题（对/错两项） */
    int16_t streak;                         /* 连对次数（3=毕业） */
    uint8_t skipped;                        /* 永不再出 */
    uint8_t wronged;                        /* 曾经答错过（错题集候选） */
    int8_t  opts;                           /* 渲染选项数：4/2（判断） */
} quiz_q_t;

struct quiz_engine_s {
    char*     text;                         /* 题库整读（PSRAM） */
    int32_t*  offs;                         /* 行偏移表 */
    int       lines;
    quiz_q_t* qs;
    int       qcount;
    char      title[96];

    char      prog_path[192];

    /* 会话 */
    quiz_phase_t phase;
    int       mode;                         /* 0=随机练习 1=错题重练 */
    int*      order;                        /* 本轮题目序（PSRAM） */
    int       order_count, order_pos;
    int       cur;
    int       asked, correct, mastered_delta;
    bool      last_correct;
    char      last_letter[8];               /* 作答字母串（多选可能多个；屏面显示字母） */
    bool      last_master;
    uint8_t   pick[4];                      /* 多选当前勾选（显示位次索引） */
    uint8_t   perm[4];                      /* 显示位次→原选项号（每题随机乱序） */
    bool      multi_armed;                  /* 多选已选至少一项（可确认） */
    int       ex_page;                      /* 解析分页：当前页（0 基） */
    int       ex_pages;                     /* 解析总页数（每页 13 行） */
    bool      reset_arm;                    /* 菜单重置二次确认 */
    bool      all_done;                     /* 池空（全部掌握/错题集空） */
    char      note[112];
    /* ASK 动态布局缓存（paint 与 tap 命中共用；进入题目/选项文字变更时重算） */
    int       opt_y[4];                     /* 各选项按钮顶 y */
    int       opt_h[4];                     /* 各选项按钮高 */
    int       row_y;                        /* 跳过/菜单行 y */
    int       multi_confirm_y;              /* 多选[确认答题] y（-1=非多选） */
};

/* ===================== 基础：行访问 / 前缀判定 ===================== */

static char* line_at(quiz_engine_t* e, int i) {
    if (i < 0 || i >= e->lines) return NULL;
    return e->text + e->offs[i];
}

/* 有界拷贝行内容（剥掉单字母/标题前缀），行尾就地截断不影响 offs。 */
static void line_copy(const quiz_engine_t* e, int li, char* out, size_t cap) {
    out[0] = 0;
    if (li < 0) return;
    const char* l = e->text + e->offs[li];
    while (*l == ' ') ++l;                  /* 行首空白 */
    if (l[0] && l[1] == ' ' &&
        ((l[0] >= 'A' && l[0] <= 'Z') || l[0] == '*' || l[0] == 'R'))
        l += 2;                              /* 剥 "Q "/"A "…"* " 前缀 */
    while (*l == ' ') ++l;
    size_t n = 0;
    while (l[n] && l[n] != '\n' && l[n] != '\r' && n + 1 < cap) ++n;
    memcpy(out, l, n);
    out[n] = 0;
}

/* ===================== 加载 / 释放 ===================== */

quiz_engine_t* quiz_load(const char* quiz_path, const char* prog_path) {
    FILE* f = fopen(quiz_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1024 * 1024) { fclose(f); return NULL; }
    quiz_engine_t* e = (quiz_engine_t*)heap_caps_calloc(1, sizeof(quiz_engine_t),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!e) { fclose(f); return NULL; }
    e->text = (char*)heap_caps_malloc((size_t)n + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!e->text) { heap_caps_free(e); fclose(f); return NULL; }
    size_t got = fread(e->text, 1, (size_t)n, f);
    fclose(f);
    e->text[got] = '\n';
    e->text[got + 1] = 0;

    e->lines = 0;
    for (size_t i = 0; i <= got; ++i)
        if (e->text[i] == '\n') ++e->lines;
    e->offs = (int32_t*)heap_caps_calloc((size_t)e->lines + 1, sizeof(int32_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!e->offs) { quiz_free(e); return NULL; }
    {
        int li = 0;
        e->offs[li++] = 0;
        for (size_t i = 0; i <= got; ++i)
            if (e->text[i] == '\n' && li < e->lines) e->offs[li++] = (int32_t)(i + 1);
    }

    /* 两遍解析：先数 Q，再填槽。 */
    int qn = 0;
    for (int i = 0; i < e->lines; ++i) {
        char* l = line_at(e, i);
        if ((l[0] == 'Q' || l[0] == '*') && l[1] == ' ') {
            if (l[0] == 'Q') ++qn;
            else if (!e->title[0]) {          /* "* title XX" → 剥双词前缀 */
                const char* p = l + 1;
                while (*p == ' ') ++p;
                p += 5;                       /* "title" */
                while (*p == ' ') ++p;
                size_t n = 0;
                while (p[n] && p[n] != '\n' && p[n] != '\r' && n + 1 < sizeof(e->title) - 1) ++n;
                memcpy(e->title, p, n);
                e->title[n] = 0;
            }
        }
    }
    if (qn <= 0 || qn > QZ_MAX_Q) { quiz_free(e); return NULL; }
    e->qs = (quiz_q_t*)heap_caps_calloc((size_t)qn, sizeof(quiz_q_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    e->order = (int*)heap_caps_calloc((size_t)qn, sizeof(int),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!e->qs || !e->order) { quiz_free(e); return NULL; }
    e->qcount = qn;

    {
        int qi = -1;
        quiz_q_t* cur = NULL;
        for (int i = 0; i < e->lines; ++i) {
            char* l = line_at(e, i);
            char c0 = l[0];
            if (c0 != 'Q' && c0 != 'A' && c0 != 'B' && c0 != 'C' &&
                c0 != 'D' && c0 != 'R' && c0 != 'E')
                continue;
            if (l[1] != ' ') continue;
            if (c0 == 'Q') { ++qi; cur = &e->qs[qi];
                cur->l_q = i; cur->l_a = cur->l_b = cur->l_c = cur->l_d = cur->l_e = -1;
                cur->correct[0] = 'A'; cur->correct[1] = 0;
                cur->multi = cur->judge = 0; cur->opts = 4;
                continue;
            }
            if (!cur || qi < 0) continue;
            switch (c0) {
            case 'A': cur->l_a = i; break;
            case 'B': cur->l_b = i; break;
            case 'C': cur->l_c = i; break;
            case 'D': cur->l_d = i; break;
            case 'E': cur->l_e = i; break;
            case 'R': {
                char r[16];
                line_copy(e, i, r, sizeof(r));
                /* 解析 R：字母升序归一（乱序作答比对与反馈翻译的公共基准）；
                 * 单字母=单选；多字母=多选；T/F/对/错/√/×=判断 */
                int nn = 0;
                for (char c = 'A'; c <= 'D' && nn < 4; ++c)
                    if (strchr(r, c) || strchr(r, (char)(c + 32)))
                        cur->correct[nn++] = c;
                cur->correct[nn] = 0;
                if (nn >= 2) { cur->multi = 1; break; }   /* 多选 */
                if (nn == 0) {                         /* 判断：R 对 / R 错 / T / F / √ / × */
                    bool yes = r[0] == 'T' || r[0] == 't' ||
                               !strncmp(r, "对", 3) || !strncmp(r, "√", 3);
                    bool no  = r[0] == 'F' || r[0] == 'f' ||
                               !strncmp(r, "错", 3) || !strncmp(r, "×", 2);
                    if (yes) { strcpy(cur->correct, "对"); cur->judge = 1; cur->opts = 2; }
                    else if (no) { strcpy(cur->correct, "错"); cur->judge = 1; cur->opts = 2; }
                    /* 未识别的 R 保持单选默认 A：交由 A/B 行文本后验转换 */
                } else cur->opts = 4;
                break;
            }
            default: break;
            }
        }
    }

    /* 判断题后验：R 未标但 A/B 行文本恰为“对/错” → 判断题（两项）。 */
    for (int i = 0; i < e->qcount; ++i) {
        quiz_q_t* q = &e->qs[i];
        if (q->judge || q->multi) continue;
        char a[24], b[24];
        line_copy(e, q->l_a, a, sizeof(a));
        line_copy(e, q->l_b, b, sizeof(b));
         bool a_tf = !strcmp(a, "对") || !strcmp(a, "错") || !strcmp(a, "正确") || !strcmp(a, "错误");
         bool b_tf = !strcmp(b, "对") || !strcmp(b, "错") || !strcmp(b, "正确") || !strcmp(b, "错误");
         if (a_tf && b_tf) {
             q->judge = 1; q->opts = 2;
             /* 正确项按 R 原字母（默认 A）映射到 对/错 文本。 */
             bool a_right = !strcmp(a, "对") || !strcmp(a, "正确");
             char ra = (q->correct[0] >= 'A' && q->correct[0] <= 'D') ? q->correct[0] : 'A';
             bool pick_a = (ra == 'A') ? a_right : !a_right;
             strcpy(q->correct, pick_a ? "对" : "错");
         }
     }

    snprintf(e->prog_path, sizeof(e->prog_path), "%s", prog_path);

    /* 进度读取 */
    FILE* p = fopen(e->prog_path, "r");
    if (p) {
        int idx, st, sk, wr;
        while (fscanf(p, "%d %d %d %d", &idx, &st, &sk, &wr) == 4) {
            if (idx >= 0 && idx < e->qcount && st <= 999) {
                e->qs[idx].streak = (int16_t)st;
                e->qs[idx].skipped = (uint8_t)sk;
                e->qs[idx].wronged = (uint8_t)wr;
            }
        }
        fclose(p);
    }

    e->phase = QZ_MENU;
    e->mode = 0;
    e->cur = 0;
    return e;
}

void quiz_free(quiz_engine_t* e) {
    if (!e) return;
    if (e->text) heap_caps_free(e->text);
    if (e->offs) heap_caps_free(e->offs);
    if (e->qs) heap_caps_free(e->qs);
    if (e->order) heap_caps_free(e->order);
    heap_caps_free(e);
}

void quiz_get_title(const quiz_engine_t* q, char* out, size_t cap) {
    snprintf(out, cap, "%s", q ? q->title : "");
}

/* ===================== 统计 / 会话 ===================== */

static bool q_eligible(const quiz_engine_t* e, int i) {
    return !e->qs[i].skipped && e->qs[i].streak < QZ_MASTER_STREAK;
}

static int pool_build(quiz_engine_t* e, int mode) {
    int n = 0;
    for (int i = 0; i < e->qcount; ++i) {
        if (!q_eligible(e, i)) continue;
        if (mode == 1 && !e->qs[i].wronged) continue;   /* 错题集：只出错过的 */
        e->order[n++] = i;
    }
    /* Fisher-Yates 洗牌（esp_random） */
    for (int i = n - 1; i > 0; --i) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        int t = e->order[i]; e->order[i] = e->order[j]; e->order[j] = t;
    }
    return n;
}

static void save_prog(quiz_engine_t* e) {
    FILE* f = fopen(e->prog_path, "w");
    if (!f) return;
    for (int i = 0; i < e->qcount; ++i)
        fprintf(f, "%d %d %d %d\n", i, e->qs[i].streak, e->qs[i].skipped, e->qs[i].wronged);
    fclose(f);
}

/* 前向：session 流程与进入题目的定义顺序（布局实现在后） */
static void layout_ask(quiz_engine_t* e);
static void enter_question(quiz_engine_t* e);

static void session_start(quiz_engine_t* e, int mode) {
    e->mode = mode;
    e->order_count = pool_build(e, mode);
    e->order_pos = 0;
    e->asked = 0;
    e->correct = 0;
    e->mastered_delta = 0;
    e->reset_arm = false;
    e->all_done = false;
    e->note[0] = 0;
    if (e->order_count == 0) {
        e->phase = QZ_DONE;
        e->all_done = true;
        if (mode == 1) snprintf(e->note, sizeof(e->note), "错题集是空的——错过且未毕业的题：0 道。");
        else snprintf(e->note, sizeof(e->note), "可出题：%d 道", 0);
        return;
    }
    e->cur = e->order[0];
    e->phase = QZ_ASK;
    enter_question(e);
}

/* 进入新题：复位多选勾选/解析分页，选项随机乱序，预算动态布局。 */
static void enter_question(quiz_engine_t* e) {
    const quiz_q_t* q = &e->qs[e->cur];
    memset(e->pick, 0, sizeof(e->pick));
    e->multi_armed = false;
    e->ex_page = 0;
    e->ex_pages = 0;
    for (int i = 0; i < 4; ++i) e->perm[i] = (uint8_t)i;
    for (int i = q->opts - 1; i > 0; --i) {   /* 选项乱序（2026-10-27 用户需求） */
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = e->perm[i]; e->perm[i] = e->perm[j]; e->perm[j] = t;
    }
    layout_ask(e);
}

static void advance(quiz_engine_t* e) {
    ++e->order_pos;
    if (e->order_pos >= e->order_count) {
        e->phase = QZ_DONE;
        snprintf(e->note, sizeof(e->note), "本轮答对 %d / %d，新毕业 %d 题。",
                 e->correct, e->asked, e->mastered_delta);
        return;
    }
    e->cur = e->order[e->order_pos];
    e->phase = QZ_ASK;
    enter_question(e);
}

/* ---------------- 动态布局（2026-10-27：题干/选项自适应，长文字不再出屏） ----------------
 * 几何：题头行 y=60；题干中心行 106.. 每 44px（上限 6 行）；选项组在
 * 「题干下缘 ↔ 跳过行上缘」区间垂直居中（+24 偏下手势区），选项高度
 * = 56+(行数-1)×34（行宽 36 半宽单位=18 汉字，24px 字），间距 12；
 * 多选追加[确认答题]；跳过/菜单行钉底 1080（组超高时顺延，越界整体上移）。
 * 全部位次存 opt_y/opt_h/multi_confirm_y/row_y，paint 与 tap 命中共用，
 * 须在绘制/命中前重算（enter_question 进入每题时调用）。 */
#define QZ_UNITS_Q   40      /* 题干/解析行宽：20 汉字（26px→520px，x=48 起） */
#define QZ_UNITS_O   36      /* 选项行宽：18 汉字（留出字母前缀与勾选框） */
#define QZ_LH        44      /* 题干行高 */
#define QZ_EX_LINES  13      /* 解析每页行数（中心行 300..828，页脚 868，按钮 932） */
#define QZ_CHECK_X   68      /* 多选勾选框左上 x */

static int opt_letter_line(const quiz_q_t* q, int i) {
    return i == 0 ? q->l_a : i == 1 ? q->l_b : i == 2 ? q->l_c : q->l_d;
}

/* 选项正文串（布局测量与绘制共用同一串，行数必然一致）：i=显示位次，
 * 原选项号=perm[i]；单选带 "A. " 前缀（显示字母），多选/判断裸文本。
 * 返回文字锚 x（判断题与选项按钮中心对齐显示）。 */
static int opt_body(const quiz_engine_t* e, const quiz_q_t* q, int i,
                    char* out, size_t cap) {
    char opt[QZ_TEXT];
    line_copy(e, opt_letter_line(q, e->perm[i]), opt, sizeof(opt));
    if (q->judge) {                          /* 判断：裸“对/错”居中 */
        snprintf(out, cap, "%s", opt);
        return A_OPT_X + A_OPT_W / 2;
    }
    snprintf(out, cap, "%c. %s", 'A' + i, opt);
    return q->multi ? QZ_CHECK_X + 28 + 16 : A_OPT_X + 18;
}

/* 正确答案转屏面字母（乱序后反馈与用户所见一致；判断题原样文本；
 * 多选按显示位次升序，如 BCD 顺读）。 */
static void correct_disp(const quiz_engine_t* e, const quiz_q_t* q,
                         char* out, size_t cap) {
    out[0] = 0;
    if (q->judge) { snprintf(out, cap, "%s", q->correct); return; }
    size_t n = 0;
    for (int j = 0; j < 4 && n + 1 < cap; ++j)
        if (strchr(q->correct, (char)('A' + e->perm[j])))
            out[n++] = (char)('A' + j);
    out[n] = 0;
}

/* 按 units 折行后取第 ln 行（宽度模型与 gc_ui_typewrap_count 一致）。 */
static void zline_piece(const char* s, int units, int ln, char* out, size_t cap) {
    out[0] = 0;
    if (!s) return;
    const char* p = s;
    for (int l = 0; l <= ln && p[0]; ++l) {
        size_t n = 0; int u = 0;
        while (p[n] && u < units && n + 1 < cap) {
            unsigned char c0 = (unsigned char)p[n];
            int len = c0 & 0x80 ? ((c0 & 0xe0) == 0xc0 ? 2 : ((c0 & 0xf0) == 0xe0 ? 3 : 4)) : 1;
            u += c0 > 0x7f ? 2 : 1;
            n += (size_t)len;
        }
        if (l == ln) { memcpy(out, p, n); out[n] = 0; return; }
        p += n;
    }
}

static void layout_ask(quiz_engine_t* e) {
    const quiz_q_t* q = &e->qs[e->cur];
    char body[QZ_TEXT + 16];
    char qt[QZ_TEXT];
    line_copy(e, q->l_q, qt, sizeof(qt));
    int qlines = gc_ui_typewrap_count(qt, QZ_UNITS_Q, 0);
    if (qlines < 1) qlines = 1;
    if (qlines > 6) qlines = 6;

    /* 先测各选项行数 → 组高（选项 + 间距 + 多选确认行，去掉末尾间距）。 */
    int group_h = 0;
    for (int i = 0; i < q->opts; ++i) {
        opt_body(e, q, i, body, sizeof(body));
        int lines = gc_ui_typewrap_count(body, QZ_UNITS_O, 0);
        if (lines < 1) lines = 1;
        if (lines > 3) lines = 3;              /* 超 3 行截断（安全） */
        e->opt_h[i] = A_OPT_H + (lines - 1) * 34;
        group_h += e->opt_h[i] + 12;
    }
    if (q->multi) group_h += QZ_BH + 12;
    group_h -= 12;

    /* 可用区间：题干下缘 .. 跳过行上缘。选项组垂直居中（+24 略偏下手势区）；
     * 用户实测反馈“选项太靠上不好点”（2026-10-27）——短题不再顶着题干排。 */
    int avail_top = 100 + qlines * QZ_LH + 10 + 16;
    int avail_bot = A_ROW_BOTTOM - 24;
    int avail_h = avail_bot - avail_top;
    int y = (group_h < avail_h) ? avail_top + (avail_h - group_h) / 2 + 24
                                : avail_top;   /* 放不下：顶格排，底部保护兜底 */

    for (int i = 0; i < q->opts; ++i) {
        e->opt_y[i] = y;
        y += e->opt_h[i] + 12;
    }
    if (q->multi) { e->multi_confirm_y = y; y += QZ_BH + 12; }
    else e->multi_confirm_y = -1;

    /* 跳过/菜单行钉底；组超高时顺延，仍越界再整体上移（极端长题）。 */
    int last_bot = y - 12;
    if (last_bot + 24 <= A_ROW_BOTTOM) {
        e->row_y = A_ROW_BOTTOM;
    } else {
        e->row_y = last_bot + 24;
        if (e->row_y + A_OPT_H > 1216 - 26) {
            int over = e->row_y + A_OPT_H - (1216 - 26);
            for (int i = 0; i < q->opts; ++i) e->opt_y[i] -= over;
            if (e->multi_confirm_y >= 0) e->multi_confirm_y -= over;
            e->row_y -= over;
        }
    }
}

/* 选项内容：按钮外框由调用方画，这里画多选勾选框与换行文字
 * （文字区垂直居中于 opt_y/opt_h）。 */
static void draw_opt_text(uint8_t* fb, const quiz_engine_t* e, int i, bool picked) {
    const quiz_q_t* q = &e->qs[e->cur];
    char body[QZ_TEXT + 16];
    int cx = opt_body(e, q, i, body, sizeof(body));
    if (q->multi) {
        int box_y = e->opt_y[i] + e->opt_h[i] / 2 - 14;
        if (picked) {                       /* 选中：实心黑框 + 白色两笔对勾（app 同款粗线） */
            ui_fill_round_rect(fb, (EpdRect){QZ_CHECK_X, box_y, 28, 28}, 6, 0x00);
            int mx = QZ_CHECK_X + 14, my = box_y + 14;
            nothing_thick_line(fb, mx - 5, my + 1, mx - 1, my + 5, 4, 0xff);
            nothing_thick_line(fb, mx - 1, my + 5, mx + 7, my - 6, 4, 0xff);
        } else {
            ui_draw_round_rect(fb, (EpdRect){QZ_CHECK_X, box_y, 28, 28}, 6, 0x00);
        }
    }
    int lines = gc_ui_typewrap_count(body, QZ_UNITS_O, 0);
    if (lines < 1) lines = 1;
    if (lines > 3) lines = 3;
    int tex_y = e->opt_y[i] + (e->opt_h[i] - lines * 34) / 2 + 17;
    for (int ln = 0; ln < lines; ++ln) {
        char piece[QZ_TEXT + 16];
        zline_piece(body, QZ_UNITS_O, ln, piece, sizeof(piece));
        if (!piece[0]) break;
        ui_text_vc(fb, cx, tex_y + ln * 34, 24, piece,
                   q->judge ? EPD_DRAW_ALIGN_CENTER : EPD_DRAW_ALIGN_LEFT, false);
    }
}

/* 作答：单选 opt=0..3；多选时 opt 若为 -1 表示确认作答。 */
static void answer(quiz_engine_t* e, int opt) {
    quiz_q_t* q = &e->qs[e->cur];
    e->last_letter[0] = 0;
    if (q->multi) {
        if (opt >= 0) return;                 /* 多选：选项点按只勾选，不走判分 */
        char got[8]; int nn = 0;              /* 原字母升序（与 correct 同基准） */
        for (int o = 0; o < 4; ++o)
            for (int j = 0; j < 4; ++j)
                if (e->perm[j] == o && e->pick[j] && nn < 4)
                    got[nn++] = (char)('A' + o);
        got[nn] = 0;
        if (!nn) return;                      /* 未勾选：不判分（防御） */
        char disp[8]; int dn = 0;             /* 反馈用屏面字母 */
        for (char c = 'A'; c <= 'D'; ++c)
            if (strchr(got, c))
                for (int j = 0; j < 4; ++j)
                    if (e->perm[j] == c - 'A') { disp[dn++] = (char)('A' + j); break; }
        disp[dn] = 0;
        snprintf(e->last_letter, sizeof(e->last_letter), "%s", disp);
        e->last_correct = strcmp(got, q->correct) == 0;
    } else if (q->judge) {
        bool got_yes = (e->perm[opt] == 0);   /* 原 A 行是对/错按题面文本 */
        char got[8];
        char a[24];
        line_copy(e, q->l_a, a, sizeof(a));
        bool a_is_yes = !strcmp(a, "对") || !strcmp(a, "正确");
        snprintf(got, sizeof(got), "%s", (a_is_yes ? got_yes : !got_yes) ? "对" : "错");
        snprintf(e->last_letter, sizeof(e->last_letter), "%s", got);
        e->last_correct = !strcmp(got, q->correct);
    } else {
        char orig_letter = (char)('A' + e->perm[opt]);
        snprintf(e->last_letter, sizeof(e->last_letter), "%c", (char)('A' + opt));
        e->last_correct = (orig_letter == q->correct[0]);
    }
    if (e->last_correct) {
        ++q->streak;
        ++e->correct;
        e->last_master = q->streak >= QZ_MASTER_STREAK;
        if (e->last_master) ++e->mastered_delta;
    } else {
        q->streak = 0;
        q->wronged = 1;
        e->last_master = false;
    }
    ++e->asked;
    save_prog(e);
    /* 解析分页：每页 QZ_EX_LINES 行（FEED 翻页用）。 */
    {
        char ex[QZ_EX];
        line_copy(e, q->l_e, ex, sizeof(ex));
        e->ex_pages = (gc_ui_typewrap_count(ex, QZ_UNITS_Q, 0) + QZ_EX_LINES - 1)
                      / QZ_EX_LINES;
    }
    e->ex_page = 0;
    e->phase = QZ_FEED;
}

static void skip_cur(quiz_engine_t* e) {
    e->qs[e->cur].skipped = 1;
    save_prog(e);
    advance(e);
}

static void reset_prog(quiz_engine_t* e) {
    for (int i = 0; i < e->qcount; ++i)
        e->qs[i].streak = 0, e->qs[i].skipped = 0, e->qs[i].wronged = 0;
    save_prog(e);
    e->reset_arm = false;
}

/* ===================== 绘制 ===================== */

static void draw26(void* user, const char* line, int y) {
    ui_text_vc((uint8_t*)user, 48, y, 26, line, EPD_DRAW_ALIGN_LEFT, false);
}

/* 解析分页绘制：typewrap 行回调，仅画当前页 [from, upto) 行（y=300+ln*44）。 */
typedef struct {
    uint8_t* fb;
    int from, upto;
} feed_pg_t;

static void feed_line(void* user, const char* line, int y) {
    feed_pg_t* fp = (feed_pg_t*)user;
    int ln = (y - 300) / QZ_LH;
    if (ln < fp->from || ln >= fp->upto) return;
    ui_text_vc(fp->fb, 48, y, 26, line, EPD_DRAW_ALIGN_LEFT, false);
}

static int mastered_count(const quiz_engine_t* e) {
    int m = 0;
    for (int i = 0; i < e->qcount; ++i)
        if (!e->qs[i].skipped && e->qs[i].streak >= QZ_MASTER_STREAK) ++m;
    return m;
}
static int wrong_count(const quiz_engine_t* e) {
    int w = 0;
    for (int i = 0; i < e->qcount; ++i)
        if (e->qs[i].wronged && q_eligible(e, i)) ++w;
    return w;
}
static int skipped_count(const quiz_engine_t* e) {
    int s = 0;
    for (int i = 0; i < e->qcount; ++i) if (e->qs[i].skipped) ++s;
    return s;
}

/* 各页绘制（按钮几何宏见文件顶部） */
void quiz_paint(quiz_engine_t* e, uint8_t* fb) {
    if (e->phase == QZ_MENU) {
        ui_clear_page(fb);
        ui_text_vc(fb, 342, 220, 42, e->title[0] ? e->title : "题库闯关",
                   EPD_DRAW_ALIGN_CENTER, false);
        epd_fill_rect((EpdRect){120, 290, 444, 3}, 0x00, fb);
        char st1[96], st2[96];
        int m = mastered_count(e), w = wrong_count(e), sk = skipped_count(e);
        int left = e->qcount - m - sk;
        snprintf(st1, sizeof(st1), "总题 %d · 已毕业 %d · 待练 %d · 已跳过 %d",
                 e->qcount, m, left, sk);
        snprintf(st2, sizeof(st2), "错题集 %d 道", w);
        ui_text_vc(fb, 342, 360, 24, st1, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 400, 24, st2, EPD_DRAW_ALIGN_CENTER, false);
        gc_ui_button(fb, (EpdRect){QZ_BX, M_Y_PRACTICE, QZ_BW, QZ_BH}, "随机练习", true);
        char wb[48];
        snprintf(wb, sizeof(wb), "错题重练%s", w ? "" : "（空）");
        gc_ui_button(fb, (EpdRect){QZ_BX, M_Y_WRONG, QZ_BW, QZ_BH}, wb, false);
        gc_ui_button(fb, (EpdRect){QZ_BX, M_Y_RESET, QZ_BW, QZ_BH},
                     e->reset_arm ? "再点一次确认重置" : "重置进度", false);
        ui_text_vc(fb, 342, 1100, 18,
                   "连对 3 次的题毕业不再出 · 答错自动进错题集 · 中长按退出",
                   EPD_DRAW_ALIGN_CENTER, false);
        return;
    }

    if (e->phase == QZ_ASK) {
        ui_clear_page(fb);
        const quiz_q_t* q = &e->qs[e->cur];
        char head[80];
        snprintf(head, sizeof(head), "第 %d / %d 题 · 本轮已对 %d · %s题",
                 e->order_pos + 1, e->order_count, e->correct,
                 q->multi ? "多选" : q->judge ? "判断" : "单选");
        ui_text_vc(fb, 342, 60, 20, head, EPD_DRAW_ALIGN_CENTER, false);
        char qt[QZ_TEXT];
        line_copy(e, q->l_q, qt, sizeof(qt));
        gc_ui_typewrap(qt, (int)strlen(qt), 6, QZ_UNITS_Q, draw26, fb, 106, QZ_LH);
        for (int i = 0; i < q->opts; ++i) {
            EpdRect r = {A_OPT_X, e->opt_y[i], A_OPT_W, e->opt_h[i]};
            bool on = q->multi && e->pick[i];
            /* 选中=外框保留+内缩加粗圈（系统惯例，不整块填黑以减轻残影）。 */
            ui_draw_choice_round_rect(fb, r, 10, on);
            draw_opt_text(fb, e, i, on);
        }
        if (q->multi) {
            gc_ui_button(fb, (EpdRect){QZ_BX, e->multi_confirm_y, QZ_BW, QZ_BH},
                         e->multi_armed ? "确认答题" : "请先勾选，再确认",
                         e->multi_armed);
        }
        gc_ui_button(fb, (EpdRect){A_SKIP_X, e->row_y, A_SKIP_W, A_OPT_H},
                     "跳过此题（永不再出）", false);
        gc_ui_button(fb, (EpdRect){A_MENU_X, e->row_y, A_MENU_W, A_OPT_H}, "菜单", false);
        return;
    }

    if (e->phase == QZ_FEED) {
        ui_clear_page(fb);
        char head[64];
        snprintf(head, sizeof(head), "第 %d / %d 题", e->order_pos + 1, e->order_count);
        ui_text_vc(fb, 342, 60, 20, head, EPD_DRAW_ALIGN_CENTER, false);
        if (e->last_correct) {
            ui_text_vc(fb, 342, 160, 40, "答对了！", EPD_DRAW_ALIGN_CENTER, false);
            if (e->last_master)
                ui_text_vc(fb, 342, 230, 22, "已连对 3 次——本题毕业，不再出现",
                           EPD_DRAW_ALIGN_CENTER, false);
        } else {
            char cd[8], w[96];
            correct_disp(e, &e->qs[e->cur], cd, sizeof(cd));
            snprintf(w, sizeof(w), "答错了，正确答案 %s", cd);
            ui_text_vc(fb, 342, 160, 40, w, EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 230, 22, "已放入错题集，可在菜单重练",
                       EPD_DRAW_ALIGN_CENTER, false);
        }
        char ex[QZ_EX];
        line_copy(e, e->qs[e->cur].l_e, ex, sizeof(ex));
        int from = e->ex_page * QZ_EX_LINES;
        feed_pg_t fp = { fb, from, from + QZ_EX_LINES };
        gc_ui_typewrap(ex, (int)strlen(ex), from + QZ_EX_LINES, QZ_UNITS_Q,
                       feed_line, &fp, 300, QZ_LH);
        if (e->ex_pages > 1) {
            char pg[48];
            snprintf(pg, sizeof(pg), "—— 解析 %d / %d ——", e->ex_page + 1, e->ex_pages);
            ui_text_vc(fb, 342, 868, 18, pg, EPD_DRAW_ALIGN_CENTER, false);
        }
        bool last = e->order_pos >= e->order_count - 1;
        gc_ui_button(fb, (EpdRect){F_NEXT_X, F_ROW_Y, F_NEXT_W, QZ_BH},
                     e->ex_page < e->ex_pages - 1 ? "下一页"
                     : last ? "查看结果" : "下一题", true);
        gc_ui_button(fb, (EpdRect){F_MENU_X, F_ROW_Y, F_MENU_W, QZ_BH}, "菜单", false);
        return;
    }

    /* QZ_DONE */
    ui_clear_page(fb);
    if (e->all_done) {
        ui_text_vc(fb, 342, 260, 42, e->mode == 1 ? "错题集已清空" : "全部掌握！",
                   EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 360, 24, e->note, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 480, 20, "可重置进度从头再练一轮",
                   EPD_DRAW_ALIGN_CENTER, false);
    } else {
        ui_text_vc(fb, 342, 260, 42, "本轮完成", EPD_DRAW_ALIGN_CENTER, false);
        char s1[64];
        snprintf(s1, sizeof(s1), "答对 %d / %d", e->correct, e->asked);
        ui_text_vc(fb, 342, 370, 30, s1, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 430, 22, e->note, EPD_DRAW_ALIGN_CENTER, false);
    }
    char s2[96];
    int m = mastered_count(e);
    snprintf(s2, sizeof(s2), "总进度：已毕业 %d / %d", m, e->qcount);
    ui_text_vc(fb, 342, 540, 24, s2, EPD_DRAW_ALIGN_CENTER, false);
    gc_ui_button(fb, (EpdRect){D_MENU_X, D_MENU_Y, D_MENU_W, QZ_BH}, "返回菜单", true);
    if (e->all_done)
        gc_ui_button(fb, (EpdRect){D_MENU_X, D_RESET_Y, D_MENU_W, QZ_BH}, "重置进度", false);
}

bool quiz_middle_key(quiz_engine_t* q) {
    switch (q->phase) {
    case QZ_FEED: return quiz_tap(q, F_NEXT_X + F_NEXT_W / 2, F_ROW_Y + QZ_BH / 2);
    case QZ_MENU: return quiz_tap(q, QZ_BX + QZ_BW / 2, M_Y_PRACTICE + QZ_BH / 2);
    case QZ_DONE: return quiz_tap(q, D_MENU_X + D_MENU_W / 2, D_MENU_Y + QZ_BH / 2);
    default:      return false;             /* ASK 不响应：防中键误触答案 */
    }
}

/* ===================== 输入 ===================== */

bool quiz_tap(quiz_engine_t* q, int x, int y) {
    switch (q->phase) {
    case QZ_MENU: {
        if (ui_rect_hit((EpdRect){QZ_BX, M_Y_PRACTICE, QZ_BW, QZ_BH}, x, y)) {
            session_start(q, 0);
            return true;
        }
        if (ui_rect_hit((EpdRect){QZ_BX, M_Y_WRONG, QZ_BW, QZ_BH}, x, y)) {
            if (wrong_count(q) > 0) { session_start(q, 1); return true; }
            return false;                   /* 空错题集：不响应 */
        }
        if (ui_rect_hit((EpdRect){QZ_BX, M_Y_RESET, QZ_BW, QZ_BH}, x, y)) {
            q->reset_arm = !q->reset_arm;
            if (!q->reset_arm) reset_prog(q);   /* 第二次点击=执行 */
            return true;
        }
        return false;
    }
    case QZ_ASK: {
        const quiz_q_t* qq = &q->qs[q->cur];
        for (int i = 0; i < qq->opts; ++i)
            if (ui_rect_hit((EpdRect){A_OPT_X, q->opt_y[i], A_OPT_W, q->opt_h[i]}, x, y)) {
                if (qq->multi) {              /* 多选：勾选/取消，不判分 */
                    q->pick[i] = !q->pick[i];
                    q->multi_armed = q->pick[0] || q->pick[1] || q->pick[2] || q->pick[3];
                } else {
                    answer(q, i);
                }
                return true;
            }
        if (qq->multi && q->multi_confirm_y >= 0 &&
            ui_rect_hit((EpdRect){QZ_BX, q->multi_confirm_y, QZ_BW, QZ_BH}, x, y)) {
            if (q->multi_armed) answer(q, -1);  /* 未勾选不确认 */
            return true;
        }
        if (ui_rect_hit((EpdRect){A_SKIP_X, q->row_y, A_SKIP_W, A_OPT_H}, x, y)) {
            skip_cur(q);
            return true;
        }
        if (ui_rect_hit((EpdRect){A_MENU_X, q->row_y, A_MENU_W, A_OPT_H}, x, y)) {
            q->phase = QZ_MENU;
            return true;
        }
        return false;
    }
    case QZ_FEED: {
        if (ui_rect_hit((EpdRect){F_NEXT_X, F_ROW_Y, F_NEXT_W, QZ_BH}, x, y)) {
            if (q->ex_page < q->ex_pages - 1) ++q->ex_page;  /* 长解析先翻页 */
            else advance(q);
            return true;
        }
        if (ui_rect_hit((EpdRect){F_MENU_X, F_ROW_Y, F_MENU_W, QZ_BH}, x, y)) {
            q->phase = QZ_MENU;
            return true;
        }
        return false;
    }
    case QZ_DONE:
    default: {
        if (ui_rect_hit((EpdRect){D_MENU_X, D_MENU_Y, D_MENU_W, QZ_BH}, x, y)) {
            q->phase = QZ_MENU;
            return true;
        }
        if (q->all_done &&
            ui_rect_hit((EpdRect){D_MENU_X, D_RESET_Y, D_MENU_W, QZ_BH}, x, y)) {
            reset_prog(q);
            q->phase = QZ_MENU;
            return true;
        }
        return false;
    }
    }
}
