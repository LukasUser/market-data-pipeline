#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "itch/passive_sim.hpp"
#include "itch_stream.hpp"

namespace itch {
namespace {

constexpr std::uint16_t kAapl = 7;
constexpr Price kBid = 1834500;  // $183.45
constexpr Price kAsk = 1834600;  // $183.46
constexpr const char* kStock = "AAPL    ";

using Fills = std::vector<PassiveFill>;

// The real book before we act: bids 1 (100) then 2 (200) at 183.45, and 3
// (100) offered at 183.46. Messages at t = 1, 2, 3.
test::Stream book() {
  test::Stream s;
  s.add(kAapl, 1, 1, 'B', 100, kStock, kBid);
  s.add(kAapl, 2, 2, 'B', 200, kStock, kBid);
  s.add(kAapl, 3, 3, 'S', 100, kStock, kAsk);
  return s;
}

// Reads the three setup quotes, so `now()` is 3 and the book is in place.
void start(PassiveSim& sim) {
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(sim.next().has_value());
}

void drain(PassiveSim& sim) {
  while (sim.next()) {
  }
}

TEST(PassiveSim, JoinsBehindEveryShareAlreadyAtItsPrice) {
  auto in = book().add(kAapl, 10, 4, 'B', 50, kStock, kBid).open();  // arrives after us
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Buy, kBid, 100);
  drain(sim);

  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 100, 300}));
  EXPECT_TRUE(sim.take_fills().empty());
}

TEST(PassiveSim, ExecutionsAheadMoveUsUpAndExecutionsBehindFillUs) {
  auto in = book()
                .add(kAapl, 10, 4, 'B', 50, kStock, kBid)  // behind us
                .execute(kAapl, 11, 1, 100)                // ahead: 300 → 200
                .execute(kAapl, 12, 2, 150)                // ahead: 200 → 50
                .execute(kAapl, 13, 2, 50)                 // ahead: 50 → 0
                .execute(kAapl, 14, 4, 30)                 // behind us: we'd have filled first
                .open();
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Buy, kBid, 100);
  drain(sim);

  EXPECT_EQ(sim.take_fills(), (Fills{{14, Side::Buy, kBid, 30}}));
  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 70, 0}));
  EXPECT_TRUE(sim.take_fills().empty());  // taking them clears them
}

TEST(PassiveSim, CancelsAheadMoveUsUpAndReplacesGoBehindUs) {
  auto in = book()
                .cancel(kAapl, 10, 1, 40)               // ahead: 300 → 260
                .remove(kAapl, 11, 2)                   // ahead: 260 → 60
                .replace(kAapl, 12, 1, 5, 60, kBid)     // 1 leaves (→ 0); 5 rejoins behind us
                .execute(kAapl, 13, 5, 60)              // behind us
                .open();
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Buy, kBid, 100);
  drain(sim);

  EXPECT_EQ(sim.take_fills(), (Fills{{13, Side::Buy, kBid, 60}}));
  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 40, 0}));
}

TEST(PassiveSim, ExecutionAtAWorsePriceFillsUs) {
  auto in = book()
                .add(kAapl, 10, 4, 'B', 500, kStock, kBid - 100)
                .execute(kAapl, 11, 4, 500)  // a seller reached 183.44, past our 183.45
                .open();
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Buy, kBid, 100);
  drain(sim);

  EXPECT_EQ(sim.take_fills(), (Fills{{11, Side::Buy, kBid, 100}}));
  EXPECT_FALSE(sim.working(Side::Buy).has_value());
}

TEST(PassiveSim, ImprovingThePriceGoesStraightToTheFront) {
  auto in = book()
                .add(kAapl, 10, 9, 'S', 10, kStock, kAsk + 500)  // lets our request arrive
                .add(kAapl, 11, 4, 'S', 100, kStock, kBid + 50)  // priced through our bid
                .open();
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Buy, kBid + 50, 100);  // 183.455: better than the 183.45 bid
  drain(sim);

  EXPECT_EQ(sim.take_fills(), (Fills{{11, Side::Buy, kBid + 50, 100}}));
}

TEST(PassiveSim, OrderIsNotLiveUntilItArrives) {
  auto in = book()
                .execute(kAapl, 500, 1, 100)   // before our order arrives at 1003
                .execute(kAapl, 2000, 2, 50)   // after: ahead 200 → 150
                .open();
  PassiveSim sim(in, "AAPL", 1000);
  start(sim);
  sim.place(Side::Buy, kBid, 100);
  drain(sim);

  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 100, 150}));
}

TEST(PassiveSim, SizeCutKeepsQueuePositionAndSizeIncreaseLosesIt) {
  // Each message changes the top of book, so next() stops after each one and
  // the following request arrives with the next message.
  auto in = book()
                .execute(kAapl, 10, 1, 100)                    // ahead 300 → 200
                .add(kAapl, 11, 4, 'B', 50, kStock, kBid)      // behind us
                .add(kAapl, 12, 9, 'S', 10, kStock, kAsk - 10)
                .open();
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Buy, kBid, 100);
  ASSERT_TRUE(sim.next().has_value());  // t=10
  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 100, 200}));

  sim.place(Side::Buy, kBid, 60);
  ASSERT_TRUE(sim.next().has_value());  // t=11
  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 60, 200}));

  sim.place(Side::Buy, kBid, 80);  // bigger: back of the queue, behind order 4
  ASSERT_TRUE(sim.next().has_value());  // t=12
  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 80, 250}));
}

TEST(PassiveSim, CrossingRequestIsRejectedAndOldOrderStays) {
  auto in = book()
                .add(kAapl, 10, 8, 'S', 10, kStock, kAsk + 900)
                .add(kAapl, 11, 9, 'S', 10, kStock, kAsk + 800)
                .open();
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Buy, kBid, 100);
  sim.place(Side::Buy, kAsk, 100);  // at the ask: would trade, not rest
  drain(sim);

  EXPECT_EQ(sim.rejected(), 1u);
  EXPECT_EQ(sim.working(Side::Buy), (WorkingOrder{kBid, 100, 300}));
}

TEST(PassiveSim, ZeroSharesCancels) {
  auto in = book()
                .add(kAapl, 10, 8, 'S', 10, kStock, kAsk + 900)
                .open();
  PassiveSim sim(in, "AAPL");
  start(sim);
  sim.place(Side::Sell, kAsk, 100);
  sim.place(Side::Sell, kAsk, 0);
  drain(sim);

  EXPECT_FALSE(sim.working(Side::Sell).has_value());
  EXPECT_EQ(sim.book_errors(), 0u);
}

}  // namespace
}  // namespace itch
