#include "itch/market.hpp"

#include <variant>

namespace itch {
namespace {

// Lets std::visit take one lambda per message type.
template <typename... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

}  // namespace

BookStatus Market::apply(const Message& message) {
  return std::visit(
      Overloaded{
          [this](const AddOrder& m) {
            symbols_.try_emplace(m.header.stock_locate, m.stock);
            return books_[m.header.stock_locate].add(m.order_ref, m.side, m.price, m.shares);
          },
          [this](const OrderExecuted& m) {
            return books_[m.header.stock_locate].reduce(m.order_ref, m.executed_shares);
          },
          [this](const OrderExecutedWithPrice& m) {
            return books_[m.header.stock_locate].reduce(m.order_ref, m.executed_shares);
          },
          [this](const OrderCancel& m) {
            return books_[m.header.stock_locate].reduce(m.order_ref, m.cancelled_shares);
          },
          [this](const OrderDelete& m) {
            return books_[m.header.stock_locate].remove(m.order_ref);
          },
          [this](const OrderReplace& m) {
            return books_[m.header.stock_locate].replace(m.original_order_ref, m.new_order_ref,
                                                         m.price, m.shares);
          },
      },
      message);
}

const OrderBook* Market::book(std::uint16_t locate) const {
  const auto it = books_.find(locate);
  return it == books_.end() ? nullptr : &it->second;
}

std::optional<std::uint16_t> Market::locate_of(std::string_view symbol) const {
  for (const auto& [locate, stock] : symbols_) {
    if (stock.view() == symbol) return locate;
  }
  return std::nullopt;
}

std::size_t Market::resting_orders() const {
  std::size_t total = 0;
  for (const auto& [locate, order_book] : books_) total += order_book.order_count();
  return total;
}

}  // namespace itch
