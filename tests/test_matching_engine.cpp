#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

#include "itch/matching_engine.hpp"

namespace itch {
namespace {

using Levels = std::vector<Level>;
using Refs = std::vector<OrderRef>;
using Trades = std::vector<Trade>;

constexpr Price kBid = 2904500;  // $290.45
constexpr Price kAsk = 2904900;  // $290.49

constexpr OrderRef A = 1, B = 2, C = 3, D = 4, X = 100;

// Design question 1: A, B and C rest at the same price, in that order.
MatchingEngine abc() {
  MatchingEngine engine;
  engine.submit(A, Side::Buy, kBid, 100);
  engine.submit(B, Side::Buy, kBid, 200);
  engine.submit(C, Side::Buy, kBid, 50);
  return engine;
}

TEST(MatchingEngine, OldestOrderAtAPriceFillsFirst) {
  MatchingEngine engine = abc();
  const Submitted sell = engine.submit(X, Side::Sell, kBid, 250);

  EXPECT_EQ(sell.status, BookStatus::Ok);
  EXPECT_EQ(sell.trades, (Trades{{A, X, kBid, 100}, {B, X, kBid, 150}}));
  EXPECT_EQ(engine.queue(Side::Buy, kBid), (Refs{B, C}));
  EXPECT_EQ(engine.best_bid(), (Level{kBid, 100}));  // B's 50 + C's 50
  EXPECT_FALSE(engine.best_ask().has_value());       // the sell filled completely
}

// Design question 2: a new order joins behind everyone already at its price.
TEST(MatchingEngine, NewOrderWaitsBehindTheWholeQueue) {
  MatchingEngine engine = abc();
  engine.submit(D, Side::Buy, kBid, 100);

  EXPECT_EQ(engine.shares_ahead(A), 0u);
  EXPECT_EQ(engine.shares_ahead(D), 350u);
  EXPECT_EQ(engine.queue(Side::Buy, kBid), (Refs{A, B, C, D}));
}

// Design question 3: cancelling from the middle closes the gap.
TEST(MatchingEngine, CancelFromTheMiddleOfTheQueue) {
  MatchingEngine engine = abc();
  EXPECT_TRUE(engine.cancel(B));

  EXPECT_EQ(engine.queue(Side::Buy, kBid), (Refs{A, C}));
  EXPECT_EQ(engine.shares_ahead(C), 100u);
  EXPECT_EQ(engine.best_bid(), (Level{kBid, 150}));
  EXPECT_FALSE(engine.cancel(B));  // already gone
  EXPECT_FALSE(engine.shares_ahead(B).has_value());
}

TEST(MatchingEngine, BetterPricesFillFirstAtTheRestingPrice) {
  MatchingEngine engine;
  engine.submit(A, Side::Sell, kAsk + 200, 100);  // 290.51
  engine.submit(B, Side::Sell, kAsk, 100);        // 290.49, the best ask
  engine.submit(C, Side::Sell, kAsk + 100, 100);  // 290.50

  // Willing to pay up to 290.50: takes 290.49, then 290.50, never 290.51.
  const Submitted buy = engine.submit(X, Side::Buy, kAsk + 100, 300);

  EXPECT_EQ(buy.trades, (Trades{{B, X, kAsk, 100}, {C, X, kAsk + 100, 100}}));
  // The unfilled 100 rests as the new best bid at its limit.
  EXPECT_EQ(engine.best_bid(), (Level{kAsk + 100, 100}));
  EXPECT_EQ(engine.best_ask(), (Level{kAsk + 200, 100}));
}

TEST(MatchingEngine, OrderThatDoesNotCrossJustRests) {
  MatchingEngine engine;
  engine.submit(A, Side::Sell, kAsk, 100);
  const Submitted buy = engine.submit(B, Side::Buy, kBid, 100);

  EXPECT_TRUE(buy.trades.empty());
  EXPECT_EQ(engine.bids(5), (Levels{{kBid, 100}}));
  EXPECT_EQ(engine.asks(5), (Levels{{kAsk, 100}}));
}

TEST(MatchingEngine, PartialCancelKeepsQueuePosition) {
  MatchingEngine engine = abc();
  EXPECT_EQ(engine.reduce(A, 60), BookStatus::Ok);

  EXPECT_EQ(engine.queue(Side::Buy, kBid), (Refs{A, B, C}));
  EXPECT_EQ(engine.shares_ahead(C), 240u);  // A's remaining 40 + B's 200
  EXPECT_EQ(engine.reduce(A, 40), BookStatus::Ok);
  EXPECT_EQ(engine.queue(Side::Buy, kBid), (Refs{B, C}));
  EXPECT_EQ(engine.reduce(A, 1), BookStatus::UnknownOrder);
}

TEST(MatchingEngine, OverfillRemovesTheOrderAndIsReported) {
  MatchingEngine engine = abc();
  EXPECT_EQ(engine.reduce(B, 500), BookStatus::Overfill);
  EXPECT_EQ(engine.queue(Side::Buy, kBid), (Refs{A, C}));
  EXPECT_EQ(engine.best_bid(), (Level{kBid, 150}));
}

TEST(MatchingEngine, DuplicateReferenceIsRejected) {
  MatchingEngine engine = abc();
  const Submitted again = engine.submit(A, Side::Sell, kBid, 100);

  EXPECT_EQ(again.status, BookStatus::DuplicateOrder);
  EXPECT_TRUE(again.trades.empty());
  EXPECT_EQ(engine.order_count(), 3u);
}

TEST(MatchingEngine, ReferenceCanBeReusedOnceTheOrderHasFilled) {
  MatchingEngine engine;
  engine.submit(A, Side::Sell, kAsk, 100);
  engine.submit(X, Side::Buy, kAsk, 100);  // fills A; X never rests

  EXPECT_EQ(engine.order_count(), 0u);
  EXPECT_EQ(engine.submit(X, Side::Buy, kBid, 10).status, BookStatus::Ok);
}

std::uint64_t resting_shares(const MatchingEngine& engine) {
  const auto sum = [](std::uint64_t total, const Level& l) { return total + l.shares; };
  const auto bids = engine.bids(SIZE_MAX);
  const auto asks = engine.asks(SIZE_MAX);
  return std::accumulate(bids.begin(), bids.end(), std::uint64_t{0}, sum) +
         std::accumulate(asks.begin(), asks.end(), std::uint64_t{0}, sum);
}

// Thousands of random orders and cancels around one price. After every step
// the book must not be crossed, and no shares may appear or vanish: every
// share submitted has either traded (taking a resting share with it), been
// cancelled, or is still resting.
TEST(MatchingEngine, RandomOrderFlowKeepsTheBookConsistent) {
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> action(0, 9);
  std::uniform_int_distribution<Price> price(kBid - 500, kAsk + 500);
  std::uniform_int_distribution<Shares> size(1, 500);

  MatchingEngine engine;
  std::vector<OrderRef> submitted;
  std::uint64_t expected_resting = 0;

  for (OrderRef ref = 1; ref <= 20'000; ++ref) {
    if (action(rng) < 3 && !submitted.empty()) {
      const OrderRef victim = submitted[rng() % submitted.size()];
      if (engine.shares_ahead(victim)) {  // still resting
        const std::uint64_t before = resting_shares(engine);
        ASSERT_TRUE(engine.cancel(victim));
        expected_resting = resting_shares(engine);
        ASSERT_LT(expected_resting, before);  // a resting order always holds shares
        ASSERT_FALSE(engine.shares_ahead(victim).has_value());
      } else {
        ASSERT_FALSE(engine.cancel(victim));  // filled or cancelled earlier
      }
      continue;
    }
    const Side side = action(rng) < 5 ? Side::Buy : Side::Sell;
    const Shares shares = size(rng);
    const Submitted result = engine.submit(ref, side, price(rng), shares);
    ASSERT_EQ(result.status, BookStatus::Ok);
    submitted.push_back(ref);

    std::uint64_t traded = 0;
    for (const Trade& t : result.trades) {
      ASSERT_EQ(t.incoming, ref);
      traded += t.shares;
    }
    expected_resting += shares - 2 * traded;  // the unfilled part rests; fills consume resting shares

    ASSERT_EQ(resting_shares(engine), expected_resting) << "after order " << ref;
    const auto bid = engine.best_bid();
    const auto ask = engine.best_ask();
    if (bid && ask) ASSERT_LT(bid->price, ask->price) << "crossed after order " << ref;
  }
  EXPECT_GT(engine.order_count(), 0u);
}

}  // namespace
}  // namespace itch
