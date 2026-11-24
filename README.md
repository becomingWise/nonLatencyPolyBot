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

## Usage

```bash
pip install -r requirements.txt
python -m polyarb.scan                                   # one scan, top 200 markets
python -m polyarb.scan --fee-bps 0                       # ignore fees
python -m polyarb.scan --loop 60 --log data/snapshots.csv   # collect data over time
python -m pytest
```

## Layout

- `polyarb/arb.py`: pricing math, pure functions, fully tested
- `polyarb/client.py`: Gamma (markets) and CLOB (order books) API wrappers
- `polyarb/scan.py`: CLI scanner and CSV logger
- `tests/`: unit tests for the math

## Next ideas

- Multi-outcome events (`negRisk`): YES prices across all outcomes should sum to $1
- Logically linked markets (e.g. "wins by 10+" must be ≤ "wins")
- Analyze `data/snapshots.csv`: how often, how long, how deep do gaps get?

## Note

Polymarket restricts trading by region (see each market's `restricted` flag
and their terms). This repo only reads public data.
