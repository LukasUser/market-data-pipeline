#pragma once

// Decoded NASDAQ TotalView-ITCH 5.0 messages that change the order book.
// Field layouts follow the "Nasdaq TotalView-ITCH 5.0" specification.
// All wire integers are big-endian; these structs hold host-order values.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace itch {

// Fixed-point price with 4 implied decimal places: $183.45 == 1834500.
using Price = std::uint32_t;
using OrderRef = std::uint64_t;
using Shares = std::uint32_t;
// Nanoseconds since midnight (6 bytes on the wire).
using Timestamp = std::uint64_t;

enum class Side : char { Buy = 'B', Sell = 'S' };

// Fields shared by every ITCH 5.0 message, after the 1-byte message type.
struct Header {
  std::uint16_t stock_locate;
  std::uint16_t tracking_number;
  Timestamp timestamp;

  friend bool operator==(const Header&, const Header&) = default;
};

// 8-byte stock symbol, left-justified and space-padded on the wire.
struct Symbol {
  std::array<char, 8> chars;

  std::string_view view() const {
    const std::string_view s(chars.data(), chars.size());
    const auto last = s.find_last_not_of(' ');
    return last == std::string_view::npos ? std::string_view{} : s.substr(0, last + 1);
  }

  friend bool operator==(const Symbol&, const Symbol&) = default;
};

// 'A' Add Order, or 'F' Add Order with MPID attribution.
struct AddOrder {
  Header header;
  OrderRef order_ref;
  Side side;
  Shares shares;
  Symbol stock;
  Price price;
  std::optional<std::array<char, 4>> attribution;  // Only set for 'F'.

  friend bool operator==(const AddOrder&, const AddOrder&) = default;
};

// 'E' Order Executed: shares traded at the resting order's price.
struct OrderExecuted {
  Header header;
  OrderRef order_ref;
  Shares executed_shares;
  std::uint64_t match_number;

  friend bool operator==(const OrderExecuted&, const OrderExecuted&) = default;
};

// 'C' Order Executed With Price: shares traded at a price other than the
// order's own (e.g. in a cross). Non-printable executions must not be counted
// toward volume, but still reduce the order's size.
struct OrderExecutedWithPrice {
  Header header;
  OrderRef order_ref;
  Shares executed_shares;
  std::uint64_t match_number;
  bool printable;
  Price execution_price;

  friend bool operator==(const OrderExecutedWithPrice&, const OrderExecutedWithPrice&) = default;
};

// 'X' Order Cancel: partial cancellation; the order stays on the book.
struct OrderCancel {
  Header header;
  OrderRef order_ref;
  Shares cancelled_shares;

  friend bool operator==(const OrderCancel&, const OrderCancel&) = default;
};

// 'D' Order Delete: the order is removed from the book entirely.
struct OrderDelete {
  Header header;
  OrderRef order_ref;

  friend bool operator==(const OrderDelete&, const OrderDelete&) = default;
};

// 'U' Order Replace: removes the original order and adds a new one. The new
// order keeps the original's side and stock, which the wire message omits, so
// the book layer has to look them up from the original order.
struct OrderReplace {
  Header header;
  OrderRef original_order_ref;
  OrderRef new_order_ref;
  Shares shares;
  Price price;

  friend bool operator==(const OrderReplace&, const OrderReplace&) = default;
};

using Message = std::variant<AddOrder, OrderExecuted, OrderExecutedWithPrice, OrderCancel,
                             OrderDelete, OrderReplace>;

}  // namespace itch
