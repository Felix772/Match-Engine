#pragma once
#include "orderbook.h"
#include <span>
#include <string_view>
namespace itch {
enum class Status { ok, unsupported, malformed };
// Views borrow caller-owned packet bytes. They must not outlive the packet.
struct Message {
  char type{};
  std::uint16_t locate{};
  std::uint64_t timestamp{}, id{}, new_id{};
  std::uint32_t shares{}, price{};
  char side{};
  std::string_view stock{}, attribution{};
};
Status parse(std::span<const std::uint8_t> payload, Message &out) noexcept;
bool avx2_available() noexcept;
// Select one instrument by its daily locate code. Unknown order references fail.
bool apply(OrderBook &book, const Message &message, std::uint16_t locate);
std::size_t replay(const char *path, std::uint16_t locate);
}
