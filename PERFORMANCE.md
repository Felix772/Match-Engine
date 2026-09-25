# Performance evidence

## Local run (2026-09-23)

Windows, MSYS2 UCRT64 GCC 16.2.0, C++20, `-O3 -DNDEBUG -pthread`, runtime AVX2
available. CPU identity was unavailable through the sandbox. No CPU affinity,
frequency control, or exclusive-machine isolation was used. These are one-run
observations, not portable guarantees or a measured improvement over a baseline.

| Workload | Operations/s | Sampled p50 | Sampled p95 | Sampled p99 | Observed C++ heap allocations |
|---|---:|---:|---:|---:|---:|
| Add/match, one price | 15,837,946 | 100 ns | 300 ns | 600 ns | 0 |
| Add/cancel, one price | 16,556,483 | 100 ns | 300 ns | 600 ns | 0 |
| Add/cancel, 1,024 prices | 8,649,659 | 200 ns | 600 ns | 1,400 ns | 0 |
| ITCH add decoding | 36,812,074 | not measured | not measured | not measured | 0 |
| CSV pipeline, including startup | 1,880,367 | not measured | not measured | not measured | not measured |

The pipeline processed 200,000 events and produced 127,024 trades with a null sink.
The full-data reference comparison separately verified every trade in both the
sequential and threaded CLI modes, plus five randomized 20,000-event workloads.
Both AVX2 and forced-scalar C++ test builds passed locally.

The synthetic book runs each process one million pre-generated events after a
warmup replay. Blocks add 1,024 bids, then match or cancel them. Timings exclude
input generation, warmup, pool construction, printing, and teardown. One in 256
operations is timed with `steady_clock`; clock overhead is included and timer
resolution/periodic sampling limit interpretation. These are sampled synchronous
book-operation latencies, **not end-to-end pipeline or network latency**.
Global `new`/`new[]`, including aligned forms, are counted on the benchmark thread
during the measured loops. This does not count arbitrary C allocations or other
threads. The production pool independently prohibits heap fallback.

## Local WSL2 verification (2026-09-25)

Five independent runs on this machine's Ubuntu WSL2, Linux
`6.18.33.2-microsoft-standard-WSL2`, Intel Core Ultra 9 185H, GCC 13.3.0,
C++20, and `-O3 -DNDEBUG -pthread`. No CPU affinity, governor control, or
exclusive-machine isolation was used. Each book workload replayed one million
pre-generated operations to warm the book, then measured another million. The
latency sample buffer and `steady_clock` were touched before each measured loop.

| Workload | Throughput across 5 runs | Sampled p99 across 5 runs | C++ heap allocations | Hot-loop minor faults |
|---|---:|---:|---:|---:|
| Add/match, one price | 14.41-16.28 million ops/s | 0.495-0.715 microseconds | 0 in each run | 0 in each run |
| Add/cancel, one price | 14.39-17.58 million ops/s | 0.456-0.670 microseconds | 0 in each run | 0 in each run |
| Add/cancel, 1,024 prices | 6.82-8.91 million ops/s | 1.013-1.403 microseconds | 0 in each run | 0 in each run |
| ITCH add decoding | 35.70-41.40 million messages/s | not measured | 0 in each run | not measured |

Minor faults are the before/after difference in `getrusage(RUSAGE_THREAD)` for
the warmed matching loop. Thus **zero was observed in these 15 measured book
loops**. It is not a claim about startup, whole-process faults, all inputs, or
all environments. Each p99 is computed from one in every 256 synchronous book
operations, including timer overhead. The figures are not pipeline or network
latencies. The initial probe without a pre-touched sample buffer observed eight
minor faults in the first matching loop; touching that buffer removed most of
them, and priming the timer removed the remaining first-use faults. This matters
because instrumentation can otherwise be misattributed to the engine.

The local WSL2 `perf stat` reports `<not supported>` for `cycles`,
`instructions`, `L1-dcache-loads`, `L1-dcache-load-misses`, `LLC-loads`, and
`LLC-load-misses`; the software `minor-faults` counter works. Accordingly, the
claimed 60% cache-miss reduction and 99.8% L1 data-cache hit rate **cannot be
verified on this host**. A 60% reduction would also require a defined baseline
and matched hardware, input, and measurement interval. Throughput and allocation
counts cannot substitute for cache-event counts.

## Windows toolchain issue

Local native `thread_local` access failed with an access violation under ASLR,
including in a minimal standalone reproduction before the book constructor ran.
Linking local validation executables with `-Wl,--disable-dynamicbase` resolved it.
This workaround is intentionally NOT a repository build default: it disables
address randomization. Use a working/up-to-date compiler and linker for ordinary
builds. The measurements above used that local workaround. Static linking did not
resolve the issue. CMake/Google Benchmark and Linux sanitizers were not run locally
because their dependencies/platform were unavailable.

## Reproduce and profile on Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWITH_GOOGLE_BENCHMARK=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 tests/reference.py build/match-engine
./build/stress-benchmark data.csv
./build/benchmark-orderbook --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
perf stat -r 10 -e cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,minor-faults ./build/stress-benchmark data.csv
```

Hardware events depend on the CPU and perf permissions. Do not silently substitute
one cache event for another. The external `perf stat` command covers the entire
process, including setup, warmup, and teardown; it does not isolate the hot loop.
On Linux, the stress program also reports per-thread `getrusage` minor-fault deltas
around each warmed book loop. Those counters are separate from whole-process perf
results. Record CPU, OS, compiler, flags, governor, affinity, input, repetitions,
and exact event definitions with any published comparison.

## Resume claims: supported features versus unverified numbers

Implemented: C++20, three-stage matching pipeline, bounded SPSC and MPMC queue
implementations, thread-local pooled book memory, move semantics, cache-line
alignment, runtime-dispatched AVX2 ITCH decoding, Google Benchmark targets, and
repeatable stress/allocation instrumentation.

The MPMC algorithm's reservation protocol is not a formal lock-free progress
guarantee. ITCH support is the documented order-depth subset, not the entire feed.
No claim is made that cache layout changes have reduced misses by 60% or that
L1 hit rate is 99.8%: those require hardware-counter measurements that are
unavailable in local WSL2. The five WSL2 runs observed zero minor page faults
in each warmed book loop. Sampled synchronous p99 values were below 2.2
microseconds and throughput exceeded 450K operations/s on these workloads,
but neither establishes a universal execution-latency bound or an end-to-end
latency target. Report results with these measurement boundaries.
