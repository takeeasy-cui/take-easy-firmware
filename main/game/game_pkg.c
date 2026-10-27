/*
 * game_pkg.c — TF 卡资源包：分类扫描、zip 解挂、目录直挂。
 *
 * 挂载语义（精简可靠）：
 *  - zip 包 → 整包解压到 /sdcard/.gcache/<name>/（单层小文件逐条展开，
 *    zip 大小标记 .gcsize 变化时自动清缓存重解）；之后插件全部走普通
 *    文件 IO，现有解码器（book_image_file_*、avg_script_load_file 等）
 *    零改动复用。
 *  - 目录包 → 直接挂 /sdcard/games/<name>/。BBK 的大资源（DAT.LIB/
 *    FMJHD.BIN 数十 MB）超过 zip 单条目 8MiB 解压上限，目录直挂是标准方式。
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* 路径拼接热点：目录名+条目名的理论极值组合频繁触发保守截断告警，
 * 实测缓冲已足够（见 GC_PATH_MAX 备注）；与 ttf_font 的 unused 豁免同风格。 */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include "esp_log.h"
#include "zip_reader.h"

#include "game_core.h"

#define GAMES_ROOT   "/sdcard/games"
#define GCACHE_ROOT  "/sdcard/.gcache"
#define GC_PKG_SLOTS 8
#define GC_PATH_MAX  512

static char s_mounted_name[96];
static char s_mounted_dir[GC_PATH_MAX];

static void ensure_dirs(void) {
    (void)mkdir(GAMES_ROOT, 0775);
    (void)mkdir(GCACHE_ROOT, 0775);
}

/* 一个分类包条目：包名（zip 去扩展名）+ 形态。 */
typedef struct { char name[48]; bool zip; bool pad; } pkg_item_t;

/* UTF-8 安全截断（不劈开多字节序列）。 */
static void utf8_trunc(char* s, size_t max) {
    size_t n = strlen(s);
    if (n <= max) return;
    n = max;
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80) --n;
    s[n] = 0;
}

/* 包显示名：读 story.txt/script.txt 的标题行。兼容两代格式：
 *  - Story Engine v1（text-/novel-）："* title 标题"（取首条）。
 *  - 旧 avgr 格式："case id \"标题\""（最后一条生效，引擎同语义）。
 * zip：从 .gcache/<名>/ 读（挂载过的包必然已解出；未挂载的首扫显示原名，
 * 进一次后即有标题）。目录包：直读 <目录>/ 下。
 * 栈预算（2026-10-26 真机爆栈修复）：主任务 8KB 栈内逐包调用，
 * path[256]+line[160]，单次 ≤450B。 */
static void pkg_display_name(const pkg_item_t* it, char* out, size_t cap) {
    char path[256];
    static const char* files[] = { "story.txt", "script.txt" };
    for (unsigned fi = 0; fi < sizeof(files) / sizeof(files[0]); ++fi) {
        if (it->zip) snprintf(path, sizeof(path), GCACHE_ROOT "/%s/%s", it->name, files[fi]);
        else snprintf(path, sizeof(path), GAMES_ROOT "/%s/%s", it->name, files[fi]);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char line[160], best[128] = {0};
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "* title", 7) == 0) {      /* v1：首条生效 */
                char* t = line + 7;
                while (*t == ' ') ++t;
                char* nl = strpbrk(t, "\r\n");
                if (nl) *nl = 0;
                if (*t && !best[0]) {                     /* 只记首条 */
                    snprintf(best, sizeof(best), "%s", t);
                    utf8_trunc(best, sizeof(best) - 1);
                }
            } else if (strncmp(line, "case ", 5) == 0) {  /* 旧：最后一条生效 */
                char* q1 = strchr(line + 5, '"');
                if (q1) {
                    char* q2 = strchr(q1 + 1, '"');
                    if (q2) {
                        size_t n = (size_t)(q2 - q1 - 1);
                        if (n >= sizeof(best)) n = sizeof(best) - 1;
                        memcpy(best, q1 + 1, n);
                        best[n] = 0;
                    }
                }
            }
        }
        fclose(f);
        if (best[0]) {
            snprintf(out, cap, "%s", best);
            utf8_trunc(out, cap - 1);
            return;
        }
    }
    snprintf(out, cap, "%s", it->name);
}

/* 收集分类包；稳定排序 zip 在前、目录在后（同前缀下 zip 优先，挂载确定）。 */
static int pkg_collect(gc_cat_t cat, pkg_item_t* items, int slots) {
    const char* prefix = gc_cat_prefix(cat);
    size_t plen = strlen(prefix);
    int n = 0;
    DIR* d = opendir(GAMES_ROOT);
    if (!d) return 0;
    struct dirent* e;
    char full[256];   /* 循环外复用（2026-10-26 爆栈修复）：卡内路径 <200B，512B 浪费主任务栈 */
    while ((e = readdir(d)) != NULL && n < slots) {
        /* 前缀大小写不敏感（2026-10-23：用户 TF 上出现过 AVGR-* 大写命名）。 */
        if (strncasecmp(e->d_name, prefix, plen) != 0 || e->d_name[plen] != '-') continue;
        snprintf(full, sizeof(full), GAMES_ROOT "/%.200s", e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool is_zip = strstr(e->d_name, ".zip") != NULL && S_ISREG(st.st_mode);
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_zip && !is_dir) continue;
        snprintf(items[n].name, sizeof(items[n].name), "%.47s", e->d_name);
        char* dot = strstr(items[n].name, ".zip");
        if (is_zip && dot) *dot = 0;
        items[n].zip = is_zip;
        ++n;
    }
    closedir(d);
    /* 插入排序：zip(...zip=true) 排在目录(false)前，保持各自原序稳定。 */
    for (int i = 1; i < n; ++i) {
        pkg_item_t key = items[i];
        int j = i - 1;
        while (j >= 0 && !items[j].zip && key.zip) { items[j + 1] = items[j]; --j; }
        items[j + 1] = key;
    }
    return n;
}

/* ---- zip 展开（函数体在后）；声明前置：gc_pkg_scan 预解用 ---- */
static long file_size_of(const char* path);
static bool cache_valid(const char* cache_dir, long zip_size);
static void wipe_dir(const char* dir, int depth);
static bool unzip_into(const char* zip_path, const char* dst_dir);

int gc_pkg_scan(gc_cat_t cat, char* out, size_t capacity, int slots) {
    ensure_dirs();
    if (!out || capacity < 8 || slots <= 0) return 0;
    if (slots > GC_PKG_SLOTS) slots = GC_PKG_SLOTS;
    pkg_item_t items[GC_PKG_SLOTS];
    int n = pkg_collect(cat, items, slots);
    memset(out, 0, (size_t)n * capacity);
    for (int i = 0; i < n; ++i) {
        /* zip 缓存未建时先解出（列表页要读 story.txt 取显示名）。
         * 故事包 ≤ 数百 KB，秒级；超时/失败则显示原名不阻列表。 */
        if (items[i].zip) {
            char zip_path[GC_PATH_MAX], cache_dir[GC_PATH_MAX];
            snprintf(zip_path, sizeof(zip_path), GAMES_ROOT "/%s.zip", items[i].name);
            snprintf(cache_dir, sizeof(cache_dir), GCACHE_ROOT "/%s", items[i].name);
            long zsize = file_size_of(zip_path);
            if (zsize > 0 && !cache_valid(cache_dir, zsize)) {
                (void)mkdir(cache_dir, 0775);
                wipe_dir(cache_dir, 0);
                if (!unzip_into(zip_path, cache_dir)) wipe_dir(cache_dir, 0);
            }
        }
        char disp[96];
        pkg_display_name(&items[i], disp, sizeof(disp));
        utf8_trunc(disp, capacity - 1);
        snprintf(out + (size_t)i * capacity, capacity, "%s", disp);
    }
    return n;
}

/* ---- zip 展开；.gcsize 标记缓存有效性（zip 变了自动重解） ---- */

static long file_size_of(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static bool cache_valid(const char* cache_dir, long zip_size) {
    char meta[GC_PATH_MAX], line[32];
    snprintf(meta, sizeof(meta), "%s/.gcsize", cache_dir);
    FILE* f = fopen(meta, "r");
    if (!f) return false;
    bool ok = fgets(line, sizeof(line), f) != NULL && atol(line) == zip_size;
    fclose(f);
    return ok;
}

/* 递归清空包缓存（约定深度 ≤4 层，超出按文件尝试后跳过）。 */
static void wipe_dir(const char* dir, int depth) {
    if (depth > 4) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[GC_PATH_MAX];
        snprintf(p, sizeof(p), "%s/%.250s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            wipe_dir(p, depth + 1);            /* 先清空再删目录 */
            (void)rmdir(p);
        } else {
            (void)unlink(p);
        }
    }
    closedir(d);
}

static bool unzip_into(const char* zip_path, const char* dst_dir) {
    zip_reader_t* z = NULL;
    if (zip_open(zip_path, &z) != ESP_OK || !z) return false;
    bool ok = true;
    size_t count = zip_entry_count(z);
    for (size_t i = 0; i < count && ok; ++i) {
        const char* name = zip_entry_name(z, (int)i);
        size_t size = zip_entry_size(z, (int)i);
        if (!name || !name[0] || name[strlen(name) - 1] == '/') continue;
        if (size > ZIP_OUTPUT_MAX) {          /* 大条目跳过：BBK 类资源走目录直挂 */
            ESP_LOGW("game_pkg", "skip large entry %s (%u B)", name, (unsigned)size);
            continue;
        }
        char dst[GC_PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/%.250s", dst_dir, name);
        /* 条目含子目录（images/bg/…）：逐级建目录再落盘。 */
        for (char* sep = dst + strlen(dst_dir) + 1; (sep = strchr(sep, '/')) != NULL; ++sep) {
            *sep = 0;
            (void)mkdir(dst, 0775);
            *sep = '/';
        }
        FILE* out = fopen(dst, "wb");
        if (!out) { ok = false; break; }
        if (size) {
            uint8_t* buf = malloc((size_t)size);
            if (!buf || zip_extract(z, (int)i, buf, (size_t)size) != ESP_OK ||
                fwrite(buf, 1, (size_t)size, out) != (size_t)size)
                ok = false;
            free(buf);
        }
        fclose(out);
    }
    zip_close(z);
    if (ok) {                                 /* 写缓存标记（zip 字节数）。 */
        char meta[GC_PATH_MAX], text[32];
        snprintf(meta, sizeof(meta), "%s/.gcsize", dst_dir);
        snprintf(text, sizeof(text), "%ld", file_size_of(zip_path));
        FILE* m = fopen(meta, "w");
        if (m) { fputs(text, m); fclose(m); }
    }
    return ok;
}

const char* gc_pkg_mount(gc_cat_t cat, int index) {
    ensure_dirs();
    pkg_item_t items[GC_PKG_SLOTS];
    int n = pkg_collect(cat, items, GC_PKG_SLOTS);
    if (index < 0 || index >= n) return NULL;

    const pkg_item_t* it = &items[index];
    if (!it->zip) {                           /* 目录直挂：无缓存层。 */
        snprintf(s_mounted_name, sizeof(s_mounted_name), "%s", it->name);
        snprintf(s_mounted_dir, sizeof(s_mounted_dir), GAMES_ROOT "/%s", it->name);
        return s_mounted_dir;
    }
    char zip_path[GC_PATH_MAX], cache_dir[GC_PATH_MAX];
    snprintf(zip_path, sizeof(zip_path), GAMES_ROOT "/%s.zip", it->name);
    snprintf(cache_dir, sizeof(cache_dir), GCACHE_ROOT "/%s", it->name);
    long zsize = file_size_of(zip_path);
    if (zsize <= 0) return NULL;
    if (cache_valid(cache_dir, zsize)) {      /* 命中缓存：秒挂 */
        snprintf(s_mounted_name, sizeof(s_mounted_name), "%s", it->name);
        snprintf(s_mounted_dir, sizeof(s_mounted_dir), "%s", cache_dir);
        return s_mounted_dir;
    }
    (void)mkdir(cache_dir, 0775);
    wipe_dir(cache_dir, 0);
    if (!unzip_into(zip_path, cache_dir)) {
        ESP_LOGE("game_pkg", "unzip failed: %s", zip_path);
        wipe_dir(cache_dir, 0);
        return NULL;
    }
    snprintf(s_mounted_name, sizeof(s_mounted_name), "%s", it->name);
    snprintf(s_mounted_dir, sizeof(s_mounted_dir), "%s", cache_dir);
    return s_mounted_dir;
}

void gc_pkg_unmount(void) {
    /* 保留缓存目录（下次秒挂），只丢指针。 */
    s_mounted_name[0] = 0;
    s_mounted_dir[0] = 0;
}
