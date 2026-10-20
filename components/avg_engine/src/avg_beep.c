/*
 * avg_beep.c — 事件表默认值 + 后台播放任务（FreeRTOS 队列，一次创建）。
 */
#include "avg_beep.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "read_pico_buzzer.h"

/* 默认事件表：逆转裁判风格短提示音（频率 Hz / 时长 ms）。 */
static const avg_beep_seq_t k_default_table[AVG_SFX_COUNT] = {
    [AVG_SFX_OBJECTION] = { .notes = {{1320, 120}, {880, 200}, {1320, 260}}, .count = 3 },
    [AVG_SFX_PRESS]     = { .notes = {{660, 140}, {520, 160}},               .count = 2 },
    [AVG_SFX_EV_OK]     = { .notes = {{784, 90}, {988, 90}, {1319, 240}},    .count = 3 },
    [AVG_SFX_EV_FAIL]   = { .notes = {{440, 160}, {330, 240}},               .count = 2 },
    [AVG_SFX_SAVE]      = { .notes = {{988, 90}, {988, 90}},                 .count = 2 },
    [AVG_SFX_SELECT]    = { .notes = {{880, 60}},                            .count = 1 },
};

static avg_beep_seq_t s_table[AVG_SFX_COUNT];
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_enabled = true;     /* 默认开；标题页开关可切换（2026-10-18） */

static void beep_worker(void* arg) {
    (void)arg;
    avg_sfx_t sfx;
    for (;;) {
        if (xQueueReceive(s_queue, &sfx, portMAX_DELAY) != pdTRUE) continue;
        if (sfx < 0 || sfx >= AVG_SFX_COUNT) continue;
        const avg_beep_seq_t* seq = &s_table[sfx];
        for (int i = 0; i < seq->count; ++i) {
            (void)read_pico_buzzer_tone(seq->notes[i].freq_hz, seq->notes[i].duration_ms);
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void ensure_worker(void) {
    if (s_task) return;
    s_queue = xQueueCreate(4, sizeof(avg_sfx_t));
    if (!s_queue) return;
    if (xTaskCreate(beep_worker, "avg_beep", 3072, NULL, 1, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
    }
}

void avg_beep_set_table(const avg_beep_seq_t* table) {
    if (!table) {
        memcpy(s_table, k_default_table, sizeof(s_table));
        return;
    }
    for (int i = 0; i < AVG_SFX_COUNT; ++i) s_table[i] = table[i];
}

void avg_beep_play(avg_sfx_t sfx) {
    if (!s_enabled) return;                       /* 静音开：全部丢弃 */
    if (!s_table[0].count && !s_table[1].count) memcpy(s_table, k_default_table, sizeof(s_table));
    if (!s_task) ensure_worker();
    if (!s_queue) return;
    if (uxQueueMessagesWaiting(s_queue)) return; /* 忙则丢，不排队 */
    xQueueSend(s_queue, &sfx, 0);
}

bool avg_beep_busy(void) {
    return s_task && s_queue && uxQueueMessagesWaiting(s_queue) > 0;
}

void avg_beep_set_enabled(bool on) {
    s_enabled = on;
    if (!on) avg_beep_stop();
}

bool avg_beep_enabled(void) { return s_enabled; }

void avg_beep_stop(void) {
    if (s_queue) xQueueReset(s_queue);
    (void)read_pico_buzzer_score_stop();
}
