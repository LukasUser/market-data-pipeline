#include "itch/quote_stream.hpp"

#include <utility>
#include <variant>

#include "itch/decoder.hpp"

namespace itch {

QuoteStream::QuoteStream(std::istream& in, std::string symbol)
    : reader_(in), symbol_(std::move(symbol)) {}

std::optional<Quote> QuoteStream::next() {
  while (auto payload = reader_.next()) {
    const DecodeResult result = decode(*payload);
    if (result.status == DecodeStatus::BadLength || result.status == DecodeStatus::BadField) {
      ++malformed_;
      continue;
    }
    if (result.status != DecodeStatus::Ok) continue;

    const Header header = std::visit([](const auto& m) { return m.header; }, result.message);

    if (!locate_) {
      const auto* add = std::get_if<AddOrder>(&result.message);
      if (add == nullptr || add->stock.view() != symbol_) continue;
      locate_ = header.stock_locate;
    }
    if (header.stock_locate != *locate_) continue;

    if (market_.apply(result.message) != BookStatus::Ok) ++book_errors_;

    const OrderBook* current = market_.book(*locate_);
    Quote quote{header.timestamp, current->best_bid(), current->best_ask()};
    if (!last_ || quote.bid != last_->bid || quote.ask != last_->ask) {
      last_ = quote;
      return quote;
    }
  }
  return std::nullopt;
}

const OrderBook* QuoteStream::book() const {
  return locate_ ? market_.book(*locate_) : nullptr;
}

}  // namespace itch
