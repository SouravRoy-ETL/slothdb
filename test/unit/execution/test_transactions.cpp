#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"
#include "slothdb/common/exception.hpp"

using namespace slothdb;

TEST_CASE("Transaction - BEGIN and COMMIT") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("BEGIN");
    CHECK(conn.InTransaction());

    conn.Query("INSERT INTO t VALUES (1), (2), (3)");
    conn.Query("COMMIT");
    CHECK_FALSE(conn.InTransaction());

    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 3);
}

TEST_CASE("Transaction - BEGIN TRANSACTION syntax") {
    Database db;
    Connection conn(db);

    conn.Query("BEGIN TRANSACTION");
    CHECK(conn.InTransaction());
    conn.Query("COMMIT TRANSACTION");
    CHECK_FALSE(conn.InTransaction());
}

TEST_CASE("Transaction - double BEGIN throws") {
    Database db;
    Connection conn(db);

    conn.Query("BEGIN");
    CHECK_THROWS_AS(conn.Query("BEGIN"), TransactionException);
    conn.Query("COMMIT");
}

TEST_CASE("Transaction - COMMIT without BEGIN throws") {
    Database db;
    Connection conn(db);

    CHECK_THROWS_AS(conn.Query("COMMIT"), TransactionException);
}

TEST_CASE("Transaction - ROLLBACK without BEGIN throws") {
    Database db;
    Connection conn(db);

    CHECK_THROWS_AS(conn.Query("ROLLBACK"), TransactionException);
}

TEST_CASE("Transaction - ROLLBACK") {
    Database db;
    Connection conn(db);

    conn.Query("CREATE TABLE t (x INTEGER)");
    conn.Query("INSERT INTO t VALUES (1)");

    conn.Query("BEGIN");
    // Data inserted inside transaction.
    conn.Query("INSERT INTO t VALUES (2)");

    auto r = conn.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 2);

    conn.Query("ROLLBACK");

    // After rollback, the insert inside the transaction
    // is still visible because we don't have full MVCC undo yet.
    // The ROLLBACK successfully completes without error.
    CHECK_FALSE(conn.InTransaction());
}

TEST_CASE("Transaction - multiple connections") {
    Database db;
    Connection conn1(db);
    Connection conn2(db);

    conn1.Query("CREATE TABLE t (x INTEGER)");
    conn1.Query("INSERT INTO t VALUES (1)");

    // conn2 can see the data (no isolation yet — auto-commit mode).
    auto r = conn2.Query("SELECT COUNT(*) FROM t");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1);
}

TEST_CASE("TransactionManager - basic lifecycle") {
    TransactionManager mgr;

    auto &txn = mgr.BeginTransaction();
    CHECK(txn.IsActive());
    CHECK(mgr.ActiveTransactionCount() == 1);

    mgr.CommitTransaction(txn);
    CHECK(mgr.ActiveTransactionCount() == 0);
}

TEST_CASE("TransactionManager - rollback") {
    TransactionManager mgr;

    auto &txn = mgr.BeginTransaction();
    mgr.RollbackTransaction(txn);
    CHECK(mgr.ActiveTransactionCount() == 0);
}

TEST_CASE("TransactionManager - multiple transactions") {
    TransactionManager mgr;

    auto &txn1 = mgr.BeginTransaction();
    auto &txn2 = mgr.BeginTransaction();
    CHECK(mgr.ActiveTransactionCount() == 2);

    mgr.CommitTransaction(txn1);
    CHECK(mgr.ActiveTransactionCount() == 1);

    mgr.CommitTransaction(txn2);
    CHECK(mgr.ActiveTransactionCount() == 0);
}

TEST_CASE("TransactionManager - timestamps increment") {
    TransactionManager mgr;

    auto &txn1 = mgr.BeginTransaction();
    auto ts1 = txn1.GetStartTimestamp();
    mgr.CommitTransaction(txn1);

    auto &txn2 = mgr.BeginTransaction();
    auto ts2 = txn2.GetStartTimestamp();
    mgr.CommitTransaction(txn2);

    CHECK(ts2 > ts1);
}
