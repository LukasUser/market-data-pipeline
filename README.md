# Market Data Pipeline

[![CI](https://github.com/LukasUser/market-data-pipeline/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/LukasUser/market-data-pipeline/actions/workflows/ci.yml)

A C++20 and Python research pipeline on raw NASDAQ TotalView-ITCH 5.0 data. It
rebuilds every stock's order book, matches orders with price-time priority, and
backtests trading strategies with realistic latency, fees and queue position.

```
ITCH file ─► FrameReader ─► decode ─► OrderBook / MatchingEngine ─► itchpy (pybind11) ─► Python backtests
                                                        │
                       itch_split ─► one file per stock ─┴─► runner.py (parallel, resumable)
```

## Highlights

Full trading day, 2019-12-30:

| | |
|---|---|
| Replay | 268.7M messages, 8,892 stocks, **0 errors**; every book empty at the close |
| Decode speed | 54.7M messages/s (1.68 GB/s) |
| Queue model vs NASDAQ | **99.81%** of 5.7M executions hit the front of our queue |
| Market orders, AAPL | −1.36¢/share: pays the spread |
| Resting orders, AAPL | +1.02¢ spread captured, −1.70¢ adverse selection after 1 s |
| 100 stocks, 10 µs latency | −0.23¢/share; 39 of 100 profitable (16 of 100 at 1 ms) |
| Splitting by stock | 47× faster per job; 400 backtests in 21 CPU-minutes |

## Repo layout

```
include/itch/     C++ headers
src/              C++ implementation
tools/            Command-line tools (itch_stats, itch_book, itch_queue, itch_rank, itch_split)
python/           itchpy bindings, backtest.py, market_making.py, runner.py
python/examples/  Research scripts: spread, imbalance, make_markets
tests/            GoogleTest suite (C++); python/tests/ has the Python tests
```

| Component | What it does |
|---|---|
| `FrameReader`, `decode` | Zero-copy framing; strict decoding of the 7 book messages (A F E C X D U) |
| `OrderBook`, `Market` | One book per stock: hash map of orders, sorted price levels |
| `MatchingEngine` | Price-time priority; per-order queues with O(1) cancel |
| `QueueAudit` | Checks NASDAQ's executions against the engine's queues |
| `QuoteStream`, `PassiveSim` | One stock's top of book; our resting orders in the real queues |
| `itchpy` | pybind11 bindings for all of the above |
| `backtest.py`, `market_making.py` | Event-driven backtests: latency, fees, rebates, markouts |
| `itch_split`, `runner.py` | Split a day by stock; run jobs across cores, resume after interruption |

## Build and test

Needs CMake 3.25+, a C++20 compiler and Python 3.10+ with headers.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build

# AddressSanitizer + UndefinedBehaviorSanitizer (skips the Python module)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DITCH_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan
```

CI builds Release and ASan+UBSan with GCC and Clang on Linux and Apple Clang on
macOS. All six jobs must pass before a merge.

## Running on real data

Sample files: <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>

```bash
F=12302019.NASDAQ_ITCH50.gz
gunzip -c $F | ./build/itch_book - AAPL 12:00:00     # AAPL's book at noon
gunzip -c $F | ./build/itch_queue -                   # queue audit, all stocks
gunzip -c $F | ./build/itch_split - day/ AAPL MSFT    # one file per stock
PYTHONPATH=build:python python3 python/runner.py day/ results.jsonl --workers 4
```

```python
from itchpy import QuoteStream

for q in QuoteStream("day/AAPL.itch", "AAPL"):
    q.timestamp, q.bid, q.ask   # Level(price, shares); prices have 4 implied decimals
```

## Findings

- **Imbalance predicts direction but doesn't pay.** The next move is up 37% to
  64% of the time depending on imbalance, but market orders lose about half the
  spread on every trade.
- **Resting orders earn the spread, then lose it to adverse selection.** Fills
  tend to come just before the price moves against you. Latency makes it worse.
- **Limitations:** one day of data, no market impact, fills only at the best
  price. 0.19% of executions skip ahead of our queue for reasons not yet found.

## Design choices

- **Fixed-point prices** (4 implied decimals), never floats, so book lookups are exact.
- **Replays never match orders.** The exchange decides what trades; letting the
  engine match crossing orders made it drift from NASDAQ's book.
- **Thin bindings.** All logic is in C++, covered by GoogleTest and sanitizers.
- **Partition by stock.** Per-stock jobs are independent, so they run in parallel.

## Next

- Run more trading days and report results in basis points, so prices are comparable.
- Speed up the book (flat hash map, array-based price levels).
- Measure adverse selection by queue position at the moment of each fill.
