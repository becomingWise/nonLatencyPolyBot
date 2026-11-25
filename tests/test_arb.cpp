#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "polyarb/arb.hpp"

using namespace polyarb;
using Catch::Approx;
using Book = std::vector<Level>;

TEST_CASE("no arb when asks sum to one or more") {
    CHECK_FALSE(walk_complete_set(Book{{0.52, 100}}, Book{{0.49, 100}}, Side::BuyBoth));
    CHECK_FALSE(walk_complete_set(Book{{0.50, 100}}, Book{{0.50, 100}}, Side::BuyBoth));
}

TEST_CASE("simple buy both is limited by the thinner side") {
    auto o = walk_complete_set(Book{{0.45, 100}}, Book{{0.50, 40}}, Side::BuyBoth);
    REQUIRE(o);
    CHECK(o->shares == 40);
    CHECK(o->gross == Approx(0.95 * 40));
    CHECK(o->profit == Approx(0.05 * 40));
}

TEST_CASE("walks depth and stops when edge disappears") {
    Book yes{{0.40, 10}, {0.47, 10}, {0.55, 10}};
    Book no{{0.50, 15}, {0.52, 100}};
    auto o = walk_complete_set(yes, no, Side::BuyBoth);
    REQUIRE(o);
    // 10 sets @ 0.90, 5 @ 0.97, 5 @ 0.99, then 0.55 + 0.52 > 1 stops
    CHECK(o->shares == 20);
    CHECK(o->profit == Approx(10 * 0.10 + 5 * 0.03 + 5 * 0.01));
    CHECK(o->worst_yes == 0.47);
    CHECK(o->worst_no == 0.52);
}

TEST_CASE("input order does not matter") {
    auto a = walk_complete_set(Book{{0.55, 10}, {0.40, 10}, {0.47, 10}},
                               Book{{0.52, 100}, {0.50, 15}}, Side::BuyBoth);
    REQUIRE(a);
    CHECK(a->shares == 20);
}

TEST_CASE("fees can kill the edge") {
    // 0.48 + 0.49 = 0.97, but a 10% base fee costs about 0.097 per set
    CHECK_FALSE(walk_complete_set(Book{{0.48, 100}}, Book{{0.49, 100}}, Side::BuyBoth, {.fee_rate = 0.10}));
    CHECK(walk_complete_set(Book{{0.48, 100}}, Book{{0.49, 100}}, Side::BuyBoth));
}

TEST_CASE("fee is small near the extremes") {
    CHECK(taker_fee(0.99, 100, 0.10) == Approx(0.10 * 0.01 * 100));
    CHECK(taker_fee(0.50, 100, 0.10) == Approx(5.0));
}

TEST_CASE("sell both takes best bids first") {
    auto o = walk_complete_set(Book{{0.50, 50}, {0.55, 10}}, Book{{0.48, 30}}, Side::SellBoth);
    REQUIRE(o);
    CHECK(o->shares == 10);  // 0.55 + 0.48 = 1.03, then 0.50 + 0.48 < 1
    CHECK(o->profit == Approx(0.03 * 10));
}

TEST_CASE("min edge and max shares") {
    auto o = walk_complete_set(Book{{0.45, 100}}, Book{{0.50, 100}}, Side::BuyBoth, {.max_shares = 7});
    REQUIRE(o);
    CHECK(o->shares == 7);
    CHECK_FALSE(walk_complete_set(Book{{0.45, 100}}, Book{{0.50, 100}}, Side::BuyBoth, {.min_edge = 0.06}));
}

TEST_CASE("empty books and zero-size levels") {
    CHECK_FALSE(walk_complete_set(Book{}, Book{{0.4, 10}}, Side::BuyBoth));
    CHECK_FALSE(walk_complete_set(Book{{0.4, 0}}, Book{{0.4, 10}}, Side::BuyBoth));
}
