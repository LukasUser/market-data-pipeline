#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "itch/messages.hpp"
#include "itch/order_book.hpp"

namespace itch {

// Applies decoded ITCH messages to one OrderBook per instrument. Every message
// header carries the instrument's stock locate code, so even Delete and
// Execute messages (which have no symbol) can be routed to the right book.
class Market {
 public:
  BookStatus apply(const Message& message);

  // The book for a stock locate code, or nullptr if no message has touched it.
  const OrderBook* book(std::uint16_t locate) const;

  // The locate code for a symbol, learned from the first Add Order for it.
  std::optional<std::uint16_t> locate_of(std::string_view symbol) const;

  // Instruments that have received at least one book message.
  std::size_t instrument_count() const { return books_.size(); }

  std::size_t resting_orders() const;

 private:
  std::unordered_map<std::uint16_t, OrderBook> books_;
  std::unordered_map<std::uint16_t, Symbol> symbols_;
};

}  // namespace itch
