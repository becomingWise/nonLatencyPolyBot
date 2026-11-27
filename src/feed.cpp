#include "polyarb/feed.hpp"

#include <nlohmann/json.hpp>

namespace polyarb {

using nlohmann::json;

namespace {

std::string_view sv(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? std::string_view(it->get_ref<const std::string&>())
                                              : std::string_view{};
}

double size_of(const json& j) {
    auto s = sv(j, "size");
    try { return s.empty() ? 0.0 : std::stod(std::string(s)); } catch (...) { return 0.0; }
}

// The exchange reports an empty side as "0" (bids) or "1" (asks), or omits it.
bool same_best(std::optional<int> ours, std::string_view theirs, int empty_value) {
    if (theirs.empty()) return true;  // nothing to compare
    auto t = price_to_tick(theirs);
    if (!t) return true;
    if (!ours) return *t == empty_value;
    return *ours == *t;
}

}  // namespace

const TickBook* FeedState::book(const std::string& asset_id) const {
    auto it = books_.find(asset_id);
    return it == books_.end() ? nullptr : &it->second;
}

void FeedState::reset() {
    books_.clear();
    pending_.clear();
}

void FeedState::check(const std::string& asset_id, const PendingCheck& c) {
    auto it = books_.find(asset_id);
    if (it == books_.end()) return;
    ++stats_.checks;
    if (!same_best(it->second.best_bid(), c.best_bid, 0) ||
        !same_best(it->second.best_ask(), c.best_ask, kMaxTick))
        ++stats_.mismatches;
}

// Called before applying an event for `asset_id`: if a check is waiting from
// an earlier timestamp, that batch is complete, so run it now.
void FeedState::settle(const std::string& asset_id, std::string_view next_timestamp) {
    auto it = pending_.find(asset_id);
    if (it == pending_.end() || it->second.timestamp == next_timestamp) return;
    check(asset_id, it->second);
    pending_.erase(it);
}

void FeedState::flush_checks() {
    for (const auto& [asset, c] : pending_) check(asset, c);
    pending_.clear();
}

void FeedState::apply(std::string_view raw) {
    ++stats_.messages;
    const json msg = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded() || !(msg.is_object() || msg.is_array())) {
        ++stats_.ignored;
        return;
    }

    auto handle = [this](const json& e) {
        const std::string_view type = sv(e, "event_type");
        if (type == "book") {
            ++stats_.books;
            const std::string asset(sv(e, "asset_id"));
            settle(asset, sv(e, "timestamp"));
            pending_.erase(asset);  // a fresh snapshot supersedes any pending check
            TickBook& b = books_[asset];
            b.clear();
            for (auto [key, side] : {std::pair{"bids", BookSide::Bid}, std::pair{"asks", BookSide::Ask}}) {
                for (const auto& l : e.value(key, json::array())) {
                    if (auto t = price_to_tick(sv(l, "price"))) b.set(side, *t, size_of(l));
                    else ++stats_.bad_levels;
                }
            }
        } else if (type == "price_change") {
            const std::string_view ts = sv(e, "timestamp");
            for (const auto& c : e.value("price_changes", json::array())) {
                ++stats_.changes;
                const std::string asset(sv(c, "asset_id"));
                auto it = books_.find(asset);
                if (it == books_.end()) continue;  // no snapshot yet for this token
                settle(asset, ts);
                const BookSide side = sv(c, "side") == "BUY" ? BookSide::Bid : BookSide::Ask;
                if (auto t = price_to_tick(sv(c, "price"))) it->second.set(side, *t, size_of(c));
                else ++stats_.bad_levels;
                pending_[asset] = {std::string(ts), std::string(sv(c, "best_bid")), std::string(sv(c, "best_ask"))};
            }
        } else if (type == "last_trade_price") {
            ++stats_.trades;
        } else {
            ++stats_.ignored;
        }
    };

    if (msg.is_array()) {
        for (const auto& e : msg) handle(e);
    } else {
        handle(msg);
    }
}

}  // namespace polyarb
