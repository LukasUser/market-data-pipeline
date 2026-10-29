#include "itch/passive_sim.hpp"

#include <algorithm>
#include <utility>
#include <variant>

#include "itch/decoder.hpp"
#include "itch/overloaded.hpp"

namespace itch {

PassiveSim::PassiveSim(std::istream& in, std::string symbol, Timestamp latency_ns)
    : reader_(in), symbol_(std::move(symbol)), latency_(latency_ns) {}

void PassiveSim::place(Side side, Price price, Shares shares) {
  requests_.push_back({now_ + latency_, side, price, shares});
}

std::optional<Quote> PassiveSim::next() {
  while (auto payload = reader_.next()) {
    const DecodeResult result = decode(*payload);
    if (result.status == DecodeStatus::BadLength || result.status == DecodeStatus::BadField) {
      ++malformed_;
      continue;
    }
    if (result.status != DecodeStatus::Ok) continue;

    const Header header = std::visit([](const auto& m) { return m.header; }, result.message);
    if (!locate_) {
      const auto* add = std::get_if<AddOrder>(&result.message);
      if (add == nullptr || add->stock.view() != symbol_) continue;
      locate_ = header.stock_locate;
    }
    if (header.stock_locate != *locate_) continue;

    // Requests that reached the exchange before this event are in place for it.
    now_ = header.timestamp;
    while (!requests_.empty() && requests_.front().arrival < now_) {
      arrive(requests_.front());
      requests_.pop_front();
    }

    before_replay(result.message);
    if (replay(engine_, result.message) != BookStatus::Ok) ++book_errors_;

    Quote quote{now_, engine_.best_bid(), engine_.best_ask()};
    if (!last_ || quote.bid != last_->bid || quote.ask != last_->ask) {
      last_ = quote;
      return quote;
    }
  }
  return std::nullopt;
}

void PassiveSim::arrive(const Request& request) {
  std::optional<WorkingOrder>& current = slot(request.side);
  if (request.shares == 0) {
    current.reset();
    return;
  }

  const bool buy = request.side == Side::Buy;
  const auto opposite = buy ? engine_.best_ask() : engine_.best_bid();
  if (opposite && (buy ? opposite->price <= request.price : opposite->price >= request.price)) {
    ++rejected_;  // would take liquidity, not rest; this simulator only rests
    return;
  }

  if (current && current->price == request.price && request.shares <= current->shares) {
    current->shares = request.shares;  // a size cut keeps the queue position
    return;
  }
  current = WorkingOrder{request.price, request.shares,
                         engine_.shares_at(request.side, request.price)};
}

// Runs before the engine applies `message`, while the orders it names are
// still in the queue.
void PassiveSim::before_replay(const Message& message) {
  std::visit(
      Overloaded{
          [this](const AddOrder& m) {
            // A new order priced through ours would have traded with us.
            if (m.side == Side::Sell) {
              const auto& bid = slot(Side::Buy);
              if (bid && m.price <= bid->price) fill(Side::Buy, m.shares);
            } else {
              const auto& ask = slot(Side::Sell);
              if (ask && m.price >= ask->price) fill(Side::Sell, m.shares);
            }
          },
          [this](const OrderExecuted& m) { executed(m.order_ref, m.executed_shares); },
          [this](const OrderExecutedWithPrice& m) { executed(m.order_ref, m.executed_shares); },
          [this](const OrderCancel& m) { left_queue(m.order_ref, m.cancelled_shares); },
          [this](const OrderDelete& m) {
            if (const auto order = engine_.order(m.order_ref)) left_queue(m.order_ref, order->shares);
          },
          [this](const OrderReplace& m) {
            if (const auto order = engine_.order(m.original_order_ref)) {
              left_queue(m.original_order_ref, order->shares);
            }
          },
      },
      message);
}

// Everything ahead of us is a prefix of the queue totalling `ahead` shares, so
// a real order is ahead of us exactly when fewer shares than that precede it.
bool PassiveSim::ahead_of_us(OrderRef ref, const OrderState& order) const {
  const auto& ours = working_[order.side == Side::Buy ? 0 : 1];
  return ours && order.price == ours->price && *engine_.shares_ahead(ref) < ours->ahead;
}

void PassiveSim::executed(OrderRef ref, Shares shares) {
  const auto order = engine_.order(ref);
  if (!order) return;
  const auto& ours = slot(order->side);
  if (!ours) return;

  if (ahead_of_us(ref, *order)) {
    left_queue(ref, shares);
  } else if (order->price == ours->price ||
             (order->side == Side::Buy ? order->price < ours->price
                                       : order->price > ours->price)) {
    fill(order->side, shares);  // behind us, or at a worse price: we'd have gone first
  }
}

void PassiveSim::left_queue(OrderRef ref, Shares shares) {
  const auto order = engine_.order(ref);
  if (!order || !ahead_of_us(ref, *order)) return;
  auto& ours = slot(order->side);
  ours->ahead -= std::min(ours->ahead, shares);
}

void PassiveSim::fill(Side side, Shares shares) {
  auto& ours = slot(side);
  if (!ours || shares == 0) return;
  const Shares filled = std::min(ours->shares, shares);
  fills_.push_back({now_, side, ours->price, filled});
  ours->shares -= filled;
  if (ours->shares == 0) ours.reset();
}

std::vector<PassiveFill> PassiveSim::take_fills() { return std::exchange(fills_, {}); }

std::optional<WorkingOrder> PassiveSim::working(Side side) const {
  return working_[side == Side::Buy ? 0 : 1];
}

}  // namespace itch
