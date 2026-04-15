#pragma once

#include <string>
#include <vector>

namespace slothdb {

// Cross-platform file globbing.
// Supports * (match any sequence) and ? (match single char) in filename.
// Supports ** for recursive directory traversal.
class FileGlob {
public:
    // Expand a glob pattern to matching file paths.
    // e.g., "data/*.csv", "logs/**/*.parquet", "file?.json"
    static std::vector<std::string> Glob(const std::string &pattern);

    // Check if a filename matches a simple glob pattern (no directory separators).
    static bool Match(const std::string &pattern, const std::string &str);

    // List files in a directory.
    static std::vector<std::string> ListDir(const std::string &dir);

    // Check if path is a directory.
    static bool IsDirectory(const std::string &path);

    // Get the directory part of a path.
    static std::string DirName(const std::string &path);

    // Get the filename part of a path.
    static std::string BaseName(const std::string &path);

    // Get file extension (lowercase, without dot).
    static std::string Extension(const std::string &path);

    // Join path components.
    static std::string JoinPath(const std::string &dir, const std::string &file);
};

} // namespace slothdb
