#pragma once
#include "orderbook.h"
#include <cstddef>
struct PipelineStats { std::size_t orders{}, trades{}; };
// Three stages: calling ingestion thread, book owner, reporting worker.
PipelineStats run_csv_pipeline(const char *path, TradeSink sink = nullptr, void *context = nullptr);
