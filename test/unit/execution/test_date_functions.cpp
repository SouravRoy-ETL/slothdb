#include "doctest.h"
#include "slothdb/main/database.hpp"
#include "slothdb/main/connection.hpp"

using namespace slothdb;

// Reference timestamp used throughout this file:
//   epoch seconds 1710499845 = 2024-03-15 10:50:45 UTC (a Friday).
//   Corresponding microsecond timestamp: 1710499845000000.

static constexpr int64_t TS_2024_03_15_10_50_45_US = 1710499845000000LL;

// ============================================================================
// DATE_TRUNC — extended interval parts
// ============================================================================

TEST_CASE("DATE_TRUNC - minute strips seconds") {
    Database db; Connection conn(db);
    auto r = conn.Query("SELECT DATE_TRUNC('minute', TO_TIMESTAMP(1710499845))");
    // 2024-03-15 10:50:00 UTC = 1710499800 s
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1710499800LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - second keeps seconds") {
    Database db; Connection conn(db);
    auto r = conn.Query("SELECT DATE_TRUNC('second', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == TS_2024_03_15_10_50_45_US);
}

TEST_CASE("DATE_TRUNC - millisecond truncates sub-ms") {
    Database db; Connection conn(db);
    // Input already whole-second so ms truncation is a no-op but must round-trip correctly.
    auto r = conn.Query("SELECT DATE_TRUNC('millisecond', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == TS_2024_03_15_10_50_45_US);
}

TEST_CASE("DATE_TRUNC - week snaps to Monday") {
    Database db; Connection conn(db);
    // 2024-03-15 is a Friday -> Monday of that week is 2024-03-11 00:00:00 UTC = 1710115200 s.
    auto r = conn.Query("SELECT DATE_TRUNC('week', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1710115200LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - week on a Monday is idempotent") {
    Database db; Connection conn(db);
    // 2024-03-11 00:00:00 UTC = 1710115200 s (this is already a Monday 00:00).
    auto r = conn.Query("SELECT DATE_TRUNC('week', TO_TIMESTAMP(1710115200))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1710115200LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - week on a Sunday snaps back 6 days") {
    Database db; Connection conn(db);
    // 2024-03-17 is a Sunday. Week should snap back to Monday 2024-03-11.
    // 2024-03-17 10:00:00 UTC = 1710669600 s.
    auto r = conn.Query("SELECT DATE_TRUNC('week', TO_TIMESTAMP(1710669600))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1710115200LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - quarter snaps to quarter start") {
    Database db; Connection conn(db);
    // March is in Q1 -> 2024-01-01 00:00:00 UTC = 1704067200 s.
    auto r = conn.Query("SELECT DATE_TRUNC('quarter', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1704067200LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - quarter for a Q3 timestamp") {
    Database db; Connection conn(db);
    // 2024-08-20 00:00:00 UTC = 1724112000 s. Q3 starts 2024-07-01 = 1719792000 s.
    auto r = conn.Query("SELECT DATE_TRUNC('quarter', TO_TIMESTAMP(1724112000))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1719792000LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - decade snaps to decade start") {
    Database db; Connection conn(db);
    // 2024 -> 2020-01-01 00:00:00 UTC = 1577836800 s.
    auto r = conn.Query("SELECT DATE_TRUNC('decade', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1577836800LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - century snaps to century start") {
    Database db; Connection conn(db);
    // 2024 -> 2000-01-01 00:00:00 UTC = 946684800 s.
    auto r = conn.Query("SELECT DATE_TRUNC('century', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 946684800LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - year still works (regression)") {
    Database db; Connection conn(db);
    // 2024-01-01 00:00:00 UTC = 1704067200 s.
    auto r = conn.Query("SELECT DATE_TRUNC('year', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1704067200LL * 1000000LL);
}

TEST_CASE("DATE_TRUNC - lowercase interval part accepted") {
    Database db; Connection conn(db);
    // StringUtil::Upper normalizes; same answer.
    auto r = conn.Query("SELECT DATE_TRUNC('quarter', TO_TIMESTAMP(1710499845))");
    auto r2 = conn.Query("SELECT DATE_TRUNC('QUARTER', TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == r2.GetValue(0, 0).GetValue<int64_t>());
}

// ============================================================================
// MONTHNAME / DAYNAME
// ============================================================================

TEST_CASE("MONTHNAME - returns English month name") {
    Database db; Connection conn(db);
    auto r = conn.Query("SELECT MONTHNAME(TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "March");
}

TEST_CASE("MONTHNAME - December boundary") {
    Database db; Connection conn(db);
    // 2024-12-31 12:00:00 UTC = 1735646400 s.
    auto r = conn.Query("SELECT MONTHNAME(TO_TIMESTAMP(1735646400))");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "December");
}

TEST_CASE("DAYNAME - returns English day-of-week") {
    Database db; Connection conn(db);
    // 2024-03-15 was a Friday.
    auto r = conn.Query("SELECT DAYNAME(TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "Friday");
}

TEST_CASE("DAYNAME - Sunday boundary") {
    Database db; Connection conn(db);
    // 2024-03-17 12:00:00 UTC was a Sunday. = 1710676800 s.
    auto r = conn.Query("SELECT DAYNAME(TO_TIMESTAMP(1710676800))");
    CHECK(r.GetValue(0, 0).GetValue<std::string>() == "Sunday");
}

// ============================================================================
// LAST_DAY
// ============================================================================

TEST_CASE("LAST_DAY - March 2024 -> March 31") {
    Database db; Connection conn(db);
    // 2024-03-31 00:00:00 UTC = 1711843200 s.
    auto r = conn.Query("SELECT LAST_DAY(TO_TIMESTAMP(1710499845))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1711843200LL * 1000000LL);
}

TEST_CASE("LAST_DAY - leap February 2024 -> Feb 29") {
    Database db; Connection conn(db);
    // 2024-02-10 12:00:00 UTC = 1707566400 s. Leap year -> last day Feb 29 = 2024-02-29.
    // 2024-02-29 00:00:00 UTC = 1709164800 s.
    auto r = conn.Query("SELECT LAST_DAY(TO_TIMESTAMP(1707566400))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1709164800LL * 1000000LL);
}

TEST_CASE("LAST_DAY - non-leap February 2023 -> Feb 28") {
    Database db; Connection conn(db);
    // 2023-02-10 12:00:00 UTC = 1676030400 s. Last day Feb 28 = 2023-02-28.
    // 2023-02-28 00:00:00 UTC = 1677542400 s.
    auto r = conn.Query("SELECT LAST_DAY(TO_TIMESTAMP(1676030400))");
    CHECK(r.GetValue(0, 0).GetValue<int64_t>() == 1677542400LL * 1000000LL);
}

// ============================================================================
// MAKE_DATE
// ============================================================================

TEST_CASE("MAKE_DATE - encodes YYYYMMDD") {
    Database db; Connection conn(db);
    auto r = conn.Query("SELECT MAKE_DATE(2024, 3, 15)");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 20240315);
}

TEST_CASE("MAKE_DATE - single-digit month and day") {
    Database db; Connection conn(db);
    auto r = conn.Query("SELECT MAKE_DATE(2024, 1, 5)");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 20240105);
}

TEST_CASE("MAKE_DATE - last day of year") {
    Database db; Connection conn(db);
    auto r = conn.Query("SELECT MAKE_DATE(2023, 12, 31)");
    CHECK(r.GetValue(0, 0).GetValue<int32_t>() == 20231231);
}
