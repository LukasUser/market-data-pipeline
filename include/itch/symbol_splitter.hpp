#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace itch {

// Copies the book messages of chosen symbols to one output each, in the same
// length-prefixed format. Each output then replays on its own, much faster
// than the whole day, and gives the same results for that symbol: every
// per-symbol tool skips other symbols' messages anyway.
class SymbolSplitter {
 public:
  explicit SymbolSplitter(std::map<std::string, std::ostream*> outputs);

  // Routes one message payload. Returns true if it was written somewhere.
  bool route(std::span<const std::uint8_t> payload);

  std::uint64_t written(std::string_view symbol) const;

 private:
  struct Output {
    std::ostream* out;
    std::uint64_t messages = 0;
  };

  std::map<std::string, Output, std::less<>> by_symbol_;
  // Filled in from each symbol's first Add Order; nothing earlier can be its.
  std::unordered_map<std::uint16_t, Output*> by_locate_;
};

}  // namespace itch
