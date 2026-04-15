#pragma once

#include "slothdb/catalog/catalog.hpp"
#include "slothdb/transaction/transaction.hpp"
#include <memory>
#include <string>

namespace slothdb {

// Configuration for a SlothDB database instance.
struct DatabaseConfig {
    std::string path;           // Empty = in-memory only.
    idx_t memory_limit = 0;     // 0 = unlimited.
    idx_t threads = 0;          // 0 = auto-detect.
};

// A SlothDB database instance. Owns the catalog and storage.
class Database {
public:
    Database();
    explicit Database(const std::string &path);
    explicit Database(DatabaseConfig config);
    ~Database();

    Catalog &GetCatalog() { return *catalog_; }
    TransactionManager &GetTransactionManager() { return *txn_mgr_; }
    const DatabaseConfig &GetConfig() const { return config_; }

private:
    DatabaseConfig config_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<TransactionManager> txn_mgr_;
};

} // namespace slothdb
