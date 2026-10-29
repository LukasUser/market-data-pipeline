#include "itch/symbol_splitter.hpp"

#include <variant>

#include "itch/decoder.hpp"

namespace itch {

SymbolSplitter::SymbolSplitter(std::map<std::string, std::ostream*> outputs) {
  for (auto& [symbol, out] : outputs) by_symbol_.emplace(symbol, Output{out});
}

bool SymbolSplitter::route(std::span<const std::uint8_t> payload) {
  const DecodeResult result = decode(payload);
  if (result.status != DecodeStatus::Ok) return false;

  const std::uint16_t locate =
      std::visit([](const auto& m) { return m.header.stock_locate; }, result.message);
  auto it = by_locate_.find(locate);
  if (it == by_locate_.end()) {
    const auto* add = std::get_if<AddOrder>(&result.message);
    if (add == nullptr) return false;
    const auto wanted = by_symbol_.find(add->stock.view());
    if (wanted == by_symbol_.end()) return false;
    it = by_locate_.emplace(locate, &wanted->second).first;
  }

  Output& output = *it->second;
  const char length[2] = {static_cast<char>(payload.size() >> 8), static_cast<char>(payload.size())};
  output.out->write(length, 2);
  output.out->write(reinterpret_cast<const char*>(payload.data()),
                    static_cast<std::streamsize>(payload.size()));
  ++output.messages;
  return true;
}

std::uint64_t SymbolSplitter::written(std::string_view symbol) const {
  const auto it = by_symbol_.find(symbol);
  return it == by_symbol_.end() ? 0 : it->second.messages;
}

}  // namespace itch
