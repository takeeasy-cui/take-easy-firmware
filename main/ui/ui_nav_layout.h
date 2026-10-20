/* SPDX-License-Identifier: Apache-2.0
 * 中文：轻量底栏边界，绘制和刷新共用，防止局部扫描碰到固定图标。
 * English: Lightweight navigation bounds shared by painting and presentation to protect fixed icons.
 */
#pragma once
/* 与 ui_nothing 的 NX_NAV_TOP 对齐（115px 四键圆盘带）；派生刷新边界经核算
 * 仍落在同一组 32 列线上（1088），不影响上游快刷机制。 */
/* Align with ui_nothing's NX_NAV_TOP (115 px four-disc band); the derived
 * refresh boundaries still round to the same 32-column lines (1088), so the
 * upstream fast-refresh machinery is untouched. */
#define UI_NAV_TOP 1101
// 只扩大触控热区，不移动图标或改变内容/底栏的刷新边界。
// Expand touch admission only; icons and content/navigation refresh boundaries stay fixed.
#define UI_NAV_TOUCH_TOP (UI_NAV_TOP - 16)
// 过渡水位线：5 行横条，字节运算要求起点奇数、终点偶数（半字节守卫）。
// 绝对位置与上游原生布局一致（1097..1101），随导航带 1101 相对上移到内容与底栏之间的间隙。
// Transition water-line: a five-row strip; byte math needs an odd top and an even
// bottom (nibble guards). Absolute rows match the upstream layout (1097..1101).
#define UI_NAV_MARKER_TOP (UI_NAV_TOP - 4)
#define UI_NAV_MARKER_BOTTOM (UI_NAV_TOP + 1)
// 内容向内收至32列边界；横条单独短刷或并入快刷，外扩范围均止于图标首笔之前。
// Round content inward to 32 columns; marker-only or fused fast scans end before any icon ink.
#define UI_NAV_REFRESH_END (UI_NAV_TOP & ~31)
#define UI_NAV_MARKER_SCAN_END ((UI_NAV_MARKER_BOTTOM + 31) & ~31)
