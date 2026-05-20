#include "xcache/xcache.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static std::string tmp_path() {
    return "/tmp/xe_" + std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".dat").c_str());
    ::unlink((p + ".idx").c_str());
}

// ── lazy expiry ──────────────────────────────────────────────────
//
// After get/get_type/exists on an expired key, the slot is CAS'd to
// kTomb (lazy expiry). This changes the behaviour of subsequent
// remove(): it now skips the tombstoned slot and returns NOT_FOUND.
// Without a prior read, remove() on an expired key still returns OK
// because it finds the valid slot and tombstoned it itself.
// get_all_keys() and scan() do NOT trigger lazy expiry.

TEST(EdgeTest, LazyExpiry_RemoveNoPriorRead_ReturnsOK) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);
        EXPECT_EQ(kv.remove("k"), XCACHE_OK);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_RemoveAfterGet_ReturnsNotFound) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);

        std::string v;
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_EXPIRED);
        EXPECT_EQ(kv.remove("k"), XCACHE_NOT_FOUND);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_RemoveAfterExists_ReturnsNotFound) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);
        EXPECT_FALSE(kv.exists("k"));
        EXPECT_EQ(kv.remove("k"), XCACHE_NOT_FOUND);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_RemoveAfterGetType_ReturnsNotFound) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);

        xcache_value_type_t t;
        EXPECT_EQ(kv.get_type("k", &t), XCACHE_EXPIRED);
        EXPECT_EQ(kv.remove("k"), XCACHE_NOT_FOUND);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_DoubleRemoveAfterGet) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);

        std::string v;
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_EXPIRED);
        EXPECT_EQ(kv.remove("k"), XCACHE_NOT_FOUND);
        EXPECT_EQ(kv.remove("k"), XCACHE_NOT_FOUND);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_RemoveAfterGetAllKeys_ReturnsOK) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);

        EXPECT_TRUE(kv.get_all_keys().empty());
        EXPECT_EQ(kv.remove("k"), XCACHE_OK);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_ScanDoesNotTombstone) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);

        int seen = 0;
        kv.scan([&](const std::string&, xcache_value_type_t) { ++seen; return true; });
        EXPECT_EQ(seen, 0);
        EXPECT_EQ(kv.remove("k"), XCACHE_OK);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_SizeAfterGet) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("live", "val");
        kv.put_string("dead", "val", 1);
        std::this_thread::sleep_for(2s);

        EXPECT_EQ(kv.size(), 2u);
        std::string v;
        EXPECT_EQ(kv.get_string("dead", &v), XCACHE_EXPIRED);
        EXPECT_EQ(kv.size(), 1u);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_ConcurrentGetOnExpiredKey) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);

        std::atomic<int> ready{0};
        std::atomic<int> expired_count{0};
        std::vector<std::thread> threads;
        int n = 4;

        for (int t = 0; t < n; ++t) {
            threads.emplace_back([&]() {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (ready.load(std::memory_order_acquire) < n) {
                    std::this_thread::yield();
                }
                std::string v;
                if (kv.get_string("k", &v) == XCACHE_EXPIRED) {
                    expired_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& t : threads) t.join();
        // First thread to CAS gets EXPIRED; others see kTomb → NOT_FOUND.
        // At least one thread must see EXPIRED.
        EXPECT_GE(expired_count.load(), 1);
        EXPECT_LE(expired_count.load(), n);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_ConcurrentGetAndRemove) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v", 1);
        std::this_thread::sleep_for(2s);

        std::atomic<int> ready{0};
        std::atomic<int> get_ok{0};
        std::atomic<int> remove_ok{0};
        std::vector<std::thread> threads;
        int n = 4;

        for (int t = 0; t < n; ++t) {
            threads.emplace_back([&, tid = t]() {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (ready.load(std::memory_order_acquire) < n) {
                    std::this_thread::yield();
                }
                if (tid % 2 == 0) {
                    std::string v;
                    if (kv.get_string("k", &v) == XCACHE_EXPIRED) {
                        get_ok.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    if (kv.remove("k") == XCACHE_OK) {
                        remove_ok.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& t : threads) t.join();

        // At most one remove() can CAS to kTomb. gets either see
        // EXPIRED (before CAS) or NOT_FOUND (after CAS). Either is valid.
        EXPECT_LE(remove_ok.load(), 1);
        EXPECT_GE(get_ok.load() + remove_ok.load(), 1);
        EXPECT_LE(get_ok.load() + remove_ok.load(), n);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_RebuildAfterTombstones) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1", 1);
        kv.put_string("b", "2", 1);
        std::this_thread::sleep_for(2s);

        std::string v;
        EXPECT_EQ(kv.get_string("a", &v), XCACHE_EXPIRED);
        EXPECT_EQ(kv.get_string("b", &v), XCACHE_EXPIRED);

        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_RebuildMixedExpiredAndLive) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("live1", "val1");
        kv.put_string("dead", "val2", 1);
        kv.put_string("live2", "val3");
        std::this_thread::sleep_for(2s);

        std::string v;
        EXPECT_EQ(kv.get_string("dead", &v), XCACHE_EXPIRED);

        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        EXPECT_EQ(kv.size(), 2u);
        EXPECT_TRUE(kv.exists("live1"));
        EXPECT_TRUE(kv.exists("live2"));
        EXPECT_FALSE(kv.exists("dead"));
    }
    cleanup(p);
}

TEST(EdgeTest, LazyExpiry_NewPutAfterLazyExpiry) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "old", 1);
        std::this_thread::sleep_for(2s);

        std::string v;
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_EXPIRED);

        EXPECT_EQ(kv.put_string("k", "new"), XCACHE_OK);
        EXPECT_EQ(kv.get_string("k", &v), XCACHE_OK);
        EXPECT_EQ(v, "new");

        EXPECT_EQ(kv.remove("k"), XCACHE_OK);
        EXPECT_FALSE(kv.exists("k"));
    }
    cleanup(p);
}

// ── boundary: empty key ──────────────────────────────────────────

TEST(EdgeTest, EmptyKey) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_string("", "empty_key_val"), XCACHE_OK);
        EXPECT_TRUE(kv.exists(""));

        std::string v;
        EXPECT_EQ(kv.get_string("", &v), XCACHE_OK);
        EXPECT_EQ(v, "empty_key_val");

        EXPECT_EQ(kv.remove(""), XCACHE_OK);
        EXPECT_FALSE(kv.exists(""));
    }
    cleanup(p);
}

// ── boundary: large value (1 MB) ─────────────────────────────────

TEST(EdgeTest, LargeValue) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 4UL << 20);
        std::string big(1UL << 20, 'x');
        EXPECT_EQ(kv.put_string("big", big), XCACHE_OK);

        std::string v;
        EXPECT_EQ(kv.get_string("big", &v), XCACHE_OK);
        EXPECT_EQ(v.size(), 1UL << 20);
        EXPECT_EQ(v[0], 'x');
        EXPECT_EQ(v[1UL << 19], 'x');
    }
    cleanup(p);
}

// ── boundary: extreme same-key CAS contention ────────────────────

TEST(EdgeTest, ExtremeSameKeyContention) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        std::atomic<int> ready{0};
        std::atomic<int> ok{0};
        std::vector<std::thread> threads;
        int n = 8;

        for (int t = 0; t < n; ++t) {
            threads.emplace_back([&]() {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (ready.load(std::memory_order_acquire) < n) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < 50; ++i) {
                    if (kv.put_i64("k", i) == XCACHE_OK) {
                        ok.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& t : threads) t.join();

        EXPECT_GT(ok.load(), 0);
        EXPECT_LE(ok.load(), n * 50);

        int64_t v;
        EXPECT_EQ(kv.get_i64("k", &v), XCACHE_OK);
        EXPECT_GE(v, 0);
        EXPECT_LT(v, 50);
    }
    cleanup(p);
}
