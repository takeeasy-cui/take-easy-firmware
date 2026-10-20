/* SPDX-License-Identifier: Apache-2.0
 * 中文：心知天气 v3 实现。复用 weread 已验证的 esp_http_client+crt_bundle
 * HTTPS 链路与 cJSON（依赖已在）；配置源 NVS 优先、回退 TF 卡 weather.txt；
 * NVS 缓存三天预报；失败 10 分钟退避。
 * English: Seniverse v3 implementation on the proven weread HTTP stack
 * (esp_http_client + crt_bundle, cJSON already in deps); config from NVS
 * first, TF card weather.txt as fallback; NVS caches the 3-day forecast;
 * failures back off for ten minutes.
 */
#include "weather_service.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "read_pico_sd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "weather";

#define WX_HOST "api.seniverse.com"
#define WX_PATH "/v3/weather/daily.json?language=zh-Hans&unit=c&days=3"
#define WX_CACHE_NVS "wx_cache"
#define WX_CFG_NVS "wx_cfg"
#define WX_BACKOFF_MS (10u * 60u * 1000u)

typedef struct {
    int code;               /* weather_code_t */
    int high, low;
    char date[12];          /* YYYY-MM-DD */
} wx_day_t;

static char s_city_cfg[48];     /* 生效配置的地点（NVS 优先，回退 weather.txt） */
static char s_key[40];
static bool s_cfg_loaded;
static weather_config_source_t s_src;   /* 当前生效配置源 */
static wx_day_t s_days[3];
static char s_city[48];         /* API 规范城市名 */
static int s_day_count;
static bool s_task_running;
static int64_t s_last_attempt_ms;
static int64_t s_refresh_done_ms;   /* 完成时刻，tick 判定"刚完成" */
static bool s_fetch_ok;

/* NVS 配置（设置页写入；优先于 TF 卡 weather.txt，用户 2026-10-19）。
 * 任一字段有值即视为记录存在；返回 true = 读到了至少一个字段。 */
static bool cfg_nvs_load(char *city, size_t city_cap, char *key, size_t key_cap) {
    if (!city || !city_cap || !key || !key_cap) return false;
    city[0] = 0; key[0] = 0;
    nvs_handle_t h;
    if (nvs_open(WX_CFG_NVS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = city_cap;
    if (nvs_get_str(h, "city", city, &len) != ESP_OK) city[0] = 0;
    len = key_cap;
    if (nvs_get_str(h, "key", key, &len) != ESP_OK) key[0] = 0;
    nvs_close(h);
    return city[0] || key[0];
}

/* 心知文本 → 图标码。雨/雪族归并，避免为每档做字模。 */
static int text_to_code(const char *text) {
    if (!text || !text[0]) return WEATHER_CODE_UNKNOWN;
    if (strstr(text, "雪")) return WEATHER_CODE_SNOW;
    if (strstr(text, "雨")) return WEATHER_CODE_RAIN;
    if (strstr(text, "雷")) return WEATHER_CODE_RAIN;
    if (strstr(text, "雾") || strstr(text, "霾") || strstr(text, "尘") || strstr(text, "沙")) return WEATHER_CODE_FOG;
    if (strstr(text, "阴")) return WEATHER_CODE_CLOUDY;
    if (strstr(text, "云")) return WEATHER_CODE_PARTLY;
    if (strstr(text, "晴")) return WEATHER_CODE_SUNNY;
    return WEATHER_CODE_UNKNOWN;
}

static void load_config(void) {
    if (s_cfg_loaded) return;
    /* NVS 优先（设置页写入，用户 2026-10-19）；未命中再回退 TF 卡 weather.txt。 */
    char nvs_city[sizeof(s_city_cfg)] = {0}, nvs_key[sizeof(s_key)] = {0};
    if (cfg_nvs_load(nvs_city, sizeof(nvs_city), nvs_key, sizeof(nvs_key))) {
        snprintf(s_city_cfg, sizeof(s_city_cfg), "%s", nvs_city);
        snprintf(s_key, sizeof(s_key), "%s", nvs_key);
        s_src = WEATHER_SRC_NVS;
        s_cfg_loaded = true;
        ESP_LOGI(TAG, "cfg(nvs) city=%s key=%u chars", s_city_cfg, (unsigned)strlen(s_key));
        return;
    }
    read_pico_sd_info_t sd = {0};
    if (read_pico_sd_get_info(&sd) == ESP_ERR_NOT_FINISHED) return;  /* 卡未就绪，稍后重试 */
    s_cfg_loaded = true;
    s_src = WEATHER_SRC_NONE;
    if (!sd.present || !sd.mounted) return;
    FILE *f = fopen("/sdcard/weather.txt", "r");
    if (!f) return;
    char line[128];
    bool got = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "city=", 5)) {
            snprintf(s_city_cfg, sizeof(s_city_cfg), "%.44s", line + 5);
            got = true;
        } else if (!strncmp(line, "key=", 4)) {
            snprintf(s_key, sizeof(s_key), "%.36s", line + 4);
            got = true;
        }
    }
    fclose(f);
    for (char *p = s_city_cfg; *p; ++p) if (*p == '\r' || *p == '\n') *p = 0;
    for (char *p = s_key; *p; ++p) if (*p == '\r' || *p == '\n') *p = 0;
    if (got) s_src = WEATHER_SRC_TF;
    ESP_LOGI(TAG, "cfg(tf) city=%s key=%u chars", s_city_cfg, (unsigned)strlen(s_key));
}

/* NVS 缓存（单 blob：天数+城市+逐天；无实况字段——当前温度已取消，用户 2026-10-19）。 */
static void cache_save(void) {
    nvs_handle_t h;
    if (nvs_open(WX_CACHE_NVS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t size = sizeof(int) + sizeof(s_city) + sizeof(s_days);
    uint8_t *buf = malloc(size);
    if (buf) {
        memcpy(buf, &s_day_count, sizeof(int));
        memcpy(buf + sizeof(int), s_city, sizeof(s_city));
        memcpy(buf + sizeof(int) + sizeof(s_city), s_days, sizeof(s_days));
        if (nvs_set_blob(h, "d3", buf, size) == ESP_OK) (void)nvs_commit(h);
        free(buf);
    }
    nvs_close(h);
}

static void cache_load(void) {
    nvs_handle_t h;
    if (nvs_open(WX_CACHE_NVS, NVS_READONLY, &h) != ESP_OK) return;
    size_t size = sizeof(int) + sizeof(s_city) + sizeof(s_days);
    uint8_t *buf = malloc(size);
    if (buf) {
        if (nvs_get_blob(h, "d3", buf, &size) == ESP_OK && size >= sizeof(int)) {
            memcpy(&s_day_count, buf, sizeof(int));
            memcpy(s_city, buf + sizeof(int), sizeof(s_city));
            memcpy(s_days, buf + sizeof(int) + sizeof(s_city), sizeof(s_days));
            if (s_day_count < 0 || s_day_count > 3) s_day_count = 0;
        }
        free(buf);
    }
    nvs_close(h);
}

/* 北京日期 YYYY-MM-DD（手工拼装，绕开 snprintf 长度告警）。 */
static void today_bj(char *buf, size_t cap) {
    if (!buf || cap < 11) { if (buf && cap) buf[0] = 0; return; }
    time_t now = time(NULL);
    struct tm tm_bj = {0};
    if (now < 946684800) { buf[0] = 0; return; }
    time_t bj = now + 8 * 3600;
    gmtime_r(&bj, &tm_bj);
    int y = tm_bj.tm_year + 1900, m = tm_bj.tm_mon + 1, d = tm_bj.tm_mday;
    if (y < 0 || y > 9999 || m < 1 || m > 99 || d < 1 || d > 99) { buf[0] = 0; return; }
    buf[0] = (char)('0' + y / 1000);
    buf[1] = (char)('0' + y / 100 % 10);
    buf[2] = (char)('0' + y / 10 % 10);
    buf[3] = (char)('0' + y % 10);
    buf[4] = '-';
    buf[5] = (char)('0' + m / 10);
    buf[6] = (char)('0' + m % 10);
    buf[7] = '-';
    buf[8] = (char)('0' + d / 10);
    buf[9] = (char)('0' + d % 10);
    buf[10] = 0;
}

static bool covered_today(void) {
    char today[16];
    today_bj(today, sizeof(today));
    if (!today[0]) return false;
    for (int i = 0; i < s_day_count; ++i)
        if (!strcmp(s_days[i].date, today)) return true;
    return false;
}

static int today_index(void) {
    char today[16];
    today_bj(today, sizeof(today));
    if (!today[0]) return -1;
    for (int i = 0; i < s_day_count; ++i)
        if (!strcmp(s_days[i].date, today)) return i;
    return -1;
}

/* 抓三天预报（同步核心；任务壳与对时窗口直调共用，用户 2026-10-19）。
 * 返回 true = 成功且缓存已更新。当前温度已取消：仅取 code/high/low/date。 */
static bool fetch_forecast(void) {
    char url[256];
    snprintf(url, sizeof(url), "https://" WX_HOST WX_PATH "&location=%s&key=%s", s_city_cfg, s_key);
    esp_http_client_config_t config = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 12000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    char *body = NULL;
    size_t body_len = 0;
    bool ok = false;
    if (client) {
        if (esp_http_client_open(client, 0) == ESP_OK) {
            int read = esp_http_client_fetch_headers(client);
            int64_t total = esp_http_client_get_content_length(client);
            if (read >= 0 && total > 0 && total < 65536) {
                body = malloc((size_t)total + 1);
                if (body) {
                    while (body_len < (size_t)total) {
                        int n = esp_http_client_read(client, body + body_len, (int)((size_t)total - body_len));
                        if (n <= 0) break;
                        body_len += (size_t)n;
                    }
                    body[body_len] = 0;
                    ok = esp_http_client_get_status_code(client) == 200 && body_len == (size_t)total;
                }
            }
        }
        esp_http_client_cleanup(client);
    }
    if (ok) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            cJSON *results = cJSON_GetObjectItem(root, "results");
            cJSON *first = results ? cJSON_GetArrayItem(results, 0) : NULL;
            cJSON *location = first ? cJSON_GetObjectItem(first, "location") : NULL;
            cJSON *name = location ? cJSON_GetObjectItem(location, "name") : NULL;
            cJSON *daily = first ? cJSON_GetObjectItem(first, "daily") : NULL;
            if (daily && cJSON_IsArray(daily)) {
                s_day_count = cJSON_GetArraySize(daily);
                if (s_day_count > 3) s_day_count = 3;
                for (int i = 0; i < s_day_count; ++i) {
                    cJSON *d = cJSON_GetArrayItem(daily, i);
                    cJSON *dtext = cJSON_GetObjectItem(d, "text_day");
                    cJSON *dhigh = cJSON_GetObjectItem(d, "high");
                    cJSON *dlow = cJSON_GetObjectItem(d, "low");
                    cJSON *ddate = cJSON_GetObjectItem(d, "date");
                    if (!cJSON_IsString(dtext) || !cJSON_IsString(dhigh) || !cJSON_IsString(dlow)) continue;
                    s_days[i].code = text_to_code(dtext->valuestring);
                    s_days[i].high = atoi(dhigh->valuestring);
                    s_days[i].low = atoi(dlow->valuestring);
                    snprintf(s_days[i].date, sizeof(s_days[i].date), "%s",
                             cJSON_IsString(ddate) ? ddate->valuestring : "");
                }
                if (name && cJSON_IsString(name))
                    snprintf(s_city, sizeof(s_city), "%s", name->valuestring);
                s_fetch_ok = true;
                cache_save();
            }
            cJSON_Delete(root);
        }
    }
    free(body);
    if (!ok) ESP_LOGW(TAG, "forecast fetch failed");
    return ok;
}

static void fetch_task(void *arg) {
    (void)arg;
    s_fetch_ok = false;
    (void)fetch_forecast();
    s_refresh_done_ms = esp_timer_get_time() / 1000;
    s_task_running = false;
    vTaskDelete(NULL);
}

/* 对时窗口直调（time_service clock_worker 内）：对时后日期可能刚滚进新一天，
 * 缓存不覆盖今天才抓（正常 3 天一次）；同窗口复用网络在线状态，
 * 联网失败仅记退避。返回 true = 本次实际发起了抓取且成功。 */
bool weather_service_fetch_sync(void) {
    load_config();
    if (!s_city_cfg[0] || !s_key[0]) return false;
    if (!s_day_count) cache_load();
    if (s_task_running) return false;
    if (covered_today()) return false;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_attempt_ms && now_ms - s_last_attempt_ms < WX_BACKOFF_MS) return false;
    s_last_attempt_ms = now_ms;
    s_fetch_ok = false;
    bool ok = fetch_forecast();
    if (ok) s_refresh_done_ms = now_ms;
    return ok;
}

/* tick 判定"刚完成"：刷新任务结束（s_task_running 清零后）由下一次 tick 报告，
 * 主页据此局部重绘天气区。完成后一秒窗口内视为新。 */
static bool refresh_just_done(int64_t now_ms) {
    return s_refresh_done_ms && s_fetch_ok &&
           now_ms - s_refresh_done_ms < 1000 && now_ms >= s_refresh_done_ms;
}


/* 配置变更后立即生效：重载配置源、清三天缓存与退避——下一次 tick/对时窗口即抓取。
 * 设置页保存与恢复默认源共用（用户 2026-10-19："设置完即可使用"）。 */
static void weather_invalidate(void) {
    s_cfg_loaded = false;
    load_config();
    s_day_count = 0;
    s_city[0] = 0;
    memset(s_days, 0, sizeof(s_days));
    cache_save();                       /* 空缓存持久化，避免重启后旧城市回放 */
    s_last_attempt_ms = 0;
    s_fetch_ok = false;
    s_refresh_done_ms = 0;
}

weather_config_source_t weather_config_source(void) {
    load_config();
    return s_src;
}

bool weather_config_get(char *city, size_t city_cap, char *key, size_t key_cap) {
    if (!city || !city_cap || !key || !key_cap) return false;
    load_config();
    snprintf(city, city_cap, "%s", s_city_cfg);
    snprintf(key, key_cap, "%s", s_key);
    return s_city_cfg[0] || s_key[0];
}

esp_err_t weather_config_save(const char *city, const char *key) {
    if (!city || !key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(WX_CFG_NVS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "city", city);
    if (err == ESP_OK) err = nvs_set_str(h, "key", key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    weather_invalidate();
    return ESP_OK;
}

esp_err_t weather_config_reset(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(WX_CFG_NVS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, "city");
    esp_err_t err2 = nvs_erase_key(h, "key");
    if (err == ESP_OK) err = err2;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return err; }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    weather_invalidate();               /* 回退 TF 卡 weather.txt 或未配置 */
    return ESP_OK;
}

bool weather_configured(void) {
    load_config();
    return s_city_cfg[0] && s_key[0];
}

bool weather_valid(void) {
    if (!s_day_count) cache_load();
    int idx = today_index();
    return idx >= 0;
}

weather_code_t weather_code(void) {
    int idx = today_index();
    if (idx < 0) return WEATHER_CODE_SUNNY;
    return (weather_code_t)s_days[idx].code;
}

void weather_city(char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    if (!s_day_count) cache_load();
    snprintf(buf, cap, "%s", s_city[0] ? s_city : s_city_cfg);
}

int weather_high(void) { int i = today_index(); return i < 0 ? 0 : s_days[i].high; }
int weather_low(void) { int i = today_index(); return i < 0 ? 0 : s_days[i].low; }

bool weather_service_tick(bool network_ready) {
    load_config();
    if (!s_city_cfg[0] || !s_key[0] || !network_ready) return false;
    /* 完成上报（任务结束后的下一次 tick）：报告一次即清零，避免重复重绘。 */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (refresh_just_done(now_ms)) {
        s_refresh_done_ms = 0;
        return true;
    }
    if (s_day_count && covered_today()) return false;
    if (s_task_running) return false;
    if (s_last_attempt_ms && now_ms - s_last_attempt_ms < WX_BACKOFF_MS) return false;
    s_last_attempt_ms = now_ms;
    s_task_running = true;
    s_fetch_ok = false;
    if (xTaskCreate(fetch_task, "weather", 8192, NULL, 1, NULL) != pdPASS) {
        s_task_running = false;
        return false;
    }
    return false;
}
