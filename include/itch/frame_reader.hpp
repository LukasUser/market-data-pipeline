#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <span>
#include <vector>

namespace itch {

// Splits a stream of length-prefixed ITCH messages, the format of NASDAQ's
// sample files: each message is preceded by a 2-byte big-endian length.
class FrameReader {
 public:
  explicit FrameReader(std::istream& in, std::size_t buffer_size = std::size_t{1} << 20);

  // Returns the next message payload, or nullopt at end of stream. The span
  // stays valid only until the next call.
  std::optional<std::span<const std::uint8_t>> next();

  // True if the stream ended partway through a frame.
  bool truncated() const { return truncated_; }

 private:
  // Ensures at least `n` unread bytes are buffered; false if the stream ends first.
  bool fill(std::size_t n);

  std::istream& in_;
  std::vector<std::uint8_t> buf_;
  std::size_t pos_ = 0;
  std::size_t end_ = 0;
  bool truncated_ = false;
};

}  // namespace itch
