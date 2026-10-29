"""How wide is a stock's bid-ask spread during regular trading hours?

    gunzip -c 12302019.NASDAQ_ITCH50.gz | PYTHONPATH=build-release python3 python/examples/spread.py - AAPL

The spread is time weighted: each quote counts for as long as it stood, so a
spread that flickers wide for a microsecond barely moves the average.
"""

import sys
import time

from itchpy import QuoteStream

NS = 1_000_000_000
OPEN, CLOSE = (9 * 3600 + 30 * 60) * NS, 16 * 3600 * NS
CENT = 100  # Prices have 4 implied decimals, so $0.01 is 100.


def main(path: str, symbol: str) -> int:
    stream = QuoteStream(path, symbol)
    started = time.perf_counter()

    quotes = 0
    weighted = 0  # sum of spread x nanoseconds it stood
    two_sided = 0  # nanoseconds with both a bid and an ask
    one_cent = 0  # nanoseconds at the minimum spread
    prev = None

    for q in stream:
        quotes += 1
        if prev is not None and prev.bid and prev.ask:
            start, end = max(prev.timestamp, OPEN), min(q.timestamp, CLOSE)
            if end > start:
                spread = prev.ask.price - prev.bid.price
                weighted += spread * (end - start)
                two_sided += end - start
                if spread == CENT:
                    one_cent += end - start
        prev = q

    elapsed = time.perf_counter() - started
    print(f"{symbol}: {quotes:,} top-of-book changes, replayed in {elapsed:.1f} s")
    if two_sided:
        print(f"time-weighted spread 09:30-16:00  ${weighted / two_sided / 10_000:.4f}")
        print(f"time at one cent                  {one_cent / two_sided:.1%}")
    print(f"malformed {stream.malformed}, book errors {stream.book_errors}, truncated {stream.truncated}")
    return 0 if not (stream.malformed or stream.book_errors or stream.truncated) else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <itch-file | -> <SYMBOL>")
    sys.exit(main(sys.argv[1], sys.argv[2]))
