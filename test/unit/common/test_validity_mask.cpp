#include "doctest.h"
#include "slothdb/common/types/validity_mask.hpp"

using namespace slothdb;

TEST_CASE("ValidityMask - all valid by default") {
    ValidityMask mask;
    CHECK(mask.AllValid());
    CHECK(mask.RowIsValid(0));
    CHECK(mask.RowIsValid(100));
    CHECK(mask.RowIsValid(VECTOR_SIZE - 1));
}

TEST_CASE("ValidityMask - set invalid") {
    ValidityMask mask;

    mask.SetInvalid(0);
    CHECK_FALSE(mask.AllValid());
    CHECK_FALSE(mask.RowIsValid(0));
    CHECK(mask.RowIsValid(1));

    mask.SetInvalid(100);
    CHECK_FALSE(mask.RowIsValid(100));
    CHECK(mask.RowIsValid(99));
    CHECK(mask.RowIsValid(101));

    mask.SetInvalid(VECTOR_SIZE - 1);
    CHECK_FALSE(mask.RowIsValid(VECTOR_SIZE - 1));
}

TEST_CASE("ValidityMask - set valid after invalid") {
    ValidityMask mask;
    mask.SetInvalid(50);
    CHECK_FALSE(mask.RowIsValid(50));

    mask.SetValid(50);
    CHECK(mask.RowIsValid(50));
}

TEST_CASE("ValidityMask - CountValid") {
    ValidityMask mask;

    // All valid.
    CHECK(mask.CountValid(VECTOR_SIZE) == VECTOR_SIZE);

    // Set 3 invalid.
    mask.SetInvalid(0);
    mask.SetInvalid(100);
    mask.SetInvalid(VECTOR_SIZE - 1);
    CHECK(mask.CountValid(VECTOR_SIZE) == VECTOR_SIZE - 3);
}

TEST_CASE("ValidityMask - Combine") {
    ValidityMask a;
    a.SetInvalid(10);

    ValidityMask b;
    b.SetInvalid(20);

    a.Combine(b, VECTOR_SIZE);
    CHECK_FALSE(a.RowIsValid(10));
    CHECK_FALSE(a.RowIsValid(20));
    CHECK(a.RowIsValid(15));
}

TEST_CASE("ValidityMask - Reset") {
    ValidityMask mask;
    mask.SetInvalid(5);
    CHECK_FALSE(mask.AllValid());

    mask.Reset();
    CHECK(mask.AllValid());
    CHECK(mask.RowIsValid(5));
}
