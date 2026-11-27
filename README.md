# nonLatencyPolyBot

Research tools for **arbitrage on Polymarket that doesn't depend on speed**:
pricing mistakes that hold for seconds or minutes, not microseconds.

Read-only for now: it pulls public market data and order books, finds trades
that would lock in profit, and logs snapshots so you can study how often they
appear. It places no orders.

## Strategy 1: YES + NO complete set

One YES share plus one NO share always pays exactly $1, and Polymarket lets you
merge a pair back into $1 (or split $1 into a pair) at any time. So:

| Trade | Condition | Profit per set |
|---|---|---|
| `BUY_BOTH` | ask(YES) + ask(NO) + fees < 1 | 1 − asks − fees |
| `SELL_BOTH` | bid(YES) + bid(NO) − fees > 1 | bids − 1 − fees |

The scanner walks **both order books level by level**, so the size and profit
it reports are what you could actually fill, not just the top of the book.

### Why this is almost never open

Polymarket's matching engine treats "buy YES at p" and "buy NO at 1 − p" as
the same order. That keeps `ask(YES) + ask(NO)` at or just above $1 most of the
time. When it does break, it's usually brief, thin, or eaten by fees. Measuring
*how* rare and *how* small it is, is the interesting result.

### Fees

Fees come from each market's `takerBaseFee` (bps), using Polymarket's formula
`fee = rate × min(p, 1 − p) × shares`. Check the current fee docs before
relying on exact numbers; use `--fee-bps` to try other assumptions.

## Build and run

Needs a C++20 compiler (GCC 13+ or Clang 17+), CMake 3.20+, libcurl,
Boost 1.81+ (headers only), OpenSSL and zstd. CMake downloads nlohmann/json
and Catch2 itself.

```bash
# Ubuntu/Debian: sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev libboost-dev libssl-dev libzstd-dev
# macOS:         brew install cmake pkg-config curl boost openssl zstd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

```bash
./build/polyarb_scan                                   # one scan, top 200 markets
./build/polyarb_scan --fee-bps 0                       # ignore fees
./build/polyarb_scan --loop 60 --log data/snapshots.csv   # collect data over time
./build/polyarb_scan --help
```

### Recording order book data

Polymarket's API has no historical order books, so `polyarb_record` builds
your own history from the live WebSocket feed:

```bash
./build/polyarb_record                        # top 50 markets into ./data, until Ctrl+C
./build/polyarb_record --duration 120         # quick two-minute test
./build/polyarb_record --markets 100 --out /path/to/disk
```

- Saves **raw messages exactly as received**, stamped with receive time in ns,
  so books can always be rebuilt even if parsing code changes later.
- Hourly files `data/YYYY-MM-DD/HH.tsv.zst` (UTC). It reconnects each hour,
  so every file starts with full book snapshots and replays on its own.
- Every change is checked against the exchange's reported best bid/ask;
  the stats line shows any mismatches (there should be none).
- Disconnects are logged as `gap` records and retried with backoff.
- Size: about 100 MB/hour (about 2.4 GB/day) for 50 markets.

## Layout

- `include/polyarb/arb.hpp`, `src/arb.cpp`: pricing math, pure functions, no I/O
- `include/polyarb/client.hpp`, `src/client.cpp`: libcurl + JSON client for the Gamma (markets) and CLOB (order books) APIs; parsing is split from I/O so it can be tested offline
- `include/polyarb/book.hpp`, `src/book.cpp`: tick-indexed order book (flat array per side, O(1) updates)
- `include/polyarb/feed.hpp`, `src/feed.cpp`: applies WebSocket messages to books and cross-checks the exchange's best prices
- `include/polyarb/log.hpp`, `src/log.cpp`: zstd-compressed, hourly-rotated append-only log
- `src/main.cpp`: `polyarb_scan` CLI and CSV logger
- `src/record.cpp`: `polyarb_record`, the live recorder (Boost.Beast + C++20 coroutines)
- `tests/`: Catch2 unit tests, including a replay of a real feed capture (`tests/data/`)

## Next ideas

- Multi-outcome events (`negRisk`): YES prices across all outcomes should sum to $1
- Logically linked markets (e.g. "wins by 10+" must be ≤ "wins")
- Analyze `data/snapshots.csv`: how often, how long, how deep do gaps get?

## Note

Polymarket restricts trading by region (see each market's `restricted` flag
and their terms). This repo only reads public data.
