#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

#include "itch/matching_engine.hpp"
#include "itch/messages.hpp"

namespace itch {

// Where an executed order stood in our book just before NASDAQ executed it.
enum class QueuePosition {
  Front,          // Best price on its side, nothing ahead: what price-time priority predicts.
  BehindInQueue,  // Best price, but other orders were ahead of it at that price.
  BehindOnPrice,  // A better price was resting on its side.
  UnknownOrder,   // Not in our book at all.
};

enum class Session { PreMarket, Regular, AfterHours };  // split at 09:30 and 16:00

enum class Execution { Executed, ExecutedWithPrice };  // 'E' and 'C'

// Replays ITCH through one MatchingEngine per instrument and checks every
// execution against the engine's queues. If the queue model matches NASDAQ,
// continuous-trading executions should all land on the front of the queue.
class QueueAudit {
 public:
  // Applies one book message. For an execution, returns where the order stood
  // before it was applied.
  std::optional<QueuePosition> apply(const Message& message);

  std::uint64_t executions(Session session, Execution type, QueuePosition position) const;

  // Shares that were ahead of orders executed while BehindInQueue.
  std::uint64_t shares_skipped(Session session, Execution type) const;

  // Adds and replaces that arrived crossing the book (bid at or above the
  // ask). They rest anyway: NASDAQ reports what trades as executions.
  std::uint64_t crossing_orders() const { return crossing_orders_; }

  // Messages the engine rejected (unknown or duplicate order, overfill).
  std::uint64_t book_errors() const { return book_errors_; }

  std::size_t resting_orders() const;

 private:
  static constexpr std::size_t kSessions = 3, kTypes = 2, kPositions = 4;

  QueuePosition position_of(const MatchingEngine& engine, OrderRef ref) const;
  std::optional<QueuePosition> execute(std::uint16_t locate, Timestamp t, OrderRef ref,
                                       Shares shares, Execution type);
  void submit(MatchingEngine& engine, OrderRef ref, Side side, Price price, Shares shares);

  std::unordered_map<std::uint16_t, MatchingEngine> engines_;
  std::array<std::array<std::array<std::uint64_t, kPositions>, kTypes>, kSessions> counts_{};
  std::array<std::array<std::uint64_t, kTypes>, kSessions> skipped_{};
  std::uint64_t crossing_orders_ = 0;
  std::uint64_t book_errors_ = 0;
};

}  // namespace itch
