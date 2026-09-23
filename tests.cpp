#include "orderbook.h"
#include "pipeline.h"
#include "itch.h"
#include "ring_buffer.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <filesystem>
#define CHECK(x) do { if (!(x)) throw std::runtime_error("check failed: " #x); } while(false)
static Order order(const char *s) { Order o; CHECK(parseLine(s, o)); return o; }
static void collect(void *p, const Trade &t) { static_cast<std::vector<Trade> *>(p)->push_back(t); }
static void put(std::vector<std::uint8_t> &p, std::size_t offset, std::size_t n, std::uint64_t v) {
  CHECK(offset <= p.size() && n <= p.size() - offset);
  while (n) { p[offset + --n] = static_cast<std::uint8_t>(v); v >>= 8; }
}
static std::vector<std::uint8_t> packet(char type, std::size_t size) {
  std::vector<std::uint8_t> p(size); p[0] = type;
  put(p, 1, 2, 7); put(p, 5, 6, 0x010203040506ULL);
  if(size>=19) put(p, 11, 8, 0x0102030405060708ULL);
  return p;
}
static void test_book() {
  OrderBook b; std::vector<Trade> trades;
  b.process(order("A,1,1,S,99,5,a")); b.process(order("A,2,2,S,99,5,b"));
  b.process(order("A,3,3,S,100,5,c"));
  b.process(order("A,4,4,B,100,12,d"), collect, &trades);
  CHECK(trades.size() == 3 && trades[0].sell_id == 1 && trades[1].sell_id == 2);
  CHECK(trades[2].price == 100 && trades[2].qty == 2 && b.find(3)->qty == 3);
  bool rejected = false;
  try { b.process(order("A,5,3,B,100,5,e")); } catch (const std::invalid_argument &) { rejected = true; }
  CHECK(rejected && b.find(3)->qty == 3);
  CHECK(b.cancel(3) && !b.cancel(3) && !b.size());
  b.process(order("A,1,5,B,101,2,a")); b.process(order("A,2,6,B,100,2,b"));
  trades.clear(); b.process(order("A,3,7,S,99,4,c"), collect, &trades);
  CHECK(trades.size() == 2 && trades[0].price == 101 && trades[1].price == 100 && !b.size());
  Order o;
  for (auto s : {"", ",1,1", "A,1,1,,100,1,a", "Q,1,1,B,100,1,a", "A,1,1,B,1junk,1,a",
                 "A,1,1,B,100,-1,a", "A,1,1,B,100,0,a", "C,1,1,extra", "A,1,1,B,100,4294967296,a"}) CHECK(!parseLine(s, o));
  CHECK(parseLine("C,18446744073709551615,18446744073709551615\r", o));
  CHECK(!parseLine("C,18446744073709551616,1", o));
  bool exhausted = false;
  try { OrderBook tiny(1024); } catch (const std::bad_alloc &) { exhausted = true; }
  CHECK(exhausted);
}
static void test_itch() {
  OrderBook b; itch::Message m;
  auto a = packet('A', 36); a[19] = 'B'; put(a,20,4,100); put(a,32,4,123456);
  CHECK(itch::parse(a,m) == itch::Status::ok);
  CHECK(m.id == 0x0102030405060708ULL && m.timestamp == 0x010203040506ULL && m.shares == 100 && m.price == 123456);
  CHECK(m.stock.data() == reinterpret_cast<const char *>(a.data()+24));
  CHECK(!itch::apply(b,m,8) && itch::apply(b,m,7));
  for (std::size_t n = 0; n < a.size(); ++n) CHECK(itch::parse({a.data(),n},m) == itch::Status::malformed);
  auto f = packet('F',40); f[19]='S'; put(f,11,8,2); put(f,20,4,20); put(f,32,4,1);
  CHECK(itch::parse(f,m) == itch::Status::ok && m.attribution.size() == 4);
  itch::apply(b,m,7); CHECK(b.size()==2); // Crossed feed adds must NOT match.
  for (char t : {'E','C','X'}) {
    auto p = packet(t, t=='E'?31:t=='C'?36:23); put(p,19,4,10);
    if (t=='C') p[31]='Y';
    CHECK(itch::parse(p,m)==itch::Status::ok); itch::apply(b,m,7);
  }
  CHECK(b.find(0x0102030405060708ULL)->qty==70);
  auto u=packet('U',35); put(u,19,8,3); put(u,27,4,40); put(u,31,4,999);
  CHECK(itch::parse(u,m)==itch::Status::ok); itch::apply(b,m,7);
  CHECK(!b.find(0x0102030405060708ULL) && b.find(3)->qty==40 && b.find(3)->price==999);
  auto d=packet('D',19); put(d,11,8,3);
  CHECK(itch::parse(d,m)==itch::Status::ok); itch::apply(b,m,7); CHECK(!b.find(3));
  bool missing=false; try { itch::apply(b,m,7); } catch (const std::runtime_error &) { missing=true; } CHECK(missing);
  for (auto [type,length] : {std::pair{'F',40}, {'E',31}, {'C',36}, {'X',23}, {'D',19}, {'U',35}}) {
    auto p=packet(type,length);
    for(std::size_t n=0;n<p.size();++n) CHECK(itch::parse({p.data(),n},m)==itch::Status::malformed);
    p.push_back(0); CHECK(itch::parse(p,m)==itch::Status::malformed);
  }
  auto invalid=a; invalid[19]='Q'; CHECK(itch::parse(invalid,m)==itch::Status::malformed);
  invalid=a; put(invalid,20,4,0); CHECK(itch::parse(invalid,m)==itch::Status::malformed);
  invalid=packet('Z',11); CHECK(itch::parse(invalid,m)==itch::Status::unsupported);
  const auto file=std::filesystem::temp_directory_path()/"match-engine-itch-test.bin";
  { std::ofstream stream(file,std::ios::binary); stream.put(0); stream.put(36);
    stream.write(reinterpret_cast<const char *>(a.data()),a.size()); }
  CHECK(itch::replay(file.string().c_str(),7)==1);
  { std::ofstream stream(file,std::ios::binary); stream.put(0); stream.put(36); stream.put('A'); }
  bool truncated=false;
  try { itch::replay(file.string().c_str(),7); } catch(const std::runtime_error &) { truncated=true; }
  CHECK(truncated); std::filesystem::remove(file);
}
static void test_queues() {
  SpscRing<std::size_t, 8> q; std::size_t v;
  CHECK(!q.try_pop(v)); for (std::size_t i=0;i<8;++i) CHECK(q.try_push(i)); CHECK(!q.try_push(9));
  for (std::size_t i=0;i<8;++i) { CHECK(q.try_pop(v)); CHECK(v==i); }
  std::atomic<bool> good{true};
  std::jthread producer([&] { for(std::size_t i=0;i<100000;++i) while(!q.try_push(i)) std::this_thread::yield(); });
  for(std::size_t i=0;i<100000;++i) { while(!q.try_pop(v)) std::this_thread::yield(); CHECK(v==i); } producer.join();
  MpmcRing<std::size_t, 64> multi;
  constexpr std::size_t count=40000;
  std::vector<std::atomic<unsigned>> seen(count);
  std::atomic<std::size_t> consumed{0};
  std::vector<std::jthread> workers;
  for(std::size_t p=0;p<4;++p) workers.emplace_back([&,p] {
    for(std::size_t i=p;i<count;i+=4) while(!multi.try_push(i)) std::this_thread::yield();
  });
  for(int c=0;c<4;++c) workers.emplace_back([&] {
    std::size_t item;
    while(consumed.load()<count) {
      if(multi.try_pop(item)) {
        if(item>=count) good=false; else if(seen[item].fetch_add(1)!=0) good=false;
        ++consumed;
      } else std::this_thread::yield();
    }
  });
  workers.clear(); CHECK(good && consumed==count); for(auto &n:seen) CHECK(n==1);
}
static void test_pipeline() {
  const auto path = std::filesystem::temp_directory_path() / "match-engine-pipeline-test.csv";
  { std::ofstream f(path); for(int i=0;i<20000;++i) f << "A," << i << ',' << i << ',' << (i%2?'S':'B') << ",100,1,a\n"; }
  std::vector<Trade> trades;
  auto stats=run_csv_pipeline(path.string().c_str(),collect,&trades);
  CHECK(stats.orders==20000 && stats.trades==10000 && trades.size()==10000);
  for(std::size_t i=0;i<trades.size();++i) CHECK(trades[i].buy_id==2*i && trades[i].sell_id==2*i+1);
  bool caught=false;
  try { run_csv_pipeline(path.string().c_str(), [](void *,const Trade &) { throw std::runtime_error("sink failure"); }); }
  catch(const std::runtime_error &) { caught=true; } CHECK(caught);
  { std::ofstream f(path); f << "A,1,1,B,100,1,a\ninvalid\n"; }
  caught=false; try { run_csv_pipeline(path.string().c_str()); } catch(const std::runtime_error &) { caught=true; } CHECK(caught);
  { std::ofstream f(path); f << "A,1,1,B,100,1,a\nA,2,1,B,100,1,a\n"; }
  caught=false; try { run_csv_pipeline(path.string().c_str()); } catch(const std::invalid_argument &) { caught=true; } CHECK(caught);
  { std::ofstream f(path); }
  CHECK(run_csv_pipeline(path.string().c_str()).orders==0);
  std::filesystem::remove(path);
}
int main() {
  try { std::cerr << "book\n"; test_book(); std::cerr << "itch\n"; test_itch(); std::cerr << "queues\n"; test_queues(); std::cerr << "pipeline\n"; test_pipeline(); std::cout << "All tests passed; AVX2=" << itch::avx2_available() << '\n'; }
  catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

