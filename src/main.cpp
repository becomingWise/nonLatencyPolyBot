// Scan Polymarket for YES+NO complete-set arbitrage.
//
//   polyarb_scan                                   one scan, top 200 markets
//   polyarb_scan --loop 60 --log data/snapshots.csv   collect data over time
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "polyarb/arb.hpp"
#include "polyarb/client.hpp"

using namespace polyarb;

namespace {

std::atomic<bool> g_stop{false};

struct Args {
    std::size_t markets = 200;
    double min_volume = 0;
    std::optional<double> fee_bps;
    double min_edge = 0;
    double max_shares = std::numeric_limits<double>::infinity();
    int loop_seconds = 0;
    std::string log_path;
};

constexpr const char* kUsage = R"(usage: polyarb_scan [options]
  --markets N       how many markets to scan, by 24h volume (default 200)
  --min-volume X    skip markets below this 24h volume
  --fee-bps X       override each market's taker fee (bps)
  --min-edge X      min profit per set in USDC, e.g. 0.005
  --max-shares X    cap sets per market
  --loop N          rescan every N seconds (0 = once)
  --log PATH        append every market's snapshot to this CSV
)";

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "-h" || k == "--help") { std::cout << kUsage; std::exit(0); }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + k);
        const std::string v = argv[++i];
        if (k == "--markets") a.markets = std::stoul(v);
        else if (k == "--min-volume") a.min_volume = std::stod(v);
        else if (k == "--fee-bps") a.fee_bps = std::stod(v);
        else if (k == "--min-edge") a.min_edge = std::stod(v);
        else if (k == "--max-shares") a.max_shares = std::stod(v);
        else if (k == "--loop") a.loop_seconds = std::stoi(v);
        else if (k == "--log") a.log_path = v;
        else throw std::invalid_argument("unknown option " + k);
    }
    return a;
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::optional<double> best(const std::vector<Level>& lv, bool highest) {
    if (lv.empty()) return std::nullopt;
    auto cmp = [](const Level& x, const Level& y) { return x.price < y.price; };
    return (highest ? std::max_element(lv.begin(), lv.end(), cmp)
                    : std::min_element(lv.begin(), lv.end(), cmp))->price;
}

struct Row {
    const BinaryMarket* market;
    std::optional<double> yes_ask, no_ask, yes_bid, no_bid;
    double fee_bps;
    std::optional<Opportunity> opp;

    std::optional<double> ask_sum() const {
        if (yes_ask && no_ask) return *yes_ask + *no_ask;
        return std::nullopt;
    }
    std::optional<double> bid_sum() const {
        if (yes_bid && no_bid) return *yes_bid + *no_bid;
        return std::nullopt;
    }
};

std::vector<Row> scan_once(PolymarketClient& client, const Args& a,
                           std::vector<BinaryMarket>& markets) {
    markets = client.fetch_binary_markets(a.markets, a.min_volume);
    std::vector<std::string> tokens;
    for (const auto& m : markets) tokens.insert(tokens.end(), m.token_ids.begin(), m.token_ids.end());
    const BookMap books = client.fetch_books(tokens);

    std::vector<Row> rows;
    for (const auto& m : markets) {
        auto ya = books.find(m.token_ids[0]);
        auto na = books.find(m.token_ids[1]);
        if (ya == books.end() || na == books.end()) continue;
        const OrderBook& y = ya->second;
        const OrderBook& n = na->second;

        const double fee_bps = a.fee_bps.value_or(m.fee_bps);
        const WalkParams p{fee_bps / 10'000, a.min_edge, a.max_shares};
        std::optional<Opportunity> opp = walk_complete_set(y.asks, n.asks, Side::BuyBoth, p);
        if (auto s = walk_complete_set(y.bids, n.bids, Side::SellBoth, p); s && (!opp || s->profit > opp->profit))
            opp = s;
        rows.push_back({&m, best(y.asks, false), best(n.asks, false), best(y.bids, true),
                        best(n.bids, true), fee_bps, opp});
    }
    return rows;
}

void report(const std::vector<Row>& rows, const std::string& ts) {
    std::vector<const Row*> hits, priced;
    std::size_t tight = 0;
    for (const auto& r : rows) {
        if (r.opp) hits.push_back(&r);
        if (auto s = r.ask_sum()) {
            priced.push_back(&r);
            if (*s <= 1.001) ++tight;
        }
    }
    std::ranges::sort(hits, [](auto* x, auto* y) { return x->opp->profit > y->opp->profit; });
    std::cout << std::format("[{}] scanned {} markets | {} with YES+NO asks <= 1.001 | {} profitable after fees\n",
                             ts, rows.size(), tight, hits.size());
    auto q = [](const Row* r) { return r->market->question.substr(0, 70); };
    for (std::size_t i = 0; i < std::min<std::size_t>(hits.size(), 15); ++i) {
        const Opportunity& o = *hits[i]->opp;
        std::cout << std::format("  {:9} ${:8.2f} on {:9.1f} sets ({:5.2f}c/set, fee {:.0f}bps)  {}\n",
                                 to_string(o.side), o.profit, o.shares, o.edge_per_set() * 100,
                                 hits[i]->fee_bps, q(hits[i]));
    }
    if (hits.empty()) {
        std::ranges::sort(priced, [](auto* x, auto* y) { return *x->ask_sum() < *y->ask_sum(); });
        std::cout << "  no edge after fees. tightest YES+NO ask sums:\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(priced.size(), 5); ++i)
            std::cout << std::format("    {:.3f}  (fee {:.0f}bps)  {}\n", *priced[i]->ask_sum(),
                                     priced[i]->fee_bps, q(priced[i]));
    }
    std::cout.flush();
}

std::string cell(const std::optional<double>& v) { return v ? std::format("{:.4f}", *v) : ""; }

void append_log(const std::string& path, const std::vector<Row>& rows, const std::string& ts) {
    namespace fs = std::filesystem;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    const bool fresh = !fs::exists(p);
    std::ofstream f(p, std::ios::app);
    if (!f) throw std::runtime_error("cannot open log " + path);
    if (fresh)
        f << "ts_utc,slug,yes_ask,no_ask,ask_sum,yes_bid,no_bid,bid_sum,fee_bps,side,shares,profit,volume_24h\n";
    for (const auto& r : rows) {
        // slugs are [a-z0-9-], so no CSV quoting is needed
        f << std::format("{},{},{},{},{},{},{},{},{:.0f},{},{:.2f},{:.4f},{:.0f}\n", ts, r.market->slug,
                         cell(r.yes_ask), cell(r.no_ask), cell(r.ask_sum()), cell(r.yes_bid),
                         cell(r.no_bid), cell(r.bid_sum()), r.fee_bps,
                         r.opp ? to_string(r.opp->side) : "", r.opp ? r.opp->shares : 0.0,
                         r.opp ? r.opp->profit : 0.0, r.market->volume_24h);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n" << kUsage;
        return 2;
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    PolymarketClient client;
    std::vector<BinaryMarket> markets;  // owns the data Rows point into
    do {
        try {
            const std::string ts = utc_now();
            const auto rows = scan_once(client, args, markets);
            report(rows, ts);
            if (!args.log_path.empty()) append_log(args.log_path, rows, ts);
        } catch (const std::exception& e) {
            std::cerr << "scan failed: " << e.what() << "\n";
            if (!args.loop_seconds) return 1;  // in loop mode, keep going
        }
        for (int s = 0; s < args.loop_seconds && !g_stop; ++s)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (args.loop_seconds && !g_stop);
    return 0;
}
