"""Does top-of-book imbalance predict the next price move, and can you trade it?

    gunzip -c 12302019.NASDAQ_ITCH50.gz | PYTHONPATH=build-release:python python3 python/examples/imbalance.py - AAPL

Part 1 is research: how often the mid price next moves up, grouped by
imbalance. Part 2 backtests trading on it, with and without latency and fees.
"""

import sys
import time

from backtest import Flat, Imbalance, next_move_by_imbalance, run
from itchpy import QuoteStream

US = 1_000  # nanoseconds


def main(path: str, symbol: str) -> int:
    started = time.perf_counter()
    stream = QuoteStream(path, symbol)
    quotes = list(stream)  # replay once, then reuse for every run
    print(f"{symbol}: {len(quotes):,} quotes loaded in {time.perf_counter() - started:.1f} s")
    if stream.malformed or stream.book_errors or stream.truncated:
        print("replay had errors; results are not trustworthy")
        return 1

    print("\nPart 1: imbalance vs. direction of the next mid-price change (09:30-16:00)")
    print(f"  {'imbalance':>11}  {'quotes':>9}  {'next move up':>12}")
    for low, high, ups, downs in next_move_by_imbalance(quotes):
        total = ups + downs
        share = f"{ups / total:.1%}" if total else "-"
        print(f"  {low:.1f} - {high:.1f}  {total:>9,}  {share:>12}")

    print("\nPart 2: backtests, 100 shares, market orders, flat at 16:00")
    runs = [
        ("never trade", Flat, {}),
        ("imbalance 0.8, no latency, no fees", lambda: Imbalance(0.8), {}),
        ("imbalance 0.8, 10 us latency", lambda: Imbalance(0.8), {"latency_ns": 10 * US}),
        ("imbalance 0.8, 10 us, $0.003/share fee", lambda: Imbalance(0.8), {"latency_ns": 10 * US, "fee_per_share": 30}),
        ("imbalance 0.9, 10 us, $0.003/share fee", lambda: Imbalance(0.9), {"latency_ns": 10 * US, "fee_per_share": 30}),
    ]
    print(f"  {'strategy':<40} {'trades':>7} {'PnL':>11} {'per share':>10} {'max drawdown':>13}")
    for name, make, options in runs:
        r = run(quotes, make(), **options)
        print(f"  {name:<40} {len(r.fills):>7,} {r.pnl:>11,.2f} {r.pnl_per_share * 100:>9.2f}c {r.max_drawdown:>13,.2f}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <itch-file | -> <SYMBOL>")
    sys.exit(main(sys.argv[1], sys.argv[2]))
