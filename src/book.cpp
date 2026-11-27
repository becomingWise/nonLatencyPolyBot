#include "polyarb/book.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace polyarb {

std::optional<int> price_to_tick(double price) {
    if (!std::isfinite(price)) return std::nullopt;
    const double scaled = price * kTicksPerDollar;
    const double rounded = std::round(scaled);
    if (std::abs(scaled - rounded) > 1e-6 || rounded < 0 || rounded > kMaxTick) return std::nullopt;
    return static_cast<int>(rounded);
}

std::optional<int> price_to_tick(std::string_view s) {
    double v = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return price_to_tick(v);
}

void TickBook::clear() {
    bids_.fill(0);
    asks_.fill(0);
    best_bid_ = -1;
    best_ask_ = kMaxTick + 1;
    n_bids_ = n_asks_ = 0;
}

double TickBook::size_at(BookSide side, int tick) const {
    if (tick < 0 || tick > kMaxTick) return 0;
    return side == BookSide::Bid ? bids_[tick] : asks_[tick];
}

void TickBook::set(BookSide side, int tick, double size) {
    if (tick < 0 || tick > kMaxTick) return;
    if (!(size > 0)) size = 0;  // also maps NaN to 0
    if (side == BookSide::Bid) {
        double& slot = bids_[tick];
        if (slot == 0 && size > 0) ++n_bids_;
        if (slot > 0 && size == 0) --n_bids_;
        slot = size;
        if (size > 0 && tick > best_bid_) {
            best_bid_ = tick;
        } else if (size == 0 && tick == best_bid_) {
            while (best_bid_ >= 0 && bids_[best_bid_] == 0) --best_bid_;
        }
    } else {
        double& slot = asks_[tick];
        if (slot == 0 && size > 0) ++n_asks_;
        if (slot > 0 && size == 0) --n_asks_;
        slot = size;
        if (size > 0 && tick < best_ask_) {
            best_ask_ = tick;
        } else if (size == 0 && tick == best_ask_) {
            while (best_ask_ <= kMaxTick && asks_[best_ask_] == 0) ++best_ask_;
        }
    }
}

std::vector<Level> TickBook::levels(BookSide side, std::size_t max_levels) const {
    std::vector<Level> out;
    const std::size_t want = max_levels ? max_levels : level_count(side);
    out.reserve(std::min(want, level_count(side)));
    if (side == BookSide::Bid) {
        for (int t = best_bid_; t >= 0 && out.size() < want; --t)
            if (bids_[t] > 0) out.push_back({tick_to_price(t), bids_[t]});
    } else {
        for (int t = best_ask_; t <= kMaxTick && out.size() < want; ++t)
            if (asks_[t] > 0) out.push_back({tick_to_price(t), asks_[t]});
    }
    return out;
}

}  // namespace polyarb
