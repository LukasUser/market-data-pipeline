#pragma once

// Test-only builder for a length-prefixed ITCH stream, one message at a time.

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "bytes.hpp"
#include "itch/messages.hpp"

namespace itch::test {

class Stream {
 public:
  Stream& add(std::uint16_t locate, Timestamp ts, OrderRef ref, char side, Shares shares,
              std::string_view stock, Price price) {
    return frame(Bytes().header('A', locate, 0, ts).u64(ref).ch(side).u32(shares).str(stock).u32(price));
  }
  Stream& execute(std::uint16_t locate, Timestamp ts, OrderRef ref, Shares shares) {
    return frame(Bytes().header('E', locate, 0, ts).u64(ref).u32(shares).u64(1));
  }
  Stream& cancel(std::uint16_t locate, Timestamp ts, OrderRef ref, Shares shares) {
    return frame(Bytes().header('X', locate, 0, ts).u64(ref).u32(shares));
  }
  Stream& remove(std::uint16_t locate, Timestamp ts, OrderRef ref) {
    return frame(Bytes().header('D', locate, 0, ts).u64(ref));
  }
  Stream& replace(std::uint16_t locate, Timestamp ts, OrderRef old_ref, OrderRef new_ref,
                  Shares shares, Price price) {
    return frame(Bytes().header('U', locate, 0, ts).u64(old_ref).u64(new_ref).u32(shares).u32(price));
  }
  Stream& raw(const Bytes& bytes) { return frame(bytes); }

  std::istringstream open() const { return std::istringstream(data_); }

 private:
  Stream& frame(const Bytes& bytes) {
    for (std::uint8_t b : bytes.framed()) data_.push_back(static_cast<char>(b));
    return *this;
  }

  std::string data_;
};

}  // namespace itch::test
