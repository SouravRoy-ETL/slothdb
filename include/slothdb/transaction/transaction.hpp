#pragma once

#include "slothdb/common/constants.hpp"
#include "slothdb/common/types/value.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace slothdb {

using txn_id_t = uint64_t;

enum class TransactionState : uint8_t {
    ACTIVE,
    COMMITTED,
    ROLLED_BACK
};

// Undo record: stores the previous state of a row for rollback.
struct UndoRecord {
    std::string table_name;
    enum class Type { INSERT, DELETE, UPDATE } type;
    idx_t row_index;                    // Which row was affected.
    std::vector<Value> old_values;      // Previous values (for UPDATE/DELETE).
};

// A single transaction.
class Transaction {
public:
    Transaction(txn_id_t id, txn_id_t start_ts)
        : id_(id), start_ts_(start_ts), state_(TransactionState::ACTIVE) {}

    txn_id_t GetID() const { return id_; }
    txn_id_t GetStartTimestamp() const { return start_ts_; }
    txn_id_t GetCommitTimestamp() const { return commit_ts_; }
    TransactionState GetState() const { return state_; }

    void SetCommitTimestamp(txn_id_t ts) { commit_ts_ = ts; }
    void SetState(TransactionState state) { state_ = state; }

    // Add an undo record for rollback.
    void AddUndoRecord(UndoRecord record) {
        undo_buffer_.push_back(std::move(record));
    }

    const std::vector<UndoRecord> &GetUndoBuffer() const { return undo_buffer_; }

    bool IsActive() const { return state_ == TransactionState::ACTIVE; }
    bool HasWritten() const { return !undo_buffer_.empty(); }

private:
    txn_id_t id_;
    txn_id_t start_ts_;
    txn_id_t commit_ts_ = 0;
    TransactionState state_;
    std::vector<UndoRecord> undo_buffer_;
};

// Manages all transactions. Enforces single-writer, snapshot isolation.
class TransactionManager {
public:
    TransactionManager();

    // Begin a new transaction.
    Transaction &BeginTransaction();

    // Commit a transaction.
    void CommitTransaction(Transaction &txn);

    // Rollback a transaction.
    void RollbackTransaction(Transaction &txn);

    // Get current active transaction count.
    size_t ActiveTransactionCount() const;

    // Get the current global timestamp (for visibility checks).
    txn_id_t GetCurrentTimestamp() const { return current_ts_.load(); }

private:
    std::mutex lock_;
    std::atomic<txn_id_t> next_txn_id_{1};
    std::atomic<txn_id_t> current_ts_{0};
    std::vector<std::unique_ptr<Transaction>> active_transactions_;

    // Single-writer lock.
    std::mutex write_lock_;
    txn_id_t write_txn_id_ = 0;
};

} // namespace slothdb
