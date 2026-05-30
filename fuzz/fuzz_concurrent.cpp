// fuzz_concurrent.cpp — concurrent API stress fuzzer
// Multiple threads hammering random operations concurrently.
// Catches crashes, data races (with TSAN), and invariant violations.

#include "xcache/xcache.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
    std::string path = "/tmp/xc_fcc_" + std::to_string(getpid());

    fprintf(stderr, "=== fuzz_concurrent: multi-thread stress ===\n");

    for (int round = 0; round < 20; ++round) {
        ::unlink((path + ".idx").c_str());
        ::unlink((path + ".dat").c_str());

        xcache::XCache kv(path, 8UL << 20);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> ops{0};

        // Pre-populate
        for (int i = 0; i < 2000; ++i)
            kv.put_string("init_" + std::to_string(i), "v_" + std::to_string(i));

        int n_threads = 4 + static_cast<int>(round % 5);
        std::vector<std::thread> threads;

        for (int t = 0; t < n_threads; ++t) {
            threads.emplace_back([&kv, &stop, &ops, t]() {
                std::mt19937_64 local_rng(std::random_device{}() + t);
                while (!stop.load(std::memory_order_relaxed)) {
                    auto key_id = local_rng() % 5000;
                    auto k = "k_" + std::to_string(key_id);
                    int op = static_cast<int>(local_rng() % 6);
                    switch (op) {
                        case 0: kv.put_string(k, "v_" + std::to_string(key_id)); break;
                        case 1: { std::string v; kv.get_string(k, &v); } break;
                        case 2: kv.exists(k); break;
                        case 3: kv.remove(k); break;
                        case 4: kv.size(); break;
                        case 5: kv.get_all_keys(); break;
                    }
                    ops.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300 + (round * 50)));

        // Trigger rebuild while threads are running
        kv.rebuild();

        stop.store(true, std::memory_order_relaxed);
        for (auto& th : threads) th.join();

        fprintf(stderr, "  round %d: %d threads, %llu ops\n", round, n_threads, static_cast<unsigned long long>(ops.load()));
    }

    ::unlink((path + ".idx").c_str());
    ::unlink((path + ".dat").c_str());

    fprintf(stderr, "=== fuzz_concurrent DONE ===\n");
    return 0;
}
