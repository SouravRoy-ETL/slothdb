#include "slothdb/storage/hive_partition.hpp"
#include <algorithm>

#ifdef _MSC_VER
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

namespace slothdb {

std::unordered_map<std::string, std::string>
HivePartition::ExtractPartitions(const std::string &path) {
    std::unordered_map<std::string, std::string> result;

    // Split path by / or \.
    std::string part;
    for (size_t i = 0; i < path.size(); i++) {
        if (path[i] == '/' || path[i] == '\\') {
            // Check if this path component is key=value.
            auto eq = part.find('=');
            if (eq != std::string::npos) {
                result[part.substr(0, eq)] = part.substr(eq + 1);
            }
            part.clear();
        } else {
            part += path[i];
        }
    }
    return result;
}

bool HivePartition::HasPartitions(const std::string &path) {
    return path.find('=') != std::string::npos;
}

std::vector<HivePartition::PartitionedFile>
HivePartition::DiscoverPartitions(const std::string &base_dir,
                                   const std::string &file_ext) {
    std::vector<PartitionedFile> result;

    // Recursively find all files with the given extension.
    auto pattern = base_dir + "/**/*." + file_ext;
    auto files = FileGlob::Glob(pattern);

    // If no recursive matches, try direct glob.
    if (files.empty()) {
        pattern = base_dir + "/*." + file_ext;
        files = FileGlob::Glob(pattern);
    }

    // Also try subdirectories one level deep.
    if (files.empty()) {
        auto subdirs = FileGlob::ListDir(base_dir);
        for (auto &sub : subdirs) {
            auto sub_path = FileGlob::JoinPath(base_dir, sub);
            if (FileGlob::IsDirectory(sub_path)) {
                auto sub_files = FileGlob::ListDir(sub_path);
                for (auto &f : sub_files) {
                    if (FileGlob::Extension(f) == file_ext) {
                        files.push_back(FileGlob::JoinPath(sub_path, f));
                    }
                }
                // Go one more level.
                for (auto &sf : sub_files) {
                    auto sf_path = FileGlob::JoinPath(sub_path, sf);
                    if (FileGlob::IsDirectory(sf_path)) {
                        auto deep_files = FileGlob::ListDir(sf_path);
                        for (auto &df : deep_files) {
                            if (FileGlob::Extension(df) == file_ext) {
                                files.push_back(FileGlob::JoinPath(sf_path, df));
                            }
                        }
                    }
                }
            }
        }
    }

    for (auto &f : files) {
        PartitionedFile pf;
        pf.path = f;
        pf.partitions = ExtractPartitions(f);
        result.push_back(std::move(pf));
    }

    return result;
}

std::string HivePartition::MakePartitionPath(
    const std::string &base_dir,
    const std::vector<std::pair<std::string, std::string>> &partitions) {
    auto path = base_dir;
    MKDIR(path.c_str());

    for (auto &[key, val] : partitions) {
        path = FileGlob::JoinPath(path, key + "=" + val);
        MKDIR(path.c_str());
    }

    return path;
}

} // namespace slothdb
