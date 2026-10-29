# Market data pipeline

[![CI](https://github.com/LukasUser/market-data-pipeline/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/LukasUser/market-data-pipeline/actions/workflows/ci.yml)

A C++20 market data → order book → strategy pipeline, built on NASDAQ
TotalView-ITCH 5.0, with Python bindings for research.

1. **Feed handler** (done): decodes ITCH messages from NASDAQ's sample files.
2. **Order book** (done): rebuilds every instrument's book from those messages.
3. **Python quotes** (done): one symbol's top-of-book changes, streamed into Python.
4. **Strategy and backtester** (next).

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

## Layout

- `include/itch/messages.hpp`: decoded message structs and `Message` variant
- `include/itch/decoder.hpp`: `decode(payload)` → status + message
- `include/itch/frame_reader.hpp`: splits the 2-byte length-prefixed file format
- `include/itch/order_book.hpp`: one instrument's book (orders + price levels)
- `include/itch/market.hpp`: routes messages to one book per instrument
- `include/itch/quote_stream.hpp`: one symbol's top-of-book changes from a stream
- `python/itchpy.cpp`: pybind11 bindings
- `python/tests/`: Python tests (`unittest`), run by `ctest`
- `python/examples/spread.py`: time-weighted bid-ask spread for a symbol
- `tools/itch_stats.cpp`: counts messages by type and measures throughput
- `tools/itch_book.cpp`: replays a file and prints one symbol's book
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
3. Strategy and backtester in Python on top of `QuoteStream`.
