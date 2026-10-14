# OrderFlow — Low-Latency Limit Order Book & Matching Engine (C++20)

A single-threaded, deterministic matching engine for **limit, market, IOC and FOK** orders. It has a price-time-priority order book, pre-allocated memory, O(1) cancels, an event-sourced journal with deterministic replay, and a TCP gateway that feeds the engine through a **lock-free SPSC ring buffer**.

[![CI](https://github.com/nasarwaljatin/OrderFlow/actions/workflows/ci.yml/badge.svg)](https://github.com/nasarwaljatin/OrderFlow/actions)

| Metric (10M-command synthetic replay, 1 core) | Result |
|---|---|
| Throughput | **5.3 – 8.0 M commands/sec** |
| Latency per command, p50 / p99 / p99.9 | **~150 ns / ~0.85 µs / ~1.4 µs** |
| Add + cancel (Google Benchmark) | 68 ns per pair → ~30 M ops/sec |
| SPSC ring push + pop | 7.9 ns |
| Tests | **157 GoogleTest cases** (incl. 51 differential-fuzz runs), clean under ASan + UBSan |

<sub>Measured on a shared 2-vCPU Intel Xeon @ 2.10 GHz cloud VM, GCC 13, `-O3`, no CPU pinning. Your laptop will very likely be faster. Re-run `orderflow_latency` and **put your own numbers here**.</sub>

---

## Table of contents
1. [What this is and why it matters](#1-what-this-is-and-why-it-matters)
2. [Quick start](#2-quick-start)
3. [Architecture](#3-architecture)
4. [Matching rules](#4-matching-rules-the-spec)
5. [Design deep-dive](#5-design-deep-dive)
6. [Wire protocol](#6-wire-protocol)
7. [Testing strategy](#7-testing-strategy)
8. [Benchmarks](#8-benchmarks)
9. [Project layout](#9-project-layout)
10. [Build it yourself: a step-by-step roadmap](#10-build-it-yourself-a-step-by-step-roadmap)
11. [Interview questions you should be able to answer](#11-interview-questions-you-should-be-able-to-answer)
12. [Extensions](#12-extensions-ideas-for-v2)

---

## 1. What this is and why it matters

Every exchange (NSE, NASDAQ, Binance) has a **matching engine** at its core. It keeps the **order book** — all outstanding buy (bid) and sell (ask) orders — and when a new order arrives, it decides whether that order trades against existing ones and at what price.

This project builds that core the way trading firms build theirs:

* **Determinism.** The same input always gives the same output. That makes recovery, auditing and testing possible.
* **No heap allocation on the hot path.** Memory is pre-allocated, so latency stays predictable.
* **Measurement first.** Tail latency (p99, p99.9) is reported, not just averages.
* **Correctness proven by a reference model.** The optimised engine is fuzzed against a naive model that is obviously correct.

---

## 2. Quick start

### Prerequisites
* A C++20 compiler (GCC ≥ 11 or Clang ≥ 14), CMake ≥ 3.20, git
* Linux or macOS. On Windows, use WSL2 or Docker.
* GoogleTest and Google Benchmark are downloaded automatically by CMake (pinned versions).

### Build and test
```bash
git clone https://github.com/nasarwaljatin/OrderFlow.git && cd OrderFlow
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure -j      # 157 tests
```

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `ORDERFLOW_BUILD_TESTS` | ON | GoogleTest suite |
| `ORDERFLOW_BUILD_BENCH` | ON | Google Benchmark + latency tool |
| `ORDERFLOW_NATIVE` | OFF | `-march=native` (faster, not portable) |
| `ORDERFLOW_WERROR` | ON | warnings are errors |
| `ORDERFLOW_SANITIZE` | OFF | AddressSanitizer + UndefinedBehaviorSanitizer |

### Run the benchmarks
```bash
./build/orderflow_latency --orders 10000000     # throughput + p50/p99/p99.9
./build/orderflow_bench                         # Google Benchmark micro-benchmarks
```

### Run the server and a client
```bash
# terminal 1
./build/orderflow_server --port 9000 --symbols 4 --journal orders.ofj

# terminal 2: 200k orders, up to 64 in flight, prints round-trip latency
./build/orderflow_client --port 9000 --orders 200000 --window 64

# after stopping the server (Ctrl-C): rebuild the book from the journal
./build/orderflow_replay orders.ofj
```
Or run `scripts/demo.sh`, which does all of the above in one go.

### Docker
```bash
docker compose up --build                  # server on localhost:9000
docker compose run --rm client             # load test against it
```
The Docker build runs the whole test suite. If any test fails, the image is not produced.

---

## 3. Architecture

```
                        orderflow_server process
 ┌──────────┐   TCP   ┌──────────────────┐  SPSC ring<Inbound>  ┌─────────────────────────┐
 │ client A │◀──────▶│                  │────────────────────▶│      Engine thread       │
 └──────────┘         │  Network thread  │                     │                         │
 ┌──────────┐   TCP   │  poll(), non-    │  SPSC ring<Outbound>│  MatchingEngine         │
 │ client B │◀──────▶│  blocking I/O,   │◀────────────────────│   ├─ OrderBook[symbol]  │
 └──────────┘         │  framing         │                     │   ├─ ObjectPool<Order>  │
                      └──────────────────┘   self-pipe wakeup   │   └─ OrderId → Order*   │
                               ▲  ◀──────────────────────────── │  JournalWriter (opt.)   │
                               │                                 └─────────────────────────┘
```

* **The engine thread owns all trading state.** It never touches a socket and never takes a lock. Its only links to the outside world are two lock-free rings.
* **The network thread owns all sockets.** It splits the TCP byte stream into 32-byte frames, pushes `Command`s into the inbound ring, and drains `Event`s from the outbound ring into per-client write buffers.
* **Self-pipe wakeup.** After the engine pushes a batch of events, it writes 1 byte to a pipe that the network thread `poll()`s on. Replies therefore leave immediately instead of waiting for a poll timeout.
* **Ownership.** The gateway remembers which connection owns each live order. Trades go to *both* counterparties, and a client cannot cancel someone else's order.

The core library (`orderflow_core`) has **no threads and no I/O**. That is why it can be unit-tested, fuzzed and benchmarked in isolation.

---

## 4. Matching rules (the spec)

| Rule | Behaviour |
|---|---|
| Priority | **Price first, then time.** Best price matches first. At the same price, the oldest order matches first (FIFO). |
| Trade price | Always the **resting (maker)** order's price. An aggressive buyer at 105 against an ask at 100 trades at 100 (price improvement). |
| **Limit** | Matches as far as its limit price allows. Any remainder **rests** on the book. |
| **Market** | Matches at any price. Any remainder is **cancelled** (`MarketNoLiquidity`). Never rests. |
| **IOC** | Matches up to its limit price. Any remainder is **cancelled** (`IocRemainder`). Never rests. |
| **FOK** | First checks that the *full* quantity is available within its limit. If not, nothing trades and it is cancelled (`FokUnfilled`). Never rests. |
| Cancel | Removes a resting order and reports its remaining quantity. Unknown or already-filled ids are rejected. |
| Ids | Order ids must be unique among *live* orders across all symbols. An id can be reused once its order is gone. |

**Event sequence for a new order:** `Accepted` → zero or more `Trade` → optionally one `Cancelled` (for an IOC/FOK/Market remainder). Validation failures produce a single `Rejected` and change nothing.

Rejection reasons: `InvalidQty` (qty = 0), `InvalidPrice` (≤ 0 for non-market orders), `DuplicateId`, `UnknownSymbol`, `UnknownOrder`, `PoolExhausted`.

---

## 5. Design deep-dive

### 5.1 The order book data structure

```
bids_: std::map<Price, PriceLevel, std::greater<>>      asks_: std::map<Price, PriceLevel, std::less<>>
        begin() = best bid                                       begin() = best ask

  PriceLevel 101 ─▶ [Order#7] ⇄ [Order#12] ⇄ [Order#30]          ◀── intrusive doubly linked FIFO
  PriceLevel 100 ─▶ [Order#3] ⇄ [Order#9]                             (head = oldest = matched first)

  index_: unordered_map<OrderId, Order*>  ──▶ jump straight to any order for O(1) cancel
```

| Operation | Complexity | How |
|---|---|---|
| Best bid / ask | O(1) | `map.begin()` |
| Add at an existing level | O(log L) | `try_emplace` finds the level, then append at the tail |
| Cancel | O(1) find + O(1) unlink (+ O(log L) if the level empties) | hash index → intrusive `prev`/`next` |
| Match one fill | O(1) | pop from the head of the best level |

*L* is the number of distinct price levels, usually in the tens.

**Why intrusive lists?** The `prev`/`next` pointers live *inside* `Order`, so no separate list node is allocated. Given an `Order*` from the hash index, unlinking needs no search at all.

**Why `std::map` and not an array indexed by price?** A flat array (`levels[price - min_price]`) is faster, but it needs a bounded price band and a way to scan for the next non-empty level. `std::map` is simpler and correct for any price. This trade-off comes up in interviews, so see [Extensions](#12-extensions-ideas-for-v2).

### 5.2 Memory: `ObjectPool<Order>`
Every `Order` slot is allocated once at startup. Free slots form a singly linked list threaded *through the unused slots themselves*, so the free list costs zero extra memory. `acquire` and `release` are each one pointer swap. This removes `malloc` from the matching path, along with its unpredictable latency (locks, page faults). The engine also calls `reserve()` on the hash index up front, so a rehash never happens mid-session.

### 5.3 Lock-free SPSC ring buffer
`SpscRing<T>` is a bounded queue for exactly **one producer thread and one consumer thread**:
* The producer writes only `tail_`, and the consumer writes only `head_`. No compare-and-swap is needed.
* The producer writes the slot first, then publishes it with a **release** store of `tail_`. The consumer's **acquire** load of `tail_` guarantees it sees the slot's contents.
* `head_` and `tail_` sit on **separate 64-byte cache lines**. This prevents *false sharing*, where two cores keep invalidating the same cache line.
* Each side caches the other side's index and re-reads the shared atomic only when the ring looks full or empty. This cuts cross-core traffic.
* The capacity is a power of two, so `idx & mask` replaces the slower `idx % capacity`.

### 5.4 Event sourcing and determinism
The engine is a pure function of its input sequence, and it never reads clocks or random numbers. The gateway appends every accepted `Command` to a binary journal (16-byte header, then 32-byte records). Replaying the journal rebuilds the exact same book and the same event stream. `orderflow_replay` prints an FNV-1a hash of the full event stream, so two replays (or two machines) can be compared with one line.

### 5.5 Threading model and back-pressure
* If the inbound ring is full, the network thread stops pulling commands until the engine catches up, and TCP flow control pushes back on the client.
* If the outbound ring is full, the engine waits for the network thread.
* When idle, the engine thread spins briefly, then sleeps 50 µs. With `--busy-spin` it never sleeps, which gives the lowest latency at the cost of 100% of one core. Real trading systems busy-spin on an isolated core.

---

## 6. Wire protocol

All messages are fixed-size, 32 bytes, in host byte order (little-endian on x86 and ARM). A fixed size means framing is trivial: read 32 bytes, `memcpy` into the struct.

**Client → server: `Command`**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type (0 = New, 1 = Cancel) |
| 1 | 1 | side (0 = Buy, 1 = Sell) |
| 2 | 1 | order_type (0 Limit, 1 Market, 2 IOC, 3 FOK) |
| 3 | 1 | padding |
| 4 | 2 | symbol |
| 6 | 2 | padding |
| 8 | 4 | qty |
| 12 | 4 | padding |
| 16 | 8 | order id |
| 24 | 8 | price (integer ticks) |

**Server → client: `Event`**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type (0 Accepted, 1 Rejected, 2 Trade, 3 Cancelled) |
| 1 | 1 | reason |
| 2 | 2 | symbol |
| 4 | 4 | qty |
| 8 | 8 | order id (the taker, for trades) |
| 16 | 8 | maker id (trades only) |
| 24 | 8 | price (trades only) |

Prices are **integer ticks**, never floating point. `0.1 + 0.2 != 0.3` is not acceptable in a ledger.

---

## 7. Testing strategy

```
ctest --output-on-failure            # or: ./build/orderflow_tests --gtest_brief=1
```

| Suite | Count | What it proves |
|---|---|---|
| `BookTest` | 39 | resting, best prices, depth, price-time priority, sweeps, cancels at head/middle/tail, multi-symbol isolation |
| `OrderTypeTest` | 20 | IOC / Market / FOK semantics on both sides, including FOK leaving the book untouched |
| `ValidationTest` + `SmallPoolTest` | 17 | every rejection path, pool exhaustion and slot recycling |
| `ObjectPool` | 7 | allocator correctness under churn |
| `SpscRing` | 8 | FIFO, wrap-around, full/empty, a **2-thread stress test with 2M items** |
| `Journal` + `Workload` | 9 | round-trip, corruption detection, **replay determinism**, rebuilt book matches the live one |
| `GatewayTest` | 6 | real TCP: acks, trades reaching both sides, ownership checks, split frames, pipelining |
| `DifferentialFuzz` | 51 | 50 seeds × 5k commands + one 100k-command run against `ReferenceEngine` |

**Differential fuzzing** is the most valuable test here. `tests/reference_book.hpp` is a deliberately naive engine: one flat vector, best order found by linear scan, simple enough to check by eye. Random adversarial command streams go to both engines: a tight price band (constant crossing), every order type, zero quantities, bad prices, unknown symbols, duplicate ids, and cancels of dead ids. After every single command, the event streams must be identical.

*Sanity check performed:* deliberately breaking the cancel-unlink code made all 51 fuzz tests fail immediately.

Run the sanitizer build before every release:
```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DORDERFLOW_SANITIZE=ON -DORDERFLOW_BUILD_BENCH=OFF
cmake --build build-asan -j && ./build-asan/orderflow_tests --gtest_brief=1
```

---

## 8. Benchmarks

### 8.1 Engine latency and throughput (`orderflow_latency`)
The workload is 10M synthetic commands: about 58% limit orders, 6% IOC, 3% market and 3% FOK, with the rest cancels, across 4 symbols. It ends with about 60k resting orders. Nearly a third of the commands are rejected, because many cancels target orders that have already filled. Real exchanges see this too.

```
throughput: 6.68 M commands/s  (1.497 s total)
all commands  n=10000000  mean=241ns  p50=159  p90=457  p99=897  p99.9=3419  max=2804237 (ns)
```
Three runs gave 5.3–8.0 M commands/s and p99 between 0.81 and 0.90 µs. The **max** (milliseconds) comes from OS preemption and page faults on a shared VM. Pin the thread to an isolated core (`taskset -c 3`) to shrink it.

> Methodology: Pass 1 has no timing inside the loop, so it measures pure throughput. Pass 2 timestamps every `process()` call with `steady_clock`, which adds roughly 20–40 ns per sample, so the latencies above slightly *over*-state the true cost.

### 8.2 Micro-benchmarks (`orderflow_bench`, Google Benchmark)
```
BM_AddCancel                 67.8 ns   items_per_second=29.7M/s
BM_AddAndMatch               92.3 ns   items_per_second=21.7M/s
BM_SweepLevels/1              316 ns
BM_SweepLevels/10             735 ns   items_per_second=13.6M/s (fills)
BM_SweepLevels/100           3701 ns   items_per_second=27.0M/s (fills)
BM_ReplayWorkload/1000000     183 ms   items_per_second=5.50M/s
BM_SpscPushPop               7.89 ns
```

### 8.3 End-to-end over TCP (`orderflow_client`, loopback)
| Mode | Throughput | Round-trip p50 | Round-trip p99 |
|---|---|---|---|
| window 64 (pipelined) | ~220k cmd/s | 203 µs | 371 µs |
| window 1, default (sleeping engine) | ~8k cmd/s | 138 µs | 210 µs |
| window 1, `--busy-spin` | — | 25 µs | 83 µs |

The round trip is dominated by the kernel TCP stack and thread wake-ups, not by matching, which takes under 1 µs. Production systems use kernel-bypass networking (DPDK, Solarflare/OpenOnload) to remove that cost.

---

## 9. Project layout
```
OrderFlow/
├── include/orderflow/
│   ├── types.hpp            # Command, Event, enums - 32-byte PODs
│   ├── object_pool.hpp      # fixed-capacity free-list allocator
│   ├── order_book.hpp       # single-symbol book interface
│   ├── matching_engine.hpp  # multi-symbol engine + validation
│   ├── event_sink.hpp       # output interface: Vector/Counting/Hashing/Tee sinks
│   ├── spsc_ring.hpp        # lock-free single-producer/single-consumer queue
│   ├── journal.hpp          # binary journal writer/reader + replay()
│   ├── workload.hpp         # reproducible synthetic order-flow generator
│   └── latency_recorder.hpp # percentile reporting
├── src/                     # order_book.cpp, matching_engine.cpp, journal.cpp
├── gateway/                 # gateway.hpp/.cpp (network + engine threads), main.cpp
├── tools/                   # client.cpp (load + RTT), gen_workload.cpp, replay.cpp
├── tests/                   # 8 test files + reference_book.hpp + test_helpers.hpp
├── bench/                   # bench_engine.cpp (Google Benchmark), latency_bench.cpp
├── scripts/demo.sh
├── Dockerfile, docker-compose.yml
└── .github/workflows/ci.yml # gcc + clang, sanitizers, benchmarks, docker
```

---

## 10. Build it yourself: a step-by-step roadmap

This code is a complete reference. You will learn far more, and interview far better, by **rebuilding it in this order and committing after each phase.** Each phase ends in something you can run.

| Phase | Build | Files | Done when |
|---|---|---|---|
| **1. Types** (½ day) | `Command`, `Event`, enums. Make them 32-byte PODs and add `static_assert`s on size. | `types.hpp` | it compiles; you can explain why prices are integers |
| **2. Naive engine** (1 day) | Write `ReferenceEngine` *first*: a vector and linear scans. Implement all the rules in section 4. | `tests/reference_book.hpp`, `test_helpers.hpp` | hand-written tests for each rule pass |
| **3. Real book** (2 days) | `std::map` levels, intrusive FIFO, hash index, `add`/`match`/`cancel`. Use `new`/`delete` for now. | `order_book.*`, `matching_engine.*` | the same tests pass against the real engine |
| **4. Differential fuzz** (½ day) | Random commands into both engines, assert identical events after each one. Then break your book on purpose and watch the fuzzer catch it. | `test_fuzz.cpp` | 50 seeds green |
| **5. Object pool** (½ day) | Replace `new`/`delete`. Pre-reserve the index. | `object_pool.hpp` | the pool tests and fuzz still pass |
| **6. Benchmarks** (1 day) | Workload generator, `orderflow_latency`, Google Benchmark. Record numbers **before and after** phase 5, because that before/after comparison is a strong interview story. | `workload.hpp`, `bench/` | you have a table of p50/p99 |
| **7. Journal + replay** (½ day) | Append commands, replay, hash events. | `journal.*`, `tools/replay.cpp` | two replays print the same hash |
| **8. SPSC ring** (1 day) | Start with a mutex queue, then make it lock-free. Write the 2-thread stress test. | `spsc_ring.hpp` | stress test passes under ASan |
| **9. TCP gateway** (2 days) | `poll()` loop, framing, two threads, self-pipe wakeup, ownership map. | `gateway/`, `tools/client.cpp` | the `GatewayTest` suite passes and the client prints RTT |
| **10. Ship it** (½ day) | CMake options, Dockerfile, CI, README with *your* measured numbers. | root files | CI badge is green |

**Rules for yourself:**
1. Never paste a number into your resume that you didn't measure. Use the numbers `orderflow_latency` prints on your machine.
2. Be able to draw section 3 and section 5.1 on a whiteboard from memory.
3. Run tests under `-DORDERFLOW_SANITIZE=ON` at least once per phase.

---

## 11. Interview questions you should be able to answer

* Why must trades execute at the maker's price? What would break if they executed at the taker's?
* Walk through a FOK order that would only partially fill. Why do we pre-check instead of matching and then rolling back?
* What is the time complexity of a cancel? Why do we need both the hash map *and* intrusive pointers?
* Why does `std::map` give us the best bid in O(1)? When would you switch to an array of price levels?
* What does `memory_order_release` on `tail_` guarantee? What could go wrong with `relaxed`?
* What is false sharing, and how did you avoid it? How would you prove it matters? (Remove the `alignas` and benchmark.)
* Why is the engine single-threaded? How would you scale to 1,000 symbols? (Shard symbols across engine threads, one ring per shard.)
* Why does p99.9 matter more than the mean in trading?
* How does the journal give you crash recovery? What is missing for exactly-once semantics? (fsync policy, sequence numbers, snapshots.)
* Where does the 25 µs TCP round trip go, if matching takes 0.2 µs?

---

## 12. Extensions (ideas for v2)
* **Flat price-level array**: replace `std::map` with `levels[price - base]` plus a best-price cursor. This gives true O(1) adds and is the most common next step in real engines.
* **Market-data feed**: publish top-of-book or L2 depth updates over UDP multicast.
* **Order modify** (cancel/replace), keeping or losing queue priority by exchange rules.
* **Self-trade prevention**: stop a client's buy from matching its own resting sell.
* **Snapshots**: periodically serialise the book so replay starts from a snapshot instead of from zero.
* **Core pinning and `isolcpus`**, huge pages, and an HDR histogram for latency recording.
* **Python bindings** (pybind11) so the QuantBacktester project can backtest against a real matching engine.

---

## License
MIT. See [LICENSE](LICENSE).
