# Match Engine

C++20 price/time-priority limit-order matcher, bounded asynchronous CSV pipeline,
and an allocation-free NASDAQ ITCH 5.0 order-depth decoder.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 tests/reference.py build/match-engine
./build/match-engine --csv data.csv
./build/match-engine --pipeline data.csv
./build/match-engine --itch feed.bin 7
./build/stress-benchmark data.csv
```

Requires a C++20 compiler, CMake 3.20+, and platform threads. Windows executables
have an `.exe` suffix; with a multi-configuration generator, pass `--config Release`
and use `build/Release/`. `./make.sh` is an alternative GCC build/test entry point.
Running without arguments retains the original `data.csv` behavior.

For Google Benchmark, install its development package, then configure with
`-DWITH_GOOGLE_BENCHMARK=ON`. No dependencies are downloaded by the build.
`-DITCH_DISABLE_AVX2=ON` forces the portable decoder for verification.

## Architecture

CSV ingestion -> bounded SPSC order queue -> one matching worker -> bounded SPSC
trade queue -> reporting worker. Each queue holds 4,096 messages. Full queues
apply backpressure using retry/yield; messages are not silently dropped. FIFO
arrival order, resting-order execution prices, partial fills, and buy-first trade
output are preserved. Worker exceptions stop the pipeline, join workers, and
propagate to the caller. On failure, an already processed/reported prefix may
exist; this is not transactional replay.

Only the matching thread accesses its `thread_local` book and unsynchronized
memory pool. Price levels and FIFO nodes use PMR containers; the ID index stores
stable iterators for direct cancellation. A pre-touched 64 MiB arena supplies
reusable pool blocks with a null upstream allocator. There is no heap fallback:
capacity exhaustion throws `std::bad_alloc` and stops replay. Capacity is a byte
budget, not a fixed order count; price distribution, hash growth, and allocator
metadata affect it. Book construction and ingestion/I/O are outside the
allocation-free matching-path claim. A callback can allocate independently.

Orders contain bounded inline trader names (31 bytes plus terminator), fixed-width
numbers, and move through the matching path. Queue counters and MPMC cells are
64-byte aligned. This remains a tree/list book, not a fully contiguous ladder.

`ring_buffer.h` also supplies a tested bounded MPMC queue for independent producers
and consumers. Its atomics are statically required to be lock-free, but a stalled
reservation can block progress: **the MPMC algorithm is not formally lock-free**.
The ordered single-book pipeline uses SPSC queues. Multiple order-ingestion
producers need an explicit sequencing policy before using MPMC for one book.

## CSV contract

```text
A,timestamp,order_id,B|S,price,quantity,trader
C,timestamp,order_id
T,timestamp,price,quantity,buy_id,sell_id,buyer,seller
```

Prices and quantities are unsigned 32-bit integers; timestamps and IDs are unsigned
64-bit integers. Quantity must be positive. Prices use caller-defined integer tick
units. Trader names must contain 1-31 bytes and no comma or NUL. There is no quoted
CSV syntax. Blank lines and CRLF are supported. Malformed rows fail with their
line number. Duplicate live IDs fail before matching; unknown CSV cancels are
idempotent, and IDs may be reused after removal. FIFO follows arrival order, not
timestamp sorting. The engine permits self-trades and has no risk checks.

## ITCH depth replay

The decoder implements order-depth messages **A, F, E, C, X, D, U** against the
[NASDAQ TotalView-ITCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf).
It validates exact payload lengths, sides, quantities, and C-message printable
flags, decodes network byte order, and retains borrowed views of stock/MPID fields.
Those views expire when the input buffer is reused. Scalar fields are decoded to
values; payloads and strings are not heap-copied by the parser.

On GCC/Clang x86, runtime AVX2 detection selects vector byte-shuffling for add-message
quantity/price fields. Loads stay within the validated 36-byte payload. Other
platforms use scalar decoding. This is a targeted SIMD path, not SIMD acceleration
of every message type.

`--itch FILE LOCATE` reads **two-byte big-endian length-prefixed payloads**, filters
one daily stock-locate code, and reports its applied event count. Feed adds populate
depth without matching; E/C/X reduce shares, D deletes, and U replaces the ID and
loses time priority. Unknown selected-instrument order references are errors, so
replay must start from a complete book history. Other message types are skipped,
not fully decoded or validated. Stock-directory routing, trade analytics, auction
handling, gap recovery, SoupBinTCP/MoldUDP64, and live networking are not implemented.
A historical ITCH feed is market data, not order entry.

## Validation and performance

`tests.cpp` covers matching/FIFO, strict parsing, ITCH field boundaries and depth
updates, truncated frames, scalar/AVX2 decoding, SPSC wraparound, four-producer /
four-consumer contention, pipeline draining, and worker/ingestion failures.
`tests/reference.py` compares both CSV execution modes to an independent Python
matcher on the supplied data and five deterministic randomized workloads.
Linux CI adds ASan/UBSan and ThreadSanitizer jobs; these jobs must run remotely
before their results can be claimed.

See [PERFORMANCE.md](PERFORMANCE.md) for measured local results, reproducible
profiling commands, scope limits, and the relationship to the resume's claims.
