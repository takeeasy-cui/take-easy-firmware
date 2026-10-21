/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 产品页清单。导航四栏：首页/书架/应用/设置；文件、计算器、图片为后台页。
 *
 * Product page table. Nav tabs: Home/Shelf/Apps/Settings; Files, Calculator and
 * Image stay background pages reached from the Apps tab or the file page.
 */

#include "app_registry.h"

extern const app_desc_t app_book;
extern const app_desc_t app_image;
extern const app_desc_t app_dashboard;
extern const app_desc_t app_apps;
extern const app_desc_t app_files;
extern const app_desc_t app_calc;
extern const app_desc_t app_device_settings;
extern const app_desc_t app_notes;
extern const app_desc_t app_pomodoro;
extern const app_desc_t app_muyu;

// 正式产品页面清单；工厂测试与历史示例不进入用户导航。
// V1.0 定稿（用户 2026-10-21）：三游戏入口（推理/文字/步步高）已按用户要求移除；
// 薄壳与 GameCore 源码保留在树中待后续版本恢复（索引 0-6 依赖保持不变）。
static const app_desc_t* const s_apps[] = {
    &app_dashboard,
    &app_book,
    &app_apps,
    &app_device_settings,
    &app_files,
    &app_calc,
    &app_image,
    &app_notes,
    &app_pomodoro,
    &app_muyu,
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
    }
    return -1;
}

const app_desc_t* app_home_page(void) {
    return &app_dashboard;
}
