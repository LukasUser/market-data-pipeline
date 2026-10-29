#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "bytes.hpp"
#include "itch/quote_stream.hpp"
#include "itch_stream.hpp"

namespace itch {
namespace {

constexpr std::uint16_t kAapl = 7;
constexpr std::uint16_t kMsft = 9;

using test::Stream;

std::vector<Quote> all_quotes(QuoteStream& quotes) {
  std::vector<Quote> out;
  while (auto q = quotes.next()) out.push_back(*q);
  return out;
}

TEST(QuoteStream, EmitsOnlyWhenTheTopOfBookChanges) {
  auto in = Stream()
                .add(kAapl, 100, 1, 'B', 100, "AAPL    ", 1834500)  // new best bid
                .add(kAapl, 200, 2, 'B', 50, "AAPL    ", 1834000)   // behind the best bid: no quote
                .add(kAapl, 300, 3, 'S', 200, "AAPL    ", 1835000)  // new best ask
                .execute(kAapl, 400, 1, 30)                         // best bid size 100 → 70
                .remove(kAapl, 500, 2)                              // behind the best bid: no quote
                .open();
  QuoteStream quotes(in, "AAPL");

  EXPECT_EQ(all_quotes(quotes),
            (std::vector<Quote>{
                {100, Level{1834500, 100}, std::nullopt},
                {300, Level{1834500, 100}, Level{1835000, 200}},
                {400, Level{1834500, 70}, Level{1835000, 200}},
            }));
  EXPECT_EQ(quotes.malformed(), 0u);
  EXPECT_EQ(quotes.book_errors(), 0u);
  EXPECT_FALSE(quotes.truncated());
}

TEST(QuoteStream, IgnoresOtherSymbols) {
  auto in = Stream()
                .add(kMsft, 100, 1, 'B', 10, "MSFT    ", 1580000)
                .add(kAapl, 200, 2, 'S', 20, "AAPL    ", 1835000)
                .add(kMsft, 300, 3, 'S', 30, "MSFT    ", 1590000)
                .remove(kMsft, 400, 1)
                .open();
  QuoteStream quotes(in, "AAPL");

  EXPECT_EQ(all_quotes(quotes), (std::vector<Quote>{{200, std::nullopt, Level{1835000, 20}}}));
  EXPECT_EQ(quotes.book()->order_count(), 1u);
  EXPECT_EQ(quotes.book_errors(), 0u);
}

TEST(QuoteStream, EmptySideAfterTheLastOrderLeaves) {
  auto in = Stream().add(kAapl, 100, 1, 'B', 100, "AAPL    ", 1834500).remove(kAapl, 200, 1).open();
  QuoteStream quotes(in, "AAPL");

  EXPECT_EQ(all_quotes(quotes), (std::vector<Quote>{
                                    {100, Level{1834500, 100}, std::nullopt},
                                    {200, std::nullopt, std::nullopt},
                                }));
}

TEST(QuoteStream, UnknownSymbolYieldsNothing) {
  auto in = Stream().add(kMsft, 100, 1, 'B', 10, "MSFT    ", 1580000).open();
  QuoteStream quotes(in, "AAPL");

  EXPECT_FALSE(quotes.next().has_value());
  EXPECT_EQ(quotes.book(), nullptr);
}

TEST(QuoteStream, CountsMalformedMessagesAndBookErrors) {
  auto in = Stream()
                .add(kAapl, 100, 1, 'B', 100, "AAPL    ", 1834500)
                .raw(test::Bytes().header('D', kAapl, 0, 200))  // Delete with no order_ref
                .remove(kAapl, 300, 42)                         // never added
                .open();
  QuoteStream quotes(in, "AAPL");

  all_quotes(quotes);
  EXPECT_EQ(quotes.malformed(), 1u);
  EXPECT_EQ(quotes.book_errors(), 1u);
}

}  // namespace
}  // namespace itch
