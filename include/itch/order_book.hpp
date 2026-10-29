#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "itch/messages.hpp"

namespace itch {

// One price level: the total shares resting at a price.
struct Level {
  Price price;
  Shares shares;

  friend bool operator==(const Level&, const Level&) = default;
};

enum class BookStatus {
  Ok,
  UnknownOrder,    // The message referenced an order_ref the book doesn't hold.
  DuplicateOrder,  // An add reused an order_ref that is still resting.
  Overfill,        // More shares were removed than were resting; the order is gone.
};

// Limit order book for a single instrument, built from ITCH order events.
//
// Two views of the same orders are kept in step:
// - every resting order by its reference, because Execute, Cancel, Delete and
//   Replace messages carry only the reference, and
// - the total shares at each price on each side, sorted so the best price is
//   always first.
class OrderBook {
 public:
  BookStatus add(OrderRef ref, Side side, Price price, Shares shares);

  // Takes shares off a resting order (an execution or a partial cancel). The
  // order leaves the book when it reaches zero.
  BookStatus reduce(OrderRef ref, Shares shares);

  BookStatus remove(OrderRef ref);

  // Swaps an order for a new reference, price and size. The side carries
  // over, since ITCH's Replace message doesn't repeat it.
  BookStatus replace(OrderRef old_ref, OrderRef new_ref, Price price, Shares shares);

  std::optional<Level> best_bid() const;
  std::optional<Level> best_ask() const;

  // Up to `depth` levels, best price first.
  std::vector<Level> bids(std::size_t depth) const;
  std::vector<Level> asks(std::size_t depth) const;

  std::size_t order_count() const { return orders_.size(); }

 private:
  struct Order {
    Side side;
    Price price;
    Shares shares;
  };

  void add_to_level(Side side, Price price, Shares shares);
  void remove_from_level(Side side, Price price, Shares shares);

  std::unordered_map<OrderRef, Order> orders_;

  // Bids sort highest-first and asks lowest-first, so begin() is always the
  // best price on either side.
  std::map<Price, Shares, std::greater<>> bids_;
  std::map<Price, Shares, std::less<>> asks_;
};

}  // namespace itch
