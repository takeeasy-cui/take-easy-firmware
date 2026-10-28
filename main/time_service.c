/* SPDX-License-Identifier: Apache-2.0
 * 中文：S3 RTC 离线走时；在线每小时校准，离线每六小时短连已保存 WiFi。
 * 2026-10-28 新增运行期 RC 慢时钟周期重校准（页屿 PageIsle 同思路）：设备无网/
 * 未配 WiFi 时 NTP 校时永远够不着，内部 150kHz RC 漂移（实测约 2 分钟/天）
 * 裸奔累积。现由后台任务每 10 分钟用 40MHz 主晶振（±10ppm）重测 RC 周期并
 * 更新换算值——esp_clk_slowclk_cal_set 强制单调、不跳变（IDF esp_clk.c 实现保证）。
 * English: S3 RTC keeps offline time; NTP hourly online or every six hours offline.
 * 2026-10-28: periodic RC slow-clock recalibration using the 40 MHz main XTAL,
 * so wall time stays accurate even when NTP is unreachable.
 */
#include "time_service.h"
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_private/esp_clk.h"
#include "soc/rtc.h"
#include "read_pico_transfer.h"
#include "sleep.h"
#include "weather_service.h"

#define CLOCK_VALID_MIN 1704067200ULL
#define CLOCK_VALID_MAX 4102444799ULL
#define CLOCK_ONLINE_INTERVAL 3600ULL
#define CLOCK_OFFLINE_INTERVAL (6ULL * 3600)
#define CLOCK_RETRY_US (5LL * 60 * 1000000)
#define CLOCK_TRIM_PERIOD_MS (10ULL * 60 * 1000)  /* 运行期 RC 重校准周期 */
#define CLOCK_TRIM_CYCLES 8192                    /* ~60ms 测量时长，足够精确 */
static atomic_bool s_running;
static bool s_loaded;
static uint64_t s_last_sync;
static int64_t s_next_attempt_us = 15000000;

/* 运行期 RC 慢时钟重校准：用 40MHz 主晶振重测 RC 实际频率，更新 tick→微秒
 * 换算值。清醒状态的走时误差从 RC 裸漂（实测 ~2 分钟/天）降到主晶振量级
 * （±10ppm，每天 <2 秒）。浅睡段的睡前校准由 IDF esp_sleep 自带；深睡走 PMU RTC。
 * 与 NTP 校时互补：NTP 纠绝对值，本任务纠走时速率。 */
static void clock_trim_task(void* arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CLOCK_TRIM_PERIOD_MS));
        uint32_t cal = rtc_clk_cal(CLK_CAL_RTC_SLOW, CLOCK_TRIM_CYCLES);
        if (cal) {
            esp_clk_slowclk_cal_set(cal);
            ESP_LOGD("clock", "rc slow clock recalibrated: %u", cal);
        }
    }
}

void app_clock_trim_start(void) {
    static bool started;
    if (started) return;
    started = true;
    if (xTaskCreate(clock_trim_task, "clk_trim", 2560, NULL, 1, NULL) != pdPASS)
        ESP_LOGW("clock", "clock trim task not started");
}

static void clock_worker(void *arg) {
    (void)arg;
    uint32_t utc = 0;
    esp_err_t err = read_pico_transfer_sync_time_auto(&utc);
    ESP_LOGI("clock", "background calibration: %s", esp_err_to_name(err));
    /* 天气与对时同窗对齐（用户 2026-10-19）：对时后日期可能刚滚进新的一天，
     * 此刻检查天气缓存是否覆盖今天——不覆盖则同窗口抓三天预报。
     * 复用同一次网络在线窗口（在线=NTP 后网络仍就绪直接 HTTPS；
     * 离线路径对时本来就要起网，NTP 成功时窗口还在），避免天气再等
     * 首页 30s 轮询、也避免为天气单独多起一次连接。
     * weather_service_fetch_sync 自带退避与缓存覆盖判定，幂等无副作用。 */
    if (err == ESP_OK) (void)weather_service_fetch_sync();
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

void app_time_service_poll(bool idle) {
    if (!idle) return;
    if (!s_loaded) {
        nvs_handle_t handle;
        if (nvs_open("kiiko_clock", NVS_READONLY, &handle) == ESP_OK) {
            if (nvs_get_u64(handle, "last_ntp", &s_last_sync) != ESP_OK) s_last_sync = 0;
            nvs_close(handle);
        }
        if (s_last_sync < CLOCK_VALID_MIN || s_last_sync > CLOCK_VALID_MAX) s_last_sync = 0;
        s_loaded = true;
    }
    uint32_t received;
    if (read_pico_transfer_take_time_sync(&received)) {
        // 只有真实网络成功才持久化；普通重启/手动设时间不能伪装成网络校准。
        // Persist only a real network success; reboot/manual edits do not count as calibration.
        s_last_sync = received;
        (void)app_sync_time_to_pmu();
        nvs_handle_t handle;
        if (nvs_open("kiiko_clock", NVS_READWRITE, &handle) == ESP_OK) {
            if (nvs_set_u64(handle, "last_ntp", s_last_sync) == ESP_OK) (void)nvs_commit(handle);
            nvs_close(handle);
        }
    }
    const int64_t monotonic = esp_timer_get_time();
    if (atomic_load(&s_running) || monotonic < s_next_attempt_us) return;
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    bool online = status.state == READ_PICO_TRANSFER_READY &&
                  status.mode == READ_PICO_TRANSFER_MODE_STA && status.network_ready;
    if (!online && status.state != READ_PICO_TRANSFER_STOPPED) return;
    time_t now = time(NULL);
    uint64_t interval = online ? CLOCK_ONLINE_INTERVAL : CLOCK_OFFLINE_INTERVAL;
    if (now >= (time_t)s_last_sync && s_last_sync && (uint64_t)now - s_last_sync < interval) return;
    s_next_attempt_us = monotonic + CLOCK_RETRY_US;
    if (!online) {
        char ssid[33]; bool configured = false;
        if (read_pico_transfer_get_saved_wifi(ssid, &configured) != ESP_OK || !configured) return;
    }
    atomic_store(&s_running, true);
    // 仅对时期间保留任务栈；不创建常驻无线任务或页面缓存。
    // Keep a task stack only during calibration; no permanent radio task or page cache.
    if (xTaskCreate(clock_worker, "rtc_sync", 6144, NULL, 1, NULL) != pdPASS) {
        atomic_store(&s_running, false);
        ESP_LOGW("clock", "background calibration deferred: no task memory");
    }
}
