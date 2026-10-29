// Replays an ITCH 5.0 file through the matching engine and reports where each
// executed order stood in the engine's queue just before NASDAQ executed it.
//
//   gunzip -c 12302019.NASDAQ_ITCH50.gz | itch_queue -

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string_view>

#include "itch/decoder.hpp"
#include "itch/frame_reader.hpp"
#include "itch/queue_audit.hpp"

namespace {

using itch::Execution;
using itch::QueuePosition;
using itch::Session;

constexpr std::array kPositions = {QueuePosition::Front, QueuePosition::BehindInQueue,
                                   QueuePosition::BehindOnPrice, QueuePosition::UnknownOrder};

void print_row(const itch::QueueAudit& audit, Session session, Execution type, const char* label) {
  std::uint64_t total = 0;
  for (QueuePosition p : kPositions) total += audit.executions(session, type, p);
  std::printf("  %-22s %11llu", label, static_cast<unsigned long long>(total));
  for (QueuePosition p : kPositions) {
    const auto n = audit.executions(session, type, p);
    const double pct = total ? 100.0 * static_cast<double>(n) / static_cast<double>(total) : 0.0;
    std::printf("  %11llu %6.2f%%", static_cast<unsigned long long>(n), pct);
  }
  std::printf("\n");
}

}  // namespace

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

  itch::QueueAudit audit;
  std::uint64_t messages = 0, malformed = 0;
  const auto start = std::chrono::steady_clock::now();
  itch::FrameReader reader(in);
  while (auto payload = reader.next()) {
    ++messages;
    const itch::DecodeResult result = itch::decode(*payload);
    if (result.status == itch::DecodeStatus::Ok) {
      audit.apply(result.message);
    } else if (result.status == itch::DecodeStatus::BadLength ||
               result.status == itch::DecodeStatus::BadField) {
      ++malformed;
    }
  }
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

  std::printf("Where each executed order stood just before NASDAQ executed it\n\n");
  std::printf("  %-22s %11s  %19s  %19s  %19s  %19s\n", "", "executions", "front of queue",
              "behind in queue", "behind on price", "unknown order");
  print_row(audit, Session::PreMarket, Execution::Executed, "E  before 09:30");
  print_row(audit, Session::Regular, Execution::Executed, "E  09:30-16:00");
  print_row(audit, Session::AfterHours, Execution::Executed, "E  after 16:00");
  print_row(audit, Session::PreMarket, Execution::ExecutedWithPrice, "C  before 09:30");
  print_row(audit, Session::Regular, Execution::ExecutedWithPrice, "C  09:30-16:00");
  print_row(audit, Session::AfterHours, Execution::ExecutedWithPrice, "C  after 16:00");

  std::printf("\nshares ahead, summed over 'behind in queue' executions (E, 09:30-16:00): %llu\n",
              static_cast<unsigned long long>(
                  audit.shares_skipped(Session::Regular, Execution::Executed)));
  std::printf("\nmessages          %llu\n", static_cast<unsigned long long>(messages));
  std::printf("elapsed           %.3f s\n", elapsed.count());
  std::printf("crossing orders   %llu (adds or replaces that arrived crossing the book)\n",
              static_cast<unsigned long long>(audit.crossing_orders()));
  std::printf("book errors       %llu\n", static_cast<unsigned long long>(audit.book_errors()));
  std::printf("resting orders    %zu (end of stream)\n", audit.resting_orders());
  std::printf("malformed         %llu\n", static_cast<unsigned long long>(malformed));
  std::printf("truncated         %s\n", reader.truncated() ? "yes" : "no");

  const bool clean = malformed == 0 && !reader.truncated() && audit.book_errors() == 0;
  return clean ? 0 : 1;
}
