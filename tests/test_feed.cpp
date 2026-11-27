#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include "polyarb/feed.hpp"

using namespace polyarb;
using Catch::Approx;

TEST_CASE("book snapshot replaces a token's book") {
    FeedState f;
    f.apply(R"({"event_type":"book","asset_id":"A","bids":[{"price":"0.45","size":"10"}],
                "asks":[{"price":"0.47","size":"5"},{"price":"0.48","size":"7"}]})");
    const TickBook* b = f.book("A");
    REQUIRE(b);
    CHECK(b->best_bid() == 4500);
    CHECK(b->best_ask() == 4700);

    f.apply(R"({"event_type":"book","asset_id":"A","bids":[],"asks":[{"price":"0.6","size":"1"}]})");
    CHECK_FALSE(b->best_bid());
    CHECK(b->best_ask() == 6000);
    CHECK(b->level_count(BookSide::Ask) == 1);
}

TEST_CASE("price_change updates levels and checks against the exchange's best") {
    FeedState f;
    f.apply(R"([{"event_type":"book","asset_id":"A","bids":[{"price":"0.45","size":"10"}],
                 "asks":[{"price":"0.47","size":"5"}]}])");  // arrays of events are allowed
    f.apply(R"({"event_type":"price_change","timestamp":"1","price_changes":[
                 {"asset_id":"A","price":"0.46","size":"3","side":"BUY","best_bid":"0.46","best_ask":"0.47"}]})");
    f.apply(R"({"event_type":"price_change","timestamp":"2","price_changes":[
                 {"asset_id":"A","price":"0.47","size":"0","side":"SELL","best_bid":"0.46","best_ask":"1"}]})");
    f.flush_checks();
    const TickBook* b = f.book("A");
    CHECK(b->best_bid() == 4600);
    CHECK_FALSE(b->best_ask());
    CHECK(f.stats().checks == 2);
    CHECK(f.stats().mismatches == 0);

    // the exchange says best bid is 0.50, but we never saw that level
    f.apply(R"({"event_type":"price_change","timestamp":"3","price_changes":[
                 {"asset_id":"A","price":"0.30","size":"1","side":"BUY","best_bid":"0.50","best_ask":"1"}]})");
    f.flush_checks();
    CHECK(f.stats().mismatches == 1);
}

TEST_CASE("a match split across messages with one timestamp is checked once, at the end") {
    // A buy at 0.87 hits a resting ask at 0.87. The first message already reports
    // the post-match best ask (0.88); the ask removal arrives in the next message.
    FeedState f;
    f.apply(R"({"event_type":"book","asset_id":"A","timestamp":"9",
                "bids":[{"price":"0.86","size":"10"}],
                "asks":[{"price":"0.87","size":"5"},{"price":"0.88","size":"9"}]})");
    f.apply(R"({"event_type":"price_change","timestamp":"10","price_changes":[
                 {"asset_id":"A","price":"0.87","size":"5","side":"BUY","best_bid":"0.87","best_ask":"0.88"}]})");
    f.apply(R"({"event_type":"price_change","timestamp":"10","price_changes":[
                 {"asset_id":"A","price":"0.87","size":"0","side":"SELL","best_bid":"0.87","best_ask":"0.88"}]})");
    f.flush_checks();
    CHECK(f.stats().checks == 1);
    CHECK(f.stats().mismatches == 0);
}

TEST_CASE("changes for tokens without a snapshot are skipped, junk is ignored") {
    FeedState f;
    f.apply(R"({"event_type":"price_change","price_changes":[{"asset_id":"Z","price":"0.5","size":"1","side":"BUY"}]})");
    f.apply("PONG");
    f.apply(R"({"event_type":"tick_size_change"})");
    f.apply(R"({"event_type":"book","asset_id":"B","bids":[{"price":"0.00005","size":"1"}],"asks":[]})");
    CHECK(f.books().size() == 1);
    CHECK(f.stats().checks == 0);
    CHECK(f.stats().ignored == 2);
    CHECK(f.stats().bad_levels == 1);
}

TEST_CASE("replaying a real capture keeps our books in sync with the exchange") {
    std::ifstream in(POLYARB_TEST_DATA "/ws_market_sample.jsonl");
    REQUIRE(in);
    FeedState f;
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) f.apply(line);
    f.flush_checks();
    const FeedStats& s = f.stats();
    CHECK(s.books >= 10);
    CHECK(s.checks > 200);
    CHECK(s.bad_levels == 0);
    CHECK(s.mismatches == 0);
}
