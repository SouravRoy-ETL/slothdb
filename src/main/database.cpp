#include "slothdb/main/database.hpp"
#include "slothdb/storage/checkpoint.hpp"

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
    // If path is set, save on close.
    if (!config_.path.empty() && catalog_) {
        try {
            Checkpoint::Save(*catalog_, config_.path);
        } catch (...) {
            // Don't throw from destructor.
        }
    }
}

} // namespace slothdb
