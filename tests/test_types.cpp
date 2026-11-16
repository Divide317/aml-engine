// Tests for the small inline helpers in aml/graph.hpp. These need no graph.
#include <catch2/catch_test_macros.hpp>

#include "aml/graph.hpp"

TEST_CASE("pack_account round-trips bank and account", "[types]") {
    // 0x8000F4580 is a real 9-hex-digit account id from the dataset preview.
    const std::uint64_t key = aml::pack_account(3208, 0x8000F4580);
    REQUIRE(aml::key_bank(key) == 3208);
    REQUIRE(aml::key_account(key) == 0x8000F4580);
}

TEST_CASE("pack_account uses the full 36 account bits", "[types]") {
    const std::uint64_t max_account = (std::uint64_t{1} << aml::kAccountBits) - 1;
    const std::uint64_t key = aml::pack_account(1, max_account);
    REQUIRE(aml::key_bank(key) == 1);  // the account did not spill into the bank bits
    REQUIRE(aml::key_account(key) == max_account);
}

TEST_CASE("same account string at two banks gives two keys", "[types]") {
    // The reason the bank is part of the key (DESIGN.md section 1, gotcha 4).
    REQUIRE(aml::pack_account(10, 0x8000A0000) != aml::pack_account(11, 0x8000A0000));
}

TEST_CASE("TimeRange is inclusive at both ends", "[types]") {
    const aml::TimeRange w{100, 200};
    REQUIRE(w.contains(100));
    REQUIRE(w.contains(200));
    REQUIRE_FALSE(w.contains(99));
    REQUIRE_FALSE(w.contains(201));
}

TEST_CASE("Ragged starts empty and indexes groups by offsets", "[types]") {
    aml::Ragged<aml::EdgeId> r;
    REQUIRE(r.size() == 0);

    // Two groups: {7, 8} and {9}.
    r.items = {7, 8, 9};
    r.offsets = {0, 2, 3};
    REQUIRE(r.size() == 2);
    REQUIRE(r[0].size() == 2);
    REQUIRE(r[0][1] == 8);
    REQUIRE(r[1].size() == 1);
    REQUIRE(r[1][0] == 9);
}

TEST_CASE("DetectionMask ORs typology bits onto edges", "[types]") {
    aml::Ragged<aml::EdgeId> groups;
    groups.items = {1, 3};
    groups.offsets = {0, 2};

    aml::DetectionMask mask(4);
    mask.mark(groups, aml::kCycle);
    mask.mark(groups, aml::kFanOut);
    REQUIRE(mask.bits[0] == 0);
    REQUIRE(mask.bits[1] == (aml::kCycle | aml::kFanOut));
    REQUIRE(mask.bits[3] == (aml::kCycle | aml::kFanOut));
}
