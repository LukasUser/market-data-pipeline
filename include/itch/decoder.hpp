#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "itch/messages.hpp"

namespace itch {

enum class DecodeStatus {
  Ok,         // `message` holds the decoded book message.
  Ignored,    // Valid message type that doesn't affect the book (e.g. 'S', 'R', 'P').
  Empty,      // Zero-length payload.
  BadLength,  // Book message whose payload size doesn't match the spec.
  BadField,   // A field held a value the spec doesn't allow (e.g. side not 'B'/'S').
};

struct DecodeResult {
  DecodeStatus status;
  Message message;  // Only meaningful when status == DecodeStatus::Ok.
};

// Wire size of a book message type including its type byte, or 0 for any
// type this decoder doesn't handle.
std::size_t book_message_size(char type);

// Decodes one ITCH message payload (without the 2-byte length prefix used in
// NASDAQ's sample files).
DecodeResult decode(std::span<const std::uint8_t> payload);

}  // namespace itch
