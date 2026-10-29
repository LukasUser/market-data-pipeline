"""Tests for the backtester. Expected cash and PnL are worked out by hand.

Quotes here are plain named tuples: the backtester only needs `timestamp`,
`bid` and `ask`, so it doesn't depend on the C++ module.
"""

import unittest
from collections import namedtuple

from backtest import CLOSE, OPEN, Fill, Flat, Imbalance, Strategy, next_move_by_imbalance, run

Level = namedtuple("Level", "price shares")
Quote = namedtuple("Quote", "timestamp bid ask")


def quote(t, bid, ask, bid_shares=100, ask_shares=100):
    return Quote(t, Level(bid, bid_shares), Level(ask, ask_shares))


class Scripted(Strategy):
    """Holds the target given for a quote's timestamp until told otherwise."""

    def __init__(self, targets):
        self.targets = targets
        self.target = 0

    def on_quote(self, q):
        self.target = self.targets.get(q.timestamp, self.target)
        return self.target


# $100.00 / $100.02, then the market moves up 4 cents.
QUOTES = [
    quote(OPEN, 1_000_000, 1_000_200),
    quote(OPEN + 10, 1_000_400, 1_000_600),
    quote(OPEN + 20, 1_000_400, 1_000_600),
]
BUY_THEN_SELL = {OPEN: 100, OPEN + 20: 0}


class RunTest(unittest.TestCase):
    def test_buys_at_the_ask_and_sells_at_the_bid(self):
        result = run(QUOTES, Scripted(BUY_THEN_SELL))

        # Bought 100 at 100.02, sold 100 at 100.04: +$0.02 x 100.
        self.assertEqual(
            result.fills,
            [Fill(OPEN, 100, 1_000_200), Fill(OPEN + 20, -100, 1_000_400)],
        )
        self.assertAlmostEqual(result.pnl, 2.00)
        self.assertEqual(result.shares_traded, 200)
        self.assertEqual(result.final_position, 0)

    def test_latency_fills_against_the_book_at_arrival(self):
        # The buy decided at OPEN arrives at OPEN + 15, after the market moved
        # to 100.04 / 100.06: it pays 100.06 instead of 100.02.
        result = run(QUOTES, Scripted(BUY_THEN_SELL), latency_ns=15)

        self.assertEqual(
            result.fills,
            [Fill(OPEN + 15, 100, 1_000_600), Fill(OPEN + 35, -100, 1_000_400)],
        )
        self.assertAlmostEqual(result.pnl, -2.00)

    def test_fees_are_charged_per_share(self):
        result = run(QUOTES, Scripted(BUY_THEN_SELL), fee_per_share=30)  # $0.003

        self.assertAlmostEqual(result.fees, 0.60)  # 200 shares x $0.003
        self.assertAlmostEqual(result.pnl, 1.40)

    def test_drawdown_is_the_fall_from_the_peak(self):
        # Marked to the 100.05 mid, the long is worth +$3.00; it's sold at the
        # 100.04 bid for +$2.00, a $1.00 fall from the peak.
        result = run(QUOTES, Scripted(BUY_THEN_SELL))
        self.assertAlmostEqual(result.max_drawdown, 1.00)

    def test_closes_out_at_the_end_of_the_session(self):
        quotes = [
            quote(OPEN, 1_000_000, 1_000_200),
            quote(CLOSE - 10, 1_000_300, 1_000_500),
            quote(CLOSE + 10, 900_000, 900_200),  # after the close: never used
        ]
        result = run(quotes, Scripted({OPEN: 100}))

        self.assertEqual(result.fills[-1], Fill(CLOSE, -100, 1_000_300))
        self.assertAlmostEqual(result.pnl, 1.00)
        self.assertEqual(result.final_position, 0)

    def test_does_not_trade_outside_the_session(self):
        quotes = [quote(OPEN - 10, 1_000_000, 1_000_200), quote(OPEN, 1_000_000, 1_000_200)]
        result = run(quotes, Scripted({OPEN - 10: 100}))
        self.assertEqual(result.fills, [])

    def test_flat_never_trades(self):
        result = run(QUOTES, Flat())
        self.assertEqual((result.fills, result.pnl), ([], 0.0))


class ImbalanceTest(unittest.TestCase):
    def test_follows_the_heavier_side(self):
        strategy = Imbalance(threshold=0.8, size=100)
        self.assertEqual(strategy.on_quote(quote(0, 100, 102, 900, 100)), 100)  # 0.9
        self.assertEqual(strategy.on_quote(quote(0, 100, 102, 100, 900)), -100)  # 0.1
        self.assertEqual(strategy.on_quote(quote(0, 100, 102, 500, 500)), 0)  # 0.5

    def test_holds_when_one_side_is_empty(self):
        strategy = Imbalance(threshold=0.8, size=100)
        strategy.on_quote(quote(0, 100, 102, 900, 100))
        self.assertEqual(strategy.on_quote(Quote(0, Level(100, 900), None)), 100)


class NextMoveTest(unittest.TestCase):
    def test_counts_the_direction_of_the_next_mid_change(self):
        quotes = [
            quote(OPEN, 100, 102, 900, 100),  # imbalance 0.9; mid next goes up
            quote(OPEN + 1, 100, 102, 500, 500),  # 0.5; same mid, next goes up
            quote(OPEN + 2, 102, 104),  # 0.5; mid up; next goes down
            quote(OPEN + 3, 100, 102, 100, 900),  # 0.1; mid down; next goes down
            quote(OPEN + 4, 98, 100),  # last change: nothing after it
        ]
        self.assertEqual(
            next_move_by_imbalance(quotes, buckets=5),
            [(0.0, 0.2, 0, 1), (0.2, 0.4, 0, 0), (0.4, 0.6, 1, 1), (0.6, 0.8, 0, 0), (0.8, 1.0, 1, 0)],
        )


if __name__ == "__main__":
    unittest.main()
