// Decodes an ITCH 5.0 file and reports message counts and throughput.
//
//   itch_stats 01302020.NASDAQ_ITCH50
//   gunzip -c 01302020.NASDAQ_ITCH50.gz | itch_stats -

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string_view>

#include "itch/decoder.hpp"
#include "itch/frame_reader.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <itch-file | ->\n", argv[0]);
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

  std::array<std::uint64_t, 256> by_type{};
  std::array<std::uint64_t, 5> by_status{};
  std::uint64_t bytes = 0;

  const auto start = std::chrono::steady_clock::now();
  itch::FrameReader reader(in);
  while (auto payload = reader.next()) {
    bytes += 2 + payload->size();
    if (!payload->empty()) ++by_type[(*payload)[0]];
    ++by_status[static_cast<std::size_t>(itch::decode(*payload).status)];
  }
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

  std::uint64_t total = 0;
  for (auto n : by_status) total += n;

  std::printf("messages      %llu\n", static_cast<unsigned long long>(total));
  std::printf("elapsed       %.3f s\n", elapsed.count());
  std::printf("throughput    %.1f M msg/s, %.0f MB/s\n", static_cast<double>(total) / elapsed.count() / 1e6,
              static_cast<double>(bytes) / elapsed.count() / 1e6);

  std::printf("\nby type\n");
  for (std::size_t t = 0; t < by_type.size(); ++t) {
    if (by_type[t] == 0) continue;
    const bool book = itch::book_message_size(static_cast<char>(t)) != 0;
    std::printf("  %c  %12llu%s\n", static_cast<char>(t), static_cast<unsigned long long>(by_type[t]),
                book ? "  (book)" : "");
  }

  using S = itch::DecodeStatus;
  const auto count = [&](S s) { return static_cast<unsigned long long>(by_status[static_cast<std::size_t>(s)]); };
  std::printf("\ndecoded       %llu\n", count(S::Ok));
  std::printf("ignored       %llu\n", count(S::Ignored));
  std::printf("empty         %llu\n", count(S::Empty));
  std::printf("bad length    %llu\n", count(S::BadLength));
  std::printf("bad field     %llu\n", count(S::BadField));
  std::printf("truncated     %s\n", reader.truncated() ? "yes" : "no");

  const bool clean = count(S::BadLength) == 0 && count(S::BadField) == 0 && !reader.truncated();
  return clean ? 0 : 1;
}
