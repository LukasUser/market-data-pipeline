// Splits an ITCH 5.0 file into one file per symbol, holding only that
// symbol's book messages, so per-symbol jobs don't each read the whole day.
//
//   gunzip -c 12302019.NASDAQ_ITCH50.gz | itch_split - out/ AAPL MSFT TSLA
//   → out/AAPL.itch, out/MSFT.itch, out/TSLA.itch

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "itch/frame_reader.hpp"
#include "itch/symbol_splitter.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <itch-file | -> <out-dir> <SYMBOL>...\n", argv[0]);
    return 2;
  }
  const std::string_view path = argv[1];
  std::ifstream file;
  if (path != "-") {
    file.open(argv[1], std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", argv[1]);
      return 2;
    }
  }
  std::istream& in = path == "-" ? std::cin : file;

  const std::filesystem::path out_dir = argv[2];
  std::filesystem::create_directories(out_dir);
  std::vector<std::unique_ptr<std::ofstream>> files;
  std::map<std::string, std::ostream*> outputs;
  for (int i = 3; i < argc; ++i) {
    const std::string symbol = argv[i];
    auto out = std::make_unique<std::ofstream>(out_dir / (symbol + ".itch"), std::ios::binary);
    if (!*out) {
      std::fprintf(stderr, "cannot write %s\n", (out_dir / (symbol + ".itch")).c_str());
      return 2;
    }
    outputs.emplace(symbol, out.get());
    files.push_back(std::move(out));
  }

  const auto start = std::chrono::steady_clock::now();
  itch::SymbolSplitter splitter(outputs);
  std::uint64_t messages = 0, routed = 0;
  itch::FrameReader reader(in);
  while (auto payload = reader.next()) {
    ++messages;
    if (splitter.route(*payload)) ++routed;
  }
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

  bool ok = !reader.truncated();
  for (auto& f : files) {
    f->close();
    ok = ok && !f->fail();
  }
  for (const auto& [symbol, out] : outputs) {
    std::printf("%-8s %12llu\n", symbol.c_str(),
                static_cast<unsigned long long>(splitter.written(symbol)));
  }
  std::printf("\nmessages read     %llu\n", static_cast<unsigned long long>(messages));
  std::printf("messages written  %llu (%.1f%%)\n", static_cast<unsigned long long>(routed),
              100.0 * static_cast<double>(routed) / static_cast<double>(messages ? messages : 1));
  std::printf("elapsed           %.1f s\n", elapsed.count());
  std::printf("truncated         %s\n", reader.truncated() ? "yes" : "no");
  return ok ? 0 : 1;
}
