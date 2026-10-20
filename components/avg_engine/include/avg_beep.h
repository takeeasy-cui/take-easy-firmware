/*
 * avg_beep.h — 蜂鸣音效：非阻塞音调序列（底层 = read_pico_buzzer_tone，
 * GPIO2 LEDC PWM；本模块不含任何音频解码）。
 *
 * 事件→ 音调序列 全部走可配置表：默认表覆盖本作事件（异议/威慑/出示成
 * 功/失败/存档/选择），换作品只需 avg_beep_set_table 覆盖，或直接改表。
 * 播放在独立低优先级任务里逐音调用阻塞 tone，主线程只投递——对渲染
 * 完全非阻塞。
 *
 * Non-blocking note sequences over the LEDC buzzer. Event→notes is a
 * configurable table; playback runs on a low-priority worker task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVG_BEEP_MAX_NOTES 8

typedef struct { uint16_t freq_hz; uint16_t duration_ms; } avg_beep_note_t;

/* 事件音定义（可配置表）。 / SFX event ids (configurable table). */
typedef enum {
    AVG_SFX_OBJECTION = 0,   /* 异议！ */
    AVG_SFX_PRESS,           /* 威慑/追问 */
    AVG_SFX_EV_OK,           /* 出示证据·击破 */
    AVG_SFX_EV_FAIL,         /* 出示错误 */
    AVG_SFX_SAVE,            /* 存档提示 */
    AVG_SFX_SELECT,          /* 界面选择 */
    AVG_SFX_COUNT
} avg_sfx_t;

typedef struct {
    avg_beep_note_t notes[AVG_BEEP_MAX_NOTES];
    int count;
} avg_beep_seq_t;

/// 覆盖默认事件表（拷贝一份到模块内，调用后表本体可释放）。
/// Replace the default event table (contents are copied in).
void avg_beep_set_table(const avg_beep_seq_t* table /* [AVG_SFX_COUNT] */);

/// 非阻塞播放一个事件音。busy 时丢弃新请求（提示音不排队碾压）。
/// Enqueue an event non-blocking; dropped if already playing.
void avg_beep_play(avg_sfx_t sfx);

bool avg_beep_busy(void);

/// 静音开关（2026-10-18，标题页可切换）：默认开。关=play 全部丢弃；
/// stop 不受影响（收尾落低照常）。平台负责持久化。
void avg_beep_set_enabled(bool on);
bool avg_beep_enabled(void);

/// 停止当前序列并让蜂鸣器落低（含锁屏前的收尾）。
/// Stop and drive the buzzer low (used before lock/sleep).
void avg_beep_stop(void);

#ifdef __cplusplus
}
#endif
