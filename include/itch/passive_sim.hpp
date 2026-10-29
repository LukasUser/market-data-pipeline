#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <istream>
#include <optional>
#include <string>
#include <vector>

#include "itch/frame_reader.hpp"
#include "itch/matching_engine.hpp"
#include "itch/messages.hpp"
#include "itch/quote_stream.hpp"

namespace itch {

// One of our simulated orders, resting in the real queue.
struct WorkingOrder {
  Price price;
  Shares shares;  // Not yet filled.
  Shares ahead;   // Real shares in front of us at our price.

  friend bool operator==(const WorkingOrder&, const WorkingOrder&) = default;
};

struct PassiveFill {
  Timestamp timestamp;
  Side side;
  Price price;
  Shares shares;

  friend bool operator==(const PassiveFill&, const PassiveFill&) = default;
};

// Replays one symbol's ITCH stream and simulates our own resting (limit)
// orders in its real queues: at most one order per side.
//
// An order joins the back of the queue at its price, behind every real share
// already there. Real orders ahead of it that execute or cancel move it
// forward. It fills when the market would have reached it first:
// - a real order behind it at the same price executes,
// - a real order at a worse price on its side executes, or
// - a real order arrives on the other side at a price that crosses it.
//
// Our orders don't affect the market: NASDAQ's trades still happen as reported.
class PassiveSim {
 public:
  PassiveSim(std::istream& in, std::string symbol, Timestamp latency_ns = 0);

  // Advances to the next change in the market's own top of book (our orders
  // aren't part of it). Fills since the last call are in take_fills().
  std::optional<Quote> next();

  // Asks for our order on `side` to become `shares` at `price`; 0 shares
  // cancels it. The request reaches the exchange `latency_ns` after the
  // current time, and until then the old order stays live and can still fill.
  // Same price and no more shares keeps the queue position; anything else
  // rejoins at the back. A request that would cross the market is rejected
  // and the old order stays.
  void place(Side side, Price price, Shares shares);

  std::vector<PassiveFill> take_fills();
  std::optional<WorkingOrder> working(Side side) const;

  Timestamp now() const { return now_; }
  std::uint64_t rejected() const { return rejected_; }
  std::uint64_t malformed() const { return malformed_; }
  std::uint64_t book_errors() const { return book_errors_; }
  bool truncated() const { return reader_.truncated(); }

 private:
  struct Request {
    Timestamp arrival;
    Side side;
    Price price;
    Shares shares;
  };

  std::optional<WorkingOrder>& slot(Side side) { return working_[side == Side::Buy ? 0 : 1]; }
  void arrive(const Request& request);
  void before_replay(const Message& message);
  bool ahead_of_us(OrderRef ref, const OrderState& order) const;
  void executed(OrderRef ref, Shares shares);
  void left_queue(OrderRef ref, Shares shares);
  void fill(Side side, Shares shares);

  FrameReader reader_;
  std::string symbol_;
  Timestamp latency_;
  std::optional<std::uint16_t> locate_;
  MatchingEngine engine_;
  std::optional<Quote> last_;
  Timestamp now_ = 0;
  std::deque<Request> requests_;
  std::array<std::optional<WorkingOrder>, 2> working_;  // [buy, sell]
  std::vector<PassiveFill> fills_;
  std::uint64_t rejected_ = 0;
  std::uint64_t malformed_ = 0;
  std::uint64_t book_errors_ = 0;
};

}  // namespace itch
