#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "bytes.hpp"
#include "itch/decoder.hpp"

namespace itch {
namespace {

using test::Bytes;

template <typename T>
T decode_as(const std::vector<std::uint8_t>& payload) {
  const DecodeResult r = decode(payload);
  EXPECT_EQ(r.status, DecodeStatus::Ok);
  EXPECT_TRUE(std::holds_alternative<T>(r.message));
  return std::get<T>(r.message);
}

// Written out byte-by-byte from the spec rather than with the builder, so this
// test can't share a mistake with the test helper.
TEST(Decoder, AddOrderFromLiteralBytes) {
  const std::vector<std::uint8_t> payload = {
      'A',                                     // message type
      0x00, 0x01,                              // stock locate = 1
      0x00, 0x02,                              // tracking number = 2
      0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00,      // timestamp = 34'200'000'000'000 ns (09:30:00)
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,  // order ref = 42
      'B',                                     // side
      0x00, 0x00, 0x00, 0x64,                  // shares = 100
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',  // stock
      0x00, 0x1B, 0xFE, 0x04,                  // price = 1'834'500 ($183.45)
  };
  ASSERT_EQ(payload.size(), 36u);

  const auto m = decode_as<AddOrder>(payload);
  EXPECT_EQ(m.header.stock_locate, 1);
  EXPECT_EQ(m.header.tracking_number, 2);
  EXPECT_EQ(m.header.timestamp, 34'200'000'000'000u);
  EXPECT_EQ(m.order_ref, 42u);
  EXPECT_EQ(m.side, Side::Buy);
  EXPECT_EQ(m.shares, 100u);
  EXPECT_EQ(m.stock.view(), "AAPL");
  EXPECT_EQ(m.price, 1'834'500u);
  EXPECT_FALSE(m.attribution.has_value());
}

TEST(Decoder, AddOrderWithMpidAttribution) {
  const auto payload = Bytes{}
                           .header('F', 7, 0, 1000)
                           .u64(99)
                           .ch('S')
                           .u32(250)
                           .str("MSFT    ")
                           .u32(4'201'000)
                           .str("GSCO")
                           .data();
  ASSERT_EQ(payload.size(), 40u);

  const auto m = decode_as<AddOrder>(payload);
  EXPECT_EQ(m.header.stock_locate, 7);
  EXPECT_EQ(m.order_ref, 99u);
  EXPECT_EQ(m.side, Side::Sell);
  EXPECT_EQ(m.shares, 250u);
  EXPECT_EQ(m.stock.view(), "MSFT");
  EXPECT_EQ(m.price, 4'201'000u);
  ASSERT_TRUE(m.attribution.has_value());
  EXPECT_EQ(std::string_view(m.attribution->data(), 4), "GSCO");
}

TEST(Decoder, FullWidthFieldsUseAllBytes) {
  // Max values catch shifts that drop the high bytes.
  const auto payload = Bytes{}
                           .header('A', 0xFFFF, 0xFFFF, 0xFFFF'FFFF'FFFF)
                           .u64(0xFFFF'FFFF'FFFF'FFFF)
                           .ch('B')
                           .u32(0xFFFF'FFFF)
                           .str("ABCDEFGH")
                           .u32(0xFFFF'FFFF)
                           .data();

  const auto m = decode_as<AddOrder>(payload);
  EXPECT_EQ(m.header.stock_locate, 0xFFFF);
  EXPECT_EQ(m.header.tracking_number, 0xFFFF);
  EXPECT_EQ(m.header.timestamp, 0xFFFF'FFFF'FFFFu);
  EXPECT_EQ(m.order_ref, 0xFFFF'FFFF'FFFF'FFFFu);
  EXPECT_EQ(m.shares, 0xFFFF'FFFFu);
  EXPECT_EQ(m.stock.view(), "ABCDEFGH");
  EXPECT_EQ(m.price, 0xFFFF'FFFFu);
}

TEST(Decoder, OrderExecuted) {
  const auto payload = Bytes{}.header('E', 3, 4, 5).u64(42).u32(30).u64(123456).data();
  ASSERT_EQ(payload.size(), 31u);

  const auto m = decode_as<OrderExecuted>(payload);
  EXPECT_EQ(m.header, (Header{3, 4, 5}));
  EXPECT_EQ(m.order_ref, 42u);
  EXPECT_EQ(m.executed_shares, 30u);
  EXPECT_EQ(m.match_number, 123456u);
}

TEST(Decoder, OrderExecutedWithPrice) {
  const auto payload =
      Bytes{}.header('C').u64(42).u32(30).u64(777).ch('N').u32(1'834'600).data();
  ASSERT_EQ(payload.size(), 36u);

  const auto m = decode_as<OrderExecutedWithPrice>(payload);
  EXPECT_EQ(m.order_ref, 42u);
  EXPECT_EQ(m.executed_shares, 30u);
  EXPECT_EQ(m.match_number, 777u);
  EXPECT_FALSE(m.printable);
  EXPECT_EQ(m.execution_price, 1'834'600u);
}

TEST(Decoder, OrderCancel) {
  const auto payload = Bytes{}.header('X').u64(42).u32(10).data();
  ASSERT_EQ(payload.size(), 23u);

  const auto m = decode_as<OrderCancel>(payload);
  EXPECT_EQ(m.order_ref, 42u);
  EXPECT_EQ(m.cancelled_shares, 10u);
}

TEST(Decoder, OrderDelete) {
  const auto payload = Bytes{}.header('D').u64(42).data();
  ASSERT_EQ(payload.size(), 19u);

  EXPECT_EQ(decode_as<OrderDelete>(payload).order_ref, 42u);
}

TEST(Decoder, OrderReplace) {
  const auto payload = Bytes{}.header('U').u64(42).u64(43).u32(500).u32(1'834'400).data();
  ASSERT_EQ(payload.size(), 35u);

  const auto m = decode_as<OrderReplace>(payload);
  EXPECT_EQ(m.original_order_ref, 42u);
  EXPECT_EQ(m.new_order_ref, 43u);
  EXPECT_EQ(m.shares, 500u);
  EXPECT_EQ(m.price, 1'834'400u);
}

TEST(Decoder, IgnoresMessagesThatDontTouchTheBook) {
  // System event, stock directory, non-cross trade, NOII.
  for (char type : {'S', 'R', 'P', 'I'}) {
    const auto payload = Bytes{}.header(type).u64(0).data();
    EXPECT_EQ(decode(payload).status, DecodeStatus::Ignored) << "type " << type;
  }
}

TEST(Decoder, EmptyPayload) {
  EXPECT_EQ(decode({}).status, DecodeStatus::Empty);
}

TEST(Decoder, RejectsWrongLength) {
  auto valid = Bytes{}.header('D').u64(42).data();

  auto short_by_one = valid;
  short_by_one.pop_back();
  EXPECT_EQ(decode(short_by_one).status, DecodeStatus::BadLength);

  auto long_by_one = valid;
  long_by_one.push_back(0);
  EXPECT_EQ(decode(long_by_one).status, DecodeStatus::BadLength);

  const std::vector<std::uint8_t> type_only = {'A'};
  EXPECT_EQ(decode(type_only).status, DecodeStatus::BadLength);
}

TEST(Decoder, RejectsInvalidSide) {
  const auto payload =
      Bytes{}.header('A').u64(1).ch('Z').u32(100).str("AAPL    ").u32(1).data();
  EXPECT_EQ(decode(payload).status, DecodeStatus::BadField);
}

TEST(Decoder, RejectsInvalidPrintableFlag) {
  const auto payload = Bytes{}.header('C').u64(1).u32(1).u64(1).ch('?').u32(1).data();
  EXPECT_EQ(decode(payload).status, DecodeStatus::BadField);
}

TEST(Decoder, BookMessageSizesMatchSpec) {
  EXPECT_EQ(book_message_size('A'), 36u);
  EXPECT_EQ(book_message_size('F'), 40u);
  EXPECT_EQ(book_message_size('E'), 31u);
  EXPECT_EQ(book_message_size('C'), 36u);
  EXPECT_EQ(book_message_size('X'), 23u);
  EXPECT_EQ(book_message_size('D'), 19u);
  EXPECT_EQ(book_message_size('U'), 35u);
  EXPECT_EQ(book_message_size('S'), 0u);
}

TEST(Symbol, TrimsTrailingSpacesOnly) {
  EXPECT_EQ((Symbol{{'B', 'R', 'K', ' ', 'A', ' ', ' ', ' '}}).view(), "BRK A");
  EXPECT_EQ((Symbol{{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}}).view(), "");
}

}  // namespace
}  // namespace itch
