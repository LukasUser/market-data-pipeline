# Market data pipeline

[![CI](https://github.com/LukasUser/market-data-pipeline/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/LukasUser/market-data-pipeline/actions/workflows/ci.yml)

A C++20 market data → order book → strategy pipeline, built on NASDAQ
TotalView-ITCH 5.0, with Python bindings for research.

1. **Feed handler** (done): decodes ITCH messages from NASDAQ's sample files.
2. **Order book** (done): rebuilds every instrument's book from those messages.
3. **Python quotes** (done): one symbol's top-of-book changes, streamed into Python.
4. **Backtester** (done): event-driven, with latency, fees and an imbalance study.
5. **Matching engine** (done): price-time priority with per-order queues,
   validated against a full day of NASDAQ executions.
6. **Resting-order backtest** (done): market making in NASDAQ's real queues.
7. **Many days and symbols in parallel** (next).

## Feed handler

Decodes the seven message types that change the order book. All other types
are framed and counted but reported as `Ignored`.

| Type | Message                   | Size |
|------|---------------------------|------|
| `A`  | Add Order                 | 36   |
| `F`  | Add Order with MPID       | 40   |
| `E`  | Order Executed            | 31   |
| `C`  | Order Executed With Price | 36   |
| `X`  | Order Cancel (partial)    | 23   |
| `D`  | Order Delete              | 19   |
| `U`  | Order Replace             | 35   |

## Order book

`OrderBook` holds one instrument's book; `Market` routes each decoded message
to the right book using the stock locate code in its header.

| Message | Effect on the book |
|---|---|
| `A`, `F` Add | New order rests on its side at its price |
| `E`, `C` Executed | Order loses the executed shares; leaves at zero |
| `X` Cancel | Order loses the cancelled shares; leaves at zero |
| `D` Delete | Order leaves the book |
| `U` Replace | Old order leaves; new order rests on the same side |

Anything inconsistent (an unknown or reused order reference, or removing more
shares than are resting) is reported as a `BookStatus`, not silently skipped.

## Python

`itchpy` is a pybind11 module over the C++ book. `QuoteStream` replays a file
in C++ and yields a `Quote` each time one symbol's best bid or best ask changes
(price or size); Python only sees those changes, not every message.

```python
from itchpy import QuoteStream

stream = QuoteStream("12302019.NASDAQ_ITCH50", "AAPL")  # or "-" for stdin
for q in stream:
    q.timestamp            # nanoseconds since midnight
    q.bid, q.ask           # Level(price, shares), or None if that side is empty
stream.book.bids(5)        # full depth as of the last quote
```

`OrderBook`, `Level`, `Side` and `BookStatus` are exposed too. Prices stay as
fixed-point integers (4 implied decimals) on the Python side as well.

## Backtester

`python/backtest.py` is a small event-driven backtester. Each quote goes to a
`Strategy`, which returns the position it wants to hold; any change becomes a
market order.

- **Fills cross the spread.** Buys pay the ask, sells receive the bid.
- **Latency.** An order arrives `latency_ns` after the quote that triggered it
  and fills against the book *as it stands at arrival*. Filling at the quote
  the strategy reacted to is lookahead: that price may already be gone.
- **Exact cash.** Cash stays in fixed-point integers; drawdown is marked to mid.
- **Session only.** Trading runs 09:30–16:00 and any position is closed at 16:00.
- **Stated simplifications.** Orders fill in full at the best price and don't
  move the market; one order is in flight at a time.

### Result: imbalance predicts direction, but doesn't pay to cross the spread

AAPL, 2019-12-30, 09:30–16:00 (`python/examples/imbalance.py`).
Imbalance = bid shares / (bid shares + ask shares) at the top of book.

| Imbalance | Quotes | Next mid move is up |
|---|---|---|
| 0.0–0.2 | 51,169 | 37.3% |
| 0.2–0.4 | 96,238 | 40.2% |
| 0.4–0.6 | 120,252 | 49.5% |
| 0.6–0.8 | 100,168 | 59.0% |
| 0.8–1.0 | 54,468 | 63.9% |

Trading it with 100-share market orders (long above the threshold, short below
1 − threshold, flat in between):

| Strategy | Trades | PnL | Per share traded |
|---|---|---|---|
| Never trade | 0 | $0 | – |
| Threshold 0.8, no latency, no fees | 79,717 | −$110,586 | −1.36¢ |
| Threshold 0.8, 10 µs latency | 78,394 | −$108,465 | −1.36¢ |
| … plus $0.003/share taker fee | 78,394 | −$132,449 | −1.66¢ |
| Threshold 0.9, 10 µs, fee | 39,283 | −$62,987 | −1.59¢ |

The signal is real (37% → 64% up across the buckets) but small: it says which
way the next tick goes, not that it will go far. Each market order pays about
half the 2.6¢ average spread, and the loss per share traded is almost exactly
that, so the prediction earns close to nothing net of the spread. A strategy
using this signal would have to *earn* the spread with resting orders instead
of paying it, which is why the matching engine is next. One stock on one day
is a single sample, not proof.

## Matching engine

`MatchingEngine` is a limit order book that matches with **price-time
priority**: an incoming order trades with the best opposite price first and,
at one price, with the order that has waited longest, always at the resting
order's price. The rest of the order joins the back of the queue at its limit.

| Operation | Cost | How |
|---|---|---|
| Submit (no match) | O(log L) | find or create the level in a `std::map`, append to its list |
| Each fill | O(1) | pop from the front of the best level's list |
| Cancel / partial cancel | O(1) + O(log L) | hash map from reference → list iterator; unlink the node |
| Shares ahead of an order | O(position) | walk the level's list up to the order |

`L` is the number of price levels on a side. A partial cancel changes the
order's shares in place, so it keeps its place in the queue, as on NASDAQ.

Unlike `OrderBook` (total shares per price, enough to rebuild ITCH), each level
keeps its orders in a `std::list` in arrival order. A list never moves its
elements, so the iterator stored in the hash map stays valid until that order
leaves, and removing from the middle doesn't shift anyone else.

Besides worked examples, a randomized test runs 20,000 orders and cancels and
checks after each one that the book isn't crossed and that no shares appear
or vanish.

### Validation: does our queue order match NASDAQ's?

`QueueAudit` (`tools/itch_queue`) replays a full day through one
`MatchingEngine` per stock. For every execution NASDAQ reports, it records
where that order stood in our queue just before: at the front of the best
price (what price-time priority predicts), behind other orders at that price,
behind a better price, or unknown to us.

Replay uses `add`, not `submit`: NASDAQ decides what trades and reports it as
executions, so the engine must not match orders itself. A first version
used `submit`, and 957 orders that NASDAQ accepted while crossing the book
(halts, auctions) traded inside our engine, leaving 1,821 later messages
referring to orders we had removed. With `add`, 2,630 crossing orders rest as
reported and every message applies cleanly.

2019-12-30, all 8,892 instruments:

| Execution | Count | Front of queue | Behind in queue | Behind on price | Unknown |
|---|---|---|---|---|---|
| `E` before 09:30 | 28,752 | 100.00% | 0 | 0 | 0 |
| `E` 09:30–16:00 | 5,683,179 | **99.81%** | 10,565 (0.19%) | 0 | 0 |
| `E` after 16:00 | 10,893 | 100.00% | 0 | 0 | 0 |
| `C` 09:30–16:00 | 97,299 | 15.09% | 84.63% | 0.28% | 0 |
| `C` after 16:00 | 2,617 | 98.89% | 1.11% | 0 | 0 |

Book errors 0, resting orders at end of day 0.

- **Continuous trading follows our queues.** 99.81% of regular-hours `E`
  executions hit the front of the best price, and none hit a worse price.
- **`C` executions mostly don't, as expected.** They are executions at a
  price other than the order's own, mainly the opening cross, which fills
  orders by auction rules rather than continuous queue order.
- **The 0.19% isn't explained yet.** It isn't the opening cross (34 of the
  10,565 fall in 09:30:00), it is spread over 1,052 seconds and 436 stocks,
  heaviest in the busiest names (MSFT 1,075, AAPL 651, TSLA 572). It isn't
  replaces either: replaced orders were front of queue in all 345,214 of
  their executions; every out-of-queue execution was on a freshly added order.

## Resting-order backtest

`PassiveSim` (C++, exposed to Python) replays one symbol through a
`MatchingEngine` and keeps our own resting orders in its real queues, one per
side. An order joins behind every real share already at its price; real
orders ahead of it that execute or cancel move it forward. It fills when the
market would have reached it first: a real order behind it at the same price
executes, a real order at a worse price on its side executes, or a new order
arrives priced through it. Requests take `latency_ns` to arrive, and the old
order stays live (and can fill) until then. A size cut at the same price keeps
the queue position; anything else rejoins at the back.

Our orders don't move the market: NASDAQ's trades happen as reported, and
nobody reacts to us. The position limit is checked when quoting, so an order
already in flight can overshoot it by one order (600 against a 500 limit).

`python/market_making.py` runs a strategy on it and explains the PnL:
**spread captured** (distance from the mid at each fill), **markout** (how
the mid moved 1 s and 60 s after the fill), **rebates** ($0.0020/share for
resting liquidity) and closing any position at 16:00 with a market order.

### Result: the spread is earned, then lost to adverse selection

AAPL, 2019-12-30, 09:30–16:00, 100 shares a side, |position| ≤ 500
(`python/examples/make_markets.py`). Per share filled:

| Strategy | Latency | Shares filled | Spread captured | Markout 1 s | Markout 60 s | PnL before rebates | PnL with rebates |
|---|---|---|---|---|---|---|---|
| Join best bid and ask | 10 µs | 535,179 | +1.02¢ | −1.70¢ | −2.08¢ | −0.89¢ | **−0.69¢** (−$3,669) |
| Same, skip the side imbalance says is weak | 10 µs | 305,060 | +1.08¢ | −1.86¢ | −2.20¢ | −0.98¢ | −0.78¢ (−$2,391) |
| Join best bid and ask | 1 ms | 751,358 | +0.44¢ | −1.50¢ | −1.69¢ | −1.26¢ | −1.06¢ (−$7,940) |

For comparison, crossing the spread with market orders lost −1.36¢ per share
before fees (see Backtester).

- **Resting earns the spread.** Each fill is about 1¢ better than the mid,
  where a market order is about 1.3¢ worse.
- **Then adverse selection takes it back.** Within a second of a fill the mid
  has moved about 1.7¢ against us. A resting order fills when someone chooses
  to trade with it, often because the price is about to move through it.
  Joining the back of a long queue makes this worse: by the time it's our
  turn, the level is usually being cleared.
- **The simple imbalance filter doesn't help.** It halves the trading but the
  fills it keeps are no better; the markout per share gets slightly worse.
- **Latency hurts.** At 1 ms, quotes go stale: 1,466 requests arrive after the
  market has moved through them, and spread captured falls from 1.02¢ to 0.44¢.

## Layout

- `include/itch/messages.hpp`: decoded message structs and `Message` variant
- `include/itch/decoder.hpp`: `decode(payload)` → status + message
- `include/itch/frame_reader.hpp`: splits the 2-byte length-prefixed file format
- `include/itch/order_book.hpp`: one instrument's book (orders + price levels)
- `include/itch/market.hpp`: routes messages to one book per instrument
- `include/itch/matching_engine.hpp`: price-time priority matching with per-order queues
- `include/itch/queue_audit.hpp`: checks NASDAQ's executions against the engine's queues
- `include/itch/passive_sim.hpp`: our resting orders simulated in the real queues
- `include/itch/quote_stream.hpp`: one symbol's top-of-book changes from a stream
- `python/itchpy.cpp`: pybind11 bindings
- `python/tests/`: Python tests (`unittest`), run by `ctest`
- `python/examples/spread.py`: time-weighted bid-ask spread for a symbol
- `python/backtest.py`: event-driven backtester, strategies, imbalance study
- `python/examples/imbalance.py`: does imbalance predict the next move, and does it pay?
- `python/market_making.py`: market-making loop, PnL attribution, strategies
- `python/examples/make_markets.py`: runs a market-making strategy over a day
- `tools/itch_stats.cpp`: counts messages by type and measures throughput
- `tools/itch_book.cpp`: replays a file and prints one symbol's book
- `tools/itch_queue.cpp`: queue audit over a whole file
- `tests/`: GoogleTest suite; byte layouts are written from the spec, not from the decoder

## Build & test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build && ./build/itch_tests

# With AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DITCH_SANITIZE=ON
cmake --build build-asan && ./build-asan/itch_tests

# Everything, including the Python tests (needs Python 3.10+ with headers)
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release && ctest --test-dir build-release
```

Sanitizer builds skip the Python module: a normal Python interpreter can't load
an ASan-instrumented extension. Pass `-DITCH_BUILD_PYTHON=OFF` to skip it elsewhere.

CI runs both configurations (Release, and Debug with ASan+UBSan) with GCC and
Clang on Linux and Apple Clang on macOS. See `.github/workflows/ci.yml`.

## Running on real data

Sample files are published at `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`.

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DITCH_BUILD_TESTS=OFF
cmake --build build-release
gunzip -c 12302019.NASDAQ_ITCH50.gz | ./build-release/itch_stats -

# Rebuild every book; show AAPL's top 5 levels at 12:00:00 and at end of day
gunzip -c 12302019.NASDAQ_ITCH50.gz | ./build-release/itch_book - AAPL 12:00:00
```

## Design notes

- **Strict lengths.** A book message whose framed length doesn't match the spec
  is rejected (`BadLength`) rather than partially decoded. In a length-prefixed
  stream a mismatch means corruption.
- **Prices stay fixed-point** (`uint32`, 4 implied decimals). Floating point
  would make price-level lookups in the book unreliable.
- **Replace (`U`) omits side and symbol.** The book layer has to carry them
  over from the original order.
- **Zero-copy framing.** `FrameReader` hands out spans into its own buffer.
  They stay valid until the next `next()` call.
- **Orders in a hash map** (`std::unordered_map<OrderRef, Order>`). Execute,
  Cancel, Delete and Replace carry only an order reference, so every message
  needs an O(1) lookup; nothing ever needs orders sorted by reference.
- **Price levels in sorted maps** (`std::map`). Bids sort highest-first and
  asks lowest-first, so the best price on either side is always `begin()`.
  A level stores the total shares at that price, not the individual orders;
  per-order queue position will matter for the matching engine, not here.
- **Thin bindings.** All replay logic is in C++ (`QuoteStream`), tested by
  GoogleTest and the sanitizers; `itchpy.cpp` only converts types.
- **One symbol at a time in `QuoteStream`.** The symbol's locate code comes
  from its first Add Order, and messages for every other locate are decoded
  but never applied, so following one stock costs little more than decoding.
- **Routing by stock locate.** Every ITCH message header carries the
  instrument's locate code, so `Market` keeps one independent book per
  instrument without a global order index.

## Benchmarks

Synthetic file with 2M messages (90% book messages), Apple Silicon, Release build:
~39M msg/s, ~1.3 GB/s (decode only, warm page cache).

Full NASDAQ trading day (2019-12-30), Apple Silicon, Release build:

| Input | Messages | Time | Throughput |
|---|---|---|---|
| Piped through `gunzip` | 268.7M | 125.8 s | 2.1M msg/s (`gunzip`-bound) |
| Uncompressed file (warm cache, median of 5) | 268.7M | 4.92 s (4.83–4.98) | 54.7M msg/s, 1.68 GB/s |

Order book replay of the same day (`itch_book`, piped through `gunzip`):

| Check | Result |
|---|---|
| Book messages applied | 263,241,937, all `Ok` |
| Unknown / duplicate / overfill | 0 / 0 / 0 |
| Instruments with a book | 8,892 |
| Resting orders at end of day | 0 (every order deleted or filled) |
| AAPL at 12:00:00 | best bid 290.45 × 200, best ask 290.49 × 100 |
| AAPL updates with a crossed book | 0 |
| Time | 205.4 s (1.3M msg/s) |

Decoding alone took 125.8 s through `gunzip`, so the book is now the
bottleneck: one heap allocation per order in `std::unordered_map` and per
price level in `std::map`.

AAPL quotes into Python (`python/examples/spread.py`, piped through `gunzip`):

| Check | Result |
|---|---|
| Top-of-book changes yielded to Python | 424,660 |
| Time | 129.2 s (`gunzip`-bound; decoding alone took 125.8 s) |
| Time-weighted spread, 09:30–16:00 | $0.0264 |
| Share of the session at a one-cent spread | 12.9% |
| Malformed / book errors / truncated | 0 / 0 / no |

Applying only AAPL's messages makes this faster than `itch_book`, which
rebuilds all 8,892 books. The spread is NASDAQ's own book, not the
consolidated quote across all exchanges, so it is wider than AAPL's NBBO.

## Next

1. Speed up the book: a flat hash map with reserved capacity for orders, and a
   cheaper price-level structure. Benchmark before and after.
2. Validate executions against trade prints / daily volume.
3. Explain the 0.19% of regular-hours executions that skip ahead of our queue.
4. Measure markout by queue position at the moment of the fill, to test whether
   fills from the front of the queue are less adversely selected.
5. Run many days and symbols in parallel, so results aren't one stock on one day.
