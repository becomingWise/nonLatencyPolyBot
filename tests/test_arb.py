import math

import pytest

from polyarb.arb import taker_fee, walk_complete_set


def test_no_arb_when_asks_sum_to_one_or_more():
    assert walk_complete_set([(0.52, 100)], [(0.49, 100)], "BUY_BOTH") is None
    assert walk_complete_set([(0.50, 100)], [(0.50, 100)], "BUY_BOTH") is None


def test_simple_buy_both():
    opp = walk_complete_set([(0.45, 100)], [(0.50, 40)], "BUY_BOTH")
    assert opp.shares == 40                      # limited by the thinner side
    assert math.isclose(opp.gross, 0.95 * 40)
    assert math.isclose(opp.profit, 0.05 * 40)


def test_walks_depth_and_stops_when_edge_disappears():
    yes = [(0.40, 10), (0.47, 10), (0.55, 10)]   # unsorted input is fine
    no = [(0.50, 15), (0.52, 100)]
    opp = walk_complete_set(yes, no, "BUY_BOTH")
    # 10 sets @ 0.90, 5 sets @ 0.97, then 0.47+0.52 = 0.99 -> 5 more, then 0.55+0.52 > 1 stop
    assert opp.shares == 20
    assert math.isclose(opp.profit, 10 * 0.10 + 5 * 0.03 + 5 * 0.01)
    assert (opp.worst_yes, opp.worst_no) == (0.47, 0.52)


def test_fees_can_kill_the_edge():
    # 0.48 + 0.49 = 0.97, but a 10% base fee costs ~0.097 per set
    assert walk_complete_set([(0.48, 100)], [(0.49, 100)], "BUY_BOTH", fee_rate=0.10) is None
    assert walk_complete_set([(0.48, 100)], [(0.49, 100)], "BUY_BOTH", fee_rate=0.0) is not None


def test_fee_is_small_near_the_extremes():
    assert math.isclose(taker_fee(0.99, 100, 0.10), 0.10 * 0.01 * 100)
    assert math.isclose(taker_fee(0.50, 100, 0.10), 5.0)


def test_sell_both():
    opp = walk_complete_set([(0.55, 10), (0.50, 50)], [(0.48, 30)], "SELL_BOTH")
    # best bids first: 0.55+0.48 = 1.03 for 10, then 0.50+0.48 = 0.98 stop
    assert opp.shares == 10
    assert math.isclose(opp.profit, 0.03 * 10)


def test_min_edge_and_max_shares():
    opp = walk_complete_set([(0.45, 100)], [(0.50, 100)], "BUY_BOTH", max_shares=7)
    assert opp.shares == 7
    assert walk_complete_set([(0.45, 100)], [(0.50, 100)], "BUY_BOTH", min_edge=0.06) is None


def test_bad_side():
    with pytest.raises(ValueError):
        walk_complete_set([], [], "SIDEWAYS")
