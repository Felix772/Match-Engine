#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>

struct Order {
  char type; // "A" or "C"
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

struct EarlierTimeFirst {
  bool operator()(const Order &a, const Order &b) const {
    // later time -> lower priority
    if (a.ts != b.ts)
      return a.ts > b.ts;
    return a.order_id > b.order_id; // tie-breaker (optional but good)
  }
};
void addOrder(Order &&o) { // add order after matching
  std::map<int, std::list<Order>> &book = (o.side == 'B') ? bids : asks;

  auto &level = book[o.price];
  level.push_back(std::move(o));

  auto it = std::prev(level.end());
  orderIndex[it->order_id] = location{it->side, it->price, it};
}

static std::string to_string_field(auto &&field) {
  return {field.begin(), field.end()};
}

bool parseLine(const std::string &line, Order &o) {

  auto p = line | std::views::split(',');
  auto it = p.begin();

  if (it == p.end())
    return false;

  char type = *(*it).begin(); // first char of first field

  if (type == 'C') {
    o.type = 'C';
    ++it; // move to ts
    o.ts = std::stoi(to_string_field(*it++));
    o.order_id = std::stoi(to_string_field(*it));
    return true;
  }

  o.type = 'A';
  ++it; // move to ts
  o.ts = std::stoi(to_string_field(*it++));
  o.order_id = std::stoi(to_string_field(*it++));
  o.side = *(*it++).begin();
  o.price = std::stoi(to_string_field(*it++));
  o.qty = static_cast<unsigned>(std::stoul(to_string_field(*it++)));
  o.trader = to_string_field(*it);

  return true;
}

bool cancelOrder(int order_id) {
  auto idx = orderIndex.find(order_id);
  if (idx == orderIndex.end()) {
    return false;
  }

  const location loc = idx->second; // location for matching order
  std::map<int, std::list<Order>> &book = (loc.side == 'B') ? bids : asks;

  auto levelIt =
      book.find(loc.price);    // price -> list of orders with matching price
  if (levelIt == book.end()) { // order already matched
    orderIndex.erase(idx);
    return false;
  }

  levelIt->second.erase(loc.it);
  if (levelIt->second.empty()) {
    book.erase(levelIt); // remove price level
  }
  orderIndex.erase(idx);
  return true;
}
void processBuy(Order incoming) {
  while (incoming.qty > 0 && !asks.empty()) {
    auto askIt = asks.begin();
    int askPrice = askIt->first;

    if (askPrice > incoming.price) {
      break;
    }

    std::list<Order> &level = askIt->second;
    Order &topSell = level.front();

    unsigned traded = std::min(incoming.qty, topSell.qty);
    incoming.qty -= traded;
    topSell.qty -= traded;
    std::cout << "T," << incoming.ts << "," << askPrice << "," << traded << ","
              << incoming.order_id << "," << topSell.order_id << ","
              << incoming.trader << "," << topSell.trader << std::endl;
    if (topSell.qty == 0) {
      orderIndex.erase(topSell.order_id);
      level.pop_front();
    }
    if (level.empty()) {
      asks.erase(askIt);
    }
  }
  if (incoming.qty > 0) {
    addOrder(std::move(incoming));
  }
}

void processSell(Order incoming) {
  while (incoming.qty > 0 && !bids.empty()) {
    auto bidIt = std::prev(bids.end());
    int bidPrice = bidIt->first;

    if (incoming.price > bidPrice) { // add order directly
      break;
    }

    std::list<Order> &level = bidIt->second;
    Order &topBuy = level.front(); // highest bidder

    unsigned traded = std::min(incoming.qty, topBuy.qty);
    incoming.qty -= traded;
    topBuy.qty -= traded;
    std::cout << "T," << incoming.ts << "," << bidPrice << "," << traded << ","
              << topBuy.order_id << "," << incoming.order_id << ","
              << topBuy.trader << "," << incoming.trader << std::endl;
    if (topBuy.qty == 0) {
      orderIndex.erase(topBuy.order_id);
      level.pop_front();
    }
    if (level.empty()) {
      bids.erase(bidIt);
    }
  }
  if (incoming.qty > 0) { // remaining
    addOrder(std::move(incoming));
  }
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

int main() {

  std::ifstream file("data.csv");
  if (!file) {
    std::cerr << "Could not open data.csv\n";
    return 1;
  }

  std::string line;
  while (std::getline(file, line)) {
    Order o{};
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (!parseLine(line, o))
      continue;
    processOrder(std::move(o));
  }

  return 0;
}
