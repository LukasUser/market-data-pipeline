#include "itch/decoder.hpp"

#include <cstring>

namespace itch {
namespace {

// Reads big-endian fields sequentially. Callers check the total length up
// front, so individual reads don't bounds-check.
class Cursor {
 public:
  explicit Cursor(const std::uint8_t* p) : p_(p) {}

  std::uint8_t u8() { return *p_++; }
  std::uint16_t u16() { return static_cast<std::uint16_t>(take(2)); }
  std::uint32_t u32() { return static_cast<std::uint32_t>(take(4)); }
  std::uint64_t u48() { return take(6); }
  std::uint64_t u64() { return take(8); }

  template <std::size_t N>
  std::array<char, N> chars() {
    std::array<char, N> out;
    std::memcpy(out.data(), p_, N);
    p_ += N;
    return out;
  }

 private:
  std::uint64_t take(std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | p_[i];
    p_ += n;
    return v;
  }

  const std::uint8_t* p_;
};

DecodeResult ok(Message m) { return {DecodeStatus::Ok, std::move(m)}; }
DecodeResult fail(DecodeStatus s) { return {s, {}}; }

}  // namespace

std::size_t book_message_size(char type) {
  switch (type) {
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    default: return 0;
  }
}

DecodeResult decode(std::span<const std::uint8_t> payload) {
  if (payload.empty()) return fail(DecodeStatus::Empty);

  const char type = static_cast<char>(payload[0]);
  const std::size_t expected = book_message_size(type);
  if (expected == 0) return fail(DecodeStatus::Ignored);
  if (payload.size() != expected) return fail(DecodeStatus::BadLength);

  Cursor c(payload.data() + 1);
  Header h;
  h.stock_locate = c.u16();
  h.tracking_number = c.u16();
  h.timestamp = c.u48();

  switch (type) {
    case 'A':
    case 'F': {
      AddOrder m;
      m.header = h;
      m.order_ref = c.u64();
      const auto side = static_cast<char>(c.u8());
      if (side != 'B' && side != 'S') return fail(DecodeStatus::BadField);
      m.side = static_cast<Side>(side);
      m.shares = c.u32();
      m.stock = Symbol{c.chars<8>()};
      m.price = c.u32();
      if (type == 'F') m.attribution = c.chars<4>();
      return ok(m);
    }
    case 'E': {
      OrderExecuted m;
      m.header = h;
      m.order_ref = c.u64();
      m.executed_shares = c.u32();
      m.match_number = c.u64();
      return ok(m);
    }
    case 'C': {
      OrderExecutedWithPrice m;
      m.header = h;
      m.order_ref = c.u64();
      m.executed_shares = c.u32();
      m.match_number = c.u64();
      const auto printable = static_cast<char>(c.u8());
      if (printable != 'Y' && printable != 'N') return fail(DecodeStatus::BadField);
      m.printable = printable == 'Y';
      m.execution_price = c.u32();
      return ok(m);
    }
    case 'X': {
      OrderCancel m;
      m.header = h;
      m.order_ref = c.u64();
      m.cancelled_shares = c.u32();
      return ok(m);
    }
    case 'D': {
      OrderDelete m;
      m.header = h;
      m.order_ref = c.u64();
      return ok(m);
    }
    case 'U': {
      OrderReplace m;
      m.header = h;
      m.original_order_ref = c.u64();
      m.new_order_ref = c.u64();
      m.shares = c.u32();
      m.price = c.u32();
      return ok(m);
    }
    default:
      return fail(DecodeStatus::Ignored);  // Unreachable: size table covers the same types.
  }
}

}  // namespace itch
