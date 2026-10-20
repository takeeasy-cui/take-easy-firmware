/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 微信读书后台任务与 UI 快照契约。/ WeRead worker and UI snapshot contract.
 * 冻结：只读云端进度；仅成品进入现有书库。/ Frozen: read-only cloud progress; only finished books enter the library.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define WEREAD_ROWS 7
#define WEREAD_BATCH_MAX 1024
typedef enum {
    WEREAD_IDLE, ///< 就绪 / Ready
    WEREAD_CONNECTING, ///< 联网 / Connecting
    WEREAD_WORKING, ///< 同步或下载 / Syncing or downloading
    WEREAD_QR, ///< 等待扫码 / Awaiting scan
    WEREAD_COMPLETE, ///< 完成 / Complete
    WEREAD_FAILED, ///< 失败 / Failed
    WEREAD_CANCELLED, ///< 已取消 / Cancelled
} weread_state_t;
typedef enum {
    WEREAD_LOAD, ///< 读取本地书架 / Load cached shelf
    WEREAD_SYNC, ///< 联网登录与同步 / Login and sync online
    WEREAD_DOWNLOAD, ///< 下载书籍 / Download a book
    WEREAD_LOGOUT, ///< 清除会话与书架 / Clear session and shelf
    WEREAD_BATCH, ///< 串行批量下载 / Sequential batch download
} weread_action_t;
typedef struct {
    char id[64]; ///< 云端书号 / Remote book ID
    char title[192]; ///< 显示书名 / Display title
    char author[96]; ///< 作者 / Author
    char local_path[288]; ///< 已下载路径 / Completed download path
} weread_book_t;
typedef struct {
    char id[64]; ///< 稳定云端书号，跨页或重排序不串书 / Stable ID across pages or reordering
    unsigned index; ///< 缓存索引提示 / Cached index hint
} weread_selection_t;
typedef enum { WEREAD_CHAPTERS, WEREAD_PREPARING, WEREAD_IMAGES, WEREAD_PACKAGING } weread_stage_t;
typedef struct {
    weread_action_t action; ///< 当前操作 / Current operation
    weread_stage_t stage; ///< 下载阶段 / Download stage
    unsigned skipped_images; ///< 无法获取的插图数 / Unavailable illustrations
    weread_state_t state; ///< 生命周期 / Lifecycle
    bool active; ///< 后台任务仍存在 / Worker still running
    bool logged_in; ///< 本地有会话 / Local session exists
    unsigned revision; ///< 状态版本 / Status revision
    unsigned total; ///< 书架总数 / Shelf count
    unsigned page; ///< 零起始页码 / Zero-based page
    unsigned count; ///< 当前页数量 / Visible count
    unsigned done, target; ///< 下载进度 / Download progress
    unsigned batch_total, batch_current, batch_success, batch_failed; ///< 批次进度 / Batch counters
    char batch_title[192]; ///< 当前下载书名 / Current download title
    unsigned changed; ///< 成品发布版本 / Completed-publication revision
    int error; ///< 协议或连接错误 / Protocol or connection error
    char qr[320]; ///< 登录确认网址 / Login confirmation URL
    char output[288]; ///< 最近完成文件 / Last completed file
    weread_book_t books[WEREAD_ROWS]; ///< 当前页书籍 / Visible books
} weread_snapshot_t;
/// 已挂载 TF 卡目录；空闲时由 UI 配置。/ UI configures mounted SD roots while idle.
bool weread_configure(const char* cache_root, const char* books_root);
/// 启动一次任务，成功表示已派发。/ Start one worker; true means dispatched.
bool weread_start(weread_action_t action, unsigned page, unsigned index);
/// 复制选择后仅启动一个后台任务，逐本下载；失败书不阻塞后续，取消停止整个队列。
/// Copy selection into one worker; download sequentially, continue after book errors, cancel the whole queue.
bool weread_start_batch(unsigned page, const weread_selection_t* selection, unsigned count);
/// 线程安全读取快照。/ Copy a consistent snapshot across threads.
void weread_snapshot(weread_snapshot_t* out);
/// 下一次下载是否嵌入插图；仅空闲时修改。/ Set inline-image policy while idle.
bool weread_set_include_images(bool enabled);
/// 请求取消并等待清理；返回后可离页或卸载介质。/ Cancel and join cleanup before page exit or media teardown.
void weread_stop(void);
#ifdef __cplusplus
}
#endif
