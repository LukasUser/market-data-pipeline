// Ranks instruments by how many book messages they have, busiest first.
//
//   gunzip -c 12302019.NASDAQ_ITCH50.gz | itch_rank - 100 > top100.tsv

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "itch/decoder.hpp"
#include "itch/frame_reader.hpp"

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s <itch-file | -> [top-n]\n", argv[0]);
    return 2;
  }
  const std::string_view path = argv[1];
  const std::size_t top = argc == 3 ? std::strtoul(argv[2], nullptr, 10) : 100;
  std::ifstream file;
  if (path != "-") {
    file.open(argv[1], std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", argv[1]);
      return 2;
    }
  }
  std::istream& in = path == "-" ? std::cin : file;

  std::unordered_map<std::uint16_t, std::uint64_t> counts;
  std::unordered_map<std::uint16_t, itch::Symbol> symbols;
  itch::FrameReader reader(in);
  while (auto payload = reader.next()) {
    const itch::DecodeResult result = itch::decode(*payload);
    if (result.status != itch::DecodeStatus::Ok) continue;
    const std::uint16_t locate =
        std::visit([](const auto& m) { return m.header.stock_locate; }, result.message);
    ++counts[locate];
    if (const auto* add = std::get_if<itch::AddOrder>(&result.message)) {
      symbols.try_emplace(locate, add->stock);
    }
  }

  std::vector<std::pair<std::uint16_t, std::uint64_t>> ranked(counts.begin(), counts.end());
  std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (std::size_t i = 0; i < std::min(top, ranked.size()); ++i) {
    const auto symbol = symbols[ranked[i].first].view();
    std::printf("%.*s\t%llu\n", static_cast<int>(symbol.size()), symbol.data(),
                static_cast<unsigned long long>(ranked[i].second));
  }
  return reader.truncated() ? 1 : 0;
}
