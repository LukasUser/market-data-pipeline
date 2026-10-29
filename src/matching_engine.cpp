#include "itch/matching_engine.hpp"

#include <algorithm>
#include <iterator>

namespace itch {

namespace {

template <typename Levels>
std::optional<Level> best(const Levels& levels) {
  if (levels.empty()) return std::nullopt;
  return Level{levels.begin()->first, levels.begin()->second.total};
}

template <typename Levels>
std::vector<Level> top(const Levels& levels, std::size_t depth) {
  std::vector<Level> out;
  for (auto it = levels.begin(); it != levels.end() && out.size() < depth; ++it) {
    out.push_back({it->first, it->second.total});
  }
  return out;
}

}  // namespace

template <typename Self>
auto& MatchingEngine::level_of(Self& self, const Locator& where) {
  return where.side == Side::Buy ? self.bids_.at(where.price) : self.asks_.at(where.price);
}

Submitted MatchingEngine::submit(OrderRef ref, Side side, Price limit, Shares shares) {
  if (index_.contains(ref)) return {BookStatus::DuplicateOrder, {}};

  Submitted result{BookStatus::Ok, {}};
  Shares remaining = shares;
  if (side == Side::Buy) {
    match(asks_, ref, remaining, [limit](Price ask) { return ask <= limit; }, result.trades);
  } else {
    match(bids_, ref, remaining, [limit](Price bid) { return bid >= limit; }, result.trades);
  }
  // What's left rests at the back of the queue at its limit price.
  if (remaining > 0) add(ref, side, limit, remaining);
  return result;
}

BookStatus MatchingEngine::add(OrderRef ref, Side side, Price price, Shares shares) {
  if (index_.contains(ref)) return BookStatus::DuplicateOrder;
  if (shares == 0) return BookStatus::Ok;  // nothing to rest, as in OrderBook

  const auto rest = [&](auto& levels) {
    PriceLevel& level = levels[price];
    level.orders.push_back({ref, shares});
    level.total += shares;
    index_.emplace(ref, Locator{side, price, std::prev(level.orders.end())});
  };
  if (side == Side::Buy) {
    rest(bids_);
  } else {
    rest(asks_);
  }
  return BookStatus::Ok;
}

// Trades `remaining` shares of an incoming order against the opposite side:
// best price first, front of the queue first, at the resting order's price.
template <typename Levels, typename Crosses>
void MatchingEngine::match(Levels& opposite, OrderRef incoming, Shares& remaining,
                           Crosses crosses, std::vector<Trade>& trades) {
  while (remaining > 0 && !opposite.empty() && crosses(opposite.begin()->first)) {
    const auto level_it = opposite.begin();
    PriceLevel& level = level_it->second;

    while (remaining > 0 && !level.orders.empty()) {
      Resting& front = level.orders.front();
      const Shares fill = std::min(remaining, front.shares);
      trades.push_back({front.ref, incoming, level_it->first, fill});
      front.shares -= fill;
      level.total -= fill;
      remaining -= fill;
      if (front.shares == 0) {
        index_.erase(front.ref);
        level.orders.pop_front();
      }
    }
    if (level.orders.empty()) opposite.erase(level_it);
  }
}

bool MatchingEngine::cancel(OrderRef ref) {
  const auto it = index_.find(ref);
  if (it == index_.end()) return false;
  erase(it->second);
  index_.erase(it);
  return true;
}

BookStatus MatchingEngine::reduce(OrderRef ref, Shares shares) {
  const auto it = index_.find(ref);
  if (it == index_.end()) return BookStatus::UnknownOrder;

  Resting& order = *it->second.position;
  if (shares < order.shares) {
    order.shares -= shares;  // same list node, so the queue position is kept
    level_of(*this, it->second).total -= shares;
    return BookStatus::Ok;
  }
  const BookStatus status = shares == order.shares ? BookStatus::Ok : BookStatus::Overfill;
  erase(it->second);
  index_.erase(it);
  return status;
}

void MatchingEngine::erase(const Locator& where) {
  const auto remove = [&](auto& levels) {
    const auto level_it = levels.find(where.price);
    PriceLevel& level = level_it->second;
    level.total -= where.position->shares;
    level.orders.erase(where.position);  // O(1): unlink one node
    if (level.orders.empty()) levels.erase(level_it);
  };
  if (where.side == Side::Buy) {
    remove(bids_);
  } else {
    remove(asks_);
  }
}

std::optional<OrderState> MatchingEngine::order(OrderRef ref) const {
  const auto it = index_.find(ref);
  if (it == index_.end()) return std::nullopt;
  const Locator& where = it->second;
  return OrderState{where.side, where.price, where.position->shares};
}

std::optional<Shares> MatchingEngine::shares_ahead(OrderRef ref) const {
  const auto it = index_.find(ref);
  if (it == index_.end()) return std::nullopt;

  const auto& orders = level_of(*this, it->second).orders;
  Shares ahead = 0;
  for (auto o = orders.begin(); o != it->second.position; ++o) ahead += o->shares;
  return ahead;
}

std::vector<OrderRef> MatchingEngine::queue(Side side, Price price) const {
  std::vector<OrderRef> refs;
  const auto collect = [&](const auto& levels) {
    const auto level_it = levels.find(price);
    if (level_it == levels.end()) return;
    for (const Resting& o : level_it->second.orders) refs.push_back(o.ref);
  };
  if (side == Side::Buy) {
    collect(bids_);
  } else {
    collect(asks_);
  }
  return refs;
}

std::optional<Level> MatchingEngine::best_bid() const { return best(bids_); }
std::optional<Level> MatchingEngine::best_ask() const { return best(asks_); }
std::vector<Level> MatchingEngine::bids(std::size_t depth) const { return top(bids_, depth); }
std::vector<Level> MatchingEngine::asks(std::size_t depth) const { return top(asks_, depth); }

}  // namespace itch
