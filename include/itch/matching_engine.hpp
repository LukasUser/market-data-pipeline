#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "itch/messages.hpp"
#include "itch/order_book.hpp"

namespace itch {

struct Trade {
  OrderRef resting;   // The order that was already in the book.
  OrderRef incoming;  // The order that crossed the spread to trade with it.
  Price price;        // Always the resting order's price.
  Shares shares;

  friend bool operator==(const Trade&, const Trade&) = default;
};

struct OrderState {
  Side side;
  Price price;
  Shares shares;  // Still resting.

  friend bool operator==(const OrderState&, const OrderState&) = default;
};

struct Submitted {
  BookStatus status;          // Ok, or DuplicateOrder (nothing traded or rested).
  std::vector<Trade> trades;  // In the order they happened.
};

// A limit order book that matches orders with price-time priority: an
// incoming order trades with the best price on the other side first and, at
// the same price, with the order that has waited longest. Whatever doesn't
// trade rests at its limit price, behind every order already there.
//
// Unlike OrderBook, which keeps only the total shares at each price, each
// level here keeps its orders in arrival order, so queue position is known.
class MatchingEngine {
 public:
  Submitted submit(OrderRef ref, Side side, Price limit, Shares shares);

  // Rests an order at the back of its price's queue without matching it, for
  // replaying a feed where the exchange has already decided what trades. The
  // book can then be crossed (e.g. during a halt or before an auction).
  BookStatus add(OrderRef ref, Side side, Price price, Shares shares);

  // Removes a resting order. False if it isn't resting.
  bool cancel(OrderRef ref);

  // Takes shares off a resting order without losing its place in the queue
  // (an ITCH partial cancel). The order leaves the book at zero.
  BookStatus reduce(OrderRef ref, Shares shares);

  // A resting order's side, price and remaining shares, or nullopt.
  std::optional<OrderState> order(OrderRef ref) const;

  // Shares resting ahead of an order at its price, or nullopt if it isn't
  // resting. These must all trade or cancel before it can fill.
  std::optional<Shares> shares_ahead(OrderRef ref) const;

  // Order references at one price, front of the queue first.
  std::vector<OrderRef> queue(Side side, Price price) const;

  std::optional<Level> best_bid() const;
  std::optional<Level> best_ask() const;
  std::vector<Level> bids(std::size_t depth) const;
  std::vector<Level> asks(std::size_t depth) const;

  std::size_t order_count() const { return index_.size(); }

 private:
  struct Resting {
    OrderRef ref;
    Shares shares;
  };

  // A std::list never moves its elements, so an iterator into it stays valid
  // until that element is erased, and erasing from the middle is O(1).
  struct PriceLevel {
    std::list<Resting> orders;  // Front = oldest = first to fill.
    Shares total = 0;
  };

  struct Locator {
    Side side;
    Price price;
    std::list<Resting>::iterator position;
  };

  using Bids = std::map<Price, PriceLevel, std::greater<>>;
  using Asks = std::map<Price, PriceLevel, std::less<>>;

  template <typename Levels, typename Crosses>
  void match(Levels& opposite, OrderRef incoming, Shares& remaining, Crosses crosses,
             std::vector<Trade>& trades);

  // The level an order rests in. A template so that one body serves both
  // const and non-const callers.
  template <typename Self>
  static auto& level_of(Self& self, const Locator& where);

  void erase(const Locator& where);

  std::unordered_map<OrderRef, Locator> index_;
  Bids bids_;
  Asks asks_;
};

}  // namespace itch
