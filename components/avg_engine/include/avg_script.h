/*
 * avg_script.h — 案件脚本解析器：SD 卡文本脚本 → 内存命令数组。
 *
 * 脚本语法（PyWright 风格命令行，一行一令；// 后为注释）：
 *   case id "标题"
 *   label name | jump name | ifset flag name | ifclear flag name | flag name
 *   bg bg_file.png                      背景图（app 根下 bg/ 子目录）
 *   char 0|1|2 char_file.png L|C|R 0|1   立绘槽位/文件/站位/是否聚焦
 *   char_off 0|1|2
 *   say "说话人" "文本" | think "文本"
 *   voicemap "说话人" "立绘.png"          说话人→立绘映射（元数据不计命令；
 *                                        同名重复声明=覆盖，用于分章换装）
 *   gain_ev evidence_id                  加入证物簿（证据表见下）
 *   ev id "名称" "描述"                  定义证据（脚本头部集中定义）
 *   testimony "证言标题"                 开始证言面板
 *   stmt "证言文本"                      证言条目（紧随其后，1..12 条）
 *   press 3 label_name                   威慑第 3 条 → 跳转
 *   clash 3 evidence_id label_name       第 3 条与证据矛盾 → 跳转（逆转点）
 *   ask_ev label_wrong                   要求出示证据；出示错误统一跳 label_wrong
 *   end 1|0                              判决页（1=无罪 0=有罪）
 *
 * Script parser: case text file into an in-memory command array. One
 * command per line, // comments, evidence declared up front.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "avg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[48];               /* 说话人名（与 say 第一引号段全等匹配） */
    char file[96];               /* 立绘文件名（裸名即可，路径由平台层回退解析） */
} avg_voice_map_t;

typedef struct {
    /* 解析产物（cmds 按实量 PSRAM 分配并在解析完成时收缩——2026-10-18：
     * 固定 AVG_MAX_CMDS 数组吃 ~800KB，case1 仅 108 条利用率 10%，
     * 是推理游戏期间 PSRAM 峰值的主源（字体 open 被挤掉→用户字体被拉黑
     * 回滚到内置的根因链）。labels/evidence 固定小数组仍留本体。 */
    avg_cmd_t*     cmds;              /* 实量命令数组（PSRAM），NULL=未加载 */
    int            cmd_count;
    int            cmd_cap;           /* 分配容量（解析期按行数估算） */
    avg_label_t    labels[AVG_MAX_LABELS];
    int            label_count;
    avg_evidence_t evidence[AVG_MAX_EVIDENCE];
    int            evidence_count;
    avg_voice_map_t voice_maps[AVG_MAX_VOICE_MAP];  /* 说话人→立绘（同名覆盖） */
    int            voice_map_count;
    char case_id[24];
    char case_title[128];

    /* 语法/容量错误报告（解析失败时非空）。 */
    char error[160];
} avg_script_t;

/// 从 SD 卡脚本文件解析（UTF-8）。失败返回 false，error 字段给出可读原因。
/// Parse a case script file (UTF-8). false + readable error on failure.
bool avg_script_load_file(avg_script_t* s, const char* path);

/// 释放 cmds 数组（本体及 labels/evidence 由拥有者管理）。quit/load 失败时调用。
void avg_script_free(avg_script_t* s);

/// 解析内存缓冲（测试与文件共用）。失败同上。
bool avg_script_parse(avg_script_t* s, const char* text, size_t len);

/// 标签名 → 命令索引；-1 未知。
int avg_script_label(const avg_script_t* s, const char* name);

/// 证据 id → 索引；-1 未知。
int avg_script_evidence(const avg_script_t* s, const char* id);

/// 说话人 → 立绘映射；NULL=未映射（平台层据此自动跟随说话人切换立绘）。
const avg_voice_map_t* avg_script_voice(const avg_script_t* s, const char* name);

#ifdef __cplusplus
}
#endif
