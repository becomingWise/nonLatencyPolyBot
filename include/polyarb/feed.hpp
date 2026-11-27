// Applies Polymarket market-channel WebSocket messages to order books.
//
// Message types seen on wss://ws-subscriptions-clob.polymarket.com/ws/market:
//   book              full snapshot of one token's bids and asks
//   price_change      new total size at one or more price levels (0 = removed),
//                     plus the exchange's best bid/ask after the change
//   last_trade_price  a trade
// Other types and non-JSON frames (e.g. "PONG") are counted and ignored.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "polyarb/book.hpp"

namespace polyarb {

struct FeedStats {
    std::uint64_t messages = 0;
    std::uint64_t books = 0;
    std::uint64_t changes = 0;
    std::uint64_t trades = 0;
    std::uint64_t ignored = 0;     // non-JSON or unknown event types
    std::uint64_t bad_levels = 0;  // prices off the tick grid
    // price_change events carry the exchange's best bid/ask. If ours differs
    // our book has drifted (e.g. a missed message). Messages that share an
    // exchange timestamp are one atomic update (a match arrives as several
    // messages, and the reported best already reflects all of them), so the
    // check runs once that timestamp's batch is complete.
    std::uint64_t checks = 0;
    std::uint64_t mismatches = 0;
};

class FeedState {
public:
    void apply(std::string_view raw_message);

    const TickBook* book(const std::string& asset_id) const;
    const std::unordered_map<std::string, TickBook>& books() const { return books_; }
    const FeedStats& stats() const { return stats_; }
    void reset();  // drop all books, e.g. after a reconnect
    // Runs any checks still waiting for their batch to end (e.g. at end of file).
    void flush_checks();

private:
    struct PendingCheck {
        std::string timestamp;
        std::string best_bid, best_ask;
    };
    void settle(const std::string& asset_id, std::string_view next_timestamp);
    void check(const std::string& asset_id, const PendingCheck& c);

    std::unordered_map<std::string, TickBook> books_;
    std::unordered_map<std::string, PendingCheck> pending_;
    FeedStats stats_;
};

}  // namespace polyarb
