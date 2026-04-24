#include "slothdb/common/file_glob.hpp"
#include <algorithm>
#include <cstring>
#include <functional>

#ifdef _MSC_VER
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace slothdb {

bool FileGlob::Match(const std::string &pattern, const std::string &str) {
    size_t pi = 0, si = 0;
    size_t star_p = std::string::npos, star_s = 0;

    while (si < str.size()) {
        if (pi < pattern.size() && (pattern[pi] == str[si] || pattern[pi] == '?')) {
            pi++; si++;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_p = pi++; star_s = si;
        } else if (star_p != std::string::npos) {
            pi = star_p + 1; si = ++star_s;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return pi == pattern.size();
}

std::vector<std::string> FileGlob::ListDir(const std::string &dir) {
    std::vector<std::string> entries;

#ifdef _MSC_VER
    WIN32_FIND_DATAA fd;
    auto search_path = dir + "\\*";
    HANDLE hFind = FindFirstFileA(search_path.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return entries;
    do {
        std::string name = fd.cFileName;
        if (name != "." && name != "..") {
            entries.push_back(name);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#else
    DIR *d = opendir(dir.c_str());
    if (!d) return entries;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name != "." && name != "..") {
            entries.push_back(name);
        }
    }
    closedir(d);
#endif

    return entries;
}

bool FileGlob::IsDirectory(const std::string &path) {
#ifdef _MSC_VER
    DWORD attrs = GetFileAttributesA(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat s;
    return stat(path.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
#endif
}

std::string FileGlob::DirName(const std::string &path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
}

std::string FileGlob::BaseName(const std::string &path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string FileGlob::Extension(const std::string &path) {
    auto base = BaseName(path);
    auto pos = base.find_last_of('.');
    if (pos == std::string::npos) return "";
    auto ext = base.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

std::string FileGlob::JoinPath(const std::string &dir, const std::string &file) {
    if (dir.empty()) return file;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + file;
    return dir + "/" + file;
}

std::vector<std::string> FileGlob::Glob(const std::string &pattern) {
    std::vector<std::string> results;

    // Check if pattern has glob characters.
    bool has_glob = pattern.find('*') != std::string::npos ||
                    pattern.find('?') != std::string::npos;

    if (!has_glob) {
        // No glob - just return the path if it exists.
        results.push_back(pattern);
        return results;
    }

    // Split pattern into directory and filename pattern.
    auto dir = DirName(pattern);
    auto file_pattern = BaseName(pattern);

    // Handle ** for recursive traversal.
    if (file_pattern == "**") {
        // Recursively list all files in subdirectories.
        // This is a simplified version - match the rest of the pattern after **.
        // For now, handle "dir/**/*.ext" pattern.
        auto remaining = pattern.substr(pattern.find("**") + 2);
        if (!remaining.empty() && (remaining[0] == '/' || remaining[0] == '\\'))
            remaining = remaining.substr(1);

        std::function<void(const std::string &)> recurse;
        recurse = [&](const std::string &cur_dir) {
            auto entries = ListDir(cur_dir);
            for (auto &entry : entries) {
                auto full = JoinPath(cur_dir, entry);
                if (IsDirectory(full)) {
                    recurse(full);
                } else if (!remaining.empty()) {
                    if (Match(remaining, entry)) {
                        results.push_back(full);
                    }
                } else {
                    results.push_back(full);
                }
            }
        };

        recurse(dir);
    } else {
        // Simple glob in current directory.
        auto entries = ListDir(dir);
        for (auto &entry : entries) {
            if (Match(file_pattern, entry)) {
                results.push_back(JoinPath(dir, entry));
            }
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

} // namespace slothdb
