/*
 * 闹钟服务（用户 2026-10-20 第二十一批 c 段）。
 *
 * 硬件架构：闹钟走 PMU（CW32L010）RTC，独立于主机电源状态——
 *  - 主机开机/浅睡：到点推 PMU_EVT_ALARM_FIRED，事件被任何抽阅路径锁存
 *    （read_pico_pmu_take_alarm），由 app_loop / 锁屏循环消费响铃；
 *  - 主机关机/深睡（PMU 断主机 EN）：到点 PMU 自动拉起主机（wake_reason=6），
 *    app_main 开机时响铃。
 * 每日循环：PMU 只设单次（mode 1，官方 demo 已验证路径），每次响铃停止后由
 * 主机重排「下一个 HH:MM」；开机时 alarm_pmu_arm() 幂等自愈（CW 断电/复位后
 * PMU 闹钟丢失，协议明示）。
 * 闹钟独立于锁屏模式：不切锁屏、不依赖时钟锁屏（用户 2026-10-20 拍板）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cst836u.h"
#include "epd_highlevel.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 按 settings 武装/关闭 PMU 硬件闹钟（幂等，可反复调用）。
esp_err_t alarm_pmu_arm(void);

/// 响铃页：整页闹钟画面 + 蜂鸣循环（触摸或电源键停止，~2.5 分钟自动停），
/// 停止后重排下一个 HH:MM。画面由调用方在返回后自行恢复。
void alarm_ring_page(EpdiyHighlevelState* hl, uint8_t* fb, cst836u_handle_t tp);

/// 锁屏循环醒来时调用：latch 置位则响铃并返回 true（调用方重画锁屏画面）。
bool alarm_consume_ring(EpdiyHighlevelState* hl, uint8_t* fb, cst836u_handle_t tp);

/// 开机流程：本次开机是否由闹钟触发（wake_reason=6 或事件锁存）。取走锁存。
bool alarm_boot_should_ring(void);

/// 浅睡对齐：闹钟武装时返回距下一个 HH:MM 的秒数（≥1），未武装或无时间基准返回 0。
uint32_t alarm_next_sleep_sec(void);

#ifdef __cplusplus
}
#endif
