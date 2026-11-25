#include "polyarb/arb.hpp"

#include <algorithm>
#include <vector>

namespace polyarb {

std::string_view to_string(Side side) {
    return side == Side::BuyBoth ? "BUY_BOTH" : "SELL_BOTH";
}

double taker_fee(double price, double shares, double fee_rate) {
    return fee_rate * std::min(price, 1.0 - price) * shares;
}

double Opportunity::return_on_capital() const {
    const double capital = side == Side::BuyBoth ? gross + fees : shares;
    return capital > 0 ? profit / capital : 0.0;
}

namespace {

// Copy, drop empty levels, and order best price first.
std::vector<Level> best_first(std::span<const Level> levels, bool lowest_first) {
    std::vector<Level> out;
    out.reserve(levels.size());
    for (const Level& l : levels)
        if (l.size > 0) out.push_back(l);
    std::sort(out.begin(), out.end(), [lowest_first](const Level& a, const Level& b) {
        return lowest_first ? a.price < b.price : a.price > b.price;
    });
    return out;
}

}  // namespace

std::optional<Opportunity> walk_complete_set(std::span<const Level> yes_in,
                                             std::span<const Level> no_in,
                                             Side side,
                                             const WalkParams& p) {
    constexpr double kEps = 1e-12;
    const bool buying = side == Side::BuyBoth;
    std::vector<Level> yes = best_first(yes_in, buying);
    std::vector<Level> no = best_first(no_in, buying);

    Opportunity o{side, 0, 0, 0, 0, 0, 0};
    std::size_t i = 0, j = 0;
    while (i < yes.size() && j < no.size() && o.shares < p.max_shares) {
        Level& y = yes[i];
        Level& n = no[j];
        const double unit_fee = taker_fee(y.price, 1, p.fee_rate) + taker_fee(n.price, 1, p.fee_rate);
        const double pair = y.price + n.price;
        const double edge = buying ? 1.0 - pair - unit_fee : pair - 1.0 - unit_fee;
        if (edge <= p.min_edge) break;

        const double q = std::min({y.size, n.size, p.max_shares - o.shares});
        o.shares += q;
        o.gross += pair * q;
        o.fees += unit_fee * q;
        o.worst_yes = y.price;
        o.worst_no = n.price;
        y.size -= q;
        n.size -= q;
        if (y.size <= kEps) ++i;
        if (n.size <= kEps) ++j;
    }

    if (o.shares <= 0) return std::nullopt;
    o.profit = buying ? o.shares - o.gross - o.fees : o.gross - o.shares - o.fees;
    return o;
}

}  // namespace polyarb
