// Tick-indexed order book for one Polymarket token.
//
// Prices live on a fixed grid (Polymarket's finest tick is 0.0001), so each
// side is a flat array indexed by integer tick: book[tick] = size. Updates
// and lookups are O(1), and integer ticks avoid floating-point price keys.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "polyarb/arb.hpp"

namespace polyarb {

inline constexpr int kTicksPerDollar = 10'000;  // 0.0001 resolution
inline constexpr int kMaxTick = kTicksPerDollar;  // price 1.0

enum class BookSide { Bid, Ask };

// "0.47" -> 4700. Returns nullopt for anything off the grid or outside [0, 1].
std::optional<int> price_to_tick(double price);
std::optional<int> price_to_tick(std::string_view price);
constexpr double tick_to_price(int tick) { return static_cast<double>(tick) / kTicksPerDollar; }

class TickBook {
public:
    TickBook() { clear(); }

    void clear();
    // Sets the total size resting at a price level; size 0 removes it.
    void set(BookSide side, int tick, double size);
    double size_at(BookSide side, int tick) const;

    std::optional<int> best_bid() const { return best_bid_ >= 0 ? std::optional(best_bid_) : std::nullopt; }
    std::optional<int> best_ask() const { return best_ask_ <= kMaxTick ? std::optional(best_ask_) : std::nullopt; }
    std::size_t level_count(BookSide side) const { return side == BookSide::Bid ? n_bids_ : n_asks_; }

    // Best-first levels, e.g. to feed walk_complete_set. `max_levels` = 0 means all.
    std::vector<Level> levels(BookSide side, std::size_t max_levels = 0) const;

private:
    std::array<double, kMaxTick + 1> bids_{};
    std::array<double, kMaxTick + 1> asks_{};
    int best_bid_ = -1;            // -1 when empty
    int best_ask_ = kMaxTick + 1;  // kMaxTick + 1 when empty
    std::size_t n_bids_ = 0, n_asks_ = 0;
};

}  // namespace polyarb
