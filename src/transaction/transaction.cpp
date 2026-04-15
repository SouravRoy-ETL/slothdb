#include "slothdb/transaction/transaction.hpp"
#include "slothdb/common/exception.hpp"

namespace slothdb {

TransactionManager::TransactionManager() = default;

Transaction &TransactionManager::BeginTransaction() {
    std::lock_guard<std::mutex> guard(lock_);

    auto txn_id = next_txn_id_.fetch_add(1);
    auto start_ts = current_ts_.load();

    auto txn = std::make_unique<Transaction>(txn_id, start_ts);
    auto *ptr = txn.get();
    active_transactions_.push_back(std::move(txn));
    return *ptr;
}

void TransactionManager::CommitTransaction(Transaction &txn) {
    std::lock_guard<std::mutex> guard(lock_);

    if (!txn.IsActive()) {
        throw TransactionException("Cannot commit: transaction is not active");
    }

    // Assign commit timestamp.
    auto commit_ts = current_ts_.fetch_add(1) + 1;
    txn.SetCommitTimestamp(commit_ts);
    txn.SetState(TransactionState::COMMITTED);

    // Release write lock if held.
    if (txn.HasWritten() && write_txn_id_ == txn.GetID()) {
        write_txn_id_ = 0;
        write_lock_.unlock();
    }

    // Remove from active transactions.
    for (auto it = active_transactions_.begin(); it != active_transactions_.end(); ++it) {
        if ((*it)->GetID() == txn.GetID()) {
            active_transactions_.erase(it);
            break;
        }
    }
}

void TransactionManager::RollbackTransaction(Transaction &txn) {
    std::lock_guard<std::mutex> guard(lock_);

    if (!txn.IsActive()) {
        throw TransactionException("Cannot rollback: transaction is not active");
    }

    txn.SetState(TransactionState::ROLLED_BACK);

    // Release write lock if held.
    if (txn.HasWritten() && write_txn_id_ == txn.GetID()) {
        write_txn_id_ = 0;
        write_lock_.unlock();
    }

    // Remove from active transactions.
    for (auto it = active_transactions_.begin(); it != active_transactions_.end(); ++it) {
        if ((*it)->GetID() == txn.GetID()) {
            active_transactions_.erase(it);
            break;
        }
    }
}

size_t TransactionManager::ActiveTransactionCount() const {
    return active_transactions_.size();
}

} // namespace slothdb
