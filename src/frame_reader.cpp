#include "itch/frame_reader.hpp"

#include <algorithm>
#include <cstring>

namespace itch {
namespace {

// A frame is a 2-byte length plus at most 65535 payload bytes, so the buffer
// must always be able to hold one whole frame.
constexpr std::size_t kMaxFrame = 2 + 0xFFFF;

}  // namespace

FrameReader::FrameReader(std::istream& in, std::size_t buffer_size)
    : in_(in), buf_(std::max(buffer_size, kMaxFrame)) {}

std::optional<std::span<const std::uint8_t>> FrameReader::next() {
  if (!fill(2)) {
    truncated_ = end_ > pos_;  // Stray bytes that can't form a length prefix.
    return std::nullopt;
  }

  const std::size_t len = (std::size_t{buf_[pos_]} << 8) | buf_[pos_ + 1];
  if (!fill(2 + len)) {
    truncated_ = true;
    return std::nullopt;
  }

  const std::span<const std::uint8_t> payload(buf_.data() + pos_ + 2, len);
  pos_ += 2 + len;
  return payload;
}

bool FrameReader::fill(std::size_t n) {
  if (end_ - pos_ >= n) return true;

  // Move the unread tail to the front, then top up from the stream.
  std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
  end_ -= pos_;
  pos_ = 0;

  while (end_ < n) {
    in_.read(reinterpret_cast<char*>(buf_.data() + end_),
             static_cast<std::streamsize>(buf_.size() - end_));
    const auto got = in_.gcount();
    if (got <= 0) break;
    end_ += static_cast<std::size_t>(got);
  }
  return end_ >= n;
}

}  // namespace itch
