#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

enum class OrderType { Add, Cancel };
enum class Side { Buy, Sell };

struct Order {
  OrderType type; // add / cancel
  int ts;         // timestamp
  int order_id;
  Side side; // buy / sell (only meaningful for Add)
  int price;
  unsigned qty;
  std::array<char, 16> trader; // fixed-size (null-terminated when possible)
};

// location container
struct location {
  Side side;
  int price;
  std::list<Order>::iterator it;
};

std::map<int, std::list<Order>> bids, asks;   // price -> list of orders map
std::unordered_map<int, location> orderIndex; // id -> location map

static std::string to_string_field(auto &&field) {
  return {field.begin(), field.end()};
}

static void assign_trader(std::array<char, 16> &dst, std::string_view src) {
  dst.fill('\0');
  const size_t n = std::min(dst.size() - 1, src.size()); // keep room for '\0'
  std::copy_n(src.begin(), n, dst.begin());
  dst[n] = '\0';
}

static std::string_view trader_view(const std::array<char, 16> &t) {
  // treat as C-string within fixed buffer
  size_t n = 0;
  while (n < t.size() && t[n] != '\0')
    ++n;
  return std::string_view(t.data(), n);
}

void addOrder(Order &&o) { // add order after matching
  std::map<int, std::list<Order>> &book = (o.side == Side::Buy) ? bids : asks;

  auto &level = book[o.price];
  level.push_back(std::move(o));

  auto it = std::prev(level.end());
  orderIndex[it->order_id] = location{it->side, it->price, it};
}

bool parseLine(const std::string &line, Order &o) {
  auto p = line | std::views::split(',');
  auto it = p.begin();
  if (it == p.end())
    return false;

  const std::string typeField = to_string_field(*it++);

  auto parse_type = [&](const std::string &s) -> std::optional<OrderType> {
    if (s == "A" || s == "add")
      return OrderType::Add;
    if (s == "C" || s == "cancel")
      return OrderType::Cancel;
    return std::nullopt;
  };

  auto parse_side = [&](const std::string &s) -> std::optional<Side> {
    if (s == "B" || s == "buy")
      return Side::Buy;
    if (s == "S" || s == "sell")
      return Side::Sell;
    return std::nullopt;
  };

  auto ot = parse_type(typeField);
  if (!ot)
    return false;
  o.type = *ot;

  if (o.type == OrderType::Cancel) {
    // Expect: type,ts,order_id
    if (it == p.end())
      return false;
    o.ts = std::stoi(to_string_field(*it++));
    if (it == p.end())
      return false;
    o.order_id = std::stoi(to_string_field(*it));
    return true;
  }

  // Add: type,ts,order_id,side,price,qty,trader
  if (it == p.end())
    return false;
  o.ts = std::stoi(to_string_field(*it++));
  if (it == p.end())
    return false;
  o.order_id = std::stoi(to_string_field(*it++));
  if (it == p.end())
    return false;

  const std::string sideField = to_string_field(*it++);
  auto sd = parse_side(sideField);
  if (!sd)
    return false;
  o.side = *sd;

  if (it == p.end())
    return false;
  o.price = std::stoi(to_string_field(*it++));
  if (it == p.end())
    return false;
  o.qty = static_cast<unsigned>(std::stoul(to_string_field(*it++)));
  if (it == p.end())
    return false;

  assign_trader(o.trader, to_string_field(*it));
  return true;
}

bool cancelOrder(int order_id) {
  auto idx = orderIndex.find(order_id);
  if (idx == orderIndex.end())
    return false;

  const location loc = idx->second; // location for matching order
  std::map<int, std::list<Order>> &book = (loc.side == Side::Buy) ? bids : asks;

  auto levelIt = book.find(loc.price); // price -> list of orders at that price
  if (levelIt == book.end()) {         // order already matched
    orderIndex.erase(idx);
    return false;
  }

  levelIt->second.erase(loc.it);
  if (levelIt->second.empty())
    book.erase(levelIt);
  orderIndex.erase(idx);
  return true;
}

void processBuy(Order incoming) {
  while (incoming.qty > 0 && !asks.empty()) {
    auto askIt = asks.begin();
    int askPrice = askIt->first;

    if (askPrice > incoming.price)
      break;

    std::list<Order> &level = askIt->second;
    Order &topSell = level.front();

    unsigned traded = std::min(incoming.qty, topSell.qty);
    incoming.qty -= traded;
    topSell.qty -= traded;

    std::cout << "T," << incoming.ts << "," << askPrice << "," << traded << ","
              << incoming.order_id << "," << topSell.order_id << ","
              << trader_view(incoming.trader) << ","
              << trader_view(topSell.trader) << "\n";

    if (topSell.qty == 0) {
      orderIndex.erase(topSell.order_id);
      level.pop_front();
    }
    if (level.empty())
      asks.erase(askIt);
  }

  if (incoming.qty > 0)
    addOrder(std::move(incoming));
}

void processSell(Order incoming) {
  while (incoming.qty > 0 && !bids.empty()) {
    auto bidIt = std::prev(bids.end());
    int bidPrice = bidIt->first;

    if (incoming.price > bidPrice)
      break;

    std::list<Order> &level = bidIt->second;
    Order &topBuy = level.front();

    unsigned traded = std::min(incoming.qty, topBuy.qty);
    incoming.qty -= traded;
    topBuy.qty -= traded;

    std::cout << "T," << incoming.ts << "," << bidPrice << "," << traded << ","
              << topBuy.order_id << "," << incoming.order_id << ","
              << trader_view(topBuy.trader) << ","
              << trader_view(incoming.trader) << "\n";

    if (topBuy.qty == 0) {
      orderIndex.erase(topBuy.order_id);
      level.pop_front();
    }
    if (level.empty())
      bids.erase(bidIt);
  }

  if (incoming.qty > 0)
    addOrder(std::move(incoming));
}

void processOrder(Order o) {
  if (o.type == OrderType::Add) {
    if (o.side == Side::Buy)
      processBuy(o);
    else
      processSell(o);
  } else { // Cancel
    cancelOrder(o.order_id);
  }
}

int main() {
  std::ifstream file("data.csv");
  if (!file) {
    std::cerr << "Could not open data.csv\n";
    return 1;
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

  return 0;
}
