// Parser tests on trimmed-down real API responses (no network).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "polyarb/client.hpp"

using namespace polyarb;
using Catch::Approx;

static const char* kMarkets = R"([
  {"slug": "fed-hike", "question": "Fed hike?", "outcomes": "[\"Yes\", \"No\"]",
   "clobTokenIds": "[\"111\", \"222\"]", "enableOrderBook": true, "acceptingOrders": true,
   "volume24hr": 5000.5, "takerBaseFee": 1000, "negRisk": false},
  {"slug": "three-way", "outcomes": "[\"A\", \"B\", \"C\"]", "clobTokenIds": "[\"1\",\"2\",\"3\"]",
   "enableOrderBook": true, "volume24hr": 9000},
  {"slug": "no-book", "outcomes": "[\"Yes\", \"No\"]", "clobTokenIds": "[\"3\",\"4\"]",
   "enableOrderBook": false, "volume24hr": 9000},
  {"slug": "closed-to-orders", "outcomes": "[\"Yes\", \"No\"]", "clobTokenIds": "[\"5\",\"6\"]",
   "enableOrderBook": true, "acceptingOrders": false, "volume24hr": 9000},
  {"slug": "thin", "outcomes": "[\"Yes\", \"No\"]", "clobTokenIds": "[\"7\",\"8\"]",
   "enableOrderBook": true, "volume24hr": "12", "negRisk": true}
])";

TEST_CASE("parse_markets_page keeps only tradable binary markets") {
    std::vector<BinaryMarket> out;
    const PageInfo info = parse_markets_page(kMarkets, 0, out);
    CHECK(info.rows == 5);
    CHECK(info.lowest_volume_24h == 12);
    REQUIRE(out.size() == 2);
    CHECK(out[0].slug == "fed-hike");
    CHECK(out[0].outcomes[1] == "No");
    CHECK(out[0].token_ids[0] == "111");
    CHECK(out[0].fee_bps == 1000);
    CHECK(out[0].volume_24h == Approx(5000.5));
    CHECK(out[1].slug == "thin");
    CHECK(out[1].volume_24h == 12);  // numeric strings are accepted
    CHECK(out[1].neg_risk);
}

TEST_CASE("parse_markets_page applies the volume floor") {
    std::vector<BinaryMarket> out;
    parse_markets_page(kMarkets, 100, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].slug == "fed-hike");
}

TEST_CASE("parse_books reads string prices and sizes") {
    BookMap books;
    parse_books(R"([{"asset_id": "111", "timestamp": "1795563020257",
                     "bids": [{"price": "0.996", "size": "22709.35"}],
                     "asks": [{"price": "0.999", "size": "100"}, {"price": "0.998", "size": "5"}]}])",
                books);
    REQUIRE(books.count("111"));
    const OrderBook& b = books["111"];
    CHECK(b.ts_ms == 1795563020257);
    REQUIRE(b.bids.size() == 1);
    CHECK(b.bids[0].price == Approx(0.996));
    CHECK(b.bids[0].size == Approx(22709.35));
    CHECK(b.asks.size() == 2);
}

TEST_CASE("parsers reject non-array bodies") {
    std::vector<BinaryMarket> m;
    BookMap b;
    CHECK_THROWS(parse_markets_page(R"({"error": "x"})", 0, m));
    CHECK_THROWS(parse_books(R"({"error": "x"})", b));
}

TEST_CASE("empty page reports zero rows") {
    std::vector<BinaryMarket> out;
    const PageInfo info = parse_markets_page("[]", 0, out);
    CHECK(info.rows == 0);
    CHECK(out.empty());
}
