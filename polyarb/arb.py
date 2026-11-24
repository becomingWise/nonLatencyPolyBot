"""Pure pricing math for the YES+NO complete-set arbitrage.

A binary market pays $1 to whoever holds the winning side, so one YES share
plus one NO share (a "complete set") is always worth exactly $1 at resolution.
Polymarket also lets you merge a set back into $1 or split $1 into a set at
any time. That gives two trades:

* BUY_BOTH:  if best_ask(YES) + best_ask(NO) + fees < 1, buy both, merge, keep the gap.
* SELL_BOTH: if best_bid(YES) + best_bid(NO) - fees > 1, split $1, sell both sides.

Nothing here touches the network, so it is easy to test.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

Level = tuple[float, float]  # (price, size in shares)


def taker_fee(price: float, shares: float, fee_rate: float) -> float:
    """Fee in USDC for a taker fill.

    Polymarket's published formula scales the fee by min(p, 1 - p), so trades
    near 0 or 1 pay almost nothing. `fee_rate` is the market's base fee as a
    fraction (base_fee_bps / 10_000). Check the current docs before trusting
    real numbers: fee schedules change, and many markets charge nothing.
    """
    return fee_rate * min(price, 1.0 - price) * shares


@dataclass(frozen=True)
class Opportunity:
    side: str            # "BUY_BOTH" or "SELL_BOTH"
    shares: float        # complete sets traded
    gross: float         # USDC paid (BUY_BOTH) or received (SELL_BOTH), before fees
    fees: float
    profit: float        # guaranteed USDC profit at resolution / on merge
    worst_yes: float     # deepest YES price touched
    worst_no: float      # deepest NO price touched

    @property
    def edge_per_set(self) -> float:
        return self.profit / self.shares if self.shares else 0.0

    @property
    def return_on_capital(self) -> float:
        capital = self.gross + self.fees if self.side == "BUY_BOTH" else self.shares
        return self.profit / capital if capital else 0.0


def _sorted(levels: Iterable[Level], best_first_low: bool) -> list[list[float]]:
    out = [[float(p), float(s)] for p, s in levels if float(s) > 0]
    out.sort(key=lambda l: l[0], reverse=not best_first_low)
    return out


def walk_complete_set(
    yes_levels: Iterable[Level],
    no_levels: Iterable[Level],
    side: str,
    fee_rate: float = 0.0,
    min_edge: float = 0.0,
    max_shares: float = float("inf"),
) -> Opportunity | None:
    """Walk both books together, taking sets while each extra set still makes
    at least `min_edge` USDC after fees. Returns None if there is no edge.

    For BUY_BOTH pass the two ask books; for SELL_BOTH pass the two bid books.
    """
    if side not in ("BUY_BOTH", "SELL_BOTH"):
        raise ValueError(f"unknown side {side!r}")
    buying = side == "BUY_BOTH"
    yes = _sorted(yes_levels, best_first_low=buying)
    no = _sorted(no_levels, best_first_low=buying)

    i = j = 0
    shares = gross = fees = 0.0
    worst_yes = worst_no = 0.0
    while i < len(yes) and j < len(no) and shares < max_shares:
        py, sy = yes[i]
        pn, sn = no[j]
        unit_fee = taker_fee(py, 1, fee_rate) + taker_fee(pn, 1, fee_rate)
        edge = (1.0 - (py + pn) - unit_fee) if buying else ((py + pn) - 1.0 - unit_fee)
        if edge <= min_edge:
            break
        q = min(sy, sn, max_shares - shares)
        shares += q
        gross += (py + pn) * q
        fees += unit_fee * q
        worst_yes, worst_no = py, pn
        yes[i][1] -= q
        no[j][1] -= q
        if yes[i][1] <= 1e-12:
            i += 1
        if no[j][1] <= 1e-12:
            j += 1

    if shares <= 0:
        return None
    profit = (shares - gross - fees) if buying else (gross - shares - fees)
    return Opportunity(side, shares, gross, fees, profit, worst_yes, worst_no)
