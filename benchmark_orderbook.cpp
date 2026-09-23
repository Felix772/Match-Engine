#include "orderbook.h"
#include "itch.h"
#include <benchmark/benchmark.h>
#include <array>
static void MatchPair(benchmark::State &state) {
  OrderBook book;
  Order buy{'A',1,1,'B',100,1,{}},sell{'A',2,2,'S',100,1,{}};
  book.process(buy); book.process(sell);
  for(auto _:state) { book.process(buy); book.process(sell); benchmark::ClobberMemory(); }
  state.SetItemsProcessed(state.iterations()*2);
}
static void ParseCsv(benchmark::State &state) {
  Order o;
  for(auto _:state) benchmark::DoNotOptimize(parseLine("A,1,1,B,100,10,trader",o));
  state.SetItemsProcessed(state.iterations());
}
static void ParseItch(benchmark::State &state) {
  std::array<std::uint8_t,36> p{}; p[0]='A';p[19]='B';p[23]=1;p[35]=100;
  itch::Message m;
  for(auto _:state) { benchmark::DoNotOptimize(itch::parse(p,m)); benchmark::DoNotOptimize(m); }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(MatchPair);
BENCHMARK(ParseCsv);
BENCHMARK(ParseItch);
BENCHMARK_MAIN();
