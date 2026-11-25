#include "polyarb/client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <cstdlib>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace polyarb {

using nlohmann::json;

namespace {

constexpr const char* kGamma = "https://gamma-api.polymarket.com";
constexpr const char* kClob = "https://clob.polymarket.com";

// Polymarket sends many numbers as strings ("0.52"); accept either.
double num(const json& j, const char* key, double fallback = 0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_number()) return it->get<double>();
    if (it->is_string()) {
        const std::string& s = it->get_ref<const std::string&>();
        if (s.empty()) return fallback;
        try { return std::stod(s); } catch (...) { return fallback; }
    }
    return fallback;
}

bool flag(const json& j, const char* key, bool fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

// Gamma encodes some arrays as JSON inside a string: "[\"Yes\", \"No\"]".
std::vector<std::string> string_array(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return {};
    json arr = it->is_string() ? json::parse(it->get<std::string>(), nullptr, false) : *it;
    std::vector<std::string> out;
    if (!arr.is_array()) return out;
    for (const auto& v : arr)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::vector<Level> levels(const json& arr) {
    std::vector<Level> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (const auto& l : arr) out.push_back({num(l, "price"), num(l, "size")});
    return out;
}

void curl_global_init_once() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t write_cb(char* data, std::size_t size, std::size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

}  // namespace

// --- parsing ---------------------------------------------------------------

PageInfo parse_markets_page(const std::string& body, double min_volume_24h,
                            std::vector<BinaryMarket>& out) {
    const json rows = json::parse(body);
    if (!rows.is_array()) throw std::runtime_error("gamma /markets: expected a JSON array");
    PageInfo info{rows.size(), std::numeric_limits<double>::infinity()};
    for (const auto& m : rows) {
        auto outcomes = string_array(m, "outcomes");
        auto tokens = string_array(m, "clobTokenIds");
        const double vol = num(m, "volume24hr");
        info.lowest_volume_24h = std::min(info.lowest_volume_24h, vol);
        if (outcomes.size() != 2 || tokens.size() != 2 || !flag(m, "enableOrderBook", false) ||
            !flag(m, "acceptingOrders", true) || vol < min_volume_24h)
            continue;
        out.push_back({
            .slug = str(m, "slug"),
            .question = str(m, "question"),
            .outcomes = {outcomes[0], outcomes[1]},
            .token_ids = {tokens[0], tokens[1]},
            .fee_bps = num(m, "takerBaseFee"),
            .volume_24h = vol,
            .neg_risk = flag(m, "negRisk", false),
        });
    }
    if (info.rows == 0) info.lowest_volume_24h = 0;
    return info;
}

void parse_books(const std::string& body, BookMap& out) {
    const json arr = json::parse(body);
    if (!arr.is_array()) throw std::runtime_error("clob /books: expected a JSON array");
    for (const auto& b : arr) {
        OrderBook book{levels(b.value("bids", json::array())), levels(b.value("asks", json::array())),
                       static_cast<std::int64_t>(num(b, "timestamp"))};
        out[str(b, "asset_id")] = std::move(book);
    }
}

// --- HTTP --------------------------------------------------------------------

struct HttpClient::Impl {
    CURL* curl = nullptr;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {
    curl_global_init_once();
    impl_->curl = curl_easy_init();
    if (!impl_->curl) throw std::runtime_error("curl_easy_init failed");
}

HttpClient::~HttpClient() {
    if (impl_ && impl_->curl) curl_easy_cleanup(impl_->curl);
}

std::string HttpClient::get(const std::string& url) { return request(url, nullptr); }

std::string HttpClient::post_json(const std::string& url, const std::string& body) {
    return request(url, &body);
}

std::string HttpClient::request(const std::string& url, const std::string* post_body) {
    CURL* c = impl_->curl;
    for (int attempt = 0;; ++attempt) {
        std::string resp;
        curl_easy_reset(c);
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "polyarb-research/0.2");
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip
        if (const char* ca = std::getenv("CURL_CA_BUNDLE")) curl_easy_setopt(c, CURLOPT_CAINFO, ca);

        curl_slist* headers = nullptr;
        if (post_body) {
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body->c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_body->size()));
        }

        const CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);

        const bool retryable = rc != CURLE_OK || status == 429 || status >= 500;
        if (!retryable && status < 400) return resp;
        if (!retryable || attempt >= 3) {
            throw std::runtime_error(url + ": " +
                                     (rc != CURLE_OK ? curl_easy_strerror(rc)
                                                     : "HTTP " + std::to_string(status)));
        }
        std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
    }
}

// --- Polymarket ------------------------------------------------------------

std::vector<BinaryMarket> PolymarketClient::fetch_binary_markets(std::size_t limit,
                                                                 double min_volume_24h) {
    constexpr std::size_t kPage = 100;
    std::vector<BinaryMarket> out;
    for (std::size_t offset = 0; out.size() < limit; offset += kPage) {
        const std::string url = std::string(kGamma) +
            "/markets?active=true&closed=false&order=volume24hr&ascending=false&limit=" +
            std::to_string(kPage) + "&offset=" + std::to_string(offset);
        const PageInfo page = parse_markets_page(http_.get(url), min_volume_24h, out);
        // Results are sorted by 24h volume, so once a page dips below the
        // floor, every later page would be filtered out too.
        if (page.rows < kPage || page.lowest_volume_24h < min_volume_24h) break;
    }
    if (out.size() > limit) out.resize(limit);
    return out;
}

BookMap PolymarketClient::fetch_books(const std::vector<std::string>& token_ids,
                                      std::size_t batch) {
    BookMap books;
    for (std::size_t k = 0; k < token_ids.size(); k += batch) {
        json req = json::array();
        for (std::size_t i = k; i < std::min(k + batch, token_ids.size()); ++i)
            req.push_back({{"token_id", token_ids[i]}});
        parse_books(http_.post_json(std::string(kClob) + "/books", req.dump()), books);
    }
    return books;
}

}  // namespace polyarb
