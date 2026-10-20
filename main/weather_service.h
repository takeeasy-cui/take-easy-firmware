/* SPDX-License-Identifier: Apache-2.0
 * 中文：首页真实天气（用户 2026-10-19）：心知天气 v3 API（免费档）。
 * 功耗策略：一次联网抓 3 天预报（含今天），缓存管 3 天——离线期间按日期取
 * 对应天显示，仅当"今天"超出缓存覆盖且联网时才再次联网（正常 3 天一次）。
 * 配置源（用户 2026-10-19）：设置页写入 NVS 后优先；未设置时回退 TF 卡
 * /sdcard/weather.txt（city=/key= 两行）。显示仅今日天气码与最高/最低气温
 * （用户 2026-10-19 取消当前温度，不为实况联网）。
 * English: Real home-page weather via the Seniverse v3 API (free tier).
 * Power strategy: one online fetch grabs a 3-day forecast (incl. today),
 * cached for 3 days; a new fetch happens only when "today" falls outside the
 * covered days and the network is up (normally once per 3 days). Config lives
 * in /sdcard/weather.txt (city=/key=). Only the day code and the high/low are
 * shown (no live temperature by design).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEATHER_CODE_SUNNY = 0,    /* 晴 / clear */
    WEATHER_CODE_PARTLY,  /* 多云 / partly cloudy */
    WEATHER_CODE_CLOUDY,  /* 阴 / overcast */
    WEATHER_CODE_FOG,          /* 雾 / haze, fog */
    WEATHER_CODE_RAIN,         /* 雨（小雨/中雨/大雨等） / rain family */
    WEATHER_CODE_SNOW,         /* 雪 / snow family */
    WEATHER_CODE_UNKNOWN,      /* 未知文本 / unknown text */
} weather_code_t;

/* 配置源（用户 2026-10-19）：设置页写入 NVS 后优先；未设置时回退 TF 卡 weather.txt。 */
typedef enum {
    WEATHER_SRC_NONE = 0,      /* 未配置 / no config */
    WEATHER_SRC_NVS,           /* 设备设置（NVS，优先） / on-device settings */
    WEATHER_SRC_TF,            /* TF 卡 weather.txt（默认源） / default source */
} weather_config_source_t;

/* 数据服务商（用户 2026-10-20 第二十一批 e 段，三源可选）：
 * 心知=城市名+私钥；和风=城市名+key（GeoAPI 自动查地点 ID）；彩云=经纬度+令牌。 */
typedef enum {
    WEATHER_PROVIDER_SENIVERSE = 0,   /* 心知天气 v3（默认） */
    WEATHER_PROVIDER_QWEATHER = 1,   /* 和风天气 v7 */
    WEATHER_PROVIDER_CAIYUN = 2,      /* 彩云天气 v2.6（地点填经纬度，如 116.41,39.9） */
} weather_provider_t;

/// 当前数据服务商（默认心知；NVS "prov"，TF weather.txt 的 prov= 行亦可）。
weather_provider_t weather_config_provider(void);
/// 只切换服务商（地点/Key 不动）：写 NVS 并清缓存退避，下次联网即按新源抓取。
esp_err_t weather_config_save_provider(weather_provider_t p);

/// 当前生效配置源（设置页标注用）。
weather_config_source_t weather_config_source(void);
/// 读取当前生效配置（地点与 Key；NVS 优先，回退 TF 卡）；返回是否有任一字段。
bool weather_config_get(char *city, size_t city_cap, char *key, size_t key_cap);
/// 保存配置到 NVS（此后优先于 TF 卡）；立即生效：重载配置并清三天缓存与退避。
esp_err_t weather_config_save(const char *city, const char *key);
/// 恢复默认源：清除 NVS 配置，回退 TF 卡 weather.txt；无 NVS 配置则视为已默认。
esp_err_t weather_config_reset(void);

/// 是否已配置（city 与 key 均非空）。
bool weather_configured(void);
/// 是否取到过至少一次有效天气（NVS 缓存或本次会话内成功）。
bool weather_valid(void);
/// 今日天气码（缓存按当前北京日期取天）。/ Today's code from the cached day.
weather_code_t weather_code(void);
/// 城市：API 规范名优先，无缓存时回退配置名；都没有则空串。
void weather_city(char *buf, size_t cap);
/// 今日最高/最低气温（°C，可为负）。/ Today's high/low in degrees C.
int weather_high(void);
int weather_low(void);
/// 首页 30 秒节流处调用：联网且"今天"超出缓存覆盖时后台刷新（低优先任务）。
/// 返回 true = 刚完成一次刷新（调用方局部重绘天气区）。
bool weather_service_tick(bool network_ready);
/// 对时窗口直调（time_service）：缓存不覆盖今天才同窗口抓取（正常 3 天一次联网）。
/// 主动对齐对时连接，不为天气单独起连接；返回 true = 实际抓取且成功。
bool weather_service_fetch_sync(void);

#ifdef __cplusplus
}
#endif
