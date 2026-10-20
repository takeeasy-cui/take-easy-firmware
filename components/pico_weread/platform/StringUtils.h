/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 有界 UTF-8 文件名转换。/ Bounded UTF-8 filename conversion.
 * 冻结：拒绝路径分隔符和控制字符。/ Frozen: reject path separators and control characters.
 */
#pragma once
#include <string>
#include <cstring>
namespace StringUtils {
inline std::string sanitizeFilename(const std::string& input, size_t limit) {
    std::string result;
    result.reserve(limit);
    for (size_t i = 0; i < input.size();) {
        unsigned char c = input[i];
        size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        if (i + n > input.size() || result.size() + n > limit) break;
        if (c < 0x20 || c == 0x7f || (c < 0x80 && strchr("/\\:*?\"<>|", c))) result += '_';
        else result.append(input, i, n);
        i += n;
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    if (result.empty() || result == "." || result == "..") result = "book";
    return result;
}
}
