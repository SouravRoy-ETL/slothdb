#include "slothdb/common/string_util.hpp"
#include <cctype>
#include <cstdio>
#include <sstream>

namespace slothdb {

std::string StringUtil::Upper(const std::string &str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

std::string StringUtil::Lower(const std::string &str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string StringUtil::Format(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    auto result = FormatV(fmt, args);
    va_end(args);
    return result;
}

std::string StringUtil::FormatV(const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int size = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);
    if (size < 0) {
        return "";
    }
    std::string result(static_cast<size_t>(size), '\0');
    std::vsnprintf(result.data(), static_cast<size_t>(size) + 1, fmt, args);
    return result;
}

std::vector<std::string> StringUtil::Split(const std::string &str, char delimiter) {
    std::vector<std::string> parts;
    std::istringstream stream(str);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

std::string StringUtil::Join(const std::vector<std::string> &parts, const std::string &separator) {
    std::string result;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) {
            result += separator;
        }
        result += parts[i];
    }
    return result;
}

bool StringUtil::StartsWith(const std::string &str, const std::string &prefix) {
    if (prefix.size() > str.size()) return false;
    return str.compare(0, prefix.size(), prefix) == 0;
}

bool StringUtil::EndsWith(const std::string &str, const std::string &suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StringUtil::Contains(const std::string &str, const std::string &substr) {
    return str.find(substr) != std::string::npos;
}

std::string StringUtil::Replace(const std::string &str, const std::string &from, const std::string &to) {
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

std::string StringUtil::Trim(const std::string &str) {
    auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

} // namespace slothdb
