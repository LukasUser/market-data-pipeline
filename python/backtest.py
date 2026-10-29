"""Event-driven backtester for one symbol's top-of-book quotes.

The loop hands each quote to a strategy, which answers with the position (in
shares, negative for short) it wants to hold. Any change becomes a market
order: buys pay the ask, sells receive the bid. The order reaches the exchange
`latency_ns` after the quote that triggered it and fills against the book as it
stood at that moment, not the quote the strategy saw. Filling at the quote the
strategy reacted to would let it trade on prices that may already be gone.

Simplifications, stated so results can be read honestly:
- An order fills in full at the best price. A size larger than the best level
  would really walk the book to worse prices.
- Our orders don't move the market: nobody reacts to them.
- One order is in flight at a time; decisions made while it travels are dropped.

Prices are fixed point with 4 implied decimals, as in the C++ code. Cash is
kept as an exact integer; dollars appear only in the Result.

Quotes only need `timestamp`, `bid` and `ask` (each a Level with `price` and
`shares`, or None), so itchpy.Quote works, and so does anything shaped like it.
"""

from __future__ import annotations

from dataclasses import dataclass

NS = 1_000_000_000
OPEN = (9 * 3600 + 30 * 60) * NS  # 09:30, in nanoseconds since midnight
CLOSE = 16 * 3600 * NS  # 16:00
PRICE_SCALE = 10_000  # 4 implied decimals: $1 is 10,000


class Strategy:
    """Called once per quote during the session; returns the target position."""

    def on_quote(self, quote) -> int:
        raise NotImplementedError


class Flat(Strategy):
    """Never trades. The baseline every strategy has to beat."""

    def on_quote(self, quote) -> int:
        return 0


class Imbalance(Strategy):
    """Long when resting buyers outweigh sellers at the top of book, short when
    sellers outweigh buyers, flat in between.

    imbalance = bid shares / (bid shares + ask shares), from 0 to 1.
    """

    def __init__(self, threshold: float = 0.8, size: int = 100):
        self.threshold = threshold
        self.size = size
        self.target = 0

    def on_quote(self, quote) -> int:
        if quote.bid and quote.ask:
            imbalance = quote.bid.shares / (quote.bid.shares + quote.ask.shares)
            if imbalance >= self.threshold:
                self.target = self.size
            elif imbalance <= 1 - self.threshold:
                self.target = -self.size
            else:
                self.target = 0
        return self.target  # one side empty: keep the last decision


@dataclass
class Fill:
    timestamp: int
    shares: int  # positive bought, negative sold
    price: int


@dataclass
class Result:
    fills: list[Fill]
    pnl: float  # dollars, after fees
    fees: float  # dollars
    max_drawdown: float  # dollars, largest fall from a peak in marked-to-mid PnL
    final_position: int  # nonzero only if the close-out couldn't trade

    @property
    def shares_traded(self) -> int:
        return sum(abs(f.shares) for f in self.fills)

    @property
    def pnl_per_share(self) -> float:
        return self.pnl / self.shares_traded if self.fills else 0.0


def run(quotes, strategy: Strategy, *, latency_ns: int = 0, fee_per_share: int = 0,
        start: int = OPEN, end: int = CLOSE) -> Result:
    """Replays `quotes` through `strategy` between `start` and `end`, then
    closes any position against the last book before `end`.

    `fee_per_share` is in price units (30 is $0.003, a typical taker fee).
    """
    position = 0
    cash = 0  # price units
    fees = 0
    fills: list[Fill] = []
    pending: tuple[int, int] | None = None  # (arrival time, target position)
    standing = None  # the latest quote, i.e. the book as it stands now
    peak = 0.0
    max_drawdown = 0.0

    def execute(target: int, quote, timestamp: int) -> None:
        nonlocal position, cash, fees
        shares = target - position
        level = quote.ask if shares > 0 else quote.bid
        if shares == 0 or level is None:
            return  # nothing to do, or nothing on that side to trade against
        cash -= shares * level.price + abs(shares) * fee_per_share
        fees += abs(shares) * fee_per_share
        position = target
        fills.append(Fill(timestamp, shares, level.price))

    def mark(quote) -> None:
        nonlocal peak, max_drawdown
        if quote.bid and quote.ask:
            equity = cash + position * (quote.bid.price + quote.ask.price) / 2
            peak = max(peak, equity)
            max_drawdown = max(max_drawdown, peak - equity)

    for quote in quotes:
        # The order in flight arrived before this quote, so it met the book
        # as it stood until now.
        if pending is not None and pending[0] < quote.timestamp:
            execute(pending[1], standing, pending[0])
            pending = None
        if quote.timestamp >= end:
            break
        standing = quote
        if quote.timestamp < start:
            continue

        target = strategy.on_quote(quote)
        if pending is None and target != position:
            pending = (quote.timestamp + latency_ns, target)
        mark(quote)

    if standing is not None:
        if pending is not None:
            execute(pending[1], standing, pending[0])
        execute(0, standing, end)  # close out at the end of the session
        mark(standing)

    equity = cash
    if position and standing is not None and standing.bid and standing.ask:
        equity += position * (standing.bid.price + standing.ask.price) / 2
    return Result(
        fills=fills,
        pnl=equity / PRICE_SCALE,
        fees=fees / PRICE_SCALE,
        max_drawdown=max_drawdown / PRICE_SCALE,
        final_position=position,
    )


def next_move_by_imbalance(quotes, buckets: int = 5, *, start: int = OPEN, end: int = CLOSE):
    """Does top-of-book imbalance predict the direction of the next mid-price
    change? Groups two-sided quotes in the session by imbalance and counts how
    often the mid's next change is up or down.

    Returns one (low, high, ups, downs) tuple per bucket. Quotes with no later
    mid change are left out.
    """
    two_sided = [q for q in quotes if start <= q.timestamp < end and q.bid and q.ask]
    doubled_mids = [q.bid.price + q.ask.price for q in two_sided]  # 2 x mid stays an integer
    counts = [[0, 0] for _ in range(buckets)]

    # Walk backwards so the next change is always known.
    next_direction = 0
    for i in range(len(two_sided) - 1, -1, -1):
        if i + 1 < len(two_sided) and doubled_mids[i + 1] != doubled_mids[i]:
            next_direction = 1 if doubled_mids[i + 1] > doubled_mids[i] else -1
        if next_direction == 0:
            continue
        q = two_sided[i]
        imbalance = q.bid.shares / (q.bid.shares + q.ask.shares)
        bucket = min(int(imbalance * buckets), buckets - 1)
        counts[bucket][0 if next_direction > 0 else 1] += 1

    return [(i / buckets, (i + 1) / buckets, up, down) for i, (up, down) in enumerate(counts)]
