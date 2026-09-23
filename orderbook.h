#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
struct Order {
  char type{};
  std::uint64_t ts{}, order_id{};
  char side{};
  std::uint32_t price{}, qty{};
  std::array<char, 32> trader{};
};
struct Trade {
  std::uint64_t ts{}, buy_id{}, sell_id{};
  std::uint32_t price{}, qty{};
  std::array<char, 32> buyer{}, seller{};
};
using TradeSink = void (*)(void *, const Trade &);
// Single owning thread. Bounded, reusable arena with no heap fallback.
class OrderBook {
public:
  explicit OrderBook(std::size_t arena_bytes = 64 * 1024 * 1024);
  ~OrderBook();
  OrderBook(const OrderBook &) = delete;
  OrderBook &operator=(const OrderBook &) = delete;
  void process(Order order, TradeSink sink = nullptr, void *context = nullptr);
  void add_depth(Order order);
  bool reduce(std::uint64_t id, std::uint32_t qty);
  bool cancel(std::uint64_t id);
  bool replace(std::uint64_t old_id, std::uint64_t new_id, std::uint32_t price, std::uint32_t qty);
  const Order *find(std::uint64_t id) const;
  std::size_t size() const;
  void clear();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
bool parseLine(std::string_view line, Order &order);
void print_trade(void *, const Trade &trade);
void processOrder(Order order);
void resetBook();
void process_csv_file(const char *path, bool should_print);
