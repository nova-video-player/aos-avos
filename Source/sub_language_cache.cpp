// Copyright 2026 Courville Software
// SPDX-License-Identifier: Apache-2.0
#include "sub_language.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
std::mutex cacheMutex, probeMutex;
std::atomic<int> players(0);
std::string directory;
// Version pins the model hash, sample policy and on-disk record layout.
const char *VERSION = "nova-lid-8f3472cf-v2";
bool keyValid(const std::string &key) {
    return key.size() == 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string path(const char *key) {
    if (directory.empty()) {
        const char *env = std::getenv("AVOS_SUBTITLE_LANGUAGE_CACHE");
        if (env) directory = env;
    }
    return !key || !keyValid(key) || directory.empty() ? "" : directory + "/" + key + ".lid";
}
bool valid(const SUB_LANGUAGE_RESULT &r) {
    const size_t n = strnlen(r.language, sizeof(r.language));
    if (r.index < 0 || r.index >= 128 || r.stream < 0 || r.format <= 0 ||
        r.confidence < 85 || r.confidence > 100 || n < 2 || n > 3) return false;
    for (size_t i = 0; i < n; ++i) if (r.language[i] < 'a' || r.language[i] > 'z') return false;
    return std::strcmp(r.language, "und") && std::strcmp(r.language, "mul") &&
        std::strcmp(r.language, "zxx") && std::strcmp(r.language, "unk");
}
std::vector<SUB_LANGUAGE_RESULT> read(const std::string &file, int *recent) {
    if (recent) *recent = 0;
    struct stat st;
    if (file.empty() || stat(file.c_str(), &st) || st.st_size > 16384) return {};
    std::ifstream in(file);
    std::string version;
    long long timestamp; int scanned, count;
    if (!(in >> version >> timestamp >> scanned >> count) || version != VERSION || (scanned != 0 && scanned != 1) || count < 0 || count > 128)
        return {};
    const auto age = static_cast<long long>(std::time(nullptr)) - timestamp;
    if (age < 0 || age > 7 * 86400) return {};
    std::vector<SUB_LANGUAGE_RESULT> results;
    bool seen[128] = {};
    for (int i = 0; i < count; ++i) {
        SUB_LANGUAGE_RESULT r{}; std::string language;
        if (!(in >> r.index >> r.stream >> r.format >> language >> r.confidence) || language.size() >= sizeof(r.language))
            return {};
        language.copy(r.language, language.size());
        if (!valid(r) || seen[r.index]) return {};
        seen[r.index] = true; results.push_back(r);
    }
    std::string extra;
    if (in >> extra) return {};
    if (recent) *recent = scanned && age < 600;
    return results;
}
void prune() {
    DIR *dir = opendir(directory.c_str());
    if (!dir) return;
    std::vector<std::pair<time_t, std::string>> files;
    while (dirent *entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() != 68 || name.substr(64) != ".lid" || !keyValid(name.substr(0, 64))) continue;
        std::string file = directory + "/" + name; struct stat st;
        if (!stat(file.c_str(), &st)) files.emplace_back(st.st_mtime, file);
    }
    closedir(dir);
    std::sort(files.begin(), files.end());
    for (size_t i = 0; files.size() > 512 && i < files.size() - 512; ++i) std::remove(files[i].second.c_str());
}
}
extern "C" void sub_language_set_cache_path(const char *value) {
    try { std::lock_guard<std::mutex> lock(cacheMutex); directory = value ? value : ""; }
    catch (...) { }
}
extern "C" int sub_language_cache_enabled(void) {
    try { std::lock_guard<std::mutex> lock(cacheMutex); return !path(std::string(64, '0').c_str()).empty(); }
    catch (...) { return 0; }
}
extern "C" int sub_language_cache_read(const char *key, SUB_LANGUAGE_RESULT *out, int capacity, int *recent) {
    if (recent) *recent = 0;
    if (!out || capacity < 0) return 0;
    try {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto results = read(path(key), recent);
        const int count = std::min(capacity, static_cast<int>(results.size()));
        std::copy_n(results.begin(), count, out);
        return count;
    } catch (...) { return 0; }
}
extern "C" void sub_language_cache_write(const char *key, const SUB_LANGUAGE_RESULT *results, int count, int scanned) {
    if (count < 0 || count > 128 || (count && !results)) return;
    try {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto file = path(key);
        if (file.empty()) return;
        if (mkdir(directory.c_str(), 0700) && errno != EEXIST) return;
        auto merged = read(file, nullptr);
        for (int i = 0; i < count; ++i) {
            if (!valid(results[i])) return;
            auto existing = std::find_if(merged.begin(), merged.end(), [&](const SUB_LANGUAGE_RESULT &r) {
                return r.index == results[i].index;
            });
            if (existing == merged.end()) merged.push_back(results[i]); else *existing = results[i];
        }
        const std::string temporary = file + "." + std::to_string(getpid()) + ".tmp";
        std::ofstream out(temporary, std::ios::trunc);
        out << VERSION << ' ' << std::time(nullptr) << ' ' << !!scanned << ' ' << merged.size() << '\n';
        for (const auto &r : merged)
            out << r.index << ' ' << r.stream << ' ' << r.format << ' ' << r.language << ' ' << r.confidence << '\n';
        out.close();
        if (!out || std::rename(temporary.c_str(), file.c_str())) std::remove(temporary.c_str());
        else prune();
    } catch (...) { }
}
extern "C" int sub_language_probe_try_lock(void) { return probeMutex.try_lock(); }
extern "C" void sub_language_probe_unlock(void) { probeMutex.unlock(); }
extern "C" void sub_language_playback_active(int delta) { players.fetch_add(delta); }
extern "C" int sub_language_playback_count(void) { return players.load(); }
