/* SPDX-License-Identifier: Apache-2.0
 * 中文：ime_glue.h 实现——薄封装 IME 单例，C 接口无 C++ 类型泄漏。
 * English: Thin wrapper over the IME singleton exposing a plain C surface.
 */
#include "ime_glue.h"
#include "IME.h"
#include <string>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

static bool s_begun;

static IME &engine(void) { return IME::getInstance(); }

bool ime_glue_begin(void) {
    if (s_begun) return true;
    IME &inst = engine();
    if (!inst.begin()) return false;
    inst.setActive(true);
    inst.setPageSize(5);             /* 键盘候选行固定 5 格（visible() 几何） */
    /* 用户词典/预测落盘目录（TF 卡），先确保存在；无卡时写入失败由引擎静默跳过。 */
    mkdir("/sdcard/settings", 0775);
    s_begun = true;
    return true;
}

void ime_glue_end(void) {
    if (!s_begun) return;
    engine().cancelComposition();
    engine().flushUserDictSavesNow();
}

void ime_glue_reset(void) { engine().cancelComposition(); }

void ime_glue_set_layout(int layout) { IME::setAmbigLayout(layout); }

void ime_glue_set_english(bool on) { engine().setEnglish(on); }
bool ime_glue_english(void) { return engine().english(); }

static bool key_out(int key, char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    std::string text;
    if (!engine().handleKey(key, text)) return false;
    if (out && cap) snprintf(out, cap, "%s", text.c_str());
    return true;
}

bool ime_glue_key(int key, char *out, size_t cap) { return key_out(key, out, cap); }
bool ime_glue_ambig(int group, char *out, size_t cap) {
    return key_out(IME::KEY_AMBIG_BASE + group, out, cap);
}
bool ime_glue_select(int idx, char *out, size_t cap) {
    if (idx < 0 || idx > 8) return false;
    return key_out('1' + idx, out, cap);
}
bool ime_glue_backspace(char *out, size_t cap) { return key_out('\b', out, cap); }

bool ime_glue_page_next(void) { return engine().nextPage(); }
bool ime_glue_page_prev(void) { return engine().prevPage(); }

void ime_glue_predict_begin(const char *text) {
    if (text && text[0]) engine().beginPredict(text);
}
void ime_glue_doc_context(const char *text) {
    if (text) engine().setDocumentContext(text);
}
void ime_glue_tick_idle(long idle_us) { engine().tickDeferredSaves(idle_us); }

bool ime_glue_composing(void) { return engine().composing(); }
bool ime_glue_predicting(void) { return engine().predicting(); }

size_t ime_glue_composition(char *buf, size_t cap) {
    if (!buf || !cap) return 0;
    const std::string &code = engine().displayCode();
    snprintf(buf, cap, "%s", code.c_str());
    return strlen(buf);
}

int ime_glue_candidate_count(void) {
    return (int)engine().candidates().size();
}
int ime_glue_candidate(int idx, char *buf, size_t cap) {
    if (!buf || !cap) return -1;
    const std::vector<std::string> &cands = engine().candidates();
    if (idx < 0 || idx >= (int)cands.size()) return -1;
    snprintf(buf, cap, "%s", cands[idx].c_str());
    return (int)strlen(buf);
}
int ime_glue_page_number(void) { return engine().currentPage(); }
int ime_glue_page_total(void) { return engine().totalPages(); }

int ime_glue_ambig_syllable(int idx, char *buf, size_t cap) {
    if (!buf || !cap) return -1;
    const std::vector<std::string> &syl = engine().ambigSyllables();
    if (idx < 0 || idx >= (int)syl.size()) return -1;
    snprintf(buf, cap, "%s", syl[idx].c_str());
    return (int)strlen(buf);
}
bool ime_glue_ambig_select(int idx) { return engine().selectAmbigCode(idx); }
bool ime_glue_ambig_active(void) { return engine().ambigActive(); }

const char *ime_glue_status(void) {
    static char status[96];
    status[0] = 0;
    std::string msg = engine().takeStatusMessage();
    if (!msg.empty()) snprintf(status, sizeof(status), "%s", msg.c_str());
    return status;
}
