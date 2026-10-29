#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "itch/frame_reader.hpp"
#include "itch/quote_stream.hpp"
#include "itch/symbol_splitter.hpp"
#include "itch_stream.hpp"

namespace itch {
namespace {

constexpr std::uint16_t kAapl = 7, kMsft = 9, kTsla = 11;

// A small day: three symbols interleaved, plus a non-book message.
std::string day() {
  test::Stream s;
  s.add(kAapl, 1, 1, 'B', 100, "AAPL    ", 1834500)
      .add(kMsft, 2, 2, 'B', 10, "MSFT    ", 1580000)
      .raw(test::Bytes().header('S', 0, 0, 3).ch('O'))  // system event: not a book message
      .add(kAapl, 4, 3, 'S', 200, "AAPL    ", 1835000)
      .execute(kMsft, 5, 2, 10)
      .add(kTsla, 6, 4, 'S', 5, "TSLA    ", 4300000)
      .execute(kAapl, 7, 1, 30)
      .remove(kAapl, 8, 3);
  std::string bytes;
  auto in = s.open();
  bytes.assign(std::istreambuf_iterator<char>(in), {});
  return bytes;
}

std::vector<Quote> quotes(const std::string& bytes, const std::string& symbol) {
  std::istringstream in(bytes);
  QuoteStream stream(in, symbol);
  std::vector<Quote> out;
  while (auto q = stream.next()) out.push_back(*q);
  return out;
}

TEST(SymbolSplitter, EachOutputHoldsOnlyItsSymbolsBookMessages) {
  std::ostringstream aapl, msft;
  SymbolSplitter splitter({{"AAPL", &aapl}, {"MSFT", &msft}});

  std::istringstream in(day());
  FrameReader reader(in);
  while (auto payload = reader.next()) splitter.route(*payload);

  EXPECT_EQ(splitter.written("AAPL"), 4u);
  EXPECT_EQ(splitter.written("MSFT"), 2u);
  EXPECT_EQ(splitter.written("TSLA"), 0u);  // not asked for

  // The split file replays to exactly the same quotes as the whole day.
  EXPECT_EQ(quotes(aapl.str(), "AAPL"), quotes(day(), "AAPL"));
  EXPECT_EQ(quotes(msft.str(), "MSFT"), quotes(day(), "MSFT"));
  EXPECT_EQ(quotes(aapl.str(), "AAPL").size(), 4u);
}

TEST(SymbolSplitter, OutputKeepsTheLengthPrefixedFormat) {
  std::ostringstream msft;
  SymbolSplitter splitter({{"MSFT", &msft}});
  std::istringstream in(day());
  FrameReader reader(in);
  while (auto payload = reader.next()) splitter.route(*payload);

  const std::string bytes = msft.str();
  ASSERT_EQ(bytes.size(), 2u + 36 + 2 + 31);  // one Add (36 bytes), one Execute (31)
  EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 0);
  EXPECT_EQ(static_cast<unsigned char>(bytes[1]), 36);
  EXPECT_EQ(bytes[2], 'A');
}

TEST(SymbolSplitter, IgnoresMessagesForSymbolsNotAsked) {
  std::ostringstream tsla;
  SymbolSplitter splitter({{"TSLA", &tsla}});
  test::Stream s;
  s.execute(kTsla, 1, 99, 10);  // before TSLA's first add: its locate isn't known yet
  auto in = s.open();
  FrameReader reader(in);
  while (auto payload = reader.next()) EXPECT_FALSE(splitter.route(*payload));
  EXPECT_TRUE(tsla.str().empty());
}

}  // namespace
}  // namespace itch
