#pragma once

#include "slothdb/common/types/value.hpp"
#include "slothdb/common/file_glob.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace slothdb {

// Hive partitioning: key=value directory structure.
class HivePartition {
public:
    // Extract partition columns from a file path.
    // e.g., "data/year=2024/month=01/file.parquet" ->
    //   {{"year", "2024"}, {"month", "01"}}
    static std::unordered_map<std::string, std::string>
    ExtractPartitions(const std::string &path);

    // Check if a path contains Hive-style partitions.
    static bool HasPartitions(const std::string &path);

    // Discover all partition columns and their values from a directory tree.
    struct PartitionedFile {
        std::string path;
        std::unordered_map<std::string, std::string> partitions;
    };

    static std::vector<PartitionedFile>
    DiscoverPartitions(const std::string &base_dir, const std::string &file_ext);

    // Create Hive-partitioned output directories.
    // Returns the directory path for a given set of partition values.
    static std::string MakePartitionPath(
        const std::string &base_dir,
        const std::vector<std::pair<std::string, std::string>> &partitions);
};

} // namespace slothdb
