#include "itch.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#if !defined(ITCH_DISABLE_AVX2) && (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define ITCH_AVX2 1
#endif
namespace itch {
static std::uint64_t be(const std::uint8_t *p, std::size_t n) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < n; ++i) value = (value << 8) | p[i];
  return value;
}
bool avx2_available() noexcept {
#ifdef ITCH_AVX2
  static const bool available = __builtin_cpu_supports("avx2");
  return available;
#else
  return false;
#endif
}
#ifdef ITCH_AVX2
__attribute__((target("avx2")))
static void add_numbers(const std::uint8_t *p, Message &m) noexcept {
  // A/F are at least 36 bytes. Load exactly bytes [4,36), no packet overread.
  const auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + 4));
  const auto shuffle = _mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,
                                       3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
  const auto words = _mm256_shuffle_epi8(bytes, shuffle);
  m.shares = static_cast<std::uint32_t>(_mm256_extract_epi32(words, 4));
  m.price = static_cast<std::uint32_t>(_mm256_extract_epi32(words, 7));
}
#endif
Status parse(std::span<const std::uint8_t> p, Message &out) noexcept {
  if (p.size() < 11) return Status::malformed;
  Message m{}; m.type = static_cast<char>(p[0]);
  std::size_t expected = 0;
  switch (m.type) {
    case 'A': expected = 36; break; case 'F': expected = 40; break;
    case 'E': expected = 31; break; case 'C': expected = 36; break;
    case 'X': expected = 23; break; case 'D': expected = 19; break;
    case 'U': expected = 35; break;
    default: return Status::unsupported;
  }
  if (p.size() != expected) return Status::malformed;
  m.locate = static_cast<std::uint16_t>(be(p.data() + 1, 2));
  m.timestamp = be(p.data() + 5, 6); m.id = be(p.data() + 11, 8);
  if (m.type == 'A' || m.type == 'F') {
    m.side = static_cast<char>(p[19]);
    if (m.side != 'B' && m.side != 'S') return Status::malformed;
    m.stock = {reinterpret_cast<const char *>(p.data() + 24), 8};
    if (m.type == 'F') m.attribution = {reinterpret_cast<const char *>(p.data() + 36), 4};
#ifdef ITCH_AVX2
    if (avx2_available()) add_numbers(p.data(), m);
    else
#endif
    { m.shares = static_cast<std::uint32_t>(be(p.data() + 20, 4));
      m.price = static_cast<std::uint32_t>(be(p.data() + 32, 4)); }
  } else if (m.type == 'U') {
    m.new_id = be(p.data() + 19, 8);
    m.shares = static_cast<std::uint32_t>(be(p.data() + 27, 4));
    m.price = static_cast<std::uint32_t>(be(p.data() + 31, 4));
  } else if (m.type != 'D') {
    m.shares = static_cast<std::uint32_t>(be(p.data() + 19, 4));
    if (m.type == 'C') {
      if (p[31] != 'Y' && p[31] != 'N') return Status::malformed;
      m.price = static_cast<std::uint32_t>(be(p.data() + 32, 4));
    }
  }
  if (m.type != 'D' && !m.shares) return Status::malformed;
  out = m;
  return Status::ok;
}
bool apply(OrderBook &book, const Message &m, std::uint16_t locate) {
  if (m.locate != locate) return false;
  if (m.type == 'A' || m.type == 'F') {
    Order o{}; o.type = 'A'; o.ts = m.timestamp; o.order_id = m.id;
    o.side = m.side; o.qty = m.shares; o.price = m.price;
    if (!m.attribution.empty())
      std::copy_n(m.attribution.begin(), std::min(m.attribution.size(), o.trader.size() - 1), o.trader.begin());
    book.add_depth(o);
  } else {
    bool found = false;
    switch (m.type) {
      case 'D': found = book.cancel(m.id); break;
      case 'U': found = book.replace(m.id, m.new_id, m.price, m.shares); break;
      case 'E': case 'C': case 'X': found = book.reduce(m.id, m.shares); break;
      default: throw std::invalid_argument("unsupported depth event");
    }
    if (!found) throw std::runtime_error("ITCH references unknown order; feed gap or incomplete snapshot");
  }
  return true;
}
std::size_t replay(const char *path, std::uint16_t locate) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Could not open ITCH file");
  OrderBook book;
  std::array<std::uint8_t, 65535> buffer{};
  std::size_t applied = 0;
  for (;;) {
    std::array<std::uint8_t, 2> prefix{};
    file.read(reinterpret_cast<char *>(prefix.data()), 2);
    if (!file.gcount() && file.eof()) break;
    if (file.gcount() != 2) throw std::runtime_error("Truncated ITCH length");
    const auto length = static_cast<std::size_t>(be(prefix.data(), 2));
    if (!length) throw std::runtime_error("Empty ITCH frame");
    file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(length));
    if (static_cast<std::size_t>(file.gcount()) != length) throw std::runtime_error("Truncated ITCH payload");
    Message m;
    auto status = parse({buffer.data(), length}, m);
    if (status == Status::malformed) throw std::runtime_error("Malformed ITCH message");
    if (status == Status::ok && apply(book, m, locate)) ++applied;
  }
  return applied;
}
}
