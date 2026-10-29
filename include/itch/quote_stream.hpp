#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <string>

#include "itch/frame_reader.hpp"
#include "itch/market.hpp"
#include "itch/messages.hpp"
#include "itch/order_book.hpp"

namespace itch {

// One symbol's top of book at a moment in time. A side with no resting
// orders is nullopt.
struct Quote {
  Timestamp timestamp;
  std::optional<Level> bid;
  std::optional<Level> ask;

  friend bool operator==(const Quote&, const Quote&) = default;
};

// Replays an ITCH stream and yields a Quote each time one symbol's best bid or
// best ask (price or size) changes. This is the input a strategy sees.
//
// Only that symbol's messages are applied: its locate code is learned from its
// first Add Order, and nothing earlier can belong to it.
class QuoteStream {
 public:
  QuoteStream(std::istream& in, std::string symbol);

  // The next change to the top of book, or nullopt at end of stream.
  std::optional<Quote> next();

  // The symbol's full book as of the last quote returned.
  const OrderBook* book() const;

  // Problems seen so far. A clean replay has none of them.
  std::uint64_t malformed() const { return malformed_; }
  std::uint64_t book_errors() const { return book_errors_; }
  bool truncated() const { return reader_.truncated(); }

 private:
  FrameReader reader_;
  std::string symbol_;
  std::optional<std::uint16_t> locate_;
  Market market_;
  std::optional<Quote> last_;
  std::uint64_t malformed_ = 0;
  std::uint64_t book_errors_ = 0;
};

}  // namespace itch
