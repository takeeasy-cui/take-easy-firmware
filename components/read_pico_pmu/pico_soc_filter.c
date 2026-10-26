/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：五样本中位数与有界高段下降，拔电回落不伪装成瞬时耗电；无电流计不声称精确容量。
 * English: A five-sample median and bounded upper-range descent reject unplug sag, without claiming coulomb-counter accuracy.
 * 移植自上游 wegooo-cell/read-pico-reader rc98（c543e2b，2026-10-25 吸收；许可证条款随文件保留）。 */
#include "pico_soc_filter.h"
#include <string.h>
uint16_t pico_soc_filter_update(pico_soc_filter_t *s, uint16_t raw, uint16_t mv,
                               bool powered, bool full, bool low, uint32_t now) {
    if (!s || raw > 1000) return raw;
    if (!s->valid || (uint32_t)(now - s->sampled_ms) > 60000) {
        *s = (pico_soc_filter_t){.shown = raw, .sampled_ms = now, .changed_ms = now,
                                 .powered = powered, .valid = true};
    }
    if (s->powered != powered) {
        s->powered = powered; s->changed_ms = now; s->count = s->next = 0; s->budget_ms = 0;
    }
    /* 低电或电压异常不延迟；真正充满只认 PMU 状态，不由高电压猜测。 */
    if (low || raw <= 200 || (mv && mv <= 3500) || (full && raw >= 995)) {
        s->shown = raw; s->sampled_ms = now; s->count = s->next = 0; s->budget_ms = 0;
        return raw;
    }
    uint32_t elapsed = now - s->sampled_ms;
    if (elapsed < 1000) return s->shown;
    s->sampled_ms = now;
    s->samples[s->next] = raw; s->next = (s->next + 1) % 5; if (s->count < 5) ++s->count;
    uint16_t sorted[5]; memcpy(sorted, s->samples, s->count * sizeof(*sorted));
    for (unsigned i = 1; i < s->count; ++i) {
        uint16_t v = sorted[i]; unsigned j = i;
        while (j && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; --j; }
        sorted[j] = v;
    }
    uint16_t target = sorted[s->count / 2];
    /* 拔电瞬间的高读数回落先观察不升也不追，电压松弛自己会回来。 */
    if (!powered && s->shown >= 850 && raw >= 800 && mv >= 3900 && target > s->shown) {
        s->budget_ms = 0; return s->shown;
    }
    bool upper = s->shown >= 850 && raw >= 800 && target >= 800 && s->shown >= target &&
        s->shown - raw <= 150 && s->shown - target <= 150 && mv >= 3900;
    if (!upper) { s->shown = raw; s->budget_ms = 0; return raw; }
    if ((uint32_t)(now - s->changed_ms) < 30000 || s->count < 3) { s->budget_ms = 0; return s->shown; }
    if (target == s->shown) { s->budget_ms = 0; return s->shown; }
    /* 只按实际采样间隔积累，不按轮询次数，也不补发深睡/长断线期间的下降。 */
    s->budget_ms += elapsed > 2000 ? 2000 : elapsed;
    uint16_t step = s->budget_ms / 1000; s->budget_ms %= 1000;
    s->shown -= s->shown - target < step ? s->shown - target : step;
    return s->shown;
}
