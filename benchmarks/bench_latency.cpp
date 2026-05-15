#include "xcache/xcache.h"
#include <benchmark/benchmark.h>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static std::string tmp_path() {
    return "/tmp/xc_bench_" + std::to_string(getpid()) + "_"
           + std::to_string(std::rand());
}

static void cleanup(const std::string& p) {
    unlink((p + ".dat").c_str());
    unlink((p + ".idx").c_str());
}

// ── put ────────────────────────────────────────────────────────────

static void BM_Put(benchmark::State& state) {
    std::vector<std::string> paths;
    for (auto _ : state) {
        state.PauseTiming();
        auto p  = tmp_path();
        paths.push_back(p);
        xcache::XCache kv(p);
        state.ResumeTiming();
        auto n = state.range(0);
        for (int64_t i = 0; i < n; ++i)
            kv.put_string("k_" + std::to_string(i), "v_" + std::to_string(i));
    }
    for (auto& p : paths) cleanup(p);
    state.counters["Ops/s"] = benchmark::Counter(
        state.iterations() * state.range(0), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Put)->Arg(1000)->Arg(10000);

static void BM_PutPerOp(benchmark::State& state) {
    auto p = tmp_path();
    xcache::XCache kv(p);
    int64_t i = 0;
    for (auto _ : state)
        kv.put_string("k_" + std::to_string(i++), "v");
    cleanup(p);
    state.counters["Ops/s"] = benchmark::Counter(
        state.iterations(), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_PutPerOp)->Iterations(50000);

// ── get / exists / remove ─────────────────────────────────────────

static void BM_GetHit(benchmark::State& state) {
    auto p = tmp_path();
    xcache::XCache kv(p);
    for (int i = 0; i < 50000; ++i)
        kv.put_string("k_" + std::to_string(i), "v_" + std::to_string(i));
    int64_t i = 0;
    for (auto _ : state) {
        std::string s;
        benchmark::DoNotOptimize(kv.get_string("k_" + std::to_string(i++ % 50000), &s));
    }
    cleanup(p);
    state.counters["Ops/s"] = benchmark::Counter(
        state.iterations(), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_GetHit)->Iterations(50000);

static void BM_GetMiss(benchmark::State& state) {
    auto p = tmp_path();
    xcache::XCache kv(p);
    for (auto _ : state) {
        std::string s;
        benchmark::DoNotOptimize(kv.get_string("nope", &s));
    }
    cleanup(p);
    state.counters["Ops/s"] = benchmark::Counter(
        state.iterations(), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_GetMiss)->Iterations(50000);

static void BM_Exists(benchmark::State& state) {
    auto p = tmp_path();
    xcache::XCache kv(p);
    kv.put_string("hello", "world");
    for (auto _ : state)
        benchmark::DoNotOptimize(kv.exists("hello"));
    cleanup(p);
    state.counters["Ops/s"] = benchmark::Counter(
        state.iterations(), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Exists)->Iterations(50000);

static void BM_Remove(benchmark::State& state) {
    auto p = tmp_path();
    xcache::XCache kv(p);
    int64_t i = 0;
    for (auto _ : state) {
        auto key = "k_" + std::to_string(i);
        kv.put_string(key, "v");
        benchmark::DoNotOptimize(kv.remove(key));
        ++i;
    }
    cleanup(p);
    state.counters["Ops/s"] = benchmark::Counter(
        state.iterations(), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Remove)->Iterations(50000);

// ── concurrent (persistent threads) ────────────────────────────────

template <typename F>
struct Workers {
    std::vector<std::thread> threads;
    std::unique_ptr<std::atomic<int64_t>[]> cnt;
    std::atomic<bool> done{false};
    int n_;

    Workers(int n, F work) : cnt(std::make_unique<std::atomic<int64_t>[]>(n)), n_(n) {
        for (int i = 0; i < n_; ++i) cnt[i].store(0, std::memory_order_relaxed);
        for (int t = 0; t < n_; ++t)
            threads.emplace_back([this, t, work] {
                int64_t local = 0;
                while (!done.load(std::memory_order_relaxed)) {
                    work(t, local);
                    ++local;
                    if ((local & 0x3ff) == 0)
                        cnt[t].store(local, std::memory_order_relaxed);
                }
                cnt[t].store(local, std::memory_order_relaxed);
            });
    }

    int64_t total() const {
        int64_t sum = 0;
        for (int i = 0; i < n_; ++i)
            sum += cnt[i].load(std::memory_order_relaxed);
        return sum;
    }

    ~Workers() {
        done.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();
    }
};

static void BM_ConcurrentPutPersistent(benchmark::State& state) {
    auto p = tmp_path();
    xcache::XCache kv(p, 64UL << 20);
    int n = state.range(0);
    Workers workers(n, [&kv](int t, int64_t local) {
        kv.put_string("k_c_" + std::to_string((t << 20) | (local & 0xfffff)), "v");
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto start_cnt = workers.total();
    for (auto _ : state)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto end_cnt = workers.total();
    state.SetItemsProcessed(end_cnt - start_cnt);
    cleanup(p);
}
BENCHMARK(BM_ConcurrentPutPersistent)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();

static void BM_ConcurrentMixedPersistent(benchmark::State& state) {
    auto p = tmp_path();
    xcache::XCache kv(p, 64UL << 20);
    for (int i = 0; i < 50000; ++i)
        kv.put_string("k_" + std::to_string(i), "v");
    int n = state.range(0);
    Workers workers(n, [&kv](int, int64_t) {
        thread_local std::mt19937 rng(std::random_device{}());
        auto k = "k_" + std::to_string(rng() % 50000);
        switch (rng() % 4) {
            case 0: kv.put_string(k, "v"); break;
            case 1: { std::string s; kv.get_string(k, &s); } break;
            case 2: kv.exists(k); break;
            case 3: kv.remove(k); break;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto start_cnt = workers.total();
    for (auto _ : state)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto end_cnt = workers.total();
    state.SetItemsProcessed(end_cnt - start_cnt);
    cleanup(p);
}
BENCHMARK(BM_ConcurrentMixedPersistent)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();

BENCHMARK_MAIN();
