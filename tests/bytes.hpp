#pragma once

// Test-only builder for big-endian ITCH byte sequences. Kept deliberately
// separate from the decoder's own byte-reading code.

#include <cstdint>
#include <string_view>
#include <vector>

namespace itch::test {

class Bytes {
 public:
  Bytes& ch(char c) { return byte(static_cast<std::uint8_t>(c)); }
  Bytes& u16(std::uint16_t v) { return be(v, 2); }
  Bytes& u32(std::uint32_t v) { return be(v, 4); }
  Bytes& u48(std::uint64_t v) { return be(v, 6); }
  Bytes& u64(std::uint64_t v) { return be(v, 8); }

  Bytes& str(std::string_view s) {
    for (char c : s) ch(c);
    return *this;
  }

  // Message type byte plus the common header.
  Bytes& header(char type, std::uint16_t locate = 1, std::uint16_t tracking = 0,
                std::uint64_t timestamp = 0) {
    return ch(type).u16(locate).u16(tracking).u48(timestamp);
  }

  // Wraps the current bytes as one length-prefixed frame.
  std::vector<std::uint8_t> framed() const {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(data_.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(data_.size()));
    out.insert(out.end(), data_.begin(), data_.end());
    return out;
  }

  const std::vector<std::uint8_t>& data() const { return data_; }

 private:
  Bytes& byte(std::uint8_t b) {
    data_.push_back(b);
    return *this;
  }

  Bytes& be(std::uint64_t v, int width) {
    for (int i = width - 1; i >= 0; --i) byte(static_cast<std::uint8_t>(v >> (8 * i)));
    return *this;
  }

  std::vector<std::uint8_t> data_;
};

}  // namespace itch::test
