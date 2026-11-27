// Append-only, zstd-compressed recording of the raw feed.
//
// One record per line, tab separated:
//     <receive time, ns since epoch> \t <kind> \t <payload>
// kinds: "msg" raw WebSocket message, "sub" subscription (JSON list of token
// ids), "gap" the connection dropped, so books are unknown until the next
// "book" snapshot. Raw JSON never contains a literal tab or newline outside
// whitespace, so those are replaced with spaces and the payload stays valid.
//
// Files rotate hourly: <root>/YYYY-MM-DD/HH.tsv.zst (UTC). The writer flushes
// periodically, so a crash loses at most the last few seconds; readers stop
// cleanly at a truncated end.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace polyarb {

std::int64_t now_ns();
// Path of the hourly file that holds records received at `ns`.
std::filesystem::path log_path_for(const std::filesystem::path& root, std::int64_t ns);

class LogWriter {
public:
    explicit LogWriter(std::filesystem::path root, int level = 3);
    ~LogWriter();
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // Returns true if this record opened a new hourly file.
    bool write(std::int64_t recv_ns, std::string_view kind, std::string_view payload);
    void flush();  // make everything written so far readable on disk
    void close();
    const std::filesystem::path& current_path() const { return path_; }
    std::uint64_t bytes_in() const { return bytes_in_; }

private:
    void open(const std::filesystem::path& p);
    void drain(int mode);

    std::filesystem::path root_;
    std::filesystem::path path_;
    int level_;
    std::FILE* file_ = nullptr;
    void* cctx_ = nullptr;  // ZSTD_CCtx
    std::string line_;
    std::vector<char> out_;
    std::uint64_t bytes_in_ = 0;
};

struct LogRecord {
    std::int64_t recv_ns;
    std::string_view kind;
    std::string_view payload;
};

// Streams every record in a .tsv.zst file. Returns false if the file ended in
// the middle of a frame (e.g. the writer crashed); records before that point
// are still delivered.
bool read_log(const std::filesystem::path& p, const std::function<void(const LogRecord&)>& on_record);

}  // namespace polyarb
