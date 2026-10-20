/*
 * SPDX-FileCopyrightText: 2026 mindreset
 *
 * 中文：Nothing V3.7 首页，绘制层为本设计，数据层复用 rc95：
 *       点阵时钟行 + 双书卡（封面等比完整显示 + 书架页同款亚克力色块
 *       占书卡一半，块内为书名、标签与进度行——进度为加大点阵数字带 % 号，
 *       底部对齐块底）+ 阅读数据卡（三列统计：点阵数字 + 中文标签，无英文；
 *       最近 7 天点阵柱状 12:4 同比例放大，星期为中文单字，每列 6 格，
 *       每日闭环目标 ÷ 6 = 涂黑一格的分钟数，默认 60 分钟 → 10 分钟）。
 *       封面走 rc95 的异步 cover jobs（含快刷 1 位格式），扫描与缓存逻辑同为 rc95 原生。
 * English: Nothing V3.7 home. This file owns the V3.7 paint layer on top of the
 *          rc95 data layer: dot-matrix clock row, twin book cards with real
 *          covers and the shelf-style acrylic block covering half of the card,
 *          carrying the title, the label, and a large dot-matrix progress
 *          number with a percent sign pinned to the block bottom, plus a stats
 *          card with three Chinese-only dot-matrix columns and the last seven
 *          days of bar cells (12:4, scaled up proportionally) — six per day,
 *          where the daily goal divided by six gives the minutes per cell.
 *          Covers use the rc95 async cover jobs; scanning and caches are native.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "app_content_open.h"
#include "app_registry.h"
#include "boot_state.h"
#include "book_cover.h"
#include "book_cover_jobs.h"
#include "ui_image_dither.h"
#include "book_epub.h"
#include "book_progress.h"
#include "book_store.h"
#include "book_title.h"
#include "book_ticket.h"
#include "display.h"
#include "display_main_waveform.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_keyboard.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_nothing.h"
#include "ui_text_input.h"
#include "weather_service.h"

/* ---- Nothing V3.7 HOME 布局账本（684x1216）----
 * 28 pad | 状态栏 40 | 呼吸 54 | 时钟+天气行 172（内容上下居中）| 呼吸 46 | tag 46
 * 双书卡 380 | 呼吸 34 | tag 46 | 阅读数据卡 230 | 呼吸 31 | 导航 115
 */
#define HOME_CARD_H    380
#define HOME_CARD_GAP  18
#define HOME_CLOCK_ROW_Y 118
/* v3（用户 2026-10-19）时钟+天气行分区刷新条带：
 * 分钟翻转只刷左侧时间条（日期与图标不变，不重刷）；
 * 天气完成只刷右组条带（图标+地点+高低温）。 */
#define HOME_CLOCK_TICK_AREA ((EpdRect){48, 131, 460, 110})
#define HOME_WX_AREA         ((EpdRect){505, 118, 120, 150})
/* 首页时间区（时钟+日期，左组）点击进入番茄时钟（用户 2026-10-20 第二十一批 c 段）。 */
#define HOME_CLOCK_AREA      ((EpdRect){36, 118, 440, 172})
/* 阅读数据卡每列格子数：每日闭环目标 ÷ HOME_GOAL_CELLS = 每格分钟数。 */
#define HOME_GOAL_CELLS 6

typedef struct {
    char path[BOOK_STORE_PATH_MAX];
    char title[128];
    char author[128];
    uint8_t *cover;
    bool cover_fast;
    uint16_t cover_width, cover_height;
    time_t modified;
    book_progress_t progress;
    bool has_progress;
} home_book_t;

static home_book_t s_current, s_recent[6];
typedef struct {
    char path[BOOK_STORE_PATH_MAX];
    char title[128];
    char author[128];
    off_t size;
    time_t modified;
    uint8_t *pixels;
    bool fast;
    uint16_t width, height;
} home_cover_cache_t;
static home_cover_cache_t s_cover_cache[7];
static unsigned s_cover_generation;
static bool s_cover_waiting;
static int s_cover_next;
static bool s_scan_pending;
static bool s_books_cache_valid;
static bool s_cached_sd_present;
static bool s_cached_sd_mounted;
static unsigned s_cached_store_revision;
static char s_cached_books_dir[128];
static char s_cached_last_path[BOOK_STORE_PATH_MAX];
static uint32_t s_cached_progress_revision = UINT32_MAX;
static EpdRect s_area;
static bool s_home_force_full_once;
/* 阅读数据：近 30 天每日秒数（最早到今天）。 */
static uint32_t s_reading_days[30];
static bool s_reading_days_valid;
/* 双书卡真实排序。 */
static home_book_t *s_card_books[2];
/* 时钟行分钟/日期轮转。 */
static int s_last_min = -1;
static int s_last_day = -1;
static int64_t s_clock_poll_ms;
typedef struct { EpdRect cards[2]; EpdRect stats; EpdRect clockrow; } home_layout_t;
static home_layout_t s_layout;

/* 随心记双态区（用户 2026-10-20）：继续阅读区下滑切为最近便签预览，再滑/上滑切回；
 * 状态会话内保持；点击进入便签应用。 */
static bool s_home_notes;
static EpdRect s_notes_zone;
static char s_note_line[160];
static char s_note_date[40];

static home_book_t *book_at(int index);

static bool date_now(struct tm *out) {
    time_t now = time(NULL);
    if (now < 1704067200) {
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        if (pmu && pmu->time_synced) now = pmu->unix_sec;
    }
    if (now < 1704067200) { memset(out, 0, sizeof(*out)); return false; }
    now += 8 * 3600;
    gmtime_r(&now, out);
    return true;
}

/* 最近一条便签（/sdcard/notes 名字倒序第一个）供首页随心记预览。 */
static void load_home_note(void) {
    s_note_line[0] = 0; s_note_date[0] = 0;
    DIR *d = opendir("/sdcard/notes");
    if (!d) return;
    char best[96] = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] != '.' && n > 4 && !strcmp(e->d_name + n - 4, ".txt") &&
            (!best[0] || strcmp(e->d_name, best) > 0))
            snprintf(best, sizeof(best), "%s", e->d_name);
    }
    closedir(d);
    if (!best[0]) return;
    char path[160];
    snprintf(path, sizeof(path), "/sdcard/notes/%s", best);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t got = fread(s_note_line, 1, sizeof(s_note_line) - 1, f);
    fclose(f);
    s_note_line[got] = 0;
    for (char *p = s_note_line; *p; ++p) if (*p == '\n' || *p == '\r') *p = ' ';
    if (strlen(best) >= 20)
        snprintf(s_note_date, sizeof(s_note_date), "最近便签 %.2s-%.2s %.2s:%.2s",
                 best + 11, best + 13, best + 16, best + 18);
}

/* 双书卡排序：合并 s_current + s_recent，按最近阅读时间取前两名；
 * 无阅读记录的书按文件修改时间排在所有有记录的书之后。 */
static uint64_t book_score(const home_book_t *b) {
    return b->has_progress ? (UINT64_C(1) << 32) + b->progress.last_open_s
                           : (uint64_t)b->modified;
}

static void pick_cards(void) {
    s_card_books[0] = s_card_books[1] = NULL;
    home_book_t *pool[7];
    int n = 0;
    for (int i = 0; i < 7; ++i) {
        home_book_t *b = book_at(i);
        if (!b->path[0]) continue;
        bool dup = false;
        for (int j = 0; j < n; ++j)
            if (!strcmp(pool[j]->path, b->path)) { dup = true; break; }
        if (!dup) pool[n++] = b;
    }
    for (int r = 0; r < 2 && r < n; ++r) {
        int best = r;
        for (int j = r + 1; j < n; ++j)
            if (book_score(pool[j]) > book_score(pool[best])) best = j;
        home_book_t *t = pool[r]; pool[r] = pool[best]; pool[best] = t;
        s_card_books[r] = pool[r];
    }
}

static bool is_book(const char *name) {
    if (!name[0] || name[0] == '.' || !strncmp(name, "._", 2)) return false;
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".epub") || !strcasecmp(dot, ".txt"));
}

static void copy_title(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void title_for(home_book_t *item) {
    if (!book_title_from_path(item->path, item->title, sizeof(item->title)))
        copy_title(item->title, sizeof(item->title), "未命名");
}

static void consider_book(const char *path, const struct stat *st) {
    if (s_current.path[0] && !strcmp(path, s_current.path)) return;
    book_progress_t progress = {0};
    bool has_progress = st->st_size >= 0 && (uint64_t)st->st_size <= UINT32_MAX &&
                        book_progress_load(path, (uint32_t)st->st_size, &progress);
    for (int i = 0; i < 6; ++i) if (!strcmp(path, s_recent[i].path)) return;
    uint64_t score = has_progress ? (UINT64_C(1) << 32) + progress.last_open_s : (uint64_t)st->st_mtime;
    int slot = -1;
    for (int i = 0; i < 6; ++i) {
        uint64_t other = s_recent[i].has_progress ?
            (UINT64_C(1) << 32) + s_recent[i].progress.last_open_s : (uint64_t)s_recent[i].modified;
        if (!s_recent[i].path[0] || score > other) { slot = i; break; }
    }
    if (slot < 0) return;
    for (int i = 5; i > slot; --i) s_recent[i] = s_recent[i - 1];
    memset(&s_recent[slot], 0, sizeof(s_recent[slot]));
    snprintf(s_recent[slot].path, sizeof(s_recent[slot].path), "%s", path);
    s_recent[slot].modified = st->st_mtime;
    s_recent[slot].has_progress = has_progress;
    if (has_progress) s_recent[slot].progress = progress;
}

static void scan_root(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!is_book(entry->d_name)) continue;
        char path[BOOK_STORE_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= sizeof(path)) continue;
        struct stat st;
        if (!stat(path, &st) && S_ISREG(st.st_mode)) consider_book(path, &st);
    }
    closedir(dir);
}

static bool recent_visit(const char *path, const book_progress_t *progress, void *ctx) {
    (void)ctx;
    struct stat st;
    if (is_book(path) && !stat(path, &st) && S_ISREG(st.st_mode) &&
        st.st_size >= 0 && (uint64_t)st.st_size == progress->file_size)
        consider_book(path, &st);
    return true;
}

static void scan_books(void) {
    char last[BOOK_STORE_PATH_MAX];
    if (book_progress_last_path(last, sizeof(last))) {
        struct stat st;
        if (!stat(last, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size <= UINT32_MAX) {
            snprintf(s_current.path, sizeof(s_current.path), "%s", last);
            s_current.has_progress = book_progress_load(last, (uint32_t)st.st_size, &s_current.progress);
            title_for(&s_current);
        }
    }
    (void)book_progress_list(recent_visit, NULL);
    read_pico_sd_info_t sd = {0};
    read_pico_sd_get_info(&sd);
    if (sd.mounted) {
        if (!strcmp(app_settings_books_dir(), "/sdcard/books")) scan_root("/sdcard/book");
        scan_root(app_settings_books_dir());
    }
    if (book_store_flash_ready()) scan_root("/flash/books");
    for (int i = 0; i < 7; ++i) {
        home_book_t *b = book_at(i);
        if (!b->path[0]) continue;
        title_for(b);
        char metadata_title[128];
        if (book_epub_metadata_cached(b->path, metadata_title, sizeof(metadata_title),
                                      b->author, sizeof(b->author)) == ESP_OK && metadata_title[0])
            copy_title(b->title, sizeof(b->title), metadata_title);
    }
    home_book_t *featured = s_card_books[0] ? s_card_books[0] : &s_recent[0];
    const char *ext = strrchr(featured->path, '.');
    if (ext && !strcasecmp(ext, ".epub") && pico_boot_asset_allowed(featured->path)) {
        char metadata_title[256];
        if (book_epub_metadata(featured->path, metadata_title, sizeof(metadata_title),
                               featured->author, sizeof(featured->author)) != ESP_OK)
            featured->author[0] = 0;
        else if (metadata_title[0]) copy_title(featured->title, sizeof(featured->title), metadata_title);
    }
    pick_cards();
}

static void refresh_progress(void) {
    for (int i = 0; i < 7; ++i) {
        home_book_t *item = book_at(i);
        struct stat st;
        if (!item->path[0] || stat(item->path, &st) || !S_ISREG(st.st_mode) ||
            st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) continue;
        item->has_progress = book_progress_load(item->path, (uint32_t)st.st_size, &item->progress);
    }
    pick_cards();
}

static void remember_scan_state(const read_pico_sd_info_t *sd) {
    s_books_cache_valid = true;
    s_cached_store_revision = book_store_revision();
    s_cached_sd_present = sd->present;
    s_cached_sd_mounted = sd->mounted;
    snprintf(s_cached_books_dir, sizeof(s_cached_books_dir), "%s", app_settings_books_dir());
    s_cached_last_path[0] = 0;
    (void)book_progress_last_path(s_cached_last_path, sizeof(s_cached_last_path));
}

static home_book_t *book_at(int index) { return index ? &s_recent[index - 1] : &s_current; }

static void clear_cover_cache(void) {
    for (int i = 0; i < 7; ++i) {
        free(s_cover_cache[i].pixels);
        memset(&s_cover_cache[i], 0, sizeof(s_cover_cache[i]));
    }
    s_current.cover = NULL;
    for (int i = 0; i < 6; ++i) s_recent[i].cover = NULL;
}

void app_home_cover_mode_changed(void) {
    clear_cover_cache();
    s_cover_next = 0;
}

/* 阅读页需要内存时释放首页的 PSRAM 封面缓存（外部契约，app_book 调用）。 */
void app_home_release_clock_cache(void) {
    clear_cover_cache();
    s_cover_next = 0;
}

static void restore_cover_cache(void) {
    bool used[7] = {0};
    int match[7];
    for (int i = 0; i < 7; ++i) {
        home_book_t *book = book_at(i);
        struct stat st;
        book->cover = NULL;
        match[i] = -1;
        if (!book->path[0] || stat(book->path, &st) || !S_ISREG(st.st_mode)) continue;
        for (int j = 0; j < 7; ++j) {
            home_cover_cache_t *cached = &s_cover_cache[j];
            if (used[j] || !cached->pixels || strcmp(book->path, cached->path) ||
                strcmp(book->title, cached->title) || strcmp(book->author, cached->author) ||
                st.st_size != cached->size || st.st_mtime != cached->modified) continue;
            match[i] = j;
            used[j] = true;
            break;
        }
    }
    for (int j = 0; j < 7; ++j) if (!used[j]) {
        free(s_cover_cache[j].pixels);
        memset(&s_cover_cache[j], 0, sizeof(s_cover_cache[j]));
    }
    for (int i = 0; i < 7; ++i) {
        int j = match[i];
        if (j < 0) for (j = i; j < 7 && s_cover_cache[j].pixels; ++j) {}
        if (j < 7 && j != i) {
            home_cover_cache_t swap = s_cover_cache[i];
            s_cover_cache[i] = s_cover_cache[j];
            s_cover_cache[j] = swap;
            for (int k = i + 1; k < 7; ++k) {
                if (match[k] == i) match[k] = j;
                else if (match[k] == j) match[k] = i;
            }
        }
        book_at(i)->cover = s_cover_cache[i].pixels;
        book_at(i)->cover_fast = s_cover_cache[i].fast;
        book_at(i)->cover_width = s_cover_cache[i].width;
        book_at(i)->cover_height = s_cover_cache[i].height;
    }
}

/* rc95 异步封面作业：等待中返回 false 并置 s_cover_waiting，tick 下轮再取。 */
static bool load_cover(home_book_t *item, int slot) {
    s_cover_waiting = false;
    if (!pico_boot_asset_allowed(item->path)) return false;
    book_cover_job_t request = {.list = true, .natural = true,
        .fast = app_settings_main_fast_refresh(), .contrast = app_settings_system_contrast(), .phase = 17};
    snprintf(request.path, sizeof(request.path), "%s", item->path);
    snprintf(request.title, sizeof(request.title), "%s", item->title);
    snprintf(request.author, sizeof(request.author), "%s", item->author);
    if (!s_cover_generation) s_cover_generation = book_cover_jobs_begin();
    if (!book_cover_jobs_submit(s_cover_generation, slot, &request)) {
        s_cover_generation = book_cover_jobs_begin();
        if (!book_cover_jobs_submit(s_cover_generation, slot, &request)) return false;
    }
    book_cover_result_t result = {0};
    int state = book_cover_jobs_take(s_cover_generation, slot, &result);
    if (!state) { s_cover_waiting = true; return false; }
    if (state < 0) return false;
    home_cover_cache_t *cached = &s_cover_cache[slot];
    free(cached->pixels);
    snprintf(cached->path, sizeof(cached->path), "%s", item->path);
    snprintf(cached->title, sizeof(cached->title), "%s", item->title);
    snprintf(cached->author, sizeof(cached->author), "%s", item->author);
    cached->size = result.size;
    cached->modified = result.modified;
    cached->pixels = result.pixels;
    cached->fast = result.fast;
    cached->width = result.width;
    cached->height = result.height;
    item->cover = result.pixels;
    item->cover_fast = result.fast;
    item->cover_width = result.width;
    item->cover_height = result.height;
    return true;
}

static void refresh_stats(void) {
    s_reading_days_valid = book_ticket_recent_days(s_reading_days);
    if (!s_reading_days_valid) memset(s_reading_days, 0, sizeof(s_reading_days));
}

/* 小节标题：中文 29 + 通栏 2px 纯黑线；avatar_end 时右端引用状态栏头像（用户 2026-10-17）。 */
static int draw_tag(uint8_t* fb, int y, const char* cn, bool avatar_end) {
    int cy = y + 21;
    ui_text_vc(fb, NX_MARGIN, cy, 29, cn, EPD_DRAW_ALIGN_LEFT, false);
    int x = NX_MARGIN + ttf_text_width_px(ui_text_effective_px(29), cn) + 24;
    int x1 = 684 - NX_MARGIN - (avatar_end ? 38 + 12 : 0);
    if (x1 > x + 8) ui_draw_separator(fb, cy + 10, x, x1 - x, NX8_INK);
    if (avatar_end) nothing_blit_status_avatar(fb, 684 - NX_MARGIN - 38, cy - 19);
    return y + 40;
}

/* rc95 原比例封面（圆角遮罩 + 快刷 1 位格式支持），复用不重写。 */
static void draw_cover(uint8_t *fb, const home_book_t *item, EpdRect bounds) {
    EpdRect box = bounds;
    if (item->cover && item->cover_width && item->cover_height) {
        unsigned w = item->cover_width, h = item->cover_height;
        if ((uint64_t)w * box.height > (uint64_t)h * box.width) box.height = (uint64_t)box.width * h / w;
        else box.width = (uint64_t)box.height * w / h;
        box.x = bounds.x + (bounds.width - box.width) / 2;
        box.y = bounds.y + bounds.height - box.height;
        for (int y = 0; y < box.height; ++y) for (int x = 0; x < box.width; ++x) {
            int dx = x < 12 ? 12 - x : x >= box.width - 12 ? x - (box.width - 13) : 0;
            int dy = y < 12 ? 12 - y : y >= box.height - 12 ? y - (box.height - 13) : 0;
            if (dx * dx + dy * dy > 144) continue;
            unsigned sx = (unsigned)x * w / box.width, sy = (unsigned)y * h / box.height;
            uint8_t g = item->cover_fast
                ? (item->cover[sy * ((w + 7) / 8) + sx / 8] & (1u << (sx & 7)) ? 255 : 0)
                : ui_image_dither_gray(ui_contrast_gray(item->cover[sy * w + sx]), box.x + x, box.y + y);
            epd_draw_pixel(box.x + x, box.y + y, g, fb);
        }
    } else ui_fill_round_rect(fb, box, 12, 0xe0);
    /* 有真封面用 2px 纯黑框；无封面占位不用黑色（用户 2026-10-17）。 */
    if (item->cover && item->cover_width && item->cover_height)
        ui_draw_control_frame(fb, box, 12, NX8_INK);
    else ui_draw_round_rect(fb, box, 12, 0xb0);
}

/* 时钟 + 天气行（v3 布局，用户 2026-10-19）：左右两组各自成块、组内垂直堆叠，
 * 172 高行（y 118..290）内整体居中——左组=点阵时间+日期，右组=天气图标+地点+高低温。
 * 美观：图标 120→96 与文字成视觉组（120 时文字只能吊在组外，松散）；组间留白≥40px。
 * 刷新：左右分区互不交叠——分钟翻转只刷左侧时间条（日期/图标不变不重刷），
 * 天气完成只刷右组条带；局部刷新面积最小化（DU 提速+功耗）。 */
static void draw_clockrow(uint8_t* fb) {
    time_t now = time(NULL);
    char clock[8] = "12:00";
    struct tm tm_bj = {0};
    bool valid = now >= 946684800;
    if (valid) {
        time_t bj = now + 8 * 3600;
        gmtime_r(&bj, &tm_bj);
        snprintf(clock, sizeof(clock), "%02d:%02d", tm_bj.tm_hour, tm_bj.tm_min);
    }
    /* 左组：点阵时间居中于 y 137..235（pitch14 高 98），日期行中心 268（与时间留 16px 呼吸）。 */
    nothing_dot_text(fb, 59, 137, clock, 5, 14, NX8_INK);
    if (valid) {
        static const char* const week[] = {"星期日","星期一","星期二","星期三",
                                            "星期四","星期五","星期六"};
        char date[48];
        snprintf(date, sizeof(date), "%d月%d日 %s",
                 tm_bj.tm_mon + 1, tm_bj.tm_mday, week[tm_bj.tm_wday]);
        ui_text_vc(fb, 59, 268, 22, date, EPD_DRAW_ALIGN_LEFT, false);
    } else {
        ui_text_vc(fb, 59, 268, 22, "--月--日", EPD_DRAW_ALIGN_LEFT, false);
    }
    /* 右组（x 505..625，120 宽）：图标 96px 居中 y 122..218；地点 16px 中心 238；
     * 高/低温 14px 中心 262 右对齐 x625。组底距行底 290 留 14px 呼吸。
     * 图标按实际天气码绘制（weather_service，未知回退太阳）。 */
    weather_code_t wxcode = weather_valid() ? weather_code() : WEATHER_CODE_SUNNY;
    nothing_draw_weather(fb, 517, 122, 96, wxcode);
    /* v6（2026-10-19 真机反馈“设置完地点不在首页显示”）：即使天气数据未就绪
     * （离线/未联网），也显示当前配置的地点名；高低温仅在有数据时显示。 */
    char city[48];
    weather_city(city, sizeof(city));
    if (city[0]) ui_text_vc(fb, 625, 238, 16, city, EPD_DRAW_ALIGN_RIGHT, false);
    if (weather_valid()) {
        char range[24];
        snprintf(range, sizeof(range), "%d° / %d°", weather_low(), weather_high());
        ui_text_vc(fb, 625, 262, 14, range, EPD_DRAW_ALIGN_RIGHT, false);
    }
}

/* 双书卡：真实封面等比完整显示（完整书本）；亚克力色块悬浮在书籍下半，
 * 高度占书卡一半（书架页同款亚克力，快刷模式下转稳定点阵）。
 * 块内文字：书名 + “上次阅读到这里”标签 + 进度行——
 * 进度行为左侧加大点阵数字带 % 号，底部对齐块底。 */
static void draw_home_card(uint8_t* fb, EpdRect card, home_book_t* item, bool focused) {
    ui_fill_round_rect(fb, card, NX_R_CARD, 0xf0);
    EpdRect cover = {card.x + 2, card.y + 2, card.width - 4, card.height - 4};
    if (!item || !item->path[0]) {
        ui_fill_round_rect(fb, cover, NX_R_CARD - 2, NX8_GRID);
        ui_text_vc(fb, cover.x + cover.width / 2, cover.y + cover.height / 2, 18,
                   focused ? "书架还没有书" : "去传几本书吧",
                   EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    draw_cover(fb, item, cover);
    /* 纯色亚克力块（无装饰图案，用户 2026-10-16）：几何上真正盖过书本——
     * 左右各超出卡片 3px（= 超出封面 5px/侧），底部低于卡底 3px（= 超出封面 5px），
     * 视觉上把封面下沿包进色块。快刷模式下转稳定点阵。 */
    int block_h = (card.height - 4) / 2;
    /* 2026-10-17：色块左右与书本封面对齐不再超出，底边与封面底齐平。 */
    EpdRect block = {card.x + 2, card.y + card.height - 2 - block_h, card.width - 4, block_h};
    ui_draw_acrylic_plain(fb, block);
    if (app_settings_main_fast_refresh()) ui_acrylic_bw_rect(fb, block);
    /* 书卡色块边框 2px 纯黑（对齐状态栏，2026-10-17）。 */
    ui_draw_control_frame(fb, block, NX_R_SM, NX8_INK);
    /* 书名：22px。按 ui_text 的真实渲染宽度截断，超出色块的部分直接不显示
     * （不加点号，文本在色块内干净截止）。 */
    char name[128];
    snprintf(name, sizeof(name), "%s", item->title);
    while (*name && ui_text_width_px(22, name) > block.width - 24) {
        size_t n = strlen(name) - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) --n;
        name[n] = 0;
    }
    ui_text_vc(fb, block.x + 12, block.y + 30, 22, name, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, block.x + 12, block.y + 58, 14,
               item->has_progress ? "上次阅读到这里" : "尚未开始阅读",
               EPD_DRAW_ALIGN_LEFT, false);
    /* 进度行：左侧加大点阵数字带 % 号，底部对齐块底；无右侧百分比。 */
    if (item->has_progress) {
        char num[8];
        snprintf(num, sizeof(num), "%u", item->progress.pct);
        int nx = nothing_dot_text(fb, block.x + 12, block.y + 118, num, 3, 8, NX8_INK);
        nothing_dot_text(fb, nx, block.y + 118, "%", 3, 8, NX8_INK);
    }
}

/* 连续阅读天数：今天未读不中断，从最后一个有效日往前数。 */
static int streak_days(void) {
    int i = 29;
    if (!s_reading_days[i]) --i;
    int n = 0;
    for (; i >= 0 && s_reading_days[i] > 0; --i) ++n;
    return n;
}

/* 阅读数据卡：三列统计（点阵数字 + 中文标签，无英文）+ 最近 7 天点阵柱状。
 * 每列 6 格：每格 = 每日闭环目标 ÷ 6（默认 60 分钟 → 10 分钟），涂满即达成当日目标；
 * 柱状格子按删除英文行后腾出的空间同比例放大（9:3 → 12:4）。 */
static void draw_stats(uint8_t* fb) {
    EpdRect box = s_layout.stats;
    ui_fill_round_rect(fb, box, NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, box, NX_R_CARD, NX8_INK);
    if (!s_reading_days_valid) {
        ui_text_vc(fb, box.x + box.width / 2, box.y + box.height / 2, 22,
                   "完成日期与时间设置后显示阅读记录",
                   EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    unsigned goal = app_settings_home_goal_minutes();
    unsigned cell_seconds = (goal ? goal / HOME_GOAL_CELLS : 0) * 60u;
    uint64_t week = 0;
    for (int i = 23; i < 30; ++i) week += s_reading_days[i];
    /* 三列语义（2026-10-17）：今日阅读 · 本周日均 · 连续天数；默认两位数补零，更大位数自然扩展。 */
    unsigned today_min = (unsigned)(s_reading_days[29] / 60u);
    unsigned avg_min = (unsigned)((week + 210) / 420);
    char v0[12], v1[12], v2[12];
    snprintf(v0, sizeof(v0), "%02u", today_min);
    snprintf(v1, sizeof(v1), "%02u", avg_min);
    snprintf(v2, sizeof(v2), "%02u", (unsigned)streak_days());
    static const char* const labels[] = {"今日阅读", "本周日均", "连续天数"};
    const char* const vals[] = {v0, v1, v2};
    int col_w = box.width / 3;
    for (int i = 0; i < 3; ++i) {
        int cx = box.x + 22 + col_w * i;
        /* 分隔线只到中文标签行底，下方柱状保持连贯 */
        if (i) epd_fill_rect((EpdRect){box.x + col_w * i - 1, box.y + 12, 2, 74}, NX8_INK, fb);
        nothing_dot_text(fb, cx, box.y + 12, vals[i], 2, 5, NX8_INK);
        ui_text_vc(fb, cx, box.y + 75, 22, labels[i], EPD_DRAW_ALIGN_LEFT, false);
    }
    /* 最近 7 天点阵柱状：左→右 6 天前→今天；每列 6 格，读满 cell_seconds 涂黑一格。 */
    struct tm tm_bj = {0};
    (void)date_now(&tm_bj);
    static const char* const day_cn[] = {"日", "一", "二", "三", "四", "五", "六"};
    const int cell = 12, gap = 4, maxc = HOME_GOAL_CELLS;
    int bw = box.width - 40;
    for (int i = 0; i < 7; ++i) {
        uint32_t seconds = s_reading_days[23 + i];
        int bx = box.x + 20 + bw / 7 * i + (bw / 7 - cell) / 2;
        int cells = cell_seconds ? (int)(seconds / cell_seconds) : (int)(seconds > 0);
        if (cells > maxc) cells = maxc;
        for (int c = 0; c < maxc; ++c) {
            int by = box.y + 192 - (c + 1) * (cell + gap) + gap;
            int dx = bx + cell / 2, dy = by + cell / 2;
            /* 点阵 1:1（系统统一）：达标=实心圆，未达标=空心圆（0 纯黑，同书架进度点，2026-10-17）。 */
            if (c < cells) epd_fill_circle(dx, dy, 5, i == 6 ? NX8_INK : NX8_DIM, fb);
            else epd_draw_circle(dx, dy, 5, NX8_INK, fb);
        }
        bool today = i == 6;
        ui_text_vc(fb, bx + cell / 2, box.y + 212, 18, day_cn[(tm_bj.tm_wday + 1 + i) % 7],
                   EPD_DRAW_ALIGN_CENTER, false);
        if (today)
            epd_fill_rect((EpdRect){bx + cell / 2 - 11, box.y + 225, 22, 3}, NX8_INK, fb);
    }
}

/* ===================== 阅读目标面板（点阅读数据卡弹出） ===================== */

static bool s_goal_open;
static int s_goal_month_offset;
static int s_goal_sel_day;   /* 2026-10-17：0=今天；>0=当前显示月份第 N 日（黄历随动）。 */

/* ============ 天气设置页（用户 2026-10-19：点击首页天气区图标进入） ============
 * 默认显示当前生效源的值（NVS 优先、回退 TF 卡 weather.txt），可改；
 * 保存写入 NVS（此后优先）；「恢复默认源」清除 NVS 回退 TF 卡。 */
static bool s_wx_setup;
static int s_wx_edit = -1;      /* -1=未编辑；0=地点；1=Key */
static char s_wx_city[48];
static char s_wx_key[40];
static ui_text_edit_t s_wx_input;
static char s_wx_notice[96];
static EpdRect s_wx_area;       /* 键盘局部刷新区 */
static bool s_wx_layout, s_wx_settle;

/* v3 布局：月历置顶 96..716 → 老黄历上卡 728..852（历法：农历40+干支26）
 * → 老黄历下卡 858..990（宜/忌，徽章r17/条30）→ 目标行 1000..1084（H84）。
 * v9（用户 2026-10-20）：一分为二重排、字体加大。 */
#define GOAL_CAL_TOP 96
#define GOAL_CAL_H   620
#define GOAL_ROW_H   82
#define GOAL_DOT_R   28
#define GOAL_ALM_TOP_Y 728
#define GOAL_ALM_TOP_H 124
#define GOAL_ALM_Y   858
#define GOAL_ALM_H   132
#define GOAL_SET_Y   1000
#define GOAL_SET_H   84


static EpdRect goal_month_arrow(int dir) { return (EpdRect){dir < 0 ? 36 : 584, 100, 64, 56}; }
static EpdRect goal_almanac_top_rect(void) { return (EpdRect){36, GOAL_ALM_TOP_Y, 612, GOAL_ALM_TOP_H}; }
static EpdRect goal_almanac_rect(void) { return (EpdRect){36, GOAL_ALM_Y, 612, GOAL_ALM_H}; }
static EpdRect goal_minus_rect(void) { return (EpdRect){36, GOAL_SET_Y, 110, GOAL_SET_H}; }
static EpdRect goal_plus_rect(void) { return (EpdRect){538, GOAL_SET_Y, 110, GOAL_SET_H}; }
static EpdRect goal_value_rect(void) { return (EpdRect){166, GOAL_SET_Y, 352, GOAL_SET_H}; }

/* 天气设置页几何（渲染与命中共用同一份坐标）：
 * 标题行 106（状态栏内容区下沿约 76 之下）；行 0/1/2 = 地点 / Key / 当前源，
 * 行高 96、行距 118（170 起）；恢复按钮 556..648。 */
#define WX_ROW_X 36
#define WX_ROW_W 612
static EpdRect wx_row_rect(int idx) { return (EpdRect){WX_ROW_X, 170 + idx * 118, WX_ROW_W, 96}; }
static EpdRect wx_reset_rect(void) { return (EpdRect){WX_ROW_X, 556, WX_ROW_W, 92}; }

/* 0=星期日。Sakamoto 算法，无需 libc 时区。 */
static int goal_weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int w = y + y / 4 - y / 100 + y / 400 + t[m - 1] + d;
    return ((w % 7) + 7) % 7;
}

/* ---- 农历（1900-2100 查表，公历→农历）---- */
static const uint32_t k_lunar_tab[201] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5b0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b6a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06b20,0x1a6c4,0x0aae0,
    0x0a2e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
    0x0d520
};
static int lunar_leap_month(int y) { return (int)(k_lunar_tab[y - 1900] & 0xf); }
static int lunar_leap_days(int y) {
    return lunar_leap_month(y) ? ((k_lunar_tab[y - 1900] & 0x10000) ? 30 : 29) : 0;
}
static int lunar_month_days(int y, int m) {
    return (k_lunar_tab[y - 1900] & (0x10000 >> m)) ? 30 : 29;
}
static int lunar_year_days(int y) {
    int sum = 348;
    for (int i = 0x8000; i > 8; i >>= 1) sum += (k_lunar_tab[y - 1900] & i) ? 1 : 0;
    return sum + lunar_leap_days(y);
}
static int64_t days_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
typedef struct { int y, m, d; bool leap; } lunar_date_t;
static bool solar_to_lunar(int sy, int sm, int sd, lunar_date_t *out) {
    if (sy < 1901 || sy > 2098 || !out) return false;
    int64_t days = days_civil(sy, sm, sd) - days_civil(1900, 1, 31);
    int y = 1900;
    while (days >= lunar_year_days(y)) { days -= lunar_year_days(y); ++y; }
    int leap = lunar_leap_month(y);
    int idx = 0; /* 0..11 常规月，12 = 闰月 */
    for (;;) {
        int md = idx == 12 ? lunar_leap_days(y) : lunar_month_days(y, idx + 1);
        if (idx == 12 && !leap) break;
        if (days < md) break;
        days -= md;
        ++idx;
        if (idx > 12) break;
    }
    out->y = y;
    out->leap = idx == 12;
    out->m = out->leap ? leap : idx + 1;
    out->d = (int)days + 1;
    return true;
}

/* ---- 老黄历文本（民间黄历风格展示，确定映射） ---- */
static void goal_almanac_chars(char *dst, size_t cap, int sy, int sm, int sd) {
    lunar_date_t lu = {0};
    if (!solar_to_lunar(sy, sm, sd, &lu)) {
        snprintf(dst, cap, "%s", "老黄历");
        return;
    }
    static const char *const gan[] = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
    static const char *const zhi[] = {"子鼠","丑牛","寅虎","卯兔","辰龙","巳蛇","午马","未羊","申猴","酉鸡","戌狗","亥猪"};
    static const char *const months[] = {"正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","冬月","腊月"};
    static const char *const days[] = {"初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
        "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
        "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"};
    static const char *const good[] = {"祭祀","祈福","出行","纳财","开市","交易","会友","裁衣","栽种","牧养"};
    static const char *const bad[] = {"动土","安葬","嫁娶","词讼","掘井","移徙","针灸","破屋"};
    int gy = (lu.y - 4) % 10, gz = (lu.y - 4) % 12;
    if (gy < 0) gy += 10;
    if (gz < 0) gz += 12;
    int seed = lu.m * 3 + lu.d;
    const char *g1 = good[seed % 10], *g2 = good[(seed + 4) % 10];
    const char *b1 = bad[(seed * 3 + 1) % 8], *b2 = bad[(seed * 5 + 4) % 8];
    char line1[64], line2[64];
    snprintf(line1, sizeof(line1), "%s%s年 %s%s%s", gan[gy], zhi[gz],
             lu.leap ? "闰" : "", months[lu.m - 1], days[lu.d - 1]);
    snprintf(line2, sizeof(line2), "宜 %s · %s        忌 %s · %s", g1, g2, b1, b2);
    snprintf(dst, cap, "%s\n%s", line1, line2);
}

static void goal_almanac_chars_two(char *gz, char *lunar, char *yi, char *ji, int sy, int sm, int sd) {
    /* v2（用户 2026-10-17，参考网上常用老黄历设计）：四字段全裸输出，不带前缀——
     * gz=干支年“丙午年”；lunar=农历“九月初三”；yi/ji=两词条目“祭祀 祈福”，
     * 前缀“宜/忌”改由渲染层的徽章承担（实心黑圆白字 / 空心圆黑字）。 */
    lunar_date_t lu = {0};
    gz[0] = lunar[0] = yi[0] = ji[0] = 0;
    if (!solar_to_lunar(sy, sm, sd, &lu)) return;
    static const char *const gan[] = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
    static const char *const zhi[] = {"子鼠","丑牛","寅虎","卯兔","辰龙","巳蛇","午马","未羊","申猴","酉鸡","戌狗","亥猪"};
    static const char *const months[] = {"正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","冬月","腊月"};
    static const char *const days[] = {"初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
        "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
        "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"};
    static const char *const good[] = {"祭祀","祈福","出行","纳财","开市","交易","会友","裁衣","栽种","牧养"};
    static const char *const bad[] = {"动土","安葬","嫁娶","词讼","掘井","移徙","针灸","破屋"};
    int gy = (lu.y - 4) % 10, gzidx = (lu.y - 4) % 12;
    if (gy < 0) gy += 10;
    if (gzidx < 0) gzidx += 12;
    snprintf(gz, 32, "%s%s年", gan[gy], zhi[gzidx]);
    snprintf(lunar, 48, "%s%s%s", lu.leap ? "闰" : "", months[lu.m - 1], days[lu.d - 1]);
    int seed = lu.m * 3 + lu.d;
    snprintf(yi, 64, "%s %s", good[seed % 10], good[(seed + 4) % 10]);
    snprintf(ji, 64, "%s %s", bad[(seed * 3 + 1) % 8], bad[(seed * 5 + 4) % 8]);
}

static void render_goal_panel(uint8_t* fb) {
    unsigned goal = app_settings_home_goal_minutes();
    struct tm tm_bj = {0};
    bool date_ok = date_now(&tm_bj);
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, 684, NX_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    /* ---- 月历卡（置顶，无面板标题；左右箭头换月） ---- */
    EpdRect cal = {36, GOAL_CAL_TOP, 612, GOAL_CAL_H};
    ui_fill_round_rect(fb, cal, NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, cal, NX_R_CARD, NX8_INK);
    int cur_y = date_ok ? tm_bj.tm_year + 1900 : 2026;
    int cur_m = date_ok ? tm_bj.tm_mon + 1 : 10;
    for (int k = 0; k < -s_goal_month_offset; ++k) { if (--cur_m == 0) { cur_m = 12; --cur_y; } }
    for (int k = 0; k < s_goal_month_offset; ++k) { if (++cur_m == 13) { cur_m = 1; ++cur_y; } }
    char title[40];
    snprintf(title, sizeof(title), "%d 年 %d 月", cur_y, cur_m);
    ui_text_vc(fb, 342, cal.y + 30, 26, title, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_icon(fb, 68, cal.y + 36, 28, UI_ICON_CHEVRON_LEFT, NX8_INK);
    ui_draw_icon(fb, 616, cal.y + 36, 28, UI_ICON_CHEVRON_RIGHT, NX8_INK);
    static const char *const week[] = {"日", "一", "二", "三", "四", "五", "六"};
    for (int c = 0; c < 7; ++c)
        ui_text_vc(fb, 36 + 43 + c * 87, cal.y + 82, 18, week[c], EPD_DRAW_ALIGN_CENTER, false);
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int y = cur_y, m = cur_m;
    int days = mdays[m - 1];
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days = 29;
    int first = goal_weekday(y, m, 1);
    const uint32_t need = goal * 60u;
    bool is_cur_month = date_ok && y == (tm_bj.tm_year + 1900) && m == (tm_bj.tm_mon + 1);
    for (int d = 1; d <= days; ++d) {
        int slot = first + d - 1;
        int col = slot % 7, row = slot / 7;
        int cx = 36 + 43 + col * 87;
        int cy = cal.y + 156 + row * GOAL_ROW_H;
        if (cy + GOAL_DOT_R > cal.y + GOAL_CAL_H - 6) break;
        char num[4] = {0};
        if (d >= 10) { num[0] = '0' + d / 10; num[1] = '0' + d % 10; num[2] = 0; }
        else { num[0] = '0' + d; num[1] = 0; }
        /* 点阵日期：单/双位均 pitch4，两位宽 44px，稳居直径 60 圆内。 */
        int wq = nothing_dot_text_w(num, 4);
        bool future = is_cur_month ? d > tm_bj.tm_mday
                    : !date_ok ? false : (y > tm_bj.tm_year + 1900 ||
                        (y == tm_bj.tm_year + 1900 && m > tm_bj.tm_mon + 1));
        int idx = is_cur_month && !future ? 29 - (tm_bj.tm_mday - d) : -1;
        bool hit = idx >= 0 && idx <= 29 && s_reading_days_valid && s_reading_days[idx] >= need;
        if (hit) {
            epd_fill_circle(cx, cy, GOAL_DOT_R, NX8_INK, fb);
            nothing_dot_text(fb, cx - wq / 2, cy - 14, num, 2, 4, NX8_PAPER);
        } else {
            uint8_t edge = future ? 0x90 : NX8_INK;
            epd_draw_circle(cx, cy, GOAL_DOT_R, edge, fb);
            nothing_dot_text(fb, cx - wq / 2, cy - 14, num, 2, 4, edge);
        }
        if (is_cur_month && d == tm_bj.tm_mday)
            epd_fill_rect((EpdRect){cx - 12, cy + GOAL_DOT_R + 8, 24, 4}, NX8_INK, fb);
        if (d == (s_goal_sel_day > 0 ? s_goal_sel_day : (is_cur_month ? tm_bj.tm_mday : 0))) {
            /* 选中日：外圈加粗框 */
            epd_draw_circle(cx, cy, GOAL_DOT_R + 4, NX8_INK, fb);
            epd_draw_circle(cx, cy, GOAL_DOT_R + 5, NX8_INK, fb);
        }
    }
    /* ---- 老黄历 v9（用户 2026-10-20，一分为二+字体加大）：
     * 上卡=历法（农历 40px 大字 + 干支年 26px 居中，无分隔线，卡内上下双行）；
     * 下卡=宜/忌（左右两块，徽章 28px，条目 30px）。 ---- */
    EpdRect alm_top = goal_almanac_top_rect();
    ui_fill_round_rect(fb, alm_top, NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, alm_top, NX_R_CARD, NX8_INK);
    EpdRect alm = goal_almanac_rect();
    ui_fill_round_rect(fb, alm, NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, alm, NX_R_CARD, NX8_INK);
    {
        int sy = cur_y, sm = cur_m, sd = is_cur_month ? tm_bj.tm_mday : 1;
        if (s_goal_sel_day > 0 && s_goal_sel_day <= days) { sd = s_goal_sel_day; }
        char gz[32], lunar[48], yi[64], ji[64];
        goal_almanac_chars_two(gz, lunar, yi, ji, sy, sm, sd);
        if (!lunar[0]) {
            ui_text_vc(fb, alm_top.x + alm_top.width / 2, alm_top.y + 44, 28, "老黄历", EPD_DRAW_ALIGN_CENTER, false);
        } else {
            /* 上卡（H124）：农历大字 40 于 +46，干支 26 于 +92，上下留白均衡。 */
            ui_text_vc(fb, alm_top.x + alm_top.width / 2, alm_top.y + 46, 40, lunar, EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, alm_top.x + alm_top.width / 2, alm_top.y + 92, 26, gz, EPD_DRAW_ALIGN_CENTER, false);
            /* 下卡：宜/忌左右两块，条目 30px；徽章圆 r18/字 24。 */
            int cy2 = alm.y + alm.height / 2;
            epd_fill_circle(alm.x + 56, cy2, 18, NX8_INK, fb);
            ui_text_vc(fb, alm.x + 56, cy2, 24, "宜", EPD_DRAW_ALIGN_CENTER, true);
            char yi_text[74] = {0};
            snprintf(yi_text, sizeof(yi_text), "%.70s", yi);
            while (yi_text[0] && ttf_text_width_px(ui_text_effective_px(30), yi_text) > 226) {
                size_t n = strlen(yi_text) - 1;
                while (n && ((unsigned char)yi_text[n] & 0xc0) == 0x80) --n;
                yi_text[n] = 0;
            }
            ui_text_vc(fb, alm.x + 88, cy2, 30, yi_text, EPD_DRAW_ALIGN_LEFT, false);
            epd_draw_circle(alm.x + 320, cy2, 18, NX8_INK, fb);
            epd_draw_circle(alm.x + 320, cy2, 17, NX8_INK, fb);
            ui_text_vc(fb, alm.x + 320, cy2, 24, "忌", EPD_DRAW_ALIGN_CENTER, false);
            char ji_text[74] = {0};
            snprintf(ji_text, sizeof(ji_text), "%.70s", ji);
            while (ji_text[0] && ttf_text_width_px(ui_text_effective_px(30), ji_text) > 226) {
                    size_t n = strlen(ji_text) - 1;
                    while (n && ((unsigned char)ji_text[n] & 0xc0) == 0x80) --n;
                    ji_text[n] = 0;
                }
            ui_text_vc(fb, alm.x + 352, cy2, 30, ji_text, EPD_DRAW_ALIGN_LEFT, false);
        }
    }
    /* ---- 目标设置行：− [数值 分钟] + （数字点阵 pitch9 缩小 1/3） ---- */
    ui_draw_button(fb, goal_minus_rect(), "−", false);
    ui_fill_round_rect(fb, goal_value_rect(), NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, goal_value_rect(), NX_R_CARD, NX8_INK);
    {
        char num[8]; snprintf(num, sizeof(num), "%u", goal);
        const int pitch = 9, dr = 4;
        int w = nothing_dot_text_w(num, pitch);
        int label_w = ttf_text_width_px(ui_text_effective_px(26), "分钟");
        int nx = goal_value_rect().x + (goal_value_rect().width - (w + 14 + label_w)) / 2;
        nothing_dot_text(fb, nx, goal_value_rect().y + (GOAL_SET_H - 7 * pitch) / 2, num, dr, pitch, NX8_INK);
        ui_text_vc(fb, nx + w + 14, goal_value_rect().y + GOAL_SET_H / 2, 26, "分钟", EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_draw_button(fb, goal_plus_rect(), "+", false);
    /* v9：下方留白已被目标行与两卡占满，提示行撤除。 */
    /* v6（2026-10-19 用户真机反馈）：阅读数据面板恢复底部导航栏（与其它页面一致），
     * 导航命中同步放开；编辑态由键盘覆盖导航区仍不画。 */
    ui_nav_draw(fb, 0);
}

/* ============ 天气设置页（点击首页天气区进入，用户 2026-10-19） ============
 * 每行卡片：标签（左 24px）+ 值（右 21px，chevron 时让位）+ 副行说明（16px）。
 * 行 2（当前源）不可点不画 chevron；恢复默认源按钮独立。 */
static void wx_row_card(uint8_t *fb, EpdRect box, const char *label,
                        const char *value, const char *hint, bool chevron) {
    ui_fill_round_rect(fb, box, NX_R_CARD, NX8_PAPER);
    ui_draw_control_frame(fb, box, NX_R_CARD, NX8_INK);
    ui_text_vc(fb, box.x + 24, box.y + 38, 24, label, EPD_DRAW_ALIGN_LEFT, false);
    if (chevron) {
        ui_draw_icon(fb, box.x + box.width - 22, box.y + 44, 24, UI_ICON_CHEVRON_RIGHT, NX8_INK);
        ui_text_vc(fb, box.x + box.width - 52, box.y + 38, 21, value, EPD_DRAW_ALIGN_RIGHT, false);
    } else {
        ui_text_vc(fb, box.x + box.width - 24, box.y + 38, 21, value, EPD_DRAW_ALIGN_RIGHT, false);
    }
    ui_text_vc(fb, box.x + 24, box.y + 74, 16, hint, EPD_DRAW_ALIGN_LEFT, false);
}

static void render_wx_setup(uint8_t *fb) {
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, 684, NX_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    /* 标题避开状态栏内容区（下沿约 76）：与书架样式子页标题同基线。 */
    ui_text_vc(fb, 342, 106, 34, "天气设置", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 150, 18, "配置后联网自动更新首页天气", EPD_DRAW_ALIGN_CENTER, false);
    char city[48] = "未设置", key[40] = "未设置";
    char cfg_city[48] = {0}, cfg_key[40] = {0};
    (void)weather_config_get(cfg_city, sizeof(cfg_city), cfg_key, sizeof(cfg_key));
    if (cfg_city[0]) snprintf(city, sizeof(city), "%s", cfg_city);
    if (cfg_key[0]) snprintf(key, sizeof(key), "已设置（%u 位）", (unsigned)strlen(cfg_key));
    wx_row_card(fb, wx_row_rect(0), "地点", city, "点击输入，天气按新地址更新", true);
    wx_row_card(fb, wx_row_rect(1), "Key", key, "心知天气密钥：在 seniverse.com 注册", true);
    const char *src_text, *src_hint;
    switch (weather_config_source()) {
        case WEATHER_SRC_NVS: src_text = "设备设置（NVS）"; src_hint = "点击可直接修改地点"; break;
        case WEATHER_SRC_TF:  src_text = "TF 卡 weather.txt"; src_hint = "/sdcard/weather.txt 内 city=/key= 两行"; break;
        default:              src_text = "未配置"; src_hint = "设置后自动回写到设备，联网更新"; break;
    }
    wx_row_card(fb, wx_row_rect(2), "当前源", src_text, src_hint, true);
    ui_draw_button(fb, wx_reset_rect(), "恢复默认源", false);
    ui_text_vc(fb, 342, wx_reset_rect().y + wx_reset_rect().height + 30, 17,
               "清除设备设置，改用 TF 卡 weather.txt", EPD_DRAW_ALIGN_CENTER, false);
    if (s_wx_notice[0]) ui_text_vc(fb, 342, wx_reset_rect().y + wx_reset_rect().height + 66, 20,
                                   s_wx_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, 342, 1072, 18, "点击行修改 · 右键返回", EPD_DRAW_ALIGN_CENTER, false);
    ui_nav_draw(fb, 0);
}

/* 编辑态：标题避开状态栏 + 完成 + 说明 + 输入框 + 键盘 694（与备忘录编辑器同底）。 */
static void render_wx_editor(uint8_t *fb) {
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, 684, NX_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    ui_text_vc(fb, 342, 96, 24, s_wx_edit == 0 ? "编辑地点" : "编辑密钥", EPD_DRAW_ALIGN_CENTER, false);
    ui_text(fb, 642, 128, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    ui_text(fb, 36, 160, 21, s_wx_edit == 0 ? "输入地点，天气按新地址更新"
                                             : "心知天气 API Key（seniverse.com 注册）",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text_input_draw(fb, &s_wx_input, (EpdRect){36, 200, 612, 82}, 28, false, NULL);
    if (s_wx_notice[0]) ui_text_fixed_vc(fb, 36, 520, 22, s_wx_notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_keyboard_draw(fb, 694);
}

/* 键盘局部刷新（device settings profile 编辑器同款）：词栏 settle 走整像素 GL16。
 * 输入框矩形必须与 render_wx_editor 一致（36,200,612,82）——此前 168 与渲染 200
 * 不一致，局部刷新在输入框上方多画一个内容框（用户 2026-10-19 真机反馈）。 */
static app_redraw_t wx_editor_paint(app_ctx_t *ctx, bool field) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, 694,
        (EpdRect){36, 200, 612, 82}, 28, false, NULL, field);
    s_wx_area = update.area; s_wx_layout = update.layout; s_wx_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}

/* 打开字段编辑：默认预填当前生效源的值（NVS 优先，回退 TF 卡）。Key 走英文全键盘。 */
static void wx_edit_begin(app_ctx_t *ctx, int field) {
    (void)ctx;
    s_wx_edit = field;
    char city[48] = {0}, key[40] = {0};
    (void)weather_config_get(city, sizeof(city), key, sizeof(key));
    if (field == 0) snprintf(s_wx_city, sizeof(s_wx_city), "%s", city);
    else snprintf(s_wx_key, sizeof(s_wx_key), "%s", key);
    ui_text_edit_init(&s_wx_input, field == 0 ? s_wx_city : s_wx_key,
                      field == 0 ? sizeof(s_wx_city) : sizeof(s_wx_key));
    ui_keyboard_begin(&s_wx_input, field == 1);
    s_wx_notice[0] = 0;
}

/* 保存：编辑字段并入另一字段的当前生效值，全量写入 NVS（此后优先）。
 * v6：先冲刷未上屏组合（点确定/物理键保存路径都带上用户输入，2026-10-19 用户反馈）。 */
static app_redraw_t wx_save_field(app_ctx_t *ctx) {
    ui_keyboard_commit_pending();
    char city[48] = {0}, key[40] = {0};
    (void)weather_config_get(city, sizeof(city), key, sizeof(key));
    int field = s_wx_edit;
    if (field == 0) {
        if (!s_wx_city[0]) { snprintf(s_wx_notice, sizeof(s_wx_notice), "地点不能为空"); return APP_REDRAW_PAGE; }
        snprintf(city, sizeof(city), "%s", s_wx_city);
    } else {
        if (!s_wx_key[0]) { snprintf(s_wx_notice, sizeof(s_wx_notice), "Key 不能为空"); return APP_REDRAW_PAGE; }
        snprintf(key, sizeof(key), "%s", s_wx_key);
    }
    esp_err_t err = weather_config_save(city, key);
    ui_keyboard_end();
    s_wx_edit = -1;
    snprintf(s_wx_notice, sizeof(s_wx_notice),
             err == ESP_OK ? "已保存，联网后自动更新" : "保存失败，请检查存储空间");
    (void)ctx;
    return APP_REDRAW_PAGE;
}

static app_redraw_t wx_editor_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(ev->x0, ev->y0, 694, ctx->now_ms) ? wx_editor_paint(ctx, false) : APP_REDRAW_NONE;
    bool feedback = ev->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_R) {
        bool page = ui_keyboard_page(ev->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? wx_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return feedback ? wx_editor_paint(ctx, false) : APP_REDRAW_NONE;
    int x = ev->x0, y = ev->y0;
    if (y < 160) {
        if (x > 510) return wx_save_field(ctx);     /* 完成：保存并回天气页 */
        return APP_REDRAW_NONE;                     /* 标题区 */
    }
    if (ui_text_input_tap(&s_wx_input, (EpdRect){36, 200, 612, 82}, 28, false, NULL, x, y))
        return wx_editor_paint(ctx, true);
    ui_keyboard_result_t result = ui_keyboard_tap(x, y, 694, ctx->now_ms);
    if (result == UI_KEYBOARD_DONE) return wx_save_field(ctx);
    return result == UI_KEYBOARD_CHANGED ? wx_editor_paint(ctx, false) : APP_REDRAW_NONE;
}

static app_redraw_t wx_setup_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    /* 三行均可点：地点 / Key / 当前源——当前源点击直接修改地点（用户 2026-10-19）。 */
    for (int f = 0; f < 2; ++f) {
        if (ui_rect_hit(wx_row_rect(f), ev->x0, ev->y0)) { wx_edit_begin(ctx, f); return APP_REDRAW_FULL; }
    }
    if (ui_rect_hit(wx_row_rect(2), ev->x0, ev->y0)) { wx_edit_begin(ctx, 0); return APP_REDRAW_FULL; }
    if (ui_rect_hit(wx_reset_rect(), ev->x0, ev->y0)) {
        esp_err_t err = weather_config_reset();
        snprintf(s_wx_notice, sizeof(s_wx_notice),
                 err == ESP_OK ? "已恢复默认源（TF 卡 weather.txt）" : "恢复失败，请重试");
        return APP_REDRAW_FULL;
    }
    return APP_REDRAW_NONE;
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    epd_fill_rect((EpdRect){0, 0, 684, NX_NAV_TOP}, 0xf0, fb);
    ui_nav_status(fb);
    if (s_wx_edit >= 0) {          /* 天气编辑优先；键盘覆盖导航区，不画导航 */
        render_wx_editor(fb);
        return;
    }
    if (s_wx_setup) {
        render_wx_setup(fb);
        return;
    }
    if (s_goal_open) {
        /* 用户 2026-10-19：数据界面无导航栏（原第十五批要求补齐）；退出走右键/中键。 */
        render_goal_panel(fb);
        return;
    }
    draw_clockrow(fb);
    /* 2026-10-17：各区域间隙统一（≈44px 节奏）。v8（用户 2026-10-20）：本区双态
     * ——继续阅读 / 随心记（最近便签预览），区内下滑切换、再滑/上滑切回。 */
    int y = draw_tag(fb, 320, s_home_notes ? "随心记" : "继续阅读", true);
    if (s_home_notes) {
        EpdRect card = {NX_MARGIN, y, 684 - 2 * NX_MARGIN, HOME_CARD_H};
        ui_fill_round_rect(fb, card, NX_R_CARD, NX8_PAPER);
        ui_draw_control_frame(fb, card, NX_R_CARD, NX8_INK);
        const char *p = s_note_line;
        int line_y = card.y + 60;
        for (int row = 0; row < 5 && *p; ++row) {
            char line[112];
            size_t use = 0;
            while (*p && use + 4 < sizeof(line)) {
                size_t len = ((unsigned char)*p & 0x80) ? ((unsigned char)*p & 0xe0) == 0xc0 ? 2
                           : ((unsigned char)*p & 0xf0) == 0xe0 ? 3 : 4 : 1;
                memcpy(line + use, p, len); use += len; line[use] = 0; p += len;
                if (ttf_text_width_px(ui_text_effective_px(30), line) > card.width - 96) { use -= len; p -= len; break; }
            }
            line[use] = 0;
            ui_text_vc(fb, card.x + 48, line_y, 30, line, EPD_DRAW_ALIGN_LEFT, false);
            line_y += 52;
        }
        if (!s_note_line[0])
            ui_text_vc(fb, card.x + 48, card.y + 60, 30, "还没有便签，点击这里新建", EPD_DRAW_ALIGN_LEFT, false);
        if (s_note_date[0])
            ui_text_vc(fb, card.x + 48, card.y + HOME_CARD_H - 40, 20, s_note_date, EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(fb, card.x + card.width - 48, card.y + HOME_CARD_H - 40, 18,
                   "点击进入 · 再滑切回阅读", EPD_DRAW_ALIGN_RIGHT, false);
    } else {
        int card_w = (684 - 2 * NX_MARGIN - HOME_CARD_GAP) / 2;
        for (int i = 0; i < 2; ++i) {
            EpdRect card = {NX_MARGIN + (card_w + HOME_CARD_GAP) * i, y, card_w, HOME_CARD_H};
            s_layout.cards[i] = card;
            draw_home_card(fb, card, s_card_books[i], i == 0);
        }
    }
    s_notes_zone = (EpdRect){0, 320, 684, y - 320 + HOME_CARD_H};
    y = draw_tag(fb, y + HOME_CARD_H + 44, "阅读数据", true);
    s_layout.stats = (EpdRect){NX_MARGIN, y, 684 - 2 * NX_MARGIN, 230};
    draw_stats(fb);
    ui_nav_draw(fb, 0);
}

static bool present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_NONE) return true;
    /* 天气编辑：键盘局部刷新（词栏 settle 走整像素 GL16，布局切换 GL16，输入 DU）。 */
    if (redraw == APP_REDRAW_AREA && s_wx_edit >= 0) {
        if (s_wx_settle) {
            s_wx_settle = false;
            guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_wx_area));
            return true;
        }
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
            s_wx_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
            s_wx_layout ? MODE_GL16 : MODE_DU, s_wx_area));
        return true;
    }
    render(ctx, ctx->fb);
    enum EpdDrawError result;
    if (redraw == APP_REDRAW_AREA)
        /* v2（2026-10-19，提速）：局部补刷跟随快刷设置——快刷模式走 DU
         * （时钟行/封面均为点阵或 1bit 目标，阈值无损），普通模式保持 GL16。 */
        result = update_display_area_diff_with(ctx->hl, &E0470_WAVEFORM,
            app_settings_main_fast_refresh() ? MODE_DU : MODE_GL16, s_area);
    else if (redraw == APP_REDRAW_FULL || s_home_force_full_once) {
        s_home_force_full_once = false;
        result = update_display_full(ctx->hl);
    } else result = update_display_fast_page(ctx->hl);
    guard_draw_result(ctx->hl, result);
    return true;
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_goal_open = false;
    s_goal_month_offset = 0;
    s_cover_generation = book_cover_jobs_begin();
    const uint32_t progress_revision = book_progress_revision();
    s_home_force_full_once = app_settings_home_full_refresh();
    refresh_stats();
    load_home_note();
    struct tm now_bj = {0};
    if (date_now(&now_bj)) { s_last_min = now_bj.tm_min; s_last_day = now_bj.tm_mday; }
    else { s_last_min = -1; s_last_day = -1; }
    s_clock_poll_ms = 0;
    read_pico_sd_info_t sd = {0};
    esp_err_t error = read_pico_sd_get_info(&sd);
    bool same = s_books_cache_valid && error != ESP_ERR_NOT_FINISHED &&
                s_cached_store_revision == book_store_revision() &&
                s_cached_sd_present == sd.present && s_cached_sd_mounted == sd.mounted &&
                !strcmp(s_cached_books_dir, app_settings_books_dir());
    char last[BOOK_STORE_PATH_MAX] = {0};
    (void)book_progress_last_path(last, sizeof(last));
    bool changed = strcmp(last, s_cached_last_path);
    if (same && !changed) {
        if (s_cached_progress_revision != progress_revision) refresh_progress();
        s_scan_pending = false;
    } else {
        memset(&s_current, 0, sizeof(s_current));
        memset(s_recent, 0, sizeof(s_recent));
        s_card_books[0] = s_card_books[1] = NULL;
        s_scan_pending = !same;
        if (s_scan_pending) read_pico_sd_start_probe();
        scan_books();
        if (same || error != ESP_ERR_NOT_FINISHED) remember_scan_state(&sd);
        restore_cover_cache();
    }
    s_cached_progress_revision = progress_revision;
    s_cover_next = 0;
}

static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    clear_cover_cache();
    s_note_line[0] = 0; s_note_date[0] = 0;
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    s_card_books[0] = s_card_books[1] = NULL;
    s_books_cache_valid = false;
}

static void on_media_ready(app_ctx_t *ctx) {
    (void)ctx;
    s_books_cache_valid = false;
    s_scan_pending = true;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    /* 天气编辑：候选窗空闲自动重绘（同备忘录编辑器）。 */
    if (s_wx_edit >= 0) {
        ui_keyboard_result_t r = ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms);
        return r == UI_KEYBOARD_CHANGED ? wx_editor_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (ctx->consumed || (ctx->touch && ctx->touch->touched)) return APP_REDRAW_NONE;
    /* 状态栏身份头像：解码完成后局部刷新状态栏（首帧/换头像后狐狸占位）。 */
    static bool s_avatar_wait;
    bool avatar_pending = nothing_statusbar_avatar_pending();
    if (!avatar_pending) {
        if (s_avatar_wait) {
            s_avatar_wait = false;
            render(ctx, ctx->fb);
            s_area = (EpdRect){0, 24, 684, 52};
            return APP_REDRAW_AREA;
        }
    } else s_avatar_wait = true;
    if (s_scan_pending) {
        read_pico_sd_info_t info = {0};
        if (read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
        s_scan_pending = false;
        if (info.mounted) {
            s_cover_generation = book_cover_jobs_begin();
            memset(&s_current, 0, sizeof(s_current));
            memset(s_recent, 0, sizeof(s_recent));
            s_card_books[0] = s_card_books[1] = NULL;
            s_cover_next = 0;
            scan_books();
            restore_cover_cache();
            remember_scan_state(&info);
            return APP_REDRAW_PAGE;
        }
        remember_scan_state(&info);
    }
    /* 双卡封面渐进加载：rc95 异步作业（双 worker 并行）。
     * v3 修复（2026-10-19 真机反馈“书卡不显示内容”）：作业异步未就绪时必须保留重试
     * （首 tick 必然 waiting），否则槽位一次性耗尽、封面永远取不回。每轮两槽都提交
     * （submit 幂等）并取结果，任一就绪即整块重画一次；全部落定（就绪/失败/无书）后停止。 */
    if (s_cover_next < 2) {
        bool painted = false, waiting = false;
        for (int slot = 0; slot < 2; ++slot) {
            home_book_t *item = s_card_books[slot];
            if (!item || !item->path[0] || item->cover) continue;
            if (load_cover(item, slot)) painted = true;
            else if (s_cover_waiting) waiting = true;
        }
        if (painted) {
            render(ctx, ctx->fb);
            s_area = (EpdRect){NX_MARGIN - 3, s_layout.cards[0].y,
                               588 + 6, HOME_CARD_H + 6};
            return APP_REDRAW_AREA;
        }
        if (!waiting) s_cover_next = 2;
    }
    /* 分钟翻转：局部重画时钟行；跨日则刷新统计后整页重排。 */
    if (ctx->now_ms - s_clock_poll_ms >= 30000) {
            s_clock_poll_ms = ctx->now_ms;
            struct tm tm_bj = {0};
            if (date_now(&tm_bj)) {
                if (tm_bj.tm_mday != s_last_day) {
                    s_last_min = tm_bj.tm_min;
                    s_last_day = tm_bj.tm_mday;
                    refresh_stats();
                    return APP_REDRAW_PAGE;
                }
                if (tm_bj.tm_min != s_last_min) {
                    s_last_min = tm_bj.tm_min;
                    render(ctx, ctx->fb);
                    s_area = HOME_CLOCK_TICK_AREA;
                return APP_REDRAW_AREA;
            }
        }
    }
    /* v2 天气（用户 2026-10-19）：30s 节流内顺带轮询（联网且缓存不覆盖今天才发起，
     * 正常 3 天一次联网）；刷新完成→只刷右组条带（图标+地点+高低温）。 */
    {
        read_pico_transfer_status_t network = {0};
        read_pico_transfer_get_status(&network);
        if (weather_service_tick(network.network_ready)) {
            render(ctx, ctx->fb);
            s_area = HOME_WX_AREA;
            return APP_REDRAW_AREA;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (s_wx_edit >= 0) return wx_editor_gesture(ctx, ev);
    /* v7（用户 2026-10-20）：左右滑动换主页面，效果与点按底部导航一致；
     * 首页为最左页只左滑去书架；起点在导航热区（下沿上扩 16px）不触发。 */
    if (ev->type == UI_GESTURE_SWIPE_L && ev->y0 < UI_NAV_TOUCH_TOP) {
        ui_nav_request(ctx, 1);
        return APP_REDRAW_NONE;
    }
    /* v8：随心记双态切换——区内下滑进入、再下滑/上滑切回；面板/设置页展开时不抢。 */
    if (!s_wx_setup && !s_goal_open &&
        (ev->type == UI_GESTURE_SWIPE_D || ev->type == UI_GESTURE_SWIPE_U) &&
        s_notes_zone.width && ui_rect_hit(s_notes_zone, ev->x0, ev->y0)) {
        if (!s_home_notes && ev->type == UI_GESTURE_SWIPE_D) { s_home_notes = true; return APP_REDRAW_PAGE; }
        if (s_home_notes) { s_home_notes = false; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    /* v6：阅读数据面板恢复导航栏，导航带命中放开（点击切页离开面板自动收尾）；天气设置页保留。 */
    int tab = ui_nav_hit(ev->x0, ev->y0);
    if (tab >= 0) { ui_nav_request(ctx, tab); return APP_REDRAW_NONE; }
    /* v8：随心记态点击本区进入便签应用。 */
    if (s_home_notes && !s_wx_setup && !s_goal_open &&
        s_notes_zone.width && ui_rect_hit(s_notes_zone, ev->x0, ev->y0)) {
        extern const app_desc_t app_notes;
        ctx->request_app = &app_notes;
        return APP_REDRAW_NONE;
    }
    if (s_wx_setup) return wx_setup_gesture(ctx, ev);
    if (s_goal_open) {
        /* 换月箭头 */
        if (ui_rect_hit(goal_month_arrow(-1), ev->x0, ev->y0) ||
            ui_rect_hit(goal_month_arrow(1), ev->x0, ev->y0)) {
            int dir = ui_rect_hit(goal_month_arrow(1), ev->x0, ev->y0) ? 1 : -1;
            int next = s_goal_month_offset + dir;
            if (next >= -12 && next <= 6) { s_goal_month_offset = next; s_goal_sel_day = 0; }
            return APP_REDRAW_PAGE;
        }
        /* 点击日期：选中并联动老黄历（选今天则回 0）。 */
        struct tm tm_hit = {0};
        if (date_now(&tm_hit)) {
            int cy0 = tm_hit.tm_year + 1900, cm0 = tm_hit.tm_mon + 1;
            for (int k = 0; k < -s_goal_month_offset; ++k) { if (--cm0 == 0) { cm0 = 12; --cy0; } }
            for (int k = 0; k < s_goal_month_offset; ++k) { if (++cm0 == 13) { cm0 = 1; ++cy0; } }
            static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            int dm = md[cm0 - 1];
            if (cm0 == 2 && (cy0 % 4 == 0 && (cy0 % 100 != 0 || cy0 % 400 == 0))) dm = 29;
            int first = goal_weekday(cy0, cm0, 1);
            for (int d = 1; d <= dm; ++d) {
                int slot = first + d - 1, col = slot % 7, row = slot / 7;
                int cx = 36 + 43 + col * 87, cyc = GOAL_CAL_TOP + 156 + row * GOAL_ROW_H;
                EpdRect hit_r = {cx - 40, cyc - 40, 80, 80};
                if (ui_rect_hit(hit_r, ev->x0, ev->y0)) {
                    s_goal_sel_day = (d == tm_hit.tm_mday && cy0 == tm_hit.tm_year + 1900 &&
                                      cm0 == tm_hit.tm_mon + 1) ? 0 : d;
                    return APP_REDRAW_PAGE;
                }
            }
        }
        bool plus = ui_rect_hit(goal_plus_rect(), ev->x0, ev->y0);
        bool minus = ui_rect_hit(goal_minus_rect(), ev->x0, ev->y0);
        if (plus || minus) {
            int g = (int)app_settings_home_goal_minutes() + (plus ? 15 : -15);
            if (g < 15) g = 15;
            if (g > 180) g = 180;
            (void)app_settings_set_home_goal_minutes((unsigned)g);
            refresh_stats();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    /* 首页时间区点击进入番茄时钟（用户 2026-10-20 第二十一批 c 段）。 */
    if (ui_rect_hit(HOME_CLOCK_AREA, ev->x0, ev->y0)) {
        extern const app_desc_t app_pomodoro;
        ctx->request_app = &app_pomodoro;
        return APP_REDRAW_NONE;
    }
    /* 首页天气区（时钟行右组）点击进入天气设置（用户 2026-10-19）。 */
    if (ui_rect_hit(HOME_WX_AREA, ev->x0, ev->y0)) {
        s_wx_setup = true;
        s_wx_notice[0] = 0;
        return APP_REDRAW_FULL;
    }
    if (ui_rect_hit(s_layout.stats, ev->x0, ev->y0)) {
        /* 点阅读数据卡弹出阅读目标 + 月历（2026-10-17）。 */
        refresh_stats();
        s_goal_open = true;
        return APP_REDRAW_FULL;
    }
    for (int i = 0; i < 2; ++i) {
        home_book_t *item = s_card_books[i];
        if (item && item->path[0] && ui_rect_hit(s_layout.cards[i], ev->x0, ev->y0)) {
            app_book_request_open_from_home(item->path);
            ui_nav_request(ctx, 1);
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    /* 天气编辑：中键/右键均先保存，回天气设置页；左键取消不保存（空字段逃逸出口）。 */
    if (s_wx_edit >= 0) {
        if (key == UI_KEY_1) { ui_keyboard_end(); s_wx_edit = -1; return APP_REDRAW_PAGE; }
        return wx_save_field(ctx);
    }
    if (s_wx_setup && key == UI_KEY_3) { s_wx_setup = false; return APP_REDRAW_PAGE; }
    /* 目标面板期间右键 = 关闭；首页本身是导航根页，按键停留。 */
    if (s_goal_open && key == UI_KEY_3) {
        s_goal_open = false;
        return APP_REDRAW_PAGE;
    }
    (void)ctx;
    return APP_REDRAW_NONE;
}

/* v1.8：切页离开首页必收尾编辑器/键盘（名字避开 stdlib 的 on_exit）。 */
static void home_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    if (s_wx_edit >= 0) { ui_keyboard_end(); s_wx_edit = -1; }
    s_wx_setup = false;
    s_goal_open = false;
}

static bool home_main_visible(app_ctx_t *ctx) { (void)ctx; return true; }
static EpdRect area_hint(app_ctx_t *ctx) { (void)ctx; return s_area; }

const app_desc_t app_dashboard = {
    .title = "首页 Home",
    .detail = "继续阅读与近七天阅读",
    .enter_full = false,
    .owns_keys = true,
    .main_page_visible = home_main_visible,
    .on_enter = on_enter,
    .on_exit = home_on_exit,
    .on_media_lost = on_media_lost,
    .on_media_ready = on_media_ready,
    .render = render,
    .present = present,
    .on_tick = on_tick,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .area_hint = area_hint,
};
