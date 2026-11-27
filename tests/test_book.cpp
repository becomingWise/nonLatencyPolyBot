#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "polyarb/book.hpp"

using namespace polyarb;
using Catch::Approx;

TEST_CASE("price_to_tick handles every Polymarket tick size") {
    CHECK(price_to_tick("0.47") == 4700);
    CHECK(price_to_tick("0.001") == 10);
    CHECK(price_to_tick("0.0001") == 1);
    CHECK(price_to_tick("1") == kMaxTick);
    CHECK(price_to_tick(0.1 + 0.2) == 3000);  // float noise still lands on the grid
}

TEST_CASE("price_to_tick rejects junk") {
    CHECK_FALSE(price_to_tick("0.00005"));  // finer than the grid
    CHECK_FALSE(price_to_tick("1.5"));
    CHECK_FALSE(price_to_tick("-0.1"));
    CHECK_FALSE(price_to_tick("abc"));
    CHECK_FALSE(price_to_tick("0.4x"));
}

TEST_CASE("best bid and ask follow adds and removals") {
    TickBook b;
    CHECK_FALSE(b.best_bid());
    CHECK_FALSE(b.best_ask());

    b.set(BookSide::Bid, 4500, 10);
    b.set(BookSide::Bid, 4700, 5);
    b.set(BookSide::Ask, 4900, 8);
    b.set(BookSide::Ask, 4800, 2);
    CHECK(b.best_bid() == 4700);
    CHECK(b.best_ask() == 4800);

    b.set(BookSide::Bid, 4700, 0);  // remove best bid -> falls back
    b.set(BookSide::Ask, 4800, 0);
    CHECK(b.best_bid() == 4500);
    CHECK(b.best_ask() == 4900);

    b.set(BookSide::Bid, 4500, 0);
    b.set(BookSide::Ask, 4900, 0);
    CHECK_FALSE(b.best_bid());
    CHECK_FALSE(b.best_ask());
    CHECK(b.level_count(BookSide::Bid) == 0);
}

TEST_CASE("updating a level replaces its size") {
    TickBook b;
    b.set(BookSide::Ask, 5000, 10);
    b.set(BookSide::Ask, 5000, 3);
    CHECK(b.size_at(BookSide::Ask, 5000) == 3);
    CHECK(b.level_count(BookSide::Ask) == 1);
}

TEST_CASE("levels come out best first and feed the arb walk") {
    TickBook yes, no;
    yes.set(BookSide::Ask, 4700, 10);
    yes.set(BookSide::Ask, 4000, 10);
    no.set(BookSide::Ask, 5000, 15);
    no.set(BookSide::Ask, 5200, 100);

    auto ya = yes.levels(BookSide::Ask);
    REQUIRE(ya.size() == 2);
    CHECK(ya[0].price == Approx(0.40));
    CHECK(yes.levels(BookSide::Ask, 1).size() == 1);

    auto o = walk_complete_set(ya, no.levels(BookSide::Ask), Side::BuyBoth);
    REQUIRE(o);
    CHECK(o->shares == 20);
}

TEST_CASE("out-of-range ticks and bad sizes are ignored") {
    TickBook b;
    b.set(BookSide::Bid, -1, 5);
    b.set(BookSide::Bid, kMaxTick + 1, 5);
    b.set(BookSide::Bid, 100, -3);
    CHECK(b.level_count(BookSide::Bid) == 0);
}
