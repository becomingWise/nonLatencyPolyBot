// Record Polymarket's live order book feed to compressed hourly files.
//
//   polyarb_record                         top 50 markets into ./data, forever
//   polyarb_record --markets 100 --out /mnt/disk/polydata
//   polyarb_record --duration 120          two minutes, for a quick test
//
// Each hour it reconnects and re-subscribes, so every hourly file begins with
// full "book" snapshots and can be replayed on its own.
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "polyarb/arb.hpp"
#include "polyarb/client.hpp"
#include "polyarb/feed.hpp"
#include "polyarb/log.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using asio::use_awaitable;
using namespace std::chrono_literals;
using namespace polyarb;

namespace {

constexpr const char* kHost = "ws-subscriptions-clob.polymarket.com";
constexpr const char* kPath = "/ws/market";

constexpr const char* kUsage = R"(usage: polyarb_record [options]
  --markets N        record the top N binary markets by 24h volume (default 50)
  --min-volume X     skip markets below this 24h volume
  --out DIR          output directory (default data)
  --duration S       stop after S seconds (default 0 = run forever)
  --stats S          print a stats line every S seconds (default 60)
)";

struct Args {
    std::size_t markets = 50;
    double min_volume = 0;
    std::string out = "data";
    int duration = 0;
    int stats = 60;
};

int to_int(const std::string& k, const std::string& v) {
    std::size_t used = 0;
    long x = -1;
    try { x = std::stol(v, &used); } catch (...) {}
    if (used != v.size() || x < 0 || x > 1'000'000'000)
        throw std::invalid_argument(k + " needs a non-negative whole number, got '" + v + "'");
    return static_cast<int>(x);
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "-h" || k == "--help") { std::cout << kUsage; std::exit(0); }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + k);
        const std::string v = argv[++i];
        if (k == "--markets") a.markets = to_int(k, v);
        else if (k == "--min-volume") a.min_volume = to_int(k, v);
        else if (k == "--out") a.out = v;
        else if (k == "--duration") a.duration = to_int(k, v);
        else if (k == "--stats") a.stats = std::max(1, to_int(k, v));
        else throw std::invalid_argument("unknown option " + k);
    }
    return a;
}

std::int64_t hour_of(std::int64_t ns) { return ns / 3'600'000'000'000; }

class Recorder {
public:
    Recorder(asio::io_context& io, Args args)
        : io_(io), args_(std::move(args)), ssl_(ssl::context::tls_client), log_(args_.out) {
        ssl_.set_default_verify_paths();
        ssl_.set_verify_mode(ssl::verify_peer);
        started_ = std::chrono::steady_clock::now();
    }

    asio::awaitable<void> run() {
        int backoff = 1;
        while (!stopping_) {
            std::string reason;
            try {
                co_await session();
                backoff = 1;  // clean exit: hourly rotation or shutdown
            } catch (const std::exception& e) {
                reason = e.what();
            }
            if (stopping_) break;
            if (!reason.empty()) {
                log_.write(now_ns(), "gap", reason);
                log_.flush();
                std::cerr << std::format("[{}] connection lost: {}; retrying in {}s\n", stamp(), reason, backoff);
                asio::steady_timer t(io_, std::chrono::seconds(backoff));
                co_await t.async_wait(use_awaitable);
                backoff = std::min(backoff * 2, 60);
            }
        }
    }

    // Periodic work: flush to disk, print stats, enforce --duration.
    asio::awaitable<void> housekeeping() {
        asio::steady_timer t(io_);
        for (int tick = 1; !stopping_; ++tick) {
            t.expires_after(1s);
            co_await t.async_wait(use_awaitable);
            if (tick % 5 == 0) log_.flush();
            if (args_.duration && tick >= args_.duration) stop();  // prints final stats
            else if (tick % args_.stats == 0) print_stats();
        }
    }

    void stop() {
        if (stopping_) return;
        stopping_ = true;
        log_.flush();
        print_stats();
        io_.stop();  // abandons pending reads; everything received is already logged
    }

    void finish() { log_.close(); }

private:
    using Ws = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    asio::awaitable<void> session() {
        // Refresh the market list each session so new hot markets get picked up.
        PolymarketClient rest;
        markets_ = rest.fetch_binary_markets(args_.markets, args_.min_volume);
        if (markets_.empty()) throw std::runtime_error("no markets returned");

        nlohmann::json sub_meta = nlohmann::json::array();
        nlohmann::json ids = nlohmann::json::array();
        for (const auto& m : markets_) {
            sub_meta.push_back({{"slug", m.slug}, {"tokens", m.token_ids}, {"outcomes", m.outcomes},
                                {"fee_bps", m.fee_bps}, {"neg_risk", m.neg_risk}});
            ids.push_back(m.token_ids[0]);
            ids.push_back(m.token_ids[1]);
        }
        const std::string subscribe = nlohmann::json{{"assets_ids", ids}, {"type", "market"}}.dump();

        auto ex = co_await asio::this_coro::executor;
        asio::ip::tcp::resolver resolver(ex);
        const auto endpoints = co_await resolver.async_resolve(kHost, "443", use_awaitable);

        Ws ws(ex, ssl_);
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), kHost))
            throw std::runtime_error("failed to set TLS SNI");
        ws.next_layer().set_verify_callback(ssl::host_name_verification(kHost));

        beast::get_lowest_layer(ws).expires_after(30s);
        co_await beast::get_lowest_layer(ws).async_connect(endpoints, use_awaitable);
        co_await ws.next_layer().async_handshake(ssl::stream_base::client, use_awaitable);
        beast::get_lowest_layer(ws).expires_never();

        websocket::stream_base::timeout opt{};
        opt.handshake_timeout = 30s;
        opt.idle_timeout = 60s;  // no traffic for 60s -> treat the connection as dead
        opt.keep_alive_pings = true;
        ws.set_option(opt);
        co_await ws.async_handshake(kHost, kPath, use_awaitable);

        feed_.reset();  // books are rebuilt from the fresh snapshots that follow
        co_await ws.async_write(asio::buffer(subscribe), use_awaitable);
        const std::int64_t sub_ns = now_ns();
        log_.write(sub_ns, "sub", sub_meta.dump());
        const std::int64_t session_hour = hour_of(sub_ns);
        std::cerr << std::format("[{}] subscribed to {} markets ({} tokens) -> {}\n", stamp(),
                                 markets_.size(), ids.size(), log_.current_path().string());

        // Run the reader and the pinger together. When either one ends (hourly
        // rotation, shutdown or an error), `||` cancels the other and waits for
        // it, so neither can touch `ws` after this frame unwinds.
        using namespace asio::experimental::awaitable_operators;
        co_await (read_loop(ws, session_hour) || ping_loop(ws));

        beast::error_code ignored;
        co_await ws.async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));
    }

    asio::awaitable<void> read_loop(Ws& ws, std::int64_t session_hour) {
        beast::flat_buffer buf;
        while (!stopping_) {
            co_await ws.async_read(buf, use_awaitable);
            const std::int64_t ns = now_ns();
            const std::string_view msg(static_cast<const char*>(buf.data().data()), buf.size());
            if (msg != "PONG") {
                log_.write(ns, "msg", msg);
                feed_.apply(msg);
                ++messages_;
            }
            buf.consume(buf.size());
            if (hour_of(ns) != session_hour) co_return;  // rotate: reconnect for fresh snapshots
        }
    }

    // Polymarket expects a text "PING" about every 10 seconds.
    asio::awaitable<void> ping_loop(Ws& ws) {
        asio::steady_timer t(co_await asio::this_coro::executor);
        for (;;) {
            t.expires_after(10s);
            co_await t.async_wait(use_awaitable);
            co_await ws.async_write(asio::buffer(std::string_view("PING")), use_awaitable);
        }
    }

    void print_stats() {
        feed_.flush_checks();
        const FeedStats& s = feed_.stats();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        std::size_t opps = 0;
        double best_profit = 0;
        for (const auto& m : markets_) {
            const TickBook* y = feed_.book(m.token_ids[0]);
            const TickBook* n = feed_.book(m.token_ids[1]);
            if (!y || !n) continue;
            const WalkParams p{m.fee_bps / 10'000};
            for (auto [side, bs] : {std::pair{Side::BuyBoth, BookSide::Ask}, std::pair{Side::SellBoth, BookSide::Bid}}) {
                if (auto o = walk_complete_set(y->levels(bs), n->levels(bs), side, p)) {
                    ++opps;
                    best_profit = std::max(best_profit, o->profit);
                }
            }
        }
        std::error_code ec;
        const auto disk = std::filesystem::file_size(log_.current_path(), ec);
        std::cerr << std::format(
            "[{}] {:.0f}s | {} msgs ({:.1f}/s) | {:.1f} MB raw, {:.2f} MB on disk this hour | "
            "book checks {} mismatches {} | live arb now: {} (best ${:.2f})\n",
            stamp(), secs, messages_, messages_ / std::max(secs, 1.0), log_.bytes_in() / 1e6,
            ec ? 0.0 : disk / 1e6, s.checks, s.mismatches, opps, best_profit);
    }

    static std::string stamp() {
        const auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return std::format("{:%FT%TZ}", t);
    }

    asio::io_context& io_;
    Args args_;
    ssl::context ssl_;
    LogWriter log_;
    FeedState feed_;
    std::vector<BinaryMarket> markets_;
    std::uint64_t messages_ = 0;
    bool stopping_ = false;
    std::chrono::steady_clock::time_point started_;
};

}  // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n" << kUsage;
        return 2;
    }

    asio::io_context io;
    Recorder rec(io, args);
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) { rec.stop(); });

    auto report = [](std::exception_ptr e) {
        if (e) try { std::rethrow_exception(e); } catch (const std::exception& x) {
            std::cerr << "fatal: " << x.what() << "\n";
        }
    };
    asio::co_spawn(io, rec.run(), report);
    asio::co_spawn(io, rec.housekeeping(), report);
    io.run();
    rec.finish();
    return 0;
}
