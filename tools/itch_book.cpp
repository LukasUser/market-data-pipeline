// Replays an ITCH 5.0 file through the order book and shows one symbol's book
// at a chosen time of day and at the end of the day.
//
//   itch_book 12302019.NASDAQ_ITCH50 AAPL
//   gunzip -c 12302019.NASDAQ_ITCH50.gz | itch_book - AAPL 10:30:00

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "itch/decoder.hpp"
#include "itch/frame_reader.hpp"
#include "itch/market.hpp"

namespace {

constexpr itch::Timestamp kNanosPerSecond = 1'000'000'000;
constexpr std::size_t kDepth = 5;

// "HH:MM:SS" → nanoseconds since midnight.
std::optional<itch::Timestamp> parse_time(const char* s) {
  unsigned h = 0, m = 0, sec = 0;
  if (std::sscanf(s, "%u:%u:%u", &h, &m, &sec) != 3 || h > 23 || m > 59 || sec > 59) {
    return std::nullopt;
  }
  return ((h * 60u + m) * 60u + sec) * kNanosPerSecond;
}

std::string format_time(itch::Timestamp t) {
  const auto secs = t / kNanosPerSecond;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu",
                static_cast<unsigned long long>(secs / 3600),
                static_cast<unsigned long long>(secs / 60 % 60),
                static_cast<unsigned long long>(secs % 60));
  return buf;
}

// Prints a fixed-point price exactly, without going through floating point.
std::string format_price(itch::Price p) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
  return buf;
}

void print_levels(const itch::OrderBook& book) {
  const auto bids = book.bids(kDepth);
  const auto asks = book.asks(kDepth);
  if (bids.empty() && asks.empty()) {
    std::printf("  (empty)\n");
    return;
  }
  std::printf("  %10s %12s | %-12s %s\n", "bid size", "bid", "ask", "ask size");
  for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
    std::string bid_size, bid, ask, ask_size;
    if (i < bids.size()) {
      bid_size = std::to_string(bids[i].shares);
      bid = format_price(bids[i].price);
    }
    if (i < asks.size()) {
      ask = format_price(asks[i].price);
      ask_size = std::to_string(asks[i].shares);
    }
    std::printf("  %10s %12s | %-12s %s\n", bid_size.c_str(), bid.c_str(), ask.c_str(),
                ask_size.c_str());
  }
  std::printf("  orders resting: %zu\n", book.order_count());
}

void print_snapshot(const itch::Market& market, std::optional<std::uint16_t> locate,
                    std::string_view symbol, const std::string& when) {
  std::printf("\n%.*s at %s\n", static_cast<int>(symbol.size()), symbol.data(), when.c_str());
  const itch::OrderBook* book = locate ? market.book(*locate) : nullptr;
  if (book == nullptr) {
    std::printf("  (no orders seen yet)\n");
    return;
  }
  print_levels(*book);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "usage: %s <itch-file | -> <SYMBOL> [HH:MM:SS]\n", argv[0]);
    return 2;
  }
  const std::string_view path = argv[1];
  const std::string_view symbol = argv[2];
  const auto snapshot_at = parse_time(argc == 4 ? argv[3] : "12:00:00");
  if (!snapshot_at) {
    std::fprintf(stderr, "bad time %s, expected HH:MM:SS\n", argv[3]);
    return 2;
  }

  std::ifstream file;
  if (path != "-") {
    file.open(argv[1], std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", argv[1]);
      return 2;
    }
  }
  std::istream& in = path == "-" ? std::cin : file;

  itch::Market market;
  std::array<std::uint64_t, 4> by_status{};
  std::uint64_t messages = 0, applied = 0, malformed = 0, crossed = 0;
  std::optional<std::uint16_t> target;  // The symbol's locate, once its first order appears.
  bool snapshot_done = false;

  const auto start = std::chrono::steady_clock::now();
  itch::FrameReader reader(in);
  while (auto payload = reader.next()) {
    ++messages;
    const itch::DecodeResult result = itch::decode(*payload);
    if (result.status == itch::DecodeStatus::Ignored || result.status == itch::DecodeStatus::Empty) {
      continue;
    }
    if (result.status != itch::DecodeStatus::Ok) {
      ++malformed;
      continue;
    }

    // Copied rather than referenced: it's 16 bytes, and GCC's dangling-reference
    // check can't tell a reference returned through std::visit is safe.
    const itch::Header header =
        std::visit([](const auto& m) { return m.header; }, result.message);

    if (!snapshot_done && header.timestamp >= *snapshot_at) {
      print_snapshot(market, target, symbol, format_time(*snapshot_at));
      snapshot_done = true;
    }

    if (!target) {
      const auto* add = std::get_if<itch::AddOrder>(&result.message);
      if (add != nullptr && add->stock.view() == symbol) target = header.stock_locate;
    }

    ++by_status[static_cast<std::size_t>(market.apply(result.message))];
    ++applied;

    // A healthy book never has its best bid at or above its best ask for long.
    if (target && header.stock_locate == *target) {
      const itch::OrderBook* book = market.book(*target);
      const auto bid = book->best_bid();
      const auto ask = book->best_ask();
      if (bid && ask && bid->price >= ask->price) ++crossed;
    }
  }
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

  if (!snapshot_done) {
    std::printf("\nstream ended before %s\n", format_time(*snapshot_at).c_str());
  }
  print_snapshot(market, target, symbol, "end of day");

  using S = itch::BookStatus;
  const auto count = [&](S s) {
    return static_cast<unsigned long long>(by_status[static_cast<std::size_t>(s)]);
  };
  std::printf("\nmessages          %llu\n", static_cast<unsigned long long>(messages));
  std::printf("book messages     %llu\n", static_cast<unsigned long long>(applied));
  std::printf("elapsed           %.3f s (%.1f M msg/s)\n", elapsed.count(),
              static_cast<double>(messages) / elapsed.count() / 1e6);
  std::printf("instruments       %zu\n", market.instrument_count());
  std::printf("resting orders    %zu (all instruments, end of stream)\n", market.resting_orders());
  std::printf("\nok                %llu\n", count(S::Ok));
  std::printf("unknown order     %llu\n", count(S::UnknownOrder));
  std::printf("duplicate order   %llu\n", count(S::DuplicateOrder));
  std::printf("overfill          %llu\n", count(S::Overfill));
  std::printf("malformed         %llu\n", static_cast<unsigned long long>(malformed));
  std::printf("truncated         %s\n", reader.truncated() ? "yes" : "no");
  std::printf("%.*s crossed      %llu updates with best bid >= best ask\n",
              static_cast<int>(symbol.size()), symbol.data(), static_cast<unsigned long long>(crossed));

  const bool clean = malformed == 0 && !reader.truncated() && count(S::UnknownOrder) == 0 &&
                     count(S::DuplicateOrder) == 0 && count(S::Overfill) == 0;
  return clean ? 0 : 1;
}
