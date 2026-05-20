#include "xcache/xcache.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

using namespace std::chrono_literals;

// ── helpers ──────────────────────────────────────────────────────

static std::string tmp_path() {
    return "/tmp/xf_" + std::to_string(::getpid()) + "_"
           + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".dat").c_str());
    ::unlink((p + ".idx").c_str());
}

// ── basic ────────────────────────────────────────────────────────

TEST(XFileTest, PutGet) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_string("hello", "world"), XCACHE_OK);
        EXPECT_EQ(kv.put_string("key2", "val2"), XCACHE_OK);
        std::string v;
        EXPECT_EQ(kv.get_string("hello", &v), XCACHE_OK);
        EXPECT_EQ(v, "world");
        EXPECT_EQ(kv.size(), 2u);
    }
    cleanup(p);
}

TEST(XFileTest, GetMissing) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        std::string v;
        EXPECT_NE(kv.get_string("nope", &v), XCACHE_OK);
    }
    cleanup(p);
}

TEST(XFileTest, Overwrite) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "old");
        kv.put_string("k", "new");
        std::string v;
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_OK);
        EXPECT_EQ(v, "new");
    }
    cleanup(p);
}

TEST(XFileTest, Remove) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v");
        EXPECT_EQ(kv.remove("k"), XCACHE_OK);
        EXPECT_NE(kv.remove("k"), XCACHE_OK);
        std::string v;
        EXPECT_NE(kv.get_string("k", &v), XCACHE_OK);
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

TEST(XFileTest, Exists) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_FALSE(kv.exists("k"));
        kv.put_string("k", "v");
        EXPECT_TRUE(kv.exists("k"));
    }
    cleanup(p);
}

TEST(XFileTest, LargeValue) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        auto big = std::string(1024 * 1024, 'X');
        EXPECT_EQ(kv.put_string("big", big), XCACHE_OK);
        std::string v;
        EXPECT_EQ(kv.get_string("big", &v), XCACHE_OK);
        EXPECT_EQ(v.size(), 1024 * 1024u);
    }
    cleanup(p);
}

TEST(XFileTest, ManyKeys) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 8UL << 20);
        for (int i = 0; i < 10000; ++i) {
            kv.put_string("k_" + std::to_string(i), "v_" + std::to_string(i));
        }
        EXPECT_EQ(kv.size(), 10000u);
        for (int i = 0; i < 10000; ++i) {
            std::string v;
            EXPECT_EQ(kv.get_string("k_" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "v_" + std::to_string(i));
        }
    }
    cleanup(p);
}

// ── persistence ──────────────────────────────────────────────────

TEST(XFileTest, PersistAcrossOpenClose) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
    }
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.size(), 2u);
        std::string va;
        EXPECT_EQ(kv.get_string("a", &va), XCACHE_OK);
        EXPECT_EQ(va, "1");
        std::string vb;
        EXPECT_EQ(kv.get_string("b", &vb), XCACHE_OK);
        EXPECT_EQ(vb, "2");
    }
    cleanup(p);
}

TEST(XFileTest, PersistWithTombstones) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        kv.remove("a");
    }
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.size(), 1u);
        EXPECT_FALSE(kv.exists("a"));
        EXPECT_TRUE(kv.exists("b"));
    }
    cleanup(p);
}

// ── concurrent same-key ──────────────────────────────────────────

TEST(XFileConcurrent, PutGetSameKey) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 16UL << 20);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> ops{0};
        std::string skey = "contended";

        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&kv, &stop, &ops, &skey, t] {
                auto suffix = std::to_string(t);
                while (!stop.load()) {
                    kv.put_string(skey, suffix);
                    std::string v;
                    if (kv.get_string(skey, &v) == XCACHE_OK) {
                        // value must be written by one of the threads
                        EXPECT_GE(v.size(), 1u);
                        EXPECT_LE(v.size(), 2u);
                    }
                    ops.fetch_add(1);
                }
            });
        }
        std::this_thread::sleep_for(2s);
        stop.store(true);
        for (auto& th : threads) th.join();
        EXPECT_GT(ops.load(), 50000u);
    }
    cleanup(p);
}

TEST(XFileConcurrent, PutRemoveSameKey) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 16UL << 20);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> ops{0};
        std::string skey = "toggled";

        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&kv, &stop, &ops, &skey] {
                while (!stop.load()) {
                    kv.put_string(skey, "v");
                    std::string v;
                    kv.get_string(skey, &v);
                    kv.remove(skey);
                    ops.fetch_add(3);
                }
            });
        }
        std::this_thread::sleep_for(2s);
        stop.store(true);
        for (auto& th : threads) th.join();
        EXPECT_GT(ops.load(), 50000u);
    }
    cleanup(p);
}

// ── concurrent ───────────────────────────────────────────────────

TEST(XFileConcurrent, ParallelPuts) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 8UL << 20);
        std::vector<std::thread> threads;
        int n = 8, per = 5000;

        for (int t = 0; t < n; ++t) {
            threads.emplace_back([&kv, t, per] {
                for (int i = 0; i < per; ++i) {
                    auto k = std::to_string(t * per + i);
                    kv.put_string(k, "v");
                }
            });
        }
        for (auto& th : threads) th.join();
        EXPECT_EQ(kv.size(), static_cast<size_t>(n * per));
    }
    cleanup(p);
}

TEST(XFileConcurrent, ParallelGets) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 8UL << 20);
        for (int i = 0; i < 10000; ++i)
            kv.put_string(std::to_string(i), std::to_string(i));

        std::vector<std::thread> threads;
        std::atomic<uint64_t> found{0};

        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&kv, &found] {
                thread_local std::mt19937 rng(std::random_device{}());
                for (int i = 0; i < 5000; ++i) {
                    auto k = std::to_string(std::uniform_int_distribution<int>(0, 9999)(rng));
                    std::string v;
                    if (kv.get_string(k, &v) == XCACHE_OK) found.fetch_add(1);
                }
            });
        }
        for (auto& th : threads) th.join();
        EXPECT_GT(found.load(), 0u);
    }
    cleanup(p);
}

TEST(XFileConcurrent, MixedWorkload) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 16UL << 20);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> ops{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&kv, &stop, &ops] {
                thread_local std::mt19937 rng(std::random_device{}());
                while (!stop.load()) {
                    auto k = std::to_string(std::uniform_int_distribution<int>(0, 4999)(rng));
                    switch (std::uniform_int_distribution<int>(0, 3)(rng)) {
                        case 0: kv.put_string(k, "v"); break;
                        case 1: { std::string v; kv.get_string(k, &v); }      break;
                        case 2: kv.exists(k);   break;
                        case 3: kv.remove(k);   break;
                    }
                    ops.fetch_add(1);
                }
            });
        }
        std::this_thread::sleep_for(2s);
        stop.store(true);
        for (auto& th : threads) th.join();
        EXPECT_GT(ops.load(), 50000u);
    }
    cleanup(p);
}

TEST(XFileConcurrent, NoCrashUnderStress) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 32UL << 20);
        std::vector<std::thread> threads;

        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&kv, t] {
                for (int i = 0; i < 10000; ++i) {
                    auto k = std::to_string(t * 10000 + i);
                    kv.put_string(k, k);
                    std::string v;
                    if (kv.get_string(k, &v) == XCACHE_OK && v != k) {
                        GTEST_FAIL();
                    }
                    if (i % 3 == 0) kv.remove(k);
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    cleanup(p);
}

// ── rebuild ──────────────────────────────────────────────────────

TEST(XFileTest, RebuildCompactsTombstones) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 256UL << 10);
        for (int i = 0; i < 5000; ++i)
            kv.put_string("k_" + std::to_string(i), "v_" + std::to_string(i));
        for (int i = 0; i < 2500; ++i)
            kv.remove("k_" + std::to_string(i));
        EXPECT_EQ(kv.size(), 2500u);
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        EXPECT_EQ(kv.size(), 2500u);
        for (int i = 2500; i < 5000; ++i) {
            std::string v;
            EXPECT_EQ(kv.get_string("k_" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "v_" + std::to_string(i));
        }
    }
    cleanup(p);
}

TEST(XFileTest, RebuildPersistsData) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        kv.remove("a");
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
    }
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.size(), 1u);
        EXPECT_FALSE(kv.exists("a"));
        EXPECT_TRUE(kv.exists("b"));
    }
    cleanup(p);
}

// ── scan ─────────────────────────────────────────────────────────

TEST(XFileTest, ScanAllEntries) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k1", "v1");
        kv.put_string("k2", "v2");
        kv.put_string("k3", "v3");
        std::vector<std::pair<std::string, std::string>> found;
        kv.scan([&](const std::string& k, xcache_value_type_t) {
            std::string v;
            if (kv.get_string(k, &v) == XCACHE_OK) found.emplace_back(k, v);
            return true;
        });
        EXPECT_EQ(found.size(), 3u);
    }
    cleanup(p);
}

TEST(XFileTest, ScanSkipsRemoved) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        kv.remove("a");
        std::vector<std::string> keys;
        kv.scan([&](const std::string& k, xcache_value_type_t) {
            keys.push_back(k);
            return true;
        });
        ASSERT_EQ(keys.size(), 1u);
        EXPECT_EQ(keys[0], "b");
    }
    cleanup(p);
}

TEST(XFileTest, ScanEmptyStore) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        int count = 0;
        kv.scan([&](const std::string&, xcache_value_type_t) {
            ++count; return true;
        });
        EXPECT_EQ(count, 0);
    }
    cleanup(p);
}

// ── multi_process ────────────────────────────────────────────────

TEST(XFileTest, MultiProcessFlag) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 256UL << 10, true);
        EXPECT_EQ(kv.put_string("k", "v"), XCACHE_OK);
        std::string v;
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_OK);
        EXPECT_EQ(v, "v");
        EXPECT_EQ(kv.size(), 1u);
    }
    {
        xcache::XCache kv(p, 256UL << 10, true);
        std::string v;
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_OK);
        EXPECT_EQ(v, "v");
    }
    cleanup(p);
}

// ── typed values ────────────────────────────────────────────────

TEST(XFileTest, TypedInt64) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_i64("k", 42), XCACHE_OK);
        EXPECT_EQ(kv.put_i64("neg", -1), XCACHE_OK);
        int64_t v;
        EXPECT_EQ(kv.get_i64("k", &v), XCACHE_OK);
        EXPECT_EQ(v, 42);
        int64_t neg_v;
        EXPECT_EQ(kv.get_i64("neg", &neg_v), XCACHE_OK);
        EXPECT_EQ(neg_v, -1);
        std::string s;
        EXPECT_NE(kv.get_string("k", &s), XCACHE_OK);
        xcache_value_type_t t;
        EXPECT_EQ(kv.get_type("k", &t), XCACHE_OK);
        EXPECT_EQ(t, XCACHE_INT64);
    }
    cleanup(p);
}

TEST(XFileTest, TypedInt32) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_i32("k", 12345), XCACHE_OK);
        int32_t v;
        EXPECT_EQ(kv.get_i32("k", &v), XCACHE_OK);
        EXPECT_EQ(v, 12345);
        xcache_value_type_t t;
        EXPECT_EQ(kv.get_type("k", &t), XCACHE_OK);
        EXPECT_EQ(t, XCACHE_INT32);
    }
    cleanup(p);
}

TEST(XFileTest, TypedFloatDouble) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_f32("f", 3.14f), XCACHE_OK);
        EXPECT_EQ(kv.put_f64("d", 2.718281828459045), XCACHE_OK);
        float f;
        EXPECT_EQ(kv.get_f32("f", &f), XCACHE_OK);
        EXPECT_FLOAT_EQ(f, 3.14f);
        double d;
        EXPECT_EQ(kv.get_f64("d", &d), XCACHE_OK);
        EXPECT_DOUBLE_EQ(d, 2.718281828459045);
    }
    cleanup(p);
}

TEST(XFileTest, TypedBool) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_bool("t", true), XCACHE_OK);
        EXPECT_EQ(kv.put_bool("f", false), XCACHE_OK);
        bool b_t;
        EXPECT_EQ(kv.get_bool("t", &b_t), XCACHE_OK);
        EXPECT_TRUE(b_t);
        bool b_f;
        EXPECT_EQ(kv.get_bool("f", &b_f), XCACHE_OK);
        EXPECT_FALSE(b_f);
    }
    cleanup(p);
}

TEST(XFileTest, TypedBlob) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        uint8_t data[] = {0x00, 0xFF, 0xAB, 0xCD};
        EXPECT_EQ(kv.put_blob("b", data, sizeof(data)), XCACHE_OK);
        std::vector<uint8_t> v;
        EXPECT_EQ(kv.get_blob("b", &v), XCACHE_OK);
        ASSERT_EQ(v.size(), 4u);
        EXPECT_EQ(v[0], 0x00);
        EXPECT_EQ(v[3], 0xCD);
        xcache_value_type_t t;
        EXPECT_EQ(kv.get_type("b", &t), XCACHE_OK);
        EXPECT_EQ(t, XCACHE_BLOB);
    }
    cleanup(p);
}

TEST(XFileTest, TypedOverwriteWithDifferentType) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "text");
        xcache_value_type_t t;
        EXPECT_EQ(kv.get_type("k", &t), XCACHE_OK);
        EXPECT_EQ(t, XCACHE_TEXT);
        kv.put_i64("k", 999);
        xcache_value_type_t t2;
        EXPECT_EQ(kv.get_type("k", &t2), XCACHE_OK);
        EXPECT_EQ(t2, XCACHE_INT64);
        int64_t v;
        EXPECT_EQ(kv.get_i64("k", &v), XCACHE_OK);
        EXPECT_EQ(v, 999);
        std::string s;
        EXPECT_NE(kv.get_string("k", &s), XCACHE_OK);
    }
    cleanup(p);
}

TEST(XFileTest, TypedRebuildPreservesTypes) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("s", "hello");
        kv.put_i64("i", 42);
        kv.put_f64("d", 3.14);
        kv.put_blob("b", "raw", 3);
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        std::string s;
        EXPECT_EQ(kv.get_string("s", &s), XCACHE_OK);
        EXPECT_EQ(s, "hello");
        int64_t i;
        EXPECT_EQ(kv.get_i64("i", &i), XCACHE_OK);
        EXPECT_EQ(i, 42);
        double d;
        EXPECT_EQ(kv.get_f64("d", &d), XCACHE_OK);
        EXPECT_DOUBLE_EQ(d, 3.14);
        std::vector<uint8_t> b;
        EXPECT_EQ(kv.get_blob("b", &b), XCACHE_OK);
        EXPECT_EQ(b.size(), 3u);
    }
    cleanup(p);
}

TEST(XFileTest, TypedPersistence) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_i64("lives", 100);
        kv.put_f32("pi", 3.14f);
    }
    {
        xcache::XCache kv(p);
        int64_t v;
        EXPECT_EQ(kv.get_i64("lives", &v), XCACHE_OK);
        EXPECT_EQ(v, 100);
        float pi;
        EXPECT_EQ(kv.get_f32("pi", &pi), XCACHE_OK);
        EXPECT_FLOAT_EQ(pi, 3.14f);
    }
    cleanup(p);
}

// ── edge / corner ───────────────────────────────────────────────

TEST(XFileTest, RebuildEmptyStore) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

TEST(XFileTest, ScanEarlyStop) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        kv.put_string("c", "3");
        int count = 0;
        kv.scan([&](const std::string&, xcache_value_type_t) {
            ++count;
            return false;
        });
        EXPECT_EQ(count, 1);
    }
    cleanup(p);
}

TEST(XFileTest, GetTypeNonExistent) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_FALSE(kv.exists("nope"));
        xcache_value_type_t t;
        EXPECT_NE(kv.get_type("nope", &t), XCACHE_OK);
    }
    cleanup(p);
}

TEST(XFileTest, NewStoreSize) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

// ── regression: tombstone num_entries ──────────────────────────

TEST(XFileTest, TombstoneOverwriteSize) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        for (int i = 0; i < 100; ++i) {
            EXPECT_EQ(kv.put_string("k", "v"), XCACHE_OK);
            EXPECT_EQ(kv.size(), 1u) << "after put cycle " << i;
            EXPECT_EQ(kv.remove("k"), XCACHE_OK);
            EXPECT_EQ(kv.size(), 0u) << "after remove cycle " << i;
        }
    }
    cleanup(p);
}

TEST(XFileTest, TombstoneOverwriteMultipleKeys) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        for (int i = 0; i < 100; ++i) {
            auto key = "k_" + std::to_string(i % 10);
            kv.put_string(key, "v");
        }
        EXPECT_EQ(kv.size(), 10u);
        for (int i = 0; i < 100; ++i) {
            auto key = "k_" + std::to_string(i % 10);
            kv.remove(key);
        }
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

TEST(XFileTest, RemoveNonExistentThenPut) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_NE(kv.remove("nope"), XCACHE_OK);
        EXPECT_EQ(kv.size(), 0u);
        EXPECT_EQ(kv.put_string("k", "v"), XCACHE_OK);
        EXPECT_EQ(kv.size(), 1u);
    }
    cleanup(p);
}

TEST(XFileTest, PutStringThenTypedRemove) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_i64("k", 42);
        EXPECT_EQ(kv.size(), 1u);
        EXPECT_EQ(kv.remove("k"), XCACHE_OK);
        EXPECT_EQ(kv.size(), 0u);
        kv.put_string("k", "text");
        EXPECT_EQ(kv.size(), 1u);
        std::string s;
        EXPECT_EQ(kv.get_string("k", &s), XCACHE_OK);
        EXPECT_EQ(s, "text");
    }
    cleanup(p);
}

TEST(XFileConcurrent, PutRemoveNoCrash) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 16UL << 20);
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&kv, &stop] {
                thread_local std::mt19937 rng(std::random_device{}());
                while (!stop.load()) {
                    auto k = std::to_string(std::uniform_int_distribution<int>(0, 19)(rng));
                    kv.put_string(k, "v");
                    if (std::uniform_int_distribution<int>(0, 1)(rng)) {
                        kv.remove(k);
                    }
                    kv.size();  // must not crash
                }
            });
        }
        std::this_thread::sleep_for(1s);
        stop.store(true);
        for (auto& th : threads) th.join();
    }
    cleanup(p);
}

// ── rebuild + size ─────────────────────────────────────────────

TEST(XFileTest, RebuildAfterManyTombstones) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        for (int i = 0; i < 1000; ++i) {
            kv.put_string("k_" + std::to_string(i), "v");
        }
        EXPECT_EQ(kv.size(), 1000u);
        for (int i = 0; i < 1000; ++i) {
            kv.remove("k_" + std::to_string(i));
        }
        EXPECT_EQ(kv.size(), 0u);
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

TEST(XFileTest, RebuildThenPutCheckSize) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        EXPECT_EQ(kv.size(), 2u);
        kv.put_string("c", "3");
        EXPECT_EQ(kv.size(), 3u);
        kv.remove("a");
        EXPECT_EQ(kv.size(), 2u);
    }
    cleanup(p);
}

TEST(XFileTest, TtlExpires) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("forever", "permanent");
        kv.put_string("gone", "temporary", 1);
        EXPECT_TRUE(kv.exists("forever"));
        EXPECT_TRUE(kv.exists("gone"));
        std::string v;
        EXPECT_EQ(kv.get_string("forever", &v), XCACHE_OK);
        EXPECT_EQ(v, "permanent");
        std::string v2;
        EXPECT_EQ(kv.get_string("gone", &v2), XCACHE_OK);
        EXPECT_EQ(v2, "temporary");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        EXPECT_TRUE(kv.exists("forever"));
        EXPECT_FALSE(kv.exists("gone"));
        std::string v3;
        EXPECT_EQ(kv.get_string("forever", &v3), XCACHE_OK);
        EXPECT_EQ(v3, "permanent");
        std::string v4;
        EXPECT_NE(kv.get_string("gone", &v4), XCACHE_OK);
    }
    cleanup(p);
}

TEST(XFileTest, GetAllKeys) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_TRUE(kv.get_all_keys().empty());
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        kv.put_string("c", "3");
        auto keys = kv.get_all_keys();
        std::sort(keys.begin(), keys.end());
        ASSERT_EQ(keys.size(), 3u);
        EXPECT_EQ(keys[0], "a");
        EXPECT_EQ(keys[1], "b");
        EXPECT_EQ(keys[2], "c");

        kv.remove("b");
        keys = kv.get_all_keys();
        ASSERT_EQ(keys.size(), 2u);

        kv.put_string("exp", "x", 1);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        keys = kv.get_all_keys();
        EXPECT_EQ(keys.size(), 2u);  // expired key excluded
    }
    cleanup(p);
}

TEST(XFileConcurrent, MultiProcessReadWrite) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 256UL << 10, true);
        int n = 100;
        for (int i = 0; i < n; ++i) {
            kv.put_string("k" + std::to_string(i), "v" + std::to_string(i));
        }

        std::atomic<bool> start{false};
        std::atomic<int> reads_done{0};
        std::atomic<int> writes_done{0};

        // Reader threads (shared lock)
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t) {
            readers.emplace_back([&, t]() {
                xcache::XCache r(p, 256UL << 10, true);
                while (!start) { std::this_thread::yield(); }
                for (int i = 0; i < n; ++i) {
                    std::string v;
                    if (r.get_string("k" + std::to_string(i), &v) == XCACHE_OK) reads_done++;
                }
            });
        }

        // Writer thread (exclusive lock)
        std::thread writer([&]() {
            xcache::XCache w(p, 256UL << 10, true);
            while (!start) { std::this_thread::yield(); }
            for (int i = n; i < n * 2; ++i) {
                if (w.put_string("k" + std::to_string(i), "v" + std::to_string(i)) == XCACHE_OK)
                    writes_done++;
            }
        });

        start = true;
        for (auto& t : readers) t.join();
        writer.join();

        EXPECT_EQ(reads_done, n * 4);
        EXPECT_EQ(writes_done, n);
    }
    cleanup(p);
}

TEST(XFileConcurrent, ForkMultiProcess) {
    auto p = tmp_path();
    {
        constexpr int N = 50;
        // parent: write initial data
        {
            xcache::XCache kv(p, 256UL << 10, true);
            for (int i = 0; i < N; ++i) {
                kv.put_string("k" + std::to_string(i), "parent_" + std::to_string(i));
            }
        }

        pid_t pid = fork();
        ASSERT_NE(pid, -1);

        if (pid == 0) {
            // child process
            xcache::XCache kv(p, 256UL << 10, true);
            // read parent's data
            for (int i = 0; i < N; ++i) {
                std::string v;
                if (kv.get_string("k" + std::to_string(i), &v) != XCACHE_OK || v != "parent_" + std::to_string(i)) {
                    _exit(1);
                }
            }
            // write own data
            for (int i = 0; i < N; ++i) {
                if (kv.put_string("c" + std::to_string(i), "child_" + std::to_string(i)) != XCACHE_OK) {
                    _exit(2);
                }
            }
            kv.close();
            _exit(0);
        }

        // parent: wait for child
        int status;
        waitpid(pid, &status, 0);
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);

        // parent: reopen and read child's data (triggers remap_if_stale)
        {
            xcache::XCache kv(p, 256UL << 10, true);
            for (int i = 0; i < N; ++i) {
                std::string v;
                EXPECT_EQ(kv.get_string("c" + std::to_string(i), &v), XCACHE_OK);
                EXPECT_EQ(v, "child_" + std::to_string(i));
            }
        }
    }
    cleanup(p);
}

TEST(XFileTest, EdgeCases) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);

        // 空 key，应该可以工作（XXH3 支持 0 长度输入）
        EXPECT_EQ(kv.put_string("", "empty_key"), XCACHE_OK);
        std::string v1;
        EXPECT_EQ(kv.get_string("", &v1), XCACHE_OK);
        EXPECT_EQ(v1, "empty_key");

        // 空 value（零长度字符串）
        EXPECT_EQ(kv.put_string("zero_val", ""), XCACHE_OK);
        std::string v2;
        EXPECT_EQ(kv.get_string("zero_val", &v2), XCACHE_OK);
        EXPECT_TRUE(v2.empty());

        // key 含特殊字符
        EXPECT_EQ(kv.put_string("key with spaces", "val"), XCACHE_OK);
        EXPECT_EQ(kv.put_string(std::string("key\0with\0nulls", 14), "bin"), XCACHE_OK);
        EXPECT_EQ(kv.put_string("\x01\x02\xff\xfe", "binary_key"), XCACHE_OK);

        // NULL data + zero len for blob
        EXPECT_EQ(kv.put_blob("null_blob", nullptr, 0), XCACHE_OK);
        std::vector<uint8_t> b;
        EXPECT_EQ(kv.get_blob("null_blob", &b), XCACHE_OK);
        EXPECT_TRUE(b.empty());

        // 超大 expire（365 天）
        EXPECT_EQ(kv.put_string("far_future", "still_here", 365U * 86400), XCACHE_OK);
        EXPECT_TRUE(kv.exists("far_future"));

        // overwrite 不同类型
        EXPECT_EQ(kv.put_i64("same_key", 42), XCACHE_OK);
        EXPECT_EQ(kv.put_string("same_key", "now_text"), XCACHE_OK);
        std::string s;
        EXPECT_EQ(kv.get_string("same_key", &s), XCACHE_OK);
        EXPECT_EQ(s, "now_text");
        int64_t iv;
        EXPECT_NE(kv.get_i64("same_key", &iv), XCACHE_OK);

        // put 后立即 remove，再 put
        EXPECT_EQ(kv.put_string("temp", "gone"), XCACHE_OK);
        EXPECT_EQ(kv.remove("temp"), XCACHE_OK);
        EXPECT_FALSE(kv.exists("temp"));
        EXPECT_EQ(kv.put_string("temp", "back"), XCACHE_OK);
        std::string ts;
        EXPECT_EQ(kv.get_string("temp", &ts), XCACHE_OK);
        EXPECT_EQ(ts, "back");

        // size 加过期 key 后返回（不要求精确但至少不为 0）
        kv.put_string("count_me", "1");
        auto sz = kv.size();
        EXPECT_GE(sz, 1u);

        // 关闭两次不 crash
        kv.close();
        kv.close();
    }
    cleanup(p);

    // move 语义
    {
        xcache::XCache a(p);
        EXPECT_EQ(a.put_string("k", "v"), XCACHE_OK);
        xcache::XCache b(std::move(a));
        std::string mv;
        EXPECT_EQ(b.get_string("k", &mv), XCACHE_OK);
        EXPECT_EQ(mv, "v");

        xcache::XCache c(p);
        c = std::move(b);
        std::string mv2;
        EXPECT_EQ(c.get_string("k", &mv2), XCACHE_OK);
        EXPECT_EQ(mv2, "v");
    }
    cleanup(p);
}

TEST(XFileTest, LargeKey) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        auto big_key = std::string(1024 * 1024, 'K');
        EXPECT_EQ(kv.put_string(big_key, "val"), XCACHE_OK);
        std::string v;
        EXPECT_EQ(kv.get_string(big_key, &v), XCACHE_OK);
        EXPECT_EQ(v, "val");
    }
    cleanup(p);
}

TEST(XFileTest, HugeValue) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        auto big = std::string(16 * 1024 * 1024, 'X');
        EXPECT_EQ(kv.put_string("big", big), XCACHE_OK);
        std::string v;
        EXPECT_EQ(kv.get_string("big", &v), XCACHE_OK);
        EXPECT_EQ(v.size(), 16 * 1024 * 1024u);
    }
    cleanup(p);
}

TEST(XFileTest, RemoveNonExistentRepeatedly) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_NE(kv.remove("nope"), XCACHE_OK);
        EXPECT_NE(kv.remove("nope"), XCACHE_OK);
        EXPECT_NE(kv.remove(""), XCACHE_OK);
        kv.put_string("k", "v");
        EXPECT_EQ(kv.remove("k"), XCACHE_OK);
        EXPECT_NE(kv.remove("k"), XCACHE_OK);
    }
    cleanup(p);
}

TEST(XFileTest, TtlOverwrite) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "old", 1);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        EXPECT_FALSE(kv.exists("k"));
        kv.put_string("k", "new");
        EXPECT_TRUE(kv.exists("k"));
        std::string v;
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_OK);
        EXPECT_EQ(v, "new");
    }
    cleanup(p);
}

TEST(XFileTest, TtlAcrossOpen) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("nottl", "forever");
        kv.put_string("will_expire", "temp", 1);
        kv.close();
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        xcache::XCache kv(p);
        EXPECT_TRUE(kv.exists("nottl"));
        std::string v;
        EXPECT_EQ(kv.get_string("nottl", &v), XCACHE_OK);
        EXPECT_EQ(v, "forever");
        EXPECT_FALSE(kv.exists("will_expire"));
    }
    cleanup(p);
}

TEST(XFileTest, RebuildAllExpired) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1", 1);
        kv.put_string("b", "2", 1);
        kv.put_string("c", "3", 1);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        EXPECT_EQ(kv.size(), 3u);
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

TEST(XFileTest, GetTypeForExpiredKey) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        xcache_value_type_t t;
        EXPECT_EQ(kv.get_type("k", &t), XCACHE_OK);
        EXPECT_EQ(t, XCACHE_TEXT);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        xcache_value_type_t t2;
        EXPECT_NE(kv.get_type("k", &t2), XCACHE_OK);
        EXPECT_FALSE(kv.exists("k"));
    }
    cleanup(p);
}

TEST(XFileTest, StressManyKeysWithRehash) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 4UL << 20);
        int n = 100000;
        for (int i = 0; i < n; ++i) {
            ASSERT_EQ(kv.put_string("k" + std::to_string(i), "v" + std::to_string(i)), XCACHE_OK);
        }
        EXPECT_EQ(kv.size(), static_cast<size_t>(n));
        for (int i = 0; i < 100; ++i) {
            std::string v;
            EXPECT_EQ(kv.get_string("k" + std::to_string(i * 1000), &v), XCACHE_OK);
            EXPECT_EQ(v, "v" + std::to_string(i * 1000));
        }
    }
    cleanup(p);
}

TEST(XFileTest, RebuildDoesNotBlockReads) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 65536, false, 64);

        for (int i = 0; i < 100; ++i)
            kv.put_string("k" + std::to_string(i), "v" + std::to_string(i));

        std::atomic<bool> stop{false};
        std::atomic<int64_t> reads_ok{0};

        std::thread reader([&] {
            int64_t i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto k = "k" + std::to_string(i % 100);
                std::string v;
                if (kv.get_string(k, &v) == XCACHE_OK && v == "v" + std::to_string(i % 100))
                    reads_ok.fetch_add(1, std::memory_order_relaxed);
                ++i;
            }
        });

        std::this_thread::sleep_for(10ms);
        for (int r = 0; r < 5; ++r) {
            EXPECT_EQ(kv.rebuild(), XCACHE_OK);
            std::this_thread::sleep_for(5ms);
        }

        stop.store(true, std::memory_order_relaxed);
        reader.join();

        EXPECT_GT(reads_ok.load(), 0u);
        for (int i = 0; i < 100; ++i) {
            std::string v;
            EXPECT_EQ(kv.get_string("k" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "v" + std::to_string(i));
        }
    }
    cleanup(p);
}

TEST(XFileTest, RehashDoesNotBlockReads) {
    auto p = tmp_path();
    {
        // small init_slots → triggers rehash quickly
        xcache::XCache kv(p, 65536, false, 64);

        std::atomic<bool> stop{false};
        std::atomic<int64_t> reads_ok{0};
        int64_t total_puts = 0;

        std::thread reader([&] {
            int64_t i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto k = "k" + std::to_string(i % 100);
                std::string v;
                // catch whatever exists
                if (kv.get_string(k, &v) == XCACHE_OK) reads_ok.fetch_add(1, std::memory_order_relaxed);
                ++i;
            }
        });

        // put enough keys to trigger multiple rehashes
        int n = 5000;
        for (int i = 0; i < n; ++i) {
            ASSERT_EQ(kv.put_string("k" + std::to_string(i), "v" + std::to_string(i)), XCACHE_OK);
            ++total_puts;
        }

        stop.store(true, std::memory_order_relaxed);
        reader.join();

        EXPECT_GT(reads_ok.load(), 0u);
        EXPECT_EQ(kv.size(), static_cast<size_t>(n));
    }
    cleanup(p);
}
