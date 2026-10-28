# ITCH 5.0 feed handler

A C++20 decoder for NASDAQ TotalView-ITCH 5.0, the first stage of a market data
→ order book → strategy pipeline.

## Scope (milestone 1)

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

## Layout

- `include/itch/messages.hpp`: decoded message structs and `Message` variant
- `include/itch/decoder.hpp`: `decode(payload)` → status + message
- `include/itch/frame_reader.hpp`: splits the 2-byte length-prefixed file format
- `tools/itch_stats.cpp`: counts messages by type and measures throughput
- `tests/`: GoogleTest suite; byte layouts are written from the spec, not from the decoder

## Build & test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build && ./build/itch_tests

# With AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DITCH_SANITIZE=ON
cmake --build build-asan && ./build-asan/itch_tests
```

## Running on real data

Sample files are published at `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`.

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DITCH_BUILD_TESTS=OFF
cmake --build build-release
gunzip -c 01302020.NASDAQ_ITCH50.gz | ./build-release/itch_stats -
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

## Benchmarks

Synthetic file with 2M messages (90% book messages), Apple Silicon, Release build:
~39M msg/s, ~1.3 GB/s (decode only, warm page cache).

## Next

1. Run against a full NASDAQ sample day and record the numbers here.
2. Order book: `order_ref → order` map plus price levels per side, for one symbol.
3. Validate the rebuilt book against trade prints / daily volume.
