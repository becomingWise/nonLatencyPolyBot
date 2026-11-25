// Read-only access to Polymarket's public APIs (no keys needed).
//   Gamma API: market metadata (questions, token ids, fees, volume)
//   CLOB API:  live order books
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "polyarb/arb.hpp"

namespace polyarb {

struct BinaryMarket {
    std::string slug;
    std::string question;
    std::array<std::string, 2> outcomes;
    std::array<std::string, 2> token_ids;
    double fee_bps = 0;
    double volume_24h = 0;
    bool neg_risk = false;
};

struct OrderBook {
    std::vector<Level> bids;
    std::vector<Level> asks;
    std::int64_t ts_ms = 0;
};

using BookMap = std::unordered_map<std::string, OrderBook>;

// --- parsing (pure, unit-tested) -------------------------------------------
struct PageInfo {
    std::size_t rows = 0;          // rows on the page, before filtering
    double lowest_volume_24h = 0;  // smallest 24h volume on the page
};

// Appends tradable two-outcome markets from one Gamma /markets page.
// The page info tells callers when to stop paging.
PageInfo parse_markets_page(const std::string& body, double min_volume_24h,
                            std::vector<BinaryMarket>& out);
void parse_books(const std::string& body, BookMap& out);

// --- HTTP ----------------------------------------------------------------
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Retries 429 and 5xx with exponential backoff; throws on other errors.
    std::string get(const std::string& url);
    std::string post_json(const std::string& url, const std::string& body);

private:
    std::string request(const std::string& url, const std::string* post_body);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PolymarketClient {
public:
    std::vector<BinaryMarket> fetch_binary_markets(std::size_t limit, double min_volume_24h = 0);
    BookMap fetch_books(const std::vector<std::string>& token_ids, std::size_t batch = 50);

private:
    HttpClient http_;
};

}  // namespace polyarb
