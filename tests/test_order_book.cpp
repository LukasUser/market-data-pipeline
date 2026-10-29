#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "itch/market.hpp"
#include "itch/order_book.hpp"

namespace itch {
namespace {

using Levels = std::vector<Level>;

// The six-message example worked through by hand during the design.
TEST(OrderBook, HandWorkedExample) {
  OrderBook book;
  EXPECT_EQ(book.add(1001, Side::Buy, 1834500, 100), BookStatus::Ok);   // A
  EXPECT_EQ(book.add(1002, Side::Sell, 1835000, 200), BookStatus::Ok);  // A
  EXPECT_EQ(book.add(1003, Side::Buy, 1834500, 50), BookStatus::Ok);    // A
  EXPECT_EQ(book.reduce(1001, 30), BookStatus::Ok);                     // E: 30 executed
  EXPECT_EQ(book.reduce(1003, 50), BookStatus::Ok);                     // X: 50 cancelled
  EXPECT_EQ(book.remove(1002), BookStatus::Ok);                         // D

  EXPECT_EQ(book.bids(10), (Levels{{1834500, 70}}));
  EXPECT_TRUE(book.asks(10).empty());
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, EmptyBookHasNoBestPrices) {
  OrderBook book;
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_FALSE(book.best_ask().has_value());
  EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, OrdersAtTheSamePriceAddUp) {
  OrderBook book;
  book.add(1, Side::Buy, 1834500, 100);
  book.add(2, Side::Buy, 1834500, 50);
  EXPECT_EQ(book.best_bid(), (Level{1834500, 150}));
  EXPECT_EQ(book.order_count(), 2u);
}

TEST(OrderBook, BestBidIsHighestAndBestAskIsLowest) {
  OrderBook book;
  book.add(1, Side::Buy, 1834000, 10);
  book.add(2, Side::Buy, 1834500, 20);
  book.add(3, Side::Buy, 1833000, 30);
  book.add(4, Side::Sell, 1836000, 40);
  book.add(5, Side::Sell, 1835000, 50);

  EXPECT_EQ(book.best_bid(), (Level{1834500, 20}));
  EXPECT_EQ(book.best_ask(), (Level{1835000, 50}));
  EXPECT_EQ(book.bids(2), (Levels{{1834500, 20}, {1834000, 10}}));
  EXPECT_EQ(book.asks(5), (Levels{{1835000, 50}, {1836000, 40}}));
}

TEST(OrderBook, LevelDisappearsWhenItsLastOrderLeaves) {
  OrderBook book;
  book.add(1, Side::Buy, 1834500, 100);
  book.add(2, Side::Buy, 1834000, 100);
  book.remove(1);
  EXPECT_EQ(book.best_bid(), (Level{1834000, 100}));
}

TEST(OrderBook, PartialReduceKeepsTheOrder) {
  OrderBook book;
  book.add(1, Side::Sell, 1835000, 100);
  EXPECT_EQ(book.reduce(1, 40), BookStatus::Ok);
  EXPECT_EQ(book.best_ask(), (Level{1835000, 60}));
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, ReplaceKeepsTheSideAndMovesTheOrder) {
  OrderBook book;
  book.add(1, Side::Sell, 1835000, 100);
  EXPECT_EQ(book.replace(1, 2, 1834900, 70), BookStatus::Ok);

  EXPECT_EQ(book.asks(10), (Levels{{1834900, 70}}));
  EXPECT_TRUE(book.bids(10).empty());
  // The old reference is gone; the new one is live.
  EXPECT_EQ(book.remove(1), BookStatus::UnknownOrder);
  EXPECT_EQ(book.remove(2), BookStatus::Ok);
}

TEST(OrderBook, UnknownOrdersAreReportedAndChangeNothing) {
  OrderBook book;
  book.add(1, Side::Buy, 1834500, 100);
  EXPECT_EQ(book.reduce(99, 10), BookStatus::UnknownOrder);
  EXPECT_EQ(book.remove(99), BookStatus::UnknownOrder);
  EXPECT_EQ(book.replace(99, 100, 1834500, 10), BookStatus::UnknownOrder);
  EXPECT_EQ(book.best_bid(), (Level{1834500, 100}));
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, DuplicateAddIsRejected) {
  OrderBook book;
  book.add(1, Side::Buy, 1834500, 100);
  EXPECT_EQ(book.add(1, Side::Sell, 1835000, 5), BookStatus::DuplicateOrder);
  EXPECT_EQ(book.best_bid(), (Level{1834500, 100}));
  EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, ReplaceOntoALiveReferenceIsRejected) {
  OrderBook book;
  book.add(1, Side::Buy, 1834500, 100);
  book.add(2, Side::Buy, 1834000, 100);
  EXPECT_EQ(book.replace(1, 2, 1834600, 10), BookStatus::DuplicateOrder);
  EXPECT_EQ(book.bids(10), (Levels{{1834500, 100}, {1834000, 100}}));
}

TEST(OrderBook, OverfillRemovesTheOrderAndIsReported) {
  OrderBook book;
  book.add(1, Side::Buy, 1834500, 100);
  book.add(2, Side::Buy, 1834500, 50);
  EXPECT_EQ(book.reduce(1, 150), BookStatus::Overfill);
  // Only order 1's 100 shares leave the level; order 2 is untouched.
  EXPECT_EQ(book.best_bid(), (Level{1834500, 50}));
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, ZeroShareAddRestsNothing) {
  OrderBook book;
  EXPECT_EQ(book.add(1, Side::Buy, 1834500, 0), BookStatus::Ok);
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_EQ(book.order_count(), 0u);
}

// --- Market: routing decoded messages to per-instrument books ---

Symbol symbol(std::string_view s) {
  Symbol out{};
  out.chars.fill(' ');
  for (std::size_t i = 0; i < s.size() && i < out.chars.size(); ++i) out.chars[i] = s[i];
  return out;
}

Header at(std::uint16_t locate) { return Header{locate, 0, 0}; }

AddOrder add(std::uint16_t locate, OrderRef ref, Side side, Price price, Shares shares,
             std::string_view stock = "AAPL") {
  return AddOrder{at(locate), ref, side, shares, symbol(stock), price, std::nullopt};
}

TEST(Market, AppliesEveryBookMessageType) {
  Market market;
  EXPECT_EQ(market.apply(add(1, 1, Side::Buy, 1000000, 100)), BookStatus::Ok);
  AddOrder with_mpid = add(1, 2, Side::Sell, 1010000, 300);
  with_mpid.attribution = std::array<char, 4>{'G', 'S', 'C', 'O'};
  EXPECT_EQ(market.apply(with_mpid), BookStatus::Ok);

  // Bid 100 → 60.
  EXPECT_EQ(market.apply(OrderExecuted{at(1), 1, 40, 1}), BookStatus::Ok);
  // Ask 300 → 200, executed at a different price than the order's.
  EXPECT_EQ(market.apply(OrderExecutedWithPrice{at(1), 2, 100, 2, true, 1009900}),
            BookStatus::Ok);
  // Ask 200 → 150.
  EXPECT_EQ(market.apply(OrderCancel{at(1), 2, 50}), BookStatus::Ok);
  // Bid moves to $100.01 with 80 shares under a new reference.
  EXPECT_EQ(market.apply(OrderReplace{at(1), 1, 3, 80, 1000100}), BookStatus::Ok);

  const OrderBook* book = market.book(1);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->best_bid(), (Level{1000100, 80}));
  EXPECT_EQ(book->best_ask(), (Level{1010000, 150}));

  EXPECT_EQ(market.apply(OrderDelete{at(1), 3}), BookStatus::Ok);
  EXPECT_FALSE(book->best_bid().has_value());
}

TEST(Market, RoutesByStockLocate) {
  Market market;
  market.apply(add(1, 10, Side::Buy, 1000000, 100, "AAPL"));
  market.apply(add(2, 20, Side::Buy, 2000000, 100, "MSFT"));
  market.apply(OrderDelete{at(1), 10});

  EXPECT_EQ(market.book(1)->order_count(), 0u);
  EXPECT_EQ(market.book(2)->order_count(), 1u);
  EXPECT_EQ(market.resting_orders(), 1u);
  EXPECT_EQ(market.instrument_count(), 2u);
  EXPECT_EQ(market.book(3), nullptr);
}

TEST(Market, LearnsSymbolsFromAddOrders) {
  Market market;
  market.apply(add(7, 1, Side::Buy, 1000000, 100, "AAPL"));
  market.apply(add(9, 2, Side::Sell, 2000000, 100, "MSFT"));
  EXPECT_EQ(market.locate_of("AAPL"), std::optional<std::uint16_t>{7});
  EXPECT_EQ(market.locate_of("MSFT"), std::optional<std::uint16_t>{9});
  EXPECT_FALSE(market.locate_of("TSLA").has_value());
}

}  // namespace
}  // namespace itch
