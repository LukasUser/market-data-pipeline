#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include "itch/queue_audit.hpp"

namespace itch {
namespace {

constexpr Timestamp kHour = 3600ull * 1'000'000'000;
constexpr Timestamp kPreMarket = 8 * kHour;
constexpr Timestamp kRegular = 12 * kHour;
constexpr Timestamp kAfterHours = 17 * kHour;

constexpr Price kBid = 2904500;
constexpr std::uint16_t kAapl = 1, kMsft = 2;

using P = QueuePosition;

AddOrder add(OrderRef ref, Side side, Price price, Shares shares, std::uint16_t locate = kAapl) {
  return {{locate, 0, kRegular}, ref, side, shares, Symbol{{'X', ' ', ' ', ' ', ' ', ' ', ' ', ' '}}, price, std::nullopt};
}
OrderExecuted executed(OrderRef ref, Shares shares, Timestamp t = kRegular, std::uint16_t locate = kAapl) {
  return {{locate, 0, t}, ref, shares, 1};
}
OrderExecutedWithPrice executed_with_price(OrderRef ref, Shares shares, Timestamp t) {
  return {{kAapl, 0, t}, ref, shares, 1, true, kBid};
}
OrderCancel cancelled(OrderRef ref, Shares shares) { return {{kAapl, 0, kRegular}, ref, shares}; }
OrderDelete deleted(OrderRef ref) { return {{kAapl, 0, kRegular}, ref}; }
OrderReplace replaced(OrderRef old_ref, OrderRef new_ref, Price price, Shares shares) {
  return {{kAapl, 0, kRegular}, old_ref, new_ref, shares, price};
}

std::uint64_t regular_e(const QueueAudit& audit, QueuePosition p) {
  return audit.executions(Session::Regular, Execution::Executed, p);
}

TEST(QueueAudit, ExecutionsAtTheFrontOfTheQueue) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 100));
  audit.apply(add(2, Side::Buy, kBid, 200));

  EXPECT_EQ(audit.apply(executed(1, 100)), P::Front);
  EXPECT_EQ(audit.apply(executed(2, 50)), P::Front);  // 1 has gone, so 2 is now first
  EXPECT_EQ(regular_e(audit, P::Front), 2u);
  EXPECT_EQ(audit.book_errors(), 0u);
}

TEST(QueueAudit, ExecutionBehindOthersAtTheSamePrice) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 100));
  audit.apply(add(2, Side::Buy, kBid, 200));

  EXPECT_EQ(audit.apply(executed(2, 50)), P::BehindInQueue);
  EXPECT_EQ(audit.shares_skipped(Session::Regular, Execution::Executed), 100u);
}

TEST(QueueAudit, ExecutionAtAWorsePriceThanTheBest) {
  QueueAudit audit;
  audit.apply(add(1, Side::Sell, kBid + 100, 100));  // best ask
  audit.apply(add(2, Side::Sell, kBid + 200, 100));

  EXPECT_EQ(audit.apply(executed(2, 100)), P::BehindOnPrice);
}

TEST(QueueAudit, ReplaceGoesToTheBackOfTheQueue) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 100));
  audit.apply(add(2, Side::Buy, kBid, 100));
  EXPECT_EQ(audit.apply(replaced(1, 3, kBid, 100)), std::nullopt);

  EXPECT_EQ(audit.apply(executed(2, 100)), P::Front);
  EXPECT_EQ(audit.apply(executed(3, 100)), P::Front);
  EXPECT_EQ(audit.book_errors(), 0u);
}

TEST(QueueAudit, PartialCancelKeepsQueuePosition) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 100));
  audit.apply(add(2, Side::Buy, kBid, 100));
  audit.apply(cancelled(1, 60));

  EXPECT_EQ(audit.apply(executed(1, 40)), P::Front);
}

TEST(QueueAudit, DeleteRemovesTheOrder) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 100));
  audit.apply(add(2, Side::Buy, kBid, 100));
  audit.apply(deleted(1));

  EXPECT_EQ(audit.apply(executed(2, 100)), P::Front);
  EXPECT_EQ(audit.resting_orders(), 0u);
}

TEST(QueueAudit, UnknownOrderIsReportedAndCounted) {
  QueueAudit audit;
  EXPECT_EQ(audit.apply(executed(99, 100)), P::UnknownOrder);
  EXPECT_EQ(audit.book_errors(), 1u);
}

TEST(QueueAudit, CountsBySessionAndMessageType) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 300));
  audit.apply(executed(1, 100, kPreMarket));
  audit.apply(executed_with_price(1, 100, kRegular));
  audit.apply(executed(1, 100, kAfterHours));

  EXPECT_EQ(audit.executions(Session::PreMarket, Execution::Executed, P::Front), 1u);
  EXPECT_EQ(audit.executions(Session::Regular, Execution::ExecutedWithPrice, P::Front), 1u);
  EXPECT_EQ(audit.executions(Session::AfterHours, Execution::Executed, P::Front), 1u);
  EXPECT_EQ(regular_e(audit, P::Front), 0u);
}

TEST(QueueAudit, InstrumentsHaveSeparateQueues) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 100, kAapl));
  audit.apply(add(2, Side::Buy, kBid, 100, kMsft));

  EXPECT_EQ(audit.apply(executed(2, 100, kRegular, kMsft)), P::Front);
}

// NASDAQ sometimes accepts orders that cross the book (halts, auctions). The
// replay must rest them as reported, not trade them itself, or our book
// drifts from NASDAQ's and later messages refer to orders we've removed.
TEST(QueueAudit, CrossingAddIsCountedButNotMatched) {
  QueueAudit audit;
  audit.apply(add(1, Side::Buy, kBid, 100));
  EXPECT_EQ(audit.apply(add(2, Side::Sell, kBid, 100)), std::nullopt);

  EXPECT_EQ(audit.crossing_orders(), 1u);
  EXPECT_EQ(audit.resting_orders(), 2u);
  EXPECT_EQ(audit.apply(executed(1, 100)), P::Front);  // NASDAQ's trade, reported later
  EXPECT_EQ(audit.book_errors(), 0u);
}

}  // namespace
}  // namespace itch
