"""Thin read-only wrappers around Polymarket's public APIs (no keys needed).

* Gamma API: market metadata (questions, token ids, fees, volume).
* CLOB API:  live order books.
"""
from __future__ import annotations

import json
import time
from dataclasses import dataclass

import requests

GAMMA = "https://gamma-api.polymarket.com"
CLOB = "https://clob.polymarket.com"

_session = requests.Session()
_session.headers["User-Agent"] = "polyarb-research/0.1"


def _get(url: str, **params):
    for attempt in range(4):
        r = _session.get(url, params=params, timeout=20)
        if r.status_code == 429 or r.status_code >= 500:
            time.sleep(2 ** attempt)
            continue
        r.raise_for_status()
        return r.json()
    r.raise_for_status()


def _post(url: str, payload):
    for attempt in range(4):
        r = _session.post(url, json=payload, timeout=30)
        if r.status_code == 429 or r.status_code >= 500:
            time.sleep(2 ** attempt)
            continue
        r.raise_for_status()
        return r.json()
    r.raise_for_status()


@dataclass(frozen=True)
class BinaryMarket:
    slug: str
    question: str
    outcomes: tuple[str, str]
    token_ids: tuple[str, str]
    fee_bps: float
    volume_24h: float
    neg_risk: bool


def fetch_binary_markets(limit: int = 200, min_volume_24h: float = 0.0) -> list[BinaryMarket]:
    """Active two-outcome markets with an order book, highest 24h volume first."""
    out: list[BinaryMarket] = []
    offset = 0
    page = 100
    while len(out) < limit:
        rows = _get(f"{GAMMA}/markets", active="true", closed="false", limit=page,
                    offset=offset, order="volume24hr", ascending="false")
        if not rows:
            break
        for m in rows:
            try:
                outcomes = json.loads(m.get("outcomes") or "[]")
                tokens = json.loads(m.get("clobTokenIds") or "[]")
            except json.JSONDecodeError:
                continue
            vol = float(m.get("volume24hr") or 0)
            if (len(tokens) != 2 or len(outcomes) != 2 or not m.get("enableOrderBook")
                    or not m.get("acceptingOrders", True) or vol < min_volume_24h):
                continue
            out.append(BinaryMarket(
                slug=m.get("slug", ""),
                question=m.get("question", ""),
                outcomes=(outcomes[0], outcomes[1]),
                token_ids=(tokens[0], tokens[1]),
                fee_bps=float(m.get("takerBaseFee") or 0),
                volume_24h=vol,
                neg_risk=bool(m.get("negRisk")),
            ))
        offset += page
    return out[:limit]


def fetch_books(token_ids: list[str], batch: int = 50) -> dict[str, dict]:
    """token_id -> {"bids": [(p, s)...], "asks": [(p, s)...], "ts": ms}."""
    books: dict[str, dict] = {}
    for k in range(0, len(token_ids), batch):
        chunk = token_ids[k:k + batch]
        for b in _post(f"{CLOB}/books", [{"token_id": t} for t in chunk]):
            books[b["asset_id"]] = {
                "bids": [(float(l["price"]), float(l["size"])) for l in b.get("bids", [])],
                "asks": [(float(l["price"]), float(l["size"])) for l in b.get("asks", [])],
                "ts": int(b.get("timestamp") or 0),
            }
    return books
