// orderbook.cpp
#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>

struct Order {
  char type; // 'A' or 'C'
  int ts;    // timestamp
  int order_id;
  char side; // 'B' or 'S'
  int price;
  unsigned qty;
  std::string trader;
};

// location container
struct location {
  char side;
  int price;
  std::list<Order>::iterator it;
};

std::map<int, std::list<Order>> bids, asks;   // price -> list of orders map
std::unordered_map<int, location> orderIndex; // id -> location map

// ---- printing control (for benchmarking) ----
static bool g_should_print = true;

static inline void maybe_print_trade(int ts, int price, unsigned qty, int id1,
                                     int id2, const std::string &trader1,
                                     const std::string &trader2) {
  if (!g_should_print)
    return;
  std::cout << "T," << ts << "," << price << "," << qty << "," << id1 << ","
            << id2 << "," << trader1 << "," << trader2 << "\n";
}

// ---- helpers ----
static std::string to_string_field(auto &&field) {
  return {field.begin(), field.end()};
}

// ---- orderbook ops ----
void resetBook() {
  bids.clear();
  asks.clear();
  orderIndex.clear();
}

static void addOrder(Order &&o) { // add order after matching
  std::map<int, std::list<Order>> &book = (o.side == 'B') ? bids : asks;

  auto &level = book[o.price];
  level.push_back(std::move(o));

  auto it = std::prev(level.end());
  orderIndex[it->order_id] = location{it->side, it->price, it};
}

bool parseLine(const std::string &line, Order &o) {
  if (line.empty())
    return false;

  auto p = line | std::views::split(',');
  auto it = p.begin();
  if (it == p.end())
    return false;

  char type = *(*it).begin(); // first char of first field

  if (type == 'C') {
    o.type = 'C';
    ++it; // ts
    if (it == p.end())
      return false;
    o.ts = std::stoi(to_string_field(*it++));
    if (it == p.end())
      return false;
    o.order_id = std::stoi(to_string_field(*it));
    return true;
  }

  // Add order: A,ts,id,side,price,qty,trader
  o.type = 'A';
  ++it; // ts
  if (it == p.end())
    return false;
  o.ts = std::stoi(to_string_field(*it++));
  if (it == p.end())
    return false;
  o.order_id = std::stoi(to_string_field(*it++));
  if (it == p.end())
    return false;
  o.side = *(*it++).begin();
  if (it == p.end())
    return false;
  o.price = std::stoi(to_string_field(*it++));
  if (it == p.end())
    return false;
  o.qty = static_cast<unsigned>(std::stoul(to_string_field(*it++)));
  if (it == p.end())
    return false;
  o.trader = to_string_field(*it);

  return true;
}

static bool cancelOrder(int order_id) {
  auto idx = orderIndex.find(order_id);
  if (idx == orderIndex.end())
    return false;

  const location &loc = idx->second;
  std::map<int, std::list<Order>> &book = (loc.side == 'B') ? bids : asks;

  auto levelIt = book.find(loc.price);
  if (levelIt == book.end()) {
    // order already matched/removed
    orderIndex.erase(idx);
    return false;
  }

  levelIt->second.erase(loc.it);
  if (levelIt->second.empty()) {
    book.erase(levelIt);
  }
  orderIndex.erase(idx);
  return true;
}

static void processBuy(Order incoming) {
  while (incoming.qty > 0 && !asks.empty()) {
    auto askIt = asks.begin(); // best ask
    int askPrice = askIt->first;

    if (askPrice > incoming.price)
      break;

    std::list<Order> &level = askIt->second;
    Order &topSell = level.front();

    unsigned traded = std::min(incoming.qty, topSell.qty);
    incoming.qty -= traded;
    topSell.qty -= traded;

    maybe_print_trade(incoming.ts, askPrice, traded, incoming.order_id,
                      topSell.order_id, incoming.trader, topSell.trader);

    if (topSell.qty == 0) {
      orderIndex.erase(topSell.order_id);
      level.pop_front();
    }
    if (level.empty()) {
      asks.erase(askIt);
    }
  }

  if (incoming.qty > 0)
    addOrder(std::move(incoming));
}

static void processSell(Order incoming) {
  while (incoming.qty > 0 && !bids.empty()) {
    auto bidIt = std::prev(bids.end()); // best bid
    int bidPrice = bidIt->first;

    if (incoming.price > bidPrice)
      break;

    std::list<Order> &level = bidIt->second;
    Order &topBuy = level.front();

    unsigned traded = std::min(incoming.qty, topBuy.qty);
    incoming.qty -= traded;
    topBuy.qty -= traded;

    maybe_print_trade(incoming.ts, bidPrice, traded, topBuy.order_id,
                      incoming.order_id, topBuy.trader, incoming.trader);

    if (topBuy.qty == 0) {
      orderIndex.erase(topBuy.order_id);
      level.pop_front();
    }
    if (level.empty()) {
      bids.erase(bidIt);
    }
  }

  if (incoming.qty > 0)
    addOrder(std::move(incoming));
}

void processOrder(Order o) {
  if (o.type == 'A') {
    if (o.side == 'B')
      processBuy(std::move(o));
    else
      processSell(std::move(o));
  } else if (o.type == 'C') {
    cancelOrder(o.order_id);
  }
}

// Bencher-style “macro” function: callable from main and benchmark
void process_csv_file(const char *path, bool should_print) {
  resetBook(); // important for benchmarking + repeatability
  g_should_print = should_print;

  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("Could not open input file");
  }

  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    Order o{};
    if (!parseLine(line, o))
      continue;
    processOrder(std::move(o));
  }
}
