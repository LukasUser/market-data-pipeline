#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "itch/frame_reader.hpp"

namespace itch {
namespace {

using test::Bytes;

std::istringstream stream_of(const std::vector<std::uint8_t>& bytes) {
  return std::istringstream(std::string(bytes.begin(), bytes.end()));
}

std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> parts) {
  std::vector<std::uint8_t> out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

std::vector<std::uint8_t> to_vector(std::span<const std::uint8_t> s) {
  return {s.begin(), s.end()};
}

TEST(FrameReader, ReadsConsecutiveFrames) {
  const auto first = Bytes{}.header('D').u64(1);
  const auto second = Bytes{}.header('X').u64(2).u32(5);
  auto in = stream_of(concat({first.framed(), second.framed()}));
  FrameReader reader(in);

  auto f1 = reader.next();
  ASSERT_TRUE(f1.has_value());
  EXPECT_EQ(to_vector(*f1), first.data());

  auto f2 = reader.next();
  ASSERT_TRUE(f2.has_value());
  EXPECT_EQ(to_vector(*f2), second.data());

  EXPECT_FALSE(reader.next().has_value());
  EXPECT_FALSE(reader.truncated());
}

TEST(FrameReader, EmptyStream) {
  auto in = stream_of({});
  FrameReader reader(in);
  EXPECT_FALSE(reader.next().has_value());
  EXPECT_FALSE(reader.truncated());
}

TEST(FrameReader, ZeroLengthFrame) {
  auto in = stream_of({0x00, 0x00});
  FrameReader reader(in);
  auto f = reader.next();
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->empty());
  EXPECT_FALSE(reader.next().has_value());
}

TEST(FrameReader, DetectsTruncatedLengthPrefix) {
  auto in = stream_of({0x00});
  FrameReader reader(in);
  EXPECT_FALSE(reader.next().has_value());
  EXPECT_TRUE(reader.truncated());
}

TEST(FrameReader, DetectsTruncatedPayload) {
  auto bytes = Bytes{}.header('D').u64(1).framed();
  bytes.pop_back();
  auto in = stream_of(bytes);
  FrameReader reader(in);
  EXPECT_FALSE(reader.next().has_value());
  EXPECT_TRUE(reader.truncated());
}

TEST(FrameReader, FramesSpanningBufferRefills) {
  // Enough data to force several refills, with frames straddling chunk edges.
  constexpr int kFrames = 20'000;
  std::vector<std::uint8_t> bytes;
  for (int i = 0; i < kFrames; ++i) {
    const auto f = Bytes{}.header('E').u64(static_cast<std::uint64_t>(i)).u32(1).u64(2).framed();
    bytes.insert(bytes.end(), f.begin(), f.end());
  }
  auto in = stream_of(bytes);
  FrameReader reader(in, 0);  // Smallest buffer the reader allows.

  int count = 0;
  while (auto frame = reader.next()) {
    ASSERT_EQ(frame->size(), 31u);
    // Order ref sits right after the 11-byte header.
    std::uint64_t ref = 0;
    for (int b = 0; b < 8; ++b) ref = (ref << 8) | (*frame)[11 + static_cast<std::size_t>(b)];
    ASSERT_EQ(ref, static_cast<std::uint64_t>(count));
    ++count;
  }
  EXPECT_EQ(count, kFrames);
  EXPECT_FALSE(reader.truncated());
}

}  // namespace
}  // namespace itch
