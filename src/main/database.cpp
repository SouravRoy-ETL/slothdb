#include "slothdb/main/database.hpp"
#include "slothdb/storage/checkpoint.hpp"
#include <cstdio>

#ifdef _MSC_VER
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

namespace slothdb {

static bool FileExists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

Database::Database() : Database(DatabaseConfig{}) {}

Database::Database(const std::string &path) {
    config_.path = path;
    catalog_ = std::make_unique<Catalog>();
    txn_mgr_ = std::make_unique<TransactionManager>();

    if (!path.empty() && FileExists(path)) {
        Checkpoint::Load(*catalog_, path);
    }
}

Database::Database(DatabaseConfig config)
    : config_(std::move(config)) {
    catalog_ = std::make_unique<Catalog>();
    txn_mgr_ = std::make_unique<TransactionManager>();

    if (!config_.path.empty() && FileExists(config_.path)) {
        Checkpoint::Load(*catalog_, config_.path);
    }
}

Database::~Database() {
    // If path is set, save on close (atomic: write to .tmp then rename).
    if (!config_.path.empty() && catalog_) {
        try {
            auto tmp_path = config_.path + ".tmp";
            Checkpoint::Save(*catalog_, tmp_path);
            std::remove(config_.path.c_str());
            std::rename(tmp_path.c_str(), config_.path.c_str());
        } catch (...) {
            // Don't throw from destructor.
        }
    }
}

} // namespace slothdb
