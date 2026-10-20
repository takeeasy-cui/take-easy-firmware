/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 已挂载目录上的文件生命周期。/ File lifetimes on already-mounted roots.
 * 冻结：不越过配置根目录，不跟随符号链接。/ Frozen: stay inside roots; do not follow symlinks.
 */
#include "HalStorage.h"
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <climits>
PicoStorage Storage;
HalFile& HalFile::operator=(HalFile&& other) noexcept {
    if (this == &other) return *this;
    close(); file_ = other.file_; dir_ = other.dir_; path_ = std::move(other.path_);
    other.file_ = nullptr; other.dir_ = nullptr;
    return *this;
}
bool HalFile::open(const std::string& path, const char* mode) {
    close();
    if (path.empty() || pico_weread_cancelled()) return false;
    struct stat st;
#ifndef ESP_PLATFORM
    // FAT 不支持符号链接；主机测试额外检查链接。/ FAT has no symlinks; guard them in host tests.
    if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return false;
#endif
    path_ = path;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) dir_ = opendir(path.c_str());
    else file_ = fopen(path.c_str(), mode);
    return isOpen();
}
void HalFile::close() {
    if (file_) fclose(file_);
    if (dir_) closedir(dir_);
    file_ = nullptr; dir_ = nullptr; path_.clear();
}
int HalFile::read(void* out, size_t size) {
    if (!file_ || pico_weread_cancelled()) return -1;
    const size_t count = fread(out, 1, size, file_);
    return ferror(file_) ? -1 : static_cast<int>(count);
}
size_t HalFile::write(const void* data, size_t size) {
    return file_ && !pico_weread_cancelled() ? fwrite(data, 1, size, file_) : 0;
}
bool HalFile::seek64(uint64_t offset) { return file_ && offset <= LONG_MAX && fseek(file_, offset, SEEK_SET) == 0; }
size_t HalFile::position() const { const long pos = file_ ? ftell(file_) : -1; return pos < 0 ? 0 : pos; }
uint64_t HalFile::fileSize64() const { struct stat st; return file_ && fstat(fileno(file_), &st) == 0 ? st.st_size : 0; }
void HalFile::flush() { if (file_) { fflush(file_); fsync(fileno(file_)); } }
HalFile HalFile::openNextFile() {
    HalFile next;
    while (dir_ && !pico_weread_cancelled()) {
        struct dirent* entry = readdir(dir_);
        if (!entry) break;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (next.open(path_ + "/" + entry->d_name, "rb")) break;
    }
    return next;
}
size_t HalFile::getName(char* out, size_t cap) const {
    const char* name = strrchr(path_.c_str(), '/'); name = name ? name + 1 : path_.c_str();
    const size_t len = strlen(name);
    if (cap) { const size_t n = len < cap - 1 ? len : cap - 1; memcpy(out, name, n); out[n] = 0; }
    return len;
}
std::string PicoStorage::map(const std::string& path) const {
    if (path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) return {};
    for (size_t offset = 0; offset < path.size();) {
        size_t end = path.find('/', offset); if (end == std::string::npos) end = path.size();
        const std::string component = path.substr(offset, end - offset);
        if (component == ".." || component == ".") return {};
        offset = end + 1;
    }
    std::string out;
    const std::string cache = "/.crosspoint/weread", books = "/WeRead";
    if (path == cache || path.compare(0, cache.size() + 1, cache + "/") == 0) out = cache_root_ + path.substr(cache.size());
    else if (path == books || path.compare(0, books.size() + 1, books + "/") == 0) out = books_root_ + path.substr(books.size());
    if (out.empty() || out.size() >= 288) return {};
#ifndef ESP_PLATFORM
    // FAT 不支持符号链接；主机检查已有父目录。/ FAT has no symlinks; check existing host parents.
    for (size_t pos = 1; pos <= out.size(); ++pos) {
        if (pos != out.size() && out[pos] != '/') continue;
        struct stat st;
        if (lstat(out.substr(0, pos).c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return {};
    }
#endif
    return out;
}
bool PicoStorage::ensureDirectoryExists(const std::string& path) const {
    const std::string full = map(path); if (full.empty()) return false;
    for (size_t pos = 1; pos <= full.size(); ++pos) {
        if (pos != full.size() && full[pos] != '/') continue;
        const std::string dir = full.substr(0, pos);
        // FAT挂载根目录的mkdir返回EINVAL，先认可已存在的目录。/ FAT rejects mkdir on a mount root; accept existing directories first.
        struct stat st;
        if (stat(dir.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return false;
            continue;
        }
        if (errno != ENOENT || (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)) return false;
        if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    }
    return true;
}
bool PicoStorage::exists(const std::string& path) const { const auto p = map(path); struct stat st; return !p.empty() && stat(p.c_str(), &st) == 0; }
bool PicoStorage::remove(const std::string& path) const { const auto p = map(path); return !p.empty() && unlink(p.c_str()) == 0; }
bool PicoStorage::removeDir(const std::string& path) const { const auto p = map(path); return !p.empty() && rmdir(p.c_str()) == 0; }
bool PicoStorage::rename(const std::string& from, const std::string& to) const {
    const auto a = map(from), b = map(to); return !a.empty() && !b.empty() && ::rename(a.c_str(), b.c_str()) == 0;
}
HalFile PicoStorage::open(const std::string& path, int flags) const {
    HalFile file; file.open(map(path), flags == O_RDWR ? "r+b" : "rb"); return file;
}
bool PicoStorage::openFileForRead(const char*, const std::string& path, HalFile& out) const { return out.open(map(path), "rb"); }
bool PicoStorage::openFileForWrite(const char*, const std::string& path, HalFile& out) const { return out.open(map(path), "wb"); }
