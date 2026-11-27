#include "polyarb/log.hpp"

#include <zstd.h>

#include <charconv>
#include <chrono>
#include <ctime>
#include <stdexcept>

namespace polyarb {

namespace fs = std::filesystem;

std::int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

fs::path log_path_for(const fs::path& root, std::int64_t ns) {
    const std::time_t secs = static_cast<std::time_t>(ns / 1'000'000'000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    char day[16], hour[16];
    std::strftime(day, sizeof day, "%Y-%m-%d", &tm);
    std::strftime(hour, sizeof hour, "%H", &tm);
    return root / day / (std::string(hour) + ".tsv.zst");
}

LogWriter::LogWriter(fs::path root, int level)
    : root_(std::move(root)), level_(level), out_(ZSTD_CStreamOutSize()) {}

LogWriter::~LogWriter() {
    try { close(); } catch (...) {}
}

void LogWriter::open(const fs::path& p) {
    close();
    fs::create_directories(p.parent_path());
    // Appending a new zstd frame to an existing file is valid: decoders read
    // concatenated frames, so a restart within the same hour is fine.
    file_ = std::fopen(p.c_str(), "ab");
    if (!file_) throw std::runtime_error("cannot open " + p.string());
    cctx_ = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx*>(cctx_), ZSTD_c_compressionLevel, level_);
    path_ = p;
}

void LogWriter::drain(int mode) {
    auto* cctx = static_cast<ZSTD_CCtx*>(cctx_);
    ZSTD_inBuffer in{line_.data(), line_.size(), 0};
    const auto directive = static_cast<ZSTD_EndDirective>(mode);
    for (;;) {
        ZSTD_outBuffer out{out_.data(), out_.size(), 0};
        const std::size_t remaining = ZSTD_compressStream2(cctx, &out, &in, directive);
        if (ZSTD_isError(remaining)) throw std::runtime_error(ZSTD_getErrorName(remaining));
        if (out.pos && std::fwrite(out_.data(), 1, out.pos, file_) != out.pos)
            throw std::runtime_error("write failed: " + path_.string());
        const bool done = directive == ZSTD_e_continue ? in.pos == in.size : remaining == 0;
        if (done) break;
    }
    line_.clear();
}

bool LogWriter::write(std::int64_t recv_ns, std::string_view kind, std::string_view payload) {
    const fs::path want = log_path_for(root_, recv_ns);
    const bool rotated = !file_ || want != path_;
    if (rotated) open(want);

    line_ += std::to_string(recv_ns);
    line_ += '\t';
    line_ += kind;
    line_ += '\t';
    for (char c : payload) line_ += (c == '\n' || c == '\t' || c == '\r') ? ' ' : c;
    line_ += '\n';
    bytes_in_ += line_.size();
    if (line_.size() >= (1 << 16)) drain(ZSTD_e_continue);
    return rotated;
}

void LogWriter::flush() {
    if (!file_) return;
    drain(ZSTD_e_flush);
    std::fflush(file_);
}

void LogWriter::close() {
    if (!file_) return;
    drain(ZSTD_e_end);
    std::fclose(file_);
    file_ = nullptr;
    ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(cctx_));
    cctx_ = nullptr;
}

bool read_log(const fs::path& p, const std::function<void(const LogRecord&)>& on_record) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + p.string());
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    std::vector<char> in_buf(ZSTD_DStreamInSize()), out_buf(ZSTD_DStreamOutSize());
    std::string pending;  // partial line carried between chunks
    std::size_t last = 0;
    bool clean = true;

    auto emit_lines = [&] {
        std::size_t start = 0;
        for (std::size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string_view line(pending.data() + start, nl - start);
            const auto t1 = line.find('\t');
            const auto t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string_view::npos) continue;
            std::int64_t ns = 0;
            std::from_chars(line.data(), line.data() + t1, ns);
            on_record({ns, line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
        }
        pending.erase(0, start);
    };

    for (std::size_t n; clean && (n = std::fread(in_buf.data(), 1, in_buf.size(), f)) > 0;) {
        ZSTD_inBuffer in{in_buf.data(), n, 0};
        // Standard zstd loop: keep calling while input remains or the output
        // buffer came back full (the decoder may still hold data).
        for (bool full = false; in.pos < in.size || full;) {
            ZSTD_outBuffer out{out_buf.data(), out_buf.size(), 0};
            last = ZSTD_decompressStream(dctx, &out, &in);
            if (ZSTD_isError(last)) { clean = false; break; }
            pending.append(out_buf.data(), out.pos);
            emit_lines();
            full = out.pos == out.size;
        }
    }
    ZSTD_freeDCtx(dctx);
    std::fclose(f);
    return clean && last == 0;  // last == 0 means the final frame was complete
}

}  // namespace polyarb
