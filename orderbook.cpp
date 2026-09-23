#include "orderbook.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <memory_resource>
#include <stdexcept>
#include <unordered_map>
#include <vector>
struct OrderBook::Impl {
  std::vector<std::byte> storage;
  std::pmr::monotonic_buffer_resource arena;
  std::pmr::unsynchronized_pool_resource pool;
  using Level = std::pmr::list<Order>;
  using Side = std::pmr::map<std::uint32_t, Level>;
  Side bids, asks;
  struct Location { Side *side; Side::iterator level; Level::iterator order; };
  std::pmr::unordered_map<std::uint64_t, Location> index;
  explicit Impl(std::size_t bytes)
      : storage(bytes), arena(storage.data(), storage.size(), std::pmr::null_memory_resource()),
        pool(&arena), bids(&pool), asks(&pool), index(&pool) { index.reserve(65536); }
  void add(Order o) {
    auto &side = o.side == 'B' ? bids : asks;
    auto [level, created] = side.try_emplace(o.price);
    try {
      level->second.push_back(std::move(o));
      auto it = std::prev(level->second.end());
      try { index.emplace(it->order_id, Location{&side, level, it}); }
      catch (...) { level->second.pop_back(); throw; }
    } catch (...) {
      if (created && level->second.empty()) side.erase(level);
      throw;
    }
  }
};
OrderBook::OrderBook(std::size_t bytes) : impl_(std::make_unique<Impl>(bytes)) {}
OrderBook::~OrderBook() = default;
std::size_t OrderBook::size() const { return impl_->index.size(); }
const Order *OrderBook::find(std::uint64_t id) const {
  auto it = impl_->index.find(id);
  return it == impl_->index.end() ? nullptr : &*it->second.order;
}
void OrderBook::clear() { impl_->index.clear(); impl_->bids.clear(); impl_->asks.clear(); }
bool OrderBook::cancel(std::uint64_t id) {
  auto &p = *impl_;
  auto it = p.index.find(id);
  if (it == p.index.end()) return false;
  auto loc = it->second;
  loc.level->second.erase(loc.order);
  if (loc.level->second.empty()) loc.side->erase(loc.level);
  p.index.erase(it);
  return true;
}
bool OrderBook::reduce(std::uint64_t id, std::uint32_t qty) {
  auto it = impl_->index.find(id);
  if (it == impl_->index.end()) return false;
  auto &o = *it->second.order;
  if (!qty || qty > o.qty) throw std::invalid_argument("invalid reduction quantity");
  if (qty == o.qty) return cancel(id);
  o.qty -= qty;
  return true;
}
static void validate(const Order &o) {
  if (o.type != 'A' || (o.side != 'B' && o.side != 'S') || !o.qty || o.trader.back() != '\0')
    throw std::invalid_argument("invalid add order");
}
void OrderBook::add_depth(Order o) {
  validate(o);
  if (find(o.order_id)) throw std::invalid_argument("duplicate live order ID");
  impl_->add(std::move(o));
}
bool OrderBook::replace(std::uint64_t old_id, std::uint64_t new_id, std::uint32_t price, std::uint32_t qty) {
  auto old = find(old_id);
  if (!old) return false;
  if (!qty || find(new_id)) throw std::invalid_argument("invalid replacement");
  Order next = *old;
  next.order_id = new_id; next.price = price; next.qty = qty;
  add_depth(next);
  cancel(old_id);
  return true;
}
void OrderBook::process(Order o, TradeSink sink, void *context) {
  if (o.type == 'C') { cancel(o.order_id); return; }
  validate(o);
  if (find(o.order_id)) throw std::invalid_argument("duplicate live order ID");
  auto &opposite = o.side == 'B' ? impl_->asks : impl_->bids;
  while (o.qty && !opposite.empty()) {
    auto level = o.side == 'B' ? opposite.begin() : std::prev(opposite.end());
    if (o.side == 'B' ? level->first > o.price : level->first < o.price) break;
    auto &resting = level->second.front();
    const auto qty = std::min(o.qty, resting.qty);
    const auto &buy = o.side == 'B' ? o : resting;
    const auto &sell = o.side == 'S' ? o : resting;
    Trade trade{o.ts, buy.order_id, sell.order_id, level->first, qty, buy.trader, sell.trader};
    o.qty -= qty; resting.qty -= qty;
    if (!resting.qty) cancel(resting.order_id);
    if (sink) sink(context, trade);
  }
  if (o.qty) impl_->add(std::move(o));
}
template<class T> static bool number(std::string_view s, T &value) {
  if (s.empty()) return false;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  return ec == std::errc{} && end == s.data() + s.size();
}
bool parseLine(std::string_view line, Order &result) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  std::array<std::string_view, 7> fields{};
  std::size_t n = 0;
  for (;;) {
    if (n == fields.size()) return false;
    auto comma = line.find(','); fields[n++] = line.substr(0, comma);
    if (comma == line.npos) break;
    line.remove_prefix(comma + 1);
  }
  Order o{};
  if (n < 3 || !number(fields[1], o.ts) || !number(fields[2], o.order_id)) return false;
  if (fields[0] == "C" && n == 3) o.type = 'C';
  else if (fields[0] == "A" && n == 7) {
    o.type = 'A';
    if (fields[3] != "B" && fields[3] != "S") return false;
    o.side = fields[3][0];
    if (!number(fields[4], o.price) || !number(fields[5], o.qty) || !o.qty ||
        fields[6].empty() || fields[6].size() >= o.trader.size() || fields[6].find('\0') != fields[6].npos) return false;
    std::copy(fields[6].begin(), fields[6].end(), o.trader.begin());
  } else return false;
  result = o;
  return true;
}
void print_trade(void *, const Trade &t) {
  std::cout << "T," << t.ts << ',' << t.price << ',' << t.qty << ',' << t.buy_id
            << ',' << t.sell_id << ',' << t.buyer.data() << ',' << t.seller.data() << '\n';
}
static OrderBook &local_book() { thread_local OrderBook book; return book; }
void resetBook() { local_book().clear(); }
void processOrder(Order o) { local_book().process(std::move(o), print_trade); }
void process_csv_file(const char *path, bool should_print) {
  resetBook();
  std::ifstream file(path);
  if (!file) throw std::runtime_error("Could not open input file");
  std::string line; std::size_t row = 0;
  while (std::getline(file, line)) {
    ++row;
    if (line.empty() || line == "\r") continue;
    Order o{};
    if (!parseLine(line, o)) throw std::runtime_error("Invalid CSV row " + std::to_string(row));
    local_book().process(std::move(o), should_print ? print_trade : nullptr);
  }
  if (file.bad()) throw std::runtime_error("Input read failed");
}
