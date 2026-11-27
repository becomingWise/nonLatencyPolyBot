#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "polyarb/log.hpp"

using namespace polyarb;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("polyarb_test_" + std::to_string(now_ns()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() { fs::remove_all(path); }
};

constexpr std::int64_t kHour = 3'600'000'000'000;
constexpr std::int64_t k2026_11_27_13h = 1795784400LL * 1'000'000'000;  // 2026-11-27 13:00:00 UTC

std::vector<std::string> read_all(const fs::path& p, bool* clean = nullptr) {
    std::vector<std::string> out;
    const bool ok = read_log(p, [&](const LogRecord& r) {
        out.push_back(std::to_string(r.recv_ns) + "|" + std::string(r.kind) + "|" + std::string(r.payload));
    });
    if (clean) *clean = ok;
    return out;
}

}  // namespace

TEST_CASE("hourly file paths are in UTC") {
    CHECK(log_path_for("data", k2026_11_27_13h) == fs::path("data/2026-11-27/13.tsv.zst"));
    CHECK(log_path_for("data", k2026_11_27_13h + kHour - 1) == fs::path("data/2026-11-27/13.tsv.zst"));
    CHECK(log_path_for("data", k2026_11_27_13h + 11 * kHour) == fs::path("data/2026-11-28/00.tsv.zst"));
}

TEST_CASE("records round-trip, and tabs or newlines in payloads are flattened") {
    TempDir tmp;
    {
        LogWriter w(tmp.path);
        CHECK(w.write(k2026_11_27_13h + 1, "sub", R"(["a","b"])"));
        CHECK_FALSE(w.write(k2026_11_27_13h + 2, "msg", "{\"x\":\n1,\t\"y\":2}"));
        w.write(k2026_11_27_13h + 3, "msg", "PONG");
        // 19 digits + 2 tabs + kind + payload + newline, per record
        CHECK(w.bytes_in() == (19 + 2 + 3 + 9 + 1) + (19 + 2 + 3 + 15 + 1) + (19 + 2 + 3 + 4 + 1));
    }
    bool clean = false;
    auto rows = read_all(log_path_for(tmp.path, k2026_11_27_13h), &clean);
    CHECK(clean);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == std::to_string(k2026_11_27_13h + 1) + R"(|sub|["a","b"])");
    CHECK(rows[1] == std::to_string(k2026_11_27_13h + 2) + "|msg|{\"x\": 1, \"y\":2}");
}

TEST_CASE("files rotate on the hour") {
    TempDir tmp;
    {
        LogWriter w(tmp.path);
        w.write(k2026_11_27_13h, "msg", "first");
        CHECK(w.write(k2026_11_27_13h + kHour, "msg", "second"));
    }
    CHECK(read_all(log_path_for(tmp.path, k2026_11_27_13h)).size() == 1);
    CHECK(read_all(log_path_for(tmp.path, k2026_11_27_13h + kHour)).size() == 1);
}

TEST_CASE("a restart in the same hour appends a new frame that reads back") {
    TempDir tmp;
    { LogWriter w(tmp.path); w.write(k2026_11_27_13h, "msg", "before restart"); }
    { LogWriter w(tmp.path); w.write(k2026_11_27_13h + 5, "msg", "after restart"); }
    bool clean = false;
    CHECK(read_all(log_path_for(tmp.path, k2026_11_27_13h), &clean).size() == 2);
    CHECK(clean);
}

TEST_CASE("a crash after flush keeps flushed records readable") {
    TempDir tmp;
    const fs::path p = log_path_for(tmp.path, k2026_11_27_13h);
    {
        LogWriter w(tmp.path);
        for (int i = 0; i < 1000; ++i) w.write(k2026_11_27_13h + i, "msg", "row " + std::to_string(i));
        w.flush();
        // Simulate a crash: copy the file as it is on disk now, before close().
        fs::copy_file(p, tmp.path / "crashed.tsv.zst");
    }
    bool clean = true;
    auto rows = read_all(tmp.path / "crashed.tsv.zst", &clean);
    CHECK_FALSE(clean);  // the final frame was never closed
    CHECK(rows.size() == 1000);
    CHECK(rows.back() == std::to_string(k2026_11_27_13h + 999) + "|msg|row 999");
}
