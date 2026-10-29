"""Can resting orders earn the spread that market orders paid?

    gunzip -c 12302019.NASDAQ_ITCH50.gz | PYTHONPATH=build-release:python \\
        python3 python/examples/make_markets.py - AAPL join 10

Strategies: "join" rests at the best bid and ask; "imbalance" does the same
but steps back from the side the top-of-book imbalance says is weak. The last
argument is latency in microseconds.
"""

import sys
import time

from itchpy import PassiveSim
from market_making import ImbalanceAware, JoinBest, make_markets

REBATE = 20  # $0.0020 per share, about NASDAQ's rebate for adding liquidity
STRATEGIES = {"join": JoinBest, "imbalance": ImbalanceAware}


def cents(dollars: float, shares: int) -> str:
    return f"{100 * dollars / shares:+.3f}c" if shares else "-"


def main(path: str, symbol: str, name: str, latency_us: int) -> int:
    started = time.perf_counter()
    sim = PassiveSim(path, symbol, latency_ns=latency_us * 1_000)
    r = make_markets(sim, STRATEGIES[name](size=100, max_position=500), rebate_per_share=REBATE)
    n = r.shares_filled

    print(f"{symbol} {name}, {latency_us} us latency, 100 shares a side, |position| <= 500")
    print(f"  replayed in {time.perf_counter() - started:.1f} s; "
          f"malformed {sim.malformed}, book errors {sim.book_errors}, truncated {sim.truncated}")
    print(f"  fills {len(r.fills):,}, shares {n:,}, max position {r.max_position}, rejected {r.rejected}")
    print(f"  {'':24} {'dollars':>12} {'per share':>10}")
    print(f"  {'spread captured':<24} {r.spread_captured:>12,.2f} {cents(r.spread_captured, n):>10}")
    print(f"  {'markout after 1 s':<24} {r.markout_1s:>12,.2f} {cents(r.markout_1s, n):>10}")
    print(f"  {'markout after 60 s':<24} {r.markout_60s:>12,.2f} {cents(r.markout_60s, n):>10}")
    print(f"  {'PnL before rebates':<24} {r.pnl - r.rebates:>12,.2f} {cents(r.pnl - r.rebates, n):>10}")
    print(f"  {'rebates ($0.002/share)':<24} {r.rebates:>12,.2f} {cents(r.rebates, n):>10}")
    print(f"  {'PnL':<24} {r.pnl:>12,.2f} {cents(r.pnl, n):>10}")
    print(f"  close-out at 16:00: {r.close_out}")
    return 0 if not (sim.malformed or sim.book_errors or sim.truncated) else 1


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[3] not in STRATEGIES:
        sys.exit(f"usage: {sys.argv[0]} <itch-file | -> <SYMBOL> <{'|'.join(STRATEGIES)}> <latency-us>")
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])))
