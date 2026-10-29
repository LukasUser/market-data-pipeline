"""Market making with resting orders, simulated in NASDAQ's real queues.

The strategy sees each top-of-book quote and says where it wants a resting
buy (bid) and sell (ask), if anywhere. itchpy.PassiveSim keeps those orders in
the real queues and reports fills; this loop does the accounting.

Profit is split into the parts that explain it:
- Spread captured: how far each fill was from the mid price, in our favour.
  Resting at the bid buys below the mid; that's the reward for waiting.
- Markout: how the mid moved after the fill (1 s and 60 s later). Resting
  orders tend to fill just before the price moves through them, which is
  adverse selection, so this is usually negative.
- Rebates: NASDAQ pays liquidity providers per share filled.
- Closing out: any position left at 16:00 is closed with a market order.
"""

from __future__ import annotations

from bisect import bisect_left, bisect_right
from dataclasses import dataclass

from backtest import CLOSE, NS, OPEN, PRICE_SCALE
from itchpy import Side


class MarketMaker:
    def on_quote(self, quote, position: int):
        """Returns (bid, ask): each a (price, shares) to rest, or None."""
        raise NotImplementedError


class JoinBest(MarketMaker):
    """Rests at the best bid and the best ask, within a position limit."""

    def __init__(self, size: int = 100, max_position: int = 500):
        self.size = size
        self.max_position = max_position

    def on_quote(self, quote, position):
        bid = ask = None
        if quote.bid and position + self.size <= self.max_position:
            bid = (quote.bid.price, self.size)
        if quote.ask and position - self.size >= -self.max_position:
            ask = (quote.ask.price, self.size)
        return bid, ask


class ImbalanceAware(JoinBest):
    """JoinBest, but stands aside on the side the imbalance says is about to be
    run over: no bid when sellers dominate the top of book, no ask when buyers do.
    """

    def __init__(self, size: int = 100, max_position: int = 500, threshold: float = 0.3):
        super().__init__(size, max_position)
        self.threshold = threshold

    def on_quote(self, quote, position):
        bid, ask = super().on_quote(quote, position)
        if quote.bid and quote.ask:
            imbalance = quote.bid.shares / (quote.bid.shares + quote.ask.shares)
            if imbalance < self.threshold:
                bid = None
            if imbalance > 1 - self.threshold:
                ask = None
        return bid, ask


@dataclass
class MarketMakingResult:
    fills: list  # (timestamp, signed shares, price)
    pnl: float  # dollars: fills, rebates and the 16:00 close-out
    spread_captured: float  # dollars
    markout_1s: float  # dollars
    markout_60s: float  # dollars
    rebates: float  # dollars
    close_out: tuple | None  # (signed shares, price) traded at 16:00
    max_position: int
    rejected: int

    @property
    def shares_filled(self) -> int:
        return sum(abs(shares) for _, shares, _ in self.fills)


def make_markets(sim, strategy: MarketMaker, *, rebate_per_share: int = 0,
                 start: int = OPEN, end: int = CLOSE) -> MarketMakingResult:
    """Runs `strategy` on `sim` (an itchpy.PassiveSim) from `start` to `end`.

    `rebate_per_share` is in price units: 20 is $0.0020, about NASDAQ's
    rebate for adding liquidity.
    """
    position = 0
    cash = 0  # price units
    max_position = 0
    fills: list[tuple[int, int, int]] = []
    mid_times: list[int] = []
    doubled_mids: list[int] = []  # 2 x mid keeps it an integer
    wanted = {Side.Buy: None, Side.Sell: None}  # what we last asked for
    last = None

    for quote in sim:
        filled_sides = set()
        for f in sim.take_fills():
            signed = f.shares if f.side == Side.Buy else -f.shares
            position += signed
            cash -= signed * f.price
            fills.append((f.timestamp, signed, f.price))
            max_position = max(max_position, abs(position))
            filled_sides.add(f.side)
        # A fully filled order is gone: ask again even if the wish is unchanged.
        for side in filled_sides:
            if sim.working(side) is None:
                wanted[side] = None

        if quote.timestamp >= end:
            break
        last = quote
        if quote.bid and quote.ask:
            mid_times.append(quote.timestamp)
            doubled_mids.append(quote.bid.price + quote.ask.price)
        if quote.timestamp < start:
            continue

        bid, ask = strategy.on_quote(quote, position)
        for side, want in ((Side.Buy, bid), (Side.Sell, ask)):
            if want != wanted[side]:
                sim.place(side, *(want or (0, 0)))
                wanted[side] = want

    close_out = None
    if position and last is not None:
        level = last.bid if position > 0 else last.ask
        if level is not None:
            cash += position * level.price  # sell a long at the bid, buy back a short at the ask
            close_out = (-position, level.price)

    def mid_before(t):  # the mid just before the event at t
        i = bisect_left(mid_times, t) - 1
        return doubled_mids[i] if i >= 0 else None

    def mid_at(t):
        i = bisect_right(mid_times, t) - 1
        return doubled_mids[i] if i >= 0 else None

    captured = markout_1s = markout_60s = 0  # in half price units
    for t, signed, price in fills:
        before = mid_before(t)
        if before is None:
            continue
        captured += signed * (before - 2 * price)
        markout_1s += signed * (mid_at(t + NS) - before)
        markout_60s += signed * (mid_at(t + 60 * NS) - before)

    shares_filled = sum(abs(s) for _, s, _ in fills)
    rebates = shares_filled * rebate_per_share
    return MarketMakingResult(
        fills=fills,
        pnl=(cash + rebates) / PRICE_SCALE if not position or close_out else float("nan"),
        spread_captured=captured / 2 / PRICE_SCALE,
        markout_1s=markout_1s / 2 / PRICE_SCALE,
        markout_60s=markout_60s / 2 / PRICE_SCALE,
        rebates=rebates / PRICE_SCALE,
        close_out=close_out,
        max_position=max_position,
        rejected=sim.rejected,
    )
