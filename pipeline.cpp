#include "pipeline.h"
#include "ring_buffer.h"
#include <atomic>
#include <exception>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct State {
  SpscRing<Order, 4096> input;
  SpscRing<Trade, 4096> output;
  std::atomic<bool> stop{false}, input_done{false}, output_done{false};
};
void publish(void *context, const Trade &trade) {
  auto &s = *static_cast<State *>(context);
  while (!s.output.try_push(trade)) {
    if (s.stop.load(std::memory_order_acquire)) return;
    std::this_thread::yield();
  }
}
}
PipelineStats run_csv_pipeline(const char *path, TradeSink sink, void *context) {
  std::ifstream file(path);
  if (!file) throw std::runtime_error("Could not open input file");
  auto state = std::make_unique<State>();
  auto &s = *state;
  PipelineStats stats;
  std::exception_ptr ingest_error, match_error, report_error;
  std::jthread reporter([&] {
    try {
      Trade trade;
      while (!s.stop.load(std::memory_order_acquire)) {
        if (s.output.try_pop(trade)) {
          if (sink) sink(context, trade);
          ++stats.trades;
        } else if (s.output_done.load(std::memory_order_acquire)) {
          // Acquire completion BEFORE the final queue check, so nothing is lost.
          while (s.output.try_pop(trade)) {
            if (sink) sink(context, trade);
            ++stats.trades;
          }
          break;
        } else std::this_thread::yield();
      }
    } catch (...) { report_error = std::current_exception(); s.stop.store(true, std::memory_order_release); }
  });
  std::jthread matcher;
  try {
    matcher = std::jthread([&] {
      try {
        // Thread-local ownership; neither pool nor book is shared with other workers.
        thread_local OrderBook book;
        book.clear();
        Order order;
        while (!s.stop.load(std::memory_order_acquire)) {
          if (s.input.try_pop(order)) book.process(std::move(order), publish, &s);
          else if (s.input_done.load(std::memory_order_acquire)) {
            while (s.input.try_pop(order)) book.process(std::move(order), publish, &s);
            break;
          } else std::this_thread::yield();
        }
      } catch (...) { match_error = std::current_exception(); s.stop.store(true, std::memory_order_release); }
      s.output_done.store(true, std::memory_order_release);
    });
    std::string line;
    std::size_t row = 0;
    while (!s.stop.load(std::memory_order_acquire) && std::getline(file, line)) {
      ++row;
      if (line.empty() || line == "\r") continue;
      Order order;
      if (!parseLine(line, order)) throw std::runtime_error("Invalid CSV row " + std::to_string(row));
      while (!s.input.try_push(order)) {
        if (s.stop.load(std::memory_order_acquire)) break;
        std::this_thread::yield();
      }
      if (!s.stop.load(std::memory_order_acquire)) ++stats.orders;
    }
    if (file.bad()) throw std::runtime_error("Input read failed");
  } catch (...) { ingest_error = std::current_exception(); s.stop.store(true, std::memory_order_release); }
  s.input_done.store(true, std::memory_order_release);
  if (matcher.joinable()) matcher.join();
  reporter.join();
  if (ingest_error) std::rethrow_exception(ingest_error);
  if (match_error) std::rethrow_exception(match_error);
  if (report_error) std::rethrow_exception(report_error);
  return stats;
}
