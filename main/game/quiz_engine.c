/*
 * quiz_engine.c — 题库闯关引擎实现。
 *
 * 题库格式（UTF-8，一行一令，// 注释）：
 *   * title 2026应知应会大闯关       标题（列表显示名）
 *   Q 题干
 *   A 选项一 … D 选项四
 *   R C                              正确项字母
 *   E 解析
 * 进度文件 .quizprog：首行题数，其后每行 "idx streak skipped wronged"。
 *
 * 规则（2026-10-27 用户需求）：
 *   - 每次练习从可出题池（未跳过且未毕业）随机洗牌出题；
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

#include "game_core.h"
#include "quiz_engine.h"

#define QZ_MAX_Q          512
#define QZ_TEXT           256   /* 题干/选项拷贝上限（B） */
#define QZ_EX             512   /* 解析拷贝上限（B） */
#define QZ_MASTER_STREAK  3
#define QZ_MASTER_DESC    "连续答对3次毕业"

typedef struct {
    int     l_q, l_a, l_b, l_c, l_d, l_e;   /* 行号；-1=缺 */
    char    correct;                        /* 'A'..'D' */
    int16_t streak;                         /* 连对次数（3=毕业） */
    uint8_t skipped;                        /* 永不再出 */
    uint8_t wronged;                        /* 曾经答错过（错题集候选） */
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
    char      last_letter;
    bool      last_master;
    bool      reset_arm;                    /* 菜单重置二次确认 */
    bool      all_done;                     /* 池空（全部掌握/错题集空） */
    char      note[112];
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
                cur->correct = 'A';
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
                char r[8];
                line_copy(e, i, r, sizeof(r));
                if (r[0] >= 'A' && r[0] <= 'D') cur->correct = r[0];
                else if (r[0] >= 'a' && r[0] <= 'd') cur->correct = (char)(r[0] - 32);
                break;
            }
            default: break;
            }
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
}

static void answer(quiz_engine_t* e, int opt) {
    quiz_q_t* q = &e->qs[e->cur];
    char letter = (char)('A' + opt);
    e->last_letter = letter;
    e->last_correct = letter == q->correct;
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

/* 菜单/按钮几何（tap 与 paint 共用） */
#define QZ_BX   48
#define QZ_BW   588
#define QZ_BH   64
#define M_Y_PRACTICE  600
#define M_Y_WRONG     692
#define M_Y_RESET     784
#define A_OPT_X 54
#define A_OPT_W 576
#define A_OPT_H 56
#define A_OPT_Y0 596
#define A_OPT_STEP 64
#define A_ROW_Y 892          /* 跳过/菜单 行 */
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
        char head[64];
        snprintf(head, sizeof(head), "第 %d / %d 题 · 本轮已对 %d",
                 e->order_pos + 1, e->order_count, e->correct);
        ui_text_vc(fb, 342, 60, 20, head, EPD_DRAW_ALIGN_CENTER, false);
        char qt[QZ_TEXT];
        line_copy(e, e->qs[e->cur].l_q, qt, sizeof(qt));
        gc_ui_typewrap(qt, (int)strlen(qt), 4, 44, draw26, fb, 106, 46);
        static const char* letters[4] = { "A", "B", "C", "D" };
        for (int i = 0; i < 4; ++i) {
            char lab[264];
            char opt[192];
            int li = i == 0 ? e->qs[e->cur].l_a
                  : i == 1 ? e->qs[e->cur].l_b
                  : i == 2 ? e->qs[e->cur].l_c : e->qs[e->cur].l_d;
            line_copy(e, li, opt, sizeof(opt));
            snprintf(lab, sizeof(lab), "%s. %s", letters[i], opt);
            gc_ui_button(fb, (EpdRect){A_OPT_X, A_OPT_Y0 + i * A_OPT_STEP, A_OPT_W, A_OPT_H},
                         lab, false);
        }
        gc_ui_button(fb, (EpdRect){A_SKIP_X, A_ROW_Y, A_SKIP_W, A_OPT_H},
                     "跳过此题（永不再出）", false);
        gc_ui_button(fb, (EpdRect){A_MENU_X, A_ROW_Y, A_MENU_W, A_OPT_H}, "菜单", false);
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
            char w[64];
            snprintf(w, sizeof(w), "答错了，正确答案 %c", e->qs[e->cur].correct);
            ui_text_vc(fb, 342, 160, 40, w, EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 230, 22, "已放入错题集，可在菜单重练",
                       EPD_DRAW_ALIGN_CENTER, false);
        }
        char ex[QZ_EX];
        line_copy(e, e->qs[e->cur].l_e, ex, sizeof(ex));
        gc_ui_typewrap(ex, (int)strlen(ex), 8, 44, draw26, fb, 300, 44);
        bool last = e->order_pos >= e->order_count - 1;
        gc_ui_button(fb, (EpdRect){F_NEXT_X, F_ROW_Y, F_NEXT_W, QZ_BH},
                     last ? "查看结果" : "下一题", true);
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
        for (int i = 0; i < 4; ++i)
            if (ui_rect_hit((EpdRect){A_OPT_X, A_OPT_Y0 + i * A_OPT_STEP, A_OPT_W, A_OPT_H}, x, y)) {
                answer(q, i);
                return true;
            }
        if (ui_rect_hit((EpdRect){A_SKIP_X, A_ROW_Y, A_SKIP_W, A_OPT_H}, x, y)) {
            skip_cur(q);
            return true;
        }
        if (ui_rect_hit((EpdRect){A_MENU_X, A_ROW_Y, A_MENU_W, A_OPT_H}, x, y)) {
            q->phase = QZ_MENU;
            return true;
        }
        return false;
    }
    case QZ_FEED: {
        if (ui_rect_hit((EpdRect){F_NEXT_X, F_ROW_Y, F_NEXT_W, QZ_BH}, x, y)) {
            advance(q);
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
