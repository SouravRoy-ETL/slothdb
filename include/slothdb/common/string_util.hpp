#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cstdarg>

namespace slothdb {

class StringUtil {
public:
    static std::string Upper(const std::string &str);
    static std::string Lower(const std::string &str);
    static std::string Format(const char *fmt, ...);
    static std::string FormatV(const char *fmt, va_list args);
    static std::vector<std::string> Split(const std::string &str, char delimiter);
    static std::string Join(const std::vector<std::string> &parts, const std::string &separator);
    static bool StartsWith(const std::string &str, const std::string &prefix);
    static bool EndsWith(const std::string &str, const std::string &suffix);
    static bool Contains(const std::string &str, const std::string &substr);
    static std::string Replace(const std::string &str, const std::string &from, const std::string &to);
    static std::string Trim(const std::string &str);
};

} // namespace slothdb
