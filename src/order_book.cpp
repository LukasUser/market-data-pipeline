#include "itch/order_book.hpp"

#include <cassert>

namespace itch {
namespace {

// The bid and ask maps sort differently, so these helpers are templates that
// work on either.

template <typename Levels>
void add_shares(Levels& levels, Price price, Shares shares) {
  levels[price] += shares;
}

template <typename Levels>
void remove_shares(Levels& levels, Price price, Shares shares) {
  const auto it = levels.find(price);
  // Invariant: a level holds at least the shares of every order resting at it.
  assert(it != levels.end() && it->second >= shares);
  it->second -= shares;
  if (it->second == 0) levels.erase(it);
}

template <typename Levels>
std::optional<Level> best(const Levels& levels) {
  if (levels.empty()) return std::nullopt;
  return Level{levels.begin()->first, levels.begin()->second};
}

template <typename Levels>
std::vector<Level> top(const Levels& levels, std::size_t depth) {
  std::vector<Level> out;
  for (auto it = levels.begin(); it != levels.end() && out.size() < depth; ++it) {
    out.push_back(Level{it->first, it->second});
  }
  return out;
}

}  // namespace

BookStatus OrderBook::add(OrderRef ref, Side side, Price price, Shares shares) {
  if (orders_.contains(ref)) return BookStatus::DuplicateOrder;
  // A zero-share order would leave an empty price level behind.
  if (shares == 0) return BookStatus::Ok;

  orders_.emplace(ref, Order{side, price, shares});
  add_to_level(side, price, shares);
  return BookStatus::Ok;
}

BookStatus OrderBook::reduce(OrderRef ref, Shares shares) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) return BookStatus::UnknownOrder;

  Order& order = it->second;
  const bool overfill = shares > order.shares;
  const Shares removed = overfill ? order.shares : shares;

  remove_from_level(order.side, order.price, removed);
  order.shares -= removed;
  if (order.shares == 0) orders_.erase(it);
  return overfill ? BookStatus::Overfill : BookStatus::Ok;
}

BookStatus OrderBook::remove(OrderRef ref) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) return BookStatus::UnknownOrder;

  const Order& order = it->second;
  remove_from_level(order.side, order.price, order.shares);
  orders_.erase(it);
  return BookStatus::Ok;
}

BookStatus OrderBook::replace(OrderRef old_ref, OrderRef new_ref, Price price, Shares shares) {
  const auto it = orders_.find(old_ref);
  if (it == orders_.end()) return BookStatus::UnknownOrder;
  // Check before touching anything, so a rejected replace leaves the book as it was.
  if (new_ref != old_ref && orders_.contains(new_ref)) return BookStatus::DuplicateOrder;

  const Side side = it->second.side;
  remove(old_ref);
  return add(new_ref, side, price, shares);
}

std::optional<Level> OrderBook::best_bid() const { return best(bids_); }
std::optional<Level> OrderBook::best_ask() const { return best(asks_); }

std::vector<Level> OrderBook::bids(std::size_t depth) const { return top(bids_, depth); }
std::vector<Level> OrderBook::asks(std::size_t depth) const { return top(asks_, depth); }

void OrderBook::add_to_level(Side side, Price price, Shares shares) {
  if (side == Side::Buy) {
    add_shares(bids_, price, shares);
  } else {
    add_shares(asks_, price, shares);
  }
}

void OrderBook::remove_from_level(Side side, Price price, Shares shares) {
  if (side == Side::Buy) {
    remove_shares(bids_, price, shares);
  } else {
    remove_shares(asks_, price, shares);
  }
}

}  // namespace itch
