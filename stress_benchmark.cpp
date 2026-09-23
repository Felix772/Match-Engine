#include "orderbook.h"
#include "pipeline.h"
#include "itch.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#endif
// Counts C++ allocations during the single-threaded measured loops, including aligned PMR upstream requests.
static thread_local bool count_allocations = false;
static thread_local std::size_t allocations = 0;
void *operator new(std::size_t n) { if(count_allocations) ++allocations; if(auto p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t n,std::align_val_t a) {
  if(count_allocations) ++allocations;
  auto alignment=static_cast<std::size_t>(a);
#ifdef _WIN32
  auto p=_aligned_malloc(n?n:1,alignment);
#else
  void *p=nullptr; if(posix_memalign(&p,alignment,n?n:1)!=0) p=nullptr;
#endif
  if(!p) throw std::bad_alloc();
  return p;
}
void *operator new[](std::size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void *p,std::align_val_t) noexcept {
#ifdef _WIN32
  _aligned_free(p);
#else
  std::free(p);
#endif
}
void operator delete[](void *p,std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete(void *p,std::size_t,std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete[](void *p,std::size_t,std::align_val_t a) noexcept { ::operator delete(p,a); }
using Clock=std::chrono::steady_clock;
int main(int argc,char **argv) {
  try {
    constexpr std::size_t count=1000000;
    std::cout << "compiler=";
#ifdef __VERSION__
    std::cout << __VERSION__;
#elif defined(_MSC_VER)
    std::cout << "MSVC " << _MSC_VER;
#else
    std::cout << "unknown";
#endif
    std::cout << ", avx2=" << itch::avx2_available() << '\n';
    for(int workload=0;workload<3;++workload) {
      OrderBook book;
      std::vector<Order> events; events.reserve(count);
      for(std::size_t i=0;i<count;++i) {
        Order o{}; o.ts=i; o.trader[0]='a'; o.qty=1;
        const auto block=i/2048, offset=i%2048;
        o.order_id=block*1024+offset%1024;
        o.price=workload==2?static_cast<std::uint32_t>(100+offset%1024):100;
        if(offset<1024) { o.type='A'; o.side='B'; }
        else if(workload==0) { o.type='A'; o.side='S'; o.order_id=count+i; }
        else o.type='C';
        events.push_back(o);
      }
      for(const auto &o:events) book.process(o);
      book.clear();
      std::vector<double> latency; latency.reserve(count/256+1);
      allocations=0; count_allocations=true;
#ifdef __linux__
      rusage before_usage{}, after_usage{};
      getrusage(RUSAGE_THREAD,&before_usage);
#endif
      const auto start=Clock::now();
      for(std::size_t i=0;i<count;++i) {
        if(i%256==0) {
          auto before=Clock::now(); book.process(events[i]);
          latency.push_back(std::chrono::duration<double,std::nano>(Clock::now()-before).count());
        } else book.process(events[i]);
      }
      const auto seconds=std::chrono::duration<double>(Clock::now()-start).count();
      count_allocations=false;
#ifdef __linux__
      getrusage(RUSAGE_THREAD,&after_usage);
#endif
      std::sort(latency.begin(),latency.end());
      auto pct=[&](double p) { return latency[static_cast<std::size_t>((latency.size()-1)*p)]; };
      std::cout << "workload=" << (workload==0?"matching":workload==1?"cancel_fifo":"cancel_1024_prices")
                << ", operations=" << count << ", ops_per_second=" << std::fixed << std::setprecision(0) << count/seconds
                << ", sampled_p50_ns=" << pct(.5) << ", sampled_p95_ns=" << pct(.95)
                << ", sampled_p99_ns=" << pct(.99) << ", cpp_heap_allocations=" << allocations << '\n';
#ifdef __linux__
      std::cout << "hot_loop_minor_faults=" << after_usage.ru_minflt-before_usage.ru_minflt << '\n';
#endif
      if(allocations) return 2;
    }
    // Repeated borrowed-packet parsing; checksum prevents dead-code removal.
    std::array<std::uint8_t,36> packet{}; packet[0]='A'; packet[19]='B'; packet[23]=1; packet[35]=100;
    itch::Message message;
    std::uint64_t checksum=0;
    allocations=0; count_allocations=true;
    auto start=Clock::now();
    for(std::size_t i=0;i<count;++i) { packet[18]=static_cast<std::uint8_t>(i); if(itch::parse(packet,message)!=itch::Status::ok) return 3; checksum+=message.id+message.price; }
    auto seconds=std::chrono::duration<double>(Clock::now()-start).count();
    count_allocations=false;
    std::cout << "workload=itch_parse, ops_per_second=" << count/seconds << ", checksum=" << checksum << ", cpp_heap_allocations=" << allocations << '\n';
    if(allocations) return 2;
    if(argc==2) {
      start=Clock::now(); auto stats=run_csv_pipeline(argv[1]);
      seconds=std::chrono::duration<double>(Clock::now()-start).count();
      std::cout << "workload=csv_pipeline_including_startup, operations=" << stats.orders << ", trades=" << stats.trades << ", ops_per_second=" << stats.orders/seconds << '\n';
    }
  } catch(const std::exception &e) { count_allocations=false; std::cerr << e.what() << '\n'; return 1; }
}
