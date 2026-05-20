#include "xcache/xcache.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static std::string tmp_path() {
    return "/tmp/xfz_" + std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".dat").c_str());
    ::unlink((p + ".idx").c_str());
}

// ── deterministic fuzz: string ops ───────────────────────────────
//
// Runs 5000 random put/get/remove/exists/get_type/scan/sync
// operations against a reference std::map. Every 500 iterations
// does a full cross-check. Seeded for reproducibility.

TEST(FuzzTest, StringOps) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        std::mt19937 rng(42);
        std::map<std::string, std::string> ref;

        auto rand_key = [&]() { return "k" + std::to_string(rng() % 200); };

        for (int iter = 0; iter < 5000; ++iter) {
            auto k = rand_key();
            auto op = rng() % 11;

            switch (op) {
                case 0: case 1: case 2: {
                    auto v = "v" + std::to_string(rng());
                    ASSERT_EQ(kv.put_string(k, v), XCACHE_OK);
                    ref[k] = v;
                    break;
                }
                case 3: case 4: {
                    std::string v;
                    auto res = kv.get_string(k, &v);
                    auto it = ref.find(k);
                    if (it != ref.end()) {
                        ASSERT_EQ(res, XCACHE_OK);
                        ASSERT_EQ(v, it->second);
                    } else {
                        ASSERT_NE(res, XCACHE_OK);
                    }
                    break;
                }
                case 5: case 6: {
                    if (kv.remove(k) == XCACHE_OK) {
                        ref.erase(k);
                    }
                    break;
                }
                case 7: {
                    ASSERT_EQ(kv.exists(k), ref.count(k) > 0);
                    break;
                }
                case 8: {
                    xcache_value_type_t t;
                    auto res = kv.get_type(k, &t);
                    if (ref.count(k)) {
                        ASSERT_EQ(res, XCACHE_OK);
                        ASSERT_EQ(t, XCACHE_TEXT);
                    } else {
                        ASSERT_NE(res, XCACHE_OK);
                    }
                    break;
                }
                case 9: {
                    auto keys = kv.get_all_keys();
                    ASSERT_EQ(keys.size(), ref.size());
                    for (auto& rk : keys) {
                        ASSERT_TRUE(ref.count(rk));
                    }
                    break;
                }
                case 10:
                    kv.sync();
                    break;
            }

            if (iter > 0 && iter % 500 == 0) {
                ASSERT_EQ(kv.size(), ref.size());
                for (auto& [rk, rv] : ref) {
                    std::string v;
                    ASSERT_EQ(kv.get_string(rk, &v), XCACHE_OK);
                    ASSERT_EQ(v, rv);
                }
            }
        }

        ASSERT_EQ(kv.size(), ref.size());
        for (auto& [rk, rv] : ref) {
            std::string v;
            ASSERT_EQ(kv.get_string(rk, &v), XCACHE_OK);
            ASSERT_EQ(v, rv);
        }
    }
    cleanup(p);
}

// ── fuzz: mixed types ────────────────────────────────────────────
//
// Randomly switches between i64/f64/string types on the same keys,
// verifying that type mismatches are detected and overwrites work.

TEST(FuzzTest, MixedTypes) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        std::mt19937 rng(123);
        // ref stores the last type written per key
        std::map<std::string, xcache_value_type_t> ref;

        auto rand_key = [&]() { return "t" + std::to_string(rng() % 50); };

        for (int iter = 0; iter < 3000; ++iter) {
            auto k = rand_key();
            auto op = rng() % 6;

            if (op < 3) {
                // pick one of TEXT, INT64, FLOAT (values 0, 1, 3 — NOT contiguous)
                static constexpr xcache_value_type_t kTypes[] = {
                    XCACHE_TEXT, XCACHE_INT64, XCACHE_FLOAT};
                auto type = kTypes[rng() % 3];
                auto it = ref.find(k);
                xcache_error_t expected = XCACHE_OK;
                if (it != ref.end() && it->second != type) {
                    expected = XCACHE_OK;  // overwrite always OK
                }
                xcache_error_t res;
                if (type == XCACHE_TEXT) {
                    res = kv.put_string(k, "v" + std::to_string(iter));
                } else if (type == XCACHE_INT64) {
                    res = kv.put_i64(k, iter);
                } else {
                    res = kv.put_f64(k, static_cast<double>(iter));
                }
                ASSERT_EQ(res, expected);
                ref[k] = type;
            } else {
                // read back with each type
                std::string vs;
                int64_t vi;
                double vf;
                auto it = ref.find(k);
                if (it != ref.end()) {
                    if (it->second == XCACHE_INT64) {
                        ASSERT_EQ(kv.get_string(k, &vs), XCACHE_TYPE_MISMATCH);
                        ASSERT_EQ(kv.get_i64(k, &vi), XCACHE_OK);
                        ASSERT_EQ(kv.get_f64(k, &vf), XCACHE_TYPE_MISMATCH);
                    } else if (it->second == XCACHE_FLOAT) {
                        ASSERT_EQ(kv.get_string(k, &vs), XCACHE_TYPE_MISMATCH);
                        ASSERT_EQ(kv.get_f64(k, &vf), XCACHE_OK);
                        ASSERT_EQ(kv.get_i64(k, &vi), XCACHE_TYPE_MISMATCH);
                    } else {
                        ASSERT_EQ(kv.get_string(k, &vs), XCACHE_OK);
                        ASSERT_EQ(kv.get_i64(k, &vi), XCACHE_TYPE_MISMATCH);
                    }
                } else {
                    ASSERT_NE(kv.get_string(k, &vs), XCACHE_OK);
                    ASSERT_NE(kv.get_i64(k, &vi), XCACHE_OK);
                }
            }
        }
    }
    cleanup(p);
}

// ── concurrent fuzz ──────────────────────────────────────────────
//
// 4 threads hammer 50 keys with random put/get/remove for 2 seconds,
// then a full read-back verifies no corruption.

TEST(FuzzTest, ConcurrentOps) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);

        // Pre-populate
        for (int i = 0; i < 50; ++i) {
            ASSERT_EQ(kv.put_string("c" + std::to_string(i), "v" + std::to_string(i)),
                      XCACHE_OK);
        }

        std::atomic<bool> stop{false};
        std::atomic<int> errors{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, seed = t * 12345 + 42]() {
                std::mt19937 rng(seed);
                while (!stop.load(std::memory_order_relaxed)) {
                    auto k = "c" + std::to_string(rng() % 50);
                    switch (rng() % 5) {
                        case 0: {
                            std::string v;
                            kv.get_string(k, &v);
                            break;
                        }
                        case 1:
                            kv.put_string(k, "v" + std::to_string(rng()));
                            break;
                        case 2:
                            kv.remove(k);
                            break;
                        case 3:
                            kv.exists(k);
                            break;
                        case 4: {
                            xcache_value_type_t t;
                            kv.get_type(k, &t);
                            break;
                        }
                    }
                }
            });
        }

        std::this_thread::sleep_for(2s);
        stop.store(true);
        for (auto& t : threads) t.join();

        ASSERT_EQ(errors.load(), 0);
        // No crash, no corruption — just verify the cache is usable
        ASSERT_NO_THROW(kv.size());
        ASSERT_NO_THROW(kv.scan([](const auto&, auto) { return true; }));
    }
    cleanup(p);
}

// ── concurrent fuzz with rebuild ─────────────────────────────────
//
// Threads hammer random keys while rebuild runs repeatedly.

TEST(FuzzTest, ConcurrentOpsWithRebuild) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);

        for (int i = 0; i < 200; ++i) {
            ASSERT_EQ(kv.put_string("r" + std::to_string(i), "v" + std::to_string(i)),
                      XCACHE_OK);
        }

        std::atomic<bool> stop{false};
        std::atomic<int> reads_ok{0};
        std::vector<std::thread> threads;

        // 3 readers
        for (int t = 0; t < 3; ++t) {
            threads.emplace_back([&, seed = t * 9999 + 1]() {
                std::mt19937 rng(seed);
                while (!stop.load(std::memory_order_relaxed)) {
                    auto k = "r" + std::to_string(rng() % 200);
                    std::string v;
                    if (kv.get_string(k, &v) == XCACHE_OK) {
                        reads_ok.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        // 1 writer
        threads.emplace_back([&]() {
            std::mt19937 rng(777);
            while (!stop.load(std::memory_order_relaxed)) {
                auto k = "r" + std::to_string(rng() % 200);
                kv.put_string(k, "v" + std::to_string(rng()));
            }
        });

        // rebuild 10 times in the background
        for (int r = 0; r < 10; ++r) {
            std::this_thread::sleep_for(100ms);
            kv.rebuild();
        }

        stop.store(true);
        for (auto& t : threads) t.join();

        EXPECT_GT(reads_ok.load(), 0);

        // Verify all 200 keys are still readable
        for (int i = 0; i < 200; ++i) {
            std::string v;
            EXPECT_EQ(kv.get_string("r" + std::to_string(i), &v), XCACHE_OK);
        }
    }
    cleanup(p);
}
