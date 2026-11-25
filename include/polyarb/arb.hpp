// Pure pricing math for the YES+NO complete-set arbitrage.
//
// One YES share plus one NO share (a "complete set") always pays exactly $1,
// and Polymarket lets you merge a set into $1 or split $1 into a set at any
// time. That gives two trades:
//
//   BuyBoth:  ask(YES) + ask(NO) + fees < 1  -> buy both, merge, keep the gap
//   SellBoth: bid(YES) + bid(NO) - fees > 1  -> split $1, sell both sides
//
// Nothing here touches the network, so it is easy to test.
#pragma once

#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace polyarb {

struct Level {
    double price;
    double size;  // shares
};

enum class Side { BuyBoth, SellBoth };

std::string_view to_string(Side side);

// Fee in USDC for a taker fill. Polymarket's published formula scales the fee
// by min(p, 1 - p), so fills near 0 or 1 pay almost nothing. `fee_rate` is the
// base fee as a fraction (bps / 10'000). Check the current docs before trusting
// real numbers: fee schedules change.
double taker_fee(double price, double shares, double fee_rate);

struct Opportunity {
    Side side;
    double shares;     // complete sets traded
    double gross;      // USDC paid (BuyBoth) or received (SellBoth), before fees
    double fees;
    double profit;     // guaranteed USDC profit
    double worst_yes;  // deepest YES price touched
    double worst_no;   // deepest NO price touched

    double edge_per_set() const { return shares > 0 ? profit / shares : 0.0; }
    double return_on_capital() const;
};

struct WalkParams {
    double fee_rate = 0.0;
    double min_edge = 0.0;  // min USDC profit per extra set
    double max_shares = std::numeric_limits<double>::infinity();
};

// Walk both books together, taking sets while each extra set still makes more
// than `min_edge` after fees. Input levels may be in any order.
// BuyBoth takes the two ask books; SellBoth takes the two bid books.
std::optional<Opportunity> walk_complete_set(std::span<const Level> yes,
                                             std::span<const Level> no,
                                             Side side,
                                             const WalkParams& params = {});

}  // namespace polyarb
