"""Scan Polymarket for YES+NO complete-set arbitrage.

    python -m polyarb.scan                       # one scan, top 200 markets
    python -m polyarb.scan --loop 60 --log data/snapshots.csv   # collect data over time
"""
from __future__ import annotations

import argparse
import csv
import os
import time
from datetime import datetime, timezone

from .arb import walk_complete_set
from .client import fetch_binary_markets, fetch_books

LOG_FIELDS = ["ts_utc", "slug", "yes_ask", "no_ask", "ask_sum", "yes_bid", "no_bid",
              "bid_sum", "fee_bps", "side", "shares", "profit", "volume_24h"]


def scan_once(args) -> list[dict]:
    markets = fetch_binary_markets(limit=args.markets, min_volume_24h=args.min_volume)
    books = fetch_books([t for m in markets for t in m.token_ids])
    now = datetime.now(timezone.utc).isoformat(timespec="seconds")
    rows = []
    for m in markets:
        a, b = (books.get(t) for t in m.token_ids)
        if not a or not b:
            continue
        fee_rate = (args.fee_bps if args.fee_bps is not None else m.fee_bps) / 10_000
        best = lambda lv, hi: (max if hi else min)((p for p, _ in lv), default=None)
        ya, na = best(a["asks"], False), best(b["asks"], False)
        yb, nb = best(a["bids"], True), best(b["bids"], True)

        opp = None
        for side, yl, nl in (("BUY_BOTH", a["asks"], b["asks"]), ("SELL_BOTH", a["bids"], b["bids"])):
            o = walk_complete_set(yl, nl, side, fee_rate=fee_rate,
                                  min_edge=args.min_edge, max_shares=args.max_shares)
            if o and (opp is None or o.profit > opp.profit):
                opp = o
        rows.append({
            "ts_utc": now, "slug": m.slug, "question": m.question,
            "yes_ask": ya, "no_ask": na, "ask_sum": (ya + na) if ya and na else None,
            "yes_bid": yb, "no_bid": nb, "bid_sum": (yb + nb) if yb and nb else None,
            "fee_bps": fee_rate * 10_000, "volume_24h": round(m.volume_24h),
            "side": opp.side if opp else "", "shares": round(opp.shares, 2) if opp else 0,
            "profit": round(opp.profit, 4) if opp else 0.0, "opp": opp,
        })
    return rows


def report(rows: list[dict]) -> None:
    hits = sorted((r for r in rows if r["opp"]), key=lambda r: -r["profit"])
    sums = [r["ask_sum"] for r in rows if r["ask_sum"]]
    tight = sum(1 for s in sums if s <= 1.001)
    print(f"[{rows[0]['ts_utc'] if rows else '-'}] scanned {len(rows)} markets | "
          f"{tight} with YES+NO asks <= 1.001 | {len(hits)} profitable after fees")
    for r in hits[:15]:
        o = r["opp"]
        print(f"  {o.side:9} ${o.profit:8.2f} on {o.shares:9.1f} sets "
              f"({o.edge_per_set*100:5.2f}c/set, fee {r['fee_bps']:.0f}bps)  {r['question'][:70]}")
    if not hits:
        closest = sorted((r for r in rows if r["ask_sum"]), key=lambda r: r["ask_sum"])[:5]
        print("  no edge after fees. tightest YES+NO ask sums:")
        for r in closest:
            print(f"    {r['ask_sum']:.3f}  (fee {r['fee_bps']:.0f}bps)  {r['question'][:70]}")


def append_log(path: str, rows: list[dict]) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=LOG_FIELDS, extrasaction="ignore")
        if new:
            w.writeheader()
        w.writerows(rows)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--markets", type=int, default=200, help="how many markets to scan (by 24h volume)")
    p.add_argument("--min-volume", type=float, default=0, help="skip markets below this 24h volume")
    p.add_argument("--fee-bps", type=float, default=None, help="override each market's taker fee (bps)")
    p.add_argument("--min-edge", type=float, default=0.0, help="min profit per set in USDC, e.g. 0.005")
    p.add_argument("--max-shares", type=float, default=float("inf"), help="cap sets per market")
    p.add_argument("--loop", type=int, default=0, help="rescan every N seconds (0 = once)")
    p.add_argument("--log", default=None, help="append every market's snapshot to this CSV")
    args = p.parse_args()

    while True:
        rows = scan_once(args)
        report(rows)
        if args.log:
            append_log(args.log, rows)
        if not args.loop:
            break
        time.sleep(args.loop)


if __name__ == "__main__":
    main()
