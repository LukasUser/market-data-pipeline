"""Tests for the market-making loop. PnL is worked out by hand.

FakeSim stands in for itchpy.PassiveSim: it plays scripted quotes and fills,
and records what the strategy asked for.
"""

import unittest
from collections import namedtuple

from backtest import CLOSE, NS, OPEN
from itchpy import Side
from market_making import ImbalanceAware, JoinBest, make_markets

Level = namedtuple("Level", "price shares")
Quote = namedtuple("Quote", "timestamp bid ask")
Fill = namedtuple("Fill", "timestamp side price shares")


def quote(t, bid, ask, bid_shares=100, ask_shares=100):
    return Quote(t, Level(bid, bid_shares), Level(ask, ask_shares))


class FakeSim:
    def __init__(self, steps):
        self.steps = steps  # [(quote, [fills that happened up to it])]
        self.orders = {Side.Buy: None, Side.Sell: None}
        self.placed = []
        self.pending = []
        self.rejected = 0

    def __iter__(self):
        for q, fills in self.steps:
            for f in fills:
                price, shares = self.orders[f.side]
                self.orders[f.side] = (price, shares - f.shares) if shares > f.shares else None
            self.pending = list(fills)
            yield q

    def take_fills(self):
        fills, self.pending = self.pending, []
        return fills

    def place(self, side, price, shares):
        self.placed.append((side, price, shares))
        self.orders[side] = (price, shares) if shares else None

    def working(self, side):
        return self.orders[side]


BID, ASK = 1_000_000, 1_000_200  # $100.00 / $100.02, mid $100.01
T0, T1, T2 = OPEN, OPEN + NS, OPEN + 2 * NS


class MakeMarketsTest(unittest.TestCase):
    def test_round_trip_earns_the_spread(self):
        sim = FakeSim([
            (quote(T0, BID, ASK), []),
            (quote(T1, BID, ASK, 50), [Fill(T1, Side.Buy, BID, 100)]),
            (quote(T2, BID, ASK, 60), [Fill(T2, Side.Sell, ASK, 100)]),
        ])
        result = make_markets(sim, JoinBest(size=100))

        # Bought 100 at 100.00, sold 100 at 100.02: each fill 1 cent from the mid.
        self.assertAlmostEqual(result.pnl, 2.00)
        self.assertAlmostEqual(result.spread_captured, 2.00)
        self.assertAlmostEqual(result.markout_1s, 0.00)
        self.assertEqual(result.shares_filled, 200)
        self.assertIsNone(result.close_out)

    def test_refills_a_side_after_it_is_filled(self):
        sim = FakeSim([
            (quote(T0, BID, ASK), []),
            (quote(T1, BID, ASK, 50), [Fill(T1, Side.Buy, BID, 100)]),
        ])
        make_markets(sim, JoinBest(size=100))
        self.assertEqual(sim.placed, [
            (Side.Buy, BID, 100), (Side.Sell, ASK, 100),  # T0
            (Side.Buy, BID, 100),                          # T1: the bid filled, so rest again
        ])

    def test_unchanged_wishes_are_not_resent(self):
        sim = FakeSim([(quote(T0, BID, ASK), []), (quote(T1, BID, ASK, 50), [])])
        make_markets(sim, JoinBest(size=100))
        self.assertEqual(len(sim.placed), 2)

    def test_rebates_are_paid_per_share_filled(self):
        sim = FakeSim([
            (quote(T0, BID, ASK), []),
            (quote(T1, BID, ASK, 50), [Fill(T1, Side.Buy, BID, 100)]),
            (quote(T2, BID, ASK, 60), [Fill(T2, Side.Sell, ASK, 100)]),
        ])
        result = make_markets(sim, JoinBest(size=100), rebate_per_share=20)  # $0.002
        self.assertAlmostEqual(result.rebates, 0.40)
        self.assertAlmostEqual(result.pnl, 2.40)

    def test_adverse_selection_shows_in_the_markout(self):
        # Bought at the bid, then the market drops 2 cents within the second.
        sim = FakeSim([
            (quote(T0, BID, ASK), []),
            (quote(T1, BID, ASK, 50), [Fill(T1, Side.Buy, BID, 100)]),
            (quote(T1 + NS // 2, BID - 200, ASK - 200), []),
        ])
        result = make_markets(sim, JoinBest(size=100))

        self.assertAlmostEqual(result.spread_captured, 1.00)  # 1 cent x 100 at the fill
        self.assertAlmostEqual(result.markout_1s, -2.00)  # mid 100.01 → 99.99
        self.assertAlmostEqual(result.markout_60s, -2.00)

    def test_closes_out_at_the_end_of_the_session(self):
        sim = FakeSim([
            (quote(T0, BID, ASK), []),
            (quote(T1, BID, ASK, 50), [Fill(T1, Side.Buy, BID, 100)]),
            (quote(CLOSE - NS, BID - 100, ASK - 100), []),
            (quote(CLOSE, BID - 900, ASK - 900), []),  # after the close: not used
        ])
        result = make_markets(sim, JoinBest(size=100))

        self.assertEqual(result.close_out, (-100, BID - 100))  # sold at the last bid
        self.assertAlmostEqual(result.pnl, -1.00)

    def test_ignores_quotes_before_the_open(self):
        sim = FakeSim([(quote(OPEN - NS, BID, ASK), [])])
        make_markets(sim, JoinBest())
        self.assertEqual(sim.placed, [])


class StrategyTest(unittest.TestCase):
    def test_join_best_respects_the_position_limit(self):
        strategy = JoinBest(size=100, max_position=100)
        self.assertEqual(strategy.on_quote(quote(T0, BID, ASK), 0), ((BID, 100), (ASK, 100)))
        self.assertEqual(strategy.on_quote(quote(T0, BID, ASK), 100), (None, (ASK, 100)))
        self.assertEqual(strategy.on_quote(quote(T0, BID, ASK), -100), ((BID, 100), None))

    def test_imbalance_aware_steps_back_from_the_weak_side(self):
        strategy = ImbalanceAware(size=100, threshold=0.3)
        self.assertEqual(strategy.on_quote(quote(T0, BID, ASK, 100, 900), 0), (None, (ASK, 100)))
        self.assertEqual(strategy.on_quote(quote(T0, BID, ASK, 900, 100), 0), ((BID, 100), None))
        self.assertEqual(strategy.on_quote(quote(T0, BID, ASK, 500, 500), 0), ((BID, 100), (ASK, 100)))


if __name__ == "__main__":
    unittest.main()
