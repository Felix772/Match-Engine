#include "orderbook.h"
#include <benchmark/benchmark.h>

static void BENCHMARK_orderbook(benchmark::State &state) {
  for (auto _ : state) {
    // optional but recommended: start from a clean book each iteration
    resetBook();

    // like Bencher: call the “macro” function with printing turned off
    process_csv_file("data.csv", false);
  }
}

BENCHMARK(BENCHMARK_orderbook);
BENCHMARK_MAIN();
