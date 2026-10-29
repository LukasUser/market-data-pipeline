#include "itch/queue_audit.hpp"

#include <variant>

#include "itch/overloaded.hpp"

namespace itch {
namespace {

constexpr Timestamp kNanosPerSecond = 1'000'000'000;
constexpr Timestamp kOpen = (9 * 3600 + 30 * 60) * kNanosPerSecond;
constexpr Timestamp kClose = 16 * 3600 * kNanosPerSecond;

Session session_at(Timestamp t) {
  if (t < kOpen) return Session::PreMarket;
  return t < kClose ? Session::Regular : Session::AfterHours;
}

constexpr std::size_t index(auto e) { return static_cast<std::size_t>(e); }

}  // namespace

std::optional<QueuePosition> QueueAudit::apply(const Message& message) {
  return std::visit(
      Overloaded{
          [this](const AddOrder& m) -> std::optional<QueuePosition> {
            submit(engines_[m.header.stock_locate], m.order_ref, m.side, m.price, m.shares);
            return std::nullopt;
          },
          [this](const OrderExecuted& m) {
            return execute(m.header.stock_locate, m.header.timestamp, m.order_ref,
                           m.executed_shares, Execution::Executed);
          },
          [this](const OrderExecutedWithPrice& m) {
            return execute(m.header.stock_locate, m.header.timestamp, m.order_ref,
                           m.executed_shares, Execution::ExecutedWithPrice);
          },
          [this](const OrderCancel& m) -> std::optional<QueuePosition> {
            // A partial cancel keeps the order's place in the queue.
            if (engines_[m.header.stock_locate].reduce(m.order_ref, m.cancelled_shares) !=
                BookStatus::Ok) {
              ++book_errors_;
            }
            return std::nullopt;
          },
          [this](const OrderDelete& m) -> std::optional<QueuePosition> {
            if (!engines_[m.header.stock_locate].cancel(m.order_ref)) ++book_errors_;
            return std::nullopt;
          },
          [this](const OrderReplace& m) -> std::optional<QueuePosition> {
            // A replace loses priority: the new order joins the back of the
            // queue at its (possibly new) price, on the original's side.
            MatchingEngine& engine = engines_[m.header.stock_locate];
            const auto original = engine.order(m.original_order_ref);
            if (!original) {
              ++book_errors_;
              return std::nullopt;
            }
            engine.cancel(m.original_order_ref);
            submit(engine, m.new_order_ref, original->side, m.price, m.shares);
            return std::nullopt;
          },
      },
      message);
}

std::optional<QueuePosition> QueueAudit::execute(std::uint16_t locate, Timestamp t, OrderRef ref,
                                                 Shares shares, Execution type) {
  MatchingEngine& engine = engines_[locate];
  const QueuePosition position = position_of(engine, ref);

  const std::size_t s = index(session_at(t)), e = index(type);
  ++counts_[s][e][index(position)];
  if (position == QueuePosition::BehindInQueue) skipped_[s][e] += *engine.shares_ahead(ref);

  if (engine.reduce(ref, shares) != BookStatus::Ok) ++book_errors_;
  return position;
}

QueuePosition QueueAudit::position_of(const MatchingEngine& engine, OrderRef ref) const {
  const auto order = engine.order(ref);
  if (!order) return QueuePosition::UnknownOrder;
  const auto best = order->side == Side::Buy ? engine.best_bid() : engine.best_ask();
  if (best->price != order->price) return QueuePosition::BehindOnPrice;
  return engine.shares_ahead(ref) == 0u ? QueuePosition::Front : QueuePosition::BehindInQueue;
}

// Rests the order exactly as NASDAQ reported it. NASDAQ decides what trades
// and reports it as executions, so the engine must not match the order itself,
// even if it arrives crossing the book.
void QueueAudit::submit(MatchingEngine& engine, OrderRef ref, Side side, Price price,
                        Shares shares) {
  const auto opposite = side == Side::Buy ? engine.best_ask() : engine.best_bid();
  if (opposite && (side == Side::Buy ? opposite->price <= price : opposite->price >= price)) {
    ++crossing_orders_;
  }
  if (engine.add(ref, side, price, shares) != BookStatus::Ok) ++book_errors_;
}

std::uint64_t QueueAudit::executions(Session session, Execution type,
                                     QueuePosition position) const {
  return counts_[index(session)][index(type)][index(position)];
}

std::uint64_t QueueAudit::shares_skipped(Session session, Execution type) const {
  return skipped_[index(session)][index(type)];
}

std::size_t QueueAudit::resting_orders() const {
  std::size_t total = 0;
  for (const auto& [locate, engine] : engines_) total += engine.order_count();
  return total;
}

}  // namespace itch
