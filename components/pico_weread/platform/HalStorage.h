/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * POSIX 文件适配，将微信缓存与成品限制到指定目录。
 * POSIX file adapter confining WeRead caches and completed books to configured roots.
 * 冻结：不挂载、不格式化、不访问驱动。/ Frozen: no mounting, formatting or driver access.
 */
#pragma once
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <utility>
#include <cstdint>
class HalFile {
    FILE* file_ = nullptr;
    DIR* dir_ = nullptr;
    std::string path_;
public:
    HalFile() = default;
    ~HalFile() { close(); }
    HalFile(const HalFile&) = delete;
    HalFile& operator=(const HalFile&) = delete;
    HalFile(HalFile&& other) noexcept { *this = std::move(other); }
    HalFile& operator=(HalFile&& other) noexcept;
    explicit operator bool() const { return isOpen(); }
    bool isOpen() const { return file_ || dir_; }
    bool isDirectory() const { return dir_; }
    bool open(const std::string& path, const char* mode);
    void close();
    int read(void* out, size_t size);
    size_t write(const void* data, size_t size);
    size_t write(uint8_t byte) { return write(&byte, 1); }
    bool seek(size_t offset) { return seek64(offset); }
    bool seek64(uint64_t offset);
    size_t position() const;
    uint64_t fileSize64() const;
    size_t fileSize() const { return static_cast<size_t>(fileSize64()); }
    bool available() const { return file_ && position() < fileSize64(); }
    void flush();
    HalFile openNextFile();
    size_t getName(char* out, size_t cap) const;
};
class PicoStorage {
    std::string cache_root_, books_root_;
public:
    /// 任务启动前配置已挂载目录。/ Configure mounted roots before starting a task.
    void configure(const char* cache, const char* books) { cache_root_ = cache; books_root_ = books; }
    std::string map(const std::string& path) const;
    bool ensureDirectoryExists(const std::string& path) const;
    bool exists(const std::string& path) const;
    bool remove(const std::string& path) const;
    bool removeDir(const std::string& path) const;
    bool rename(const std::string& from, const std::string& to) const;
    HalFile open(const std::string& path, int flags = O_RDONLY) const;
    bool openFileForRead(const char*, const std::string& path, HalFile& out) const;
    bool openFileForWrite(const char*, const std::string& path, HalFile& out) const;
};
extern PicoStorage Storage;
/// 线程安全取消标志；文件和 HTTP 数据循环共用。/ Thread-safe cancellation shared by file/HTTP loops.
bool pico_weread_cancelled();
