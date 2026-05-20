#include "xcache/xcache.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static std::string tmp_path() {
    return "/tmp/xf_qa_" + std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".dat").c_str());
    ::unlink((p + ".idx").c_str());
}

TEST(QaCpp, TypeCycling) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_i64("k", 100), XCACHE_OK);
        EXPECT_EQ(kv.put_string("k", "text"), XCACHE_OK);
        EXPECT_EQ(kv.put_f64("k", 3.14), XCACHE_OK);
        EXPECT_EQ(kv.put_bool("k", true), XCACHE_OK);
        EXPECT_EQ(kv.put_i32("k", -42), XCACHE_OK);
        int32_t v32;
        EXPECT_EQ(kv.get_i32("k", &v32), XCACHE_OK);
        EXPECT_EQ(v32, -42);
        EXPECT_EQ(kv.size(), 1u);
    }
    cleanup(p);
}

TEST(QaCpp, NumericEdges) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);

        EXPECT_EQ(kv.put_i64("neg", INT64_MIN), XCACHE_OK);
        int64_t v64;
        EXPECT_EQ(kv.get_i64("neg", &v64), XCACHE_OK);
        EXPECT_EQ(v64, INT64_MIN);

        EXPECT_EQ(kv.put_f64("inf", std::numeric_limits<double>::infinity()), XCACHE_OK);
        double vf64;
        EXPECT_EQ(kv.get_f64("inf", &vf64), XCACHE_OK);
        EXPECT_TRUE(std::isinf(vf64));
        EXPECT_GT(vf64, 0);

        EXPECT_EQ(kv.put_f64("ninf", -std::numeric_limits<double>::infinity()), XCACHE_OK);
        EXPECT_EQ(kv.get_f64("ninf", &vf64), XCACHE_OK);
        EXPECT_TRUE(std::isinf(vf64));
        EXPECT_LT(vf64, 0);

        EXPECT_EQ(kv.put_f64("nan", std::numeric_limits<double>::quiet_NaN()), XCACHE_OK);
        EXPECT_EQ(kv.get_f64("nan", &vf64), XCACHE_OK);
        EXPECT_TRUE(std::isnan(vf64));

        EXPECT_EQ(kv.put_f32("f32max", std::numeric_limits<float>::max()), XCACHE_OK);
        float vf32;
        EXPECT_EQ(kv.get_f32("f32max", &vf32), XCACHE_OK);
        EXPECT_EQ(vf32, std::numeric_limits<float>::max());
    }
    cleanup(p);
}

TEST(QaCpp, RemoveThenRetype) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v");
        kv.remove("k");
        EXPECT_EQ(kv.put_i64("k", 42), XCACHE_OK);
        xcache_value_type_t t;
        EXPECT_EQ(kv.get_type("k", &t), XCACHE_OK);
        EXPECT_EQ(t, XCACHE_INT64);
    }
    cleanup(p);
}

TEST(QaCpp, GetAllKeys) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        kv.put_string("c", "3");
        EXPECT_EQ(kv.get_all_keys().size(), 3u);
        kv.remove("b");
        auto keys = kv.get_all_keys();
        EXPECT_EQ(keys.size(), 2u);
        bool has_a = false, has_c = false;
        for (auto& k : keys) {
            if (k == "a") has_a = true;
            if (k == "c") has_c = true;
        }
        EXPECT_TRUE(has_a && has_c);
    }
    cleanup(p);
}

TEST(QaCpp, ScanStopsEarly) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        for (int i = 0; i < 100; i++) kv.put_i64("k" + std::to_string(i), i);
        int count = 0;
        kv.scan([&](const std::string&, xcache_value_type_t) {
            count++;
            return count < 10;
        });
        EXPECT_EQ(count, 10);
    }
    cleanup(p);
}

TEST(QaCpp, ReopenWithDifferentInitSlots) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 256UL << 10, false, 1024);
        for (int i = 0; i < 500; i++) kv.put_i64("r" + std::to_string(i), i);
    }
    {
        xcache::XCache kv(p, 256UL << 10, false, 99999);
        EXPECT_EQ(kv.size(), 500u);
        int64_t v;
        EXPECT_EQ(kv.get_i64("r42", &v), XCACHE_OK);
        EXPECT_EQ(v, 42);
    }
    cleanup(p);
}

TEST(QaCpp, CloseTwice) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v");
        kv.close();
        kv.close();
    }
    {
        xcache::XCache kv(p);
        kv.close();
        kv.close();
    }
}

TEST(QaCpp, MoveFromClosed) {
    auto p = tmp_path();
    {
        xcache::XCache a(p);
        a.put_string("k", "v");
        a.close();
        xcache::XCache b(std::move(a));
        b.close();
    }
    cleanup(p);
}

TEST(QaCpp, ConcurrentReadDuringRebuild) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        for (int i = 0; i < 5000; i++) kv.put_i64("k" + std::to_string(i), i);

        std::atomic<bool> stop{false};
        std::atomic<int> reads_ok{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; t++) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_relaxed)) {
                    int idx = std::rand() % 5000;
                    int64_t v;
                    if (kv.get_i64("k" + std::to_string(idx), &v) == XCACHE_OK && v == idx)
                        reads_ok++;
                }
            });
        }
        for (int i = 0; i < 5; i++) kv.rebuild();
        stop.store(true);
        for (auto& t : threads) t.join();
        EXPECT_GT(reads_ok.load(), 0);
    }
    cleanup(p);
}

TEST(QaCpp, StressManyKeysWithRebuild) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        int n = 20000;
        for (int i = 0; i < n; i++)
            kv.put_string("s" + std::to_string(i), "val" + std::to_string(i));
        EXPECT_GE(kv.size(), (size_t)n);
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        for (int i = 0; i < n; i += 1000) {
            std::string v;
            EXPECT_EQ(kv.get_string("s" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "val" + std::to_string(i));
        }
    }
    cleanup(p);
}

TEST(QaCpp, VectorSetMapRoundtrip) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        uint8_t data[] = {1, 2, 3, 4, 5};
        EXPECT_EQ(kv.put_vector("v", data, 5), XCACHE_OK);
        EXPECT_EQ(kv.put_set("s", data, 5), XCACHE_OK);
        EXPECT_EQ(kv.put_map("m", data, 5), XCACHE_OK);

        std::vector<uint8_t> out;
        EXPECT_EQ(kv.get_vector("v", &out), XCACHE_OK);
        EXPECT_EQ(out.size(), 5u);
        EXPECT_EQ(kv.get_set("s", &out), XCACHE_OK);
        EXPECT_EQ(kv.get_map("m", &out), XCACHE_OK);
    }
    cleanup(p);
}

TEST(QaCpp, BinaryBlobAllBytes) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        std::vector<uint8_t> all(256);
        for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
        EXPECT_EQ(kv.put_blob("b", all.data(), 256), XCACHE_OK);
        std::vector<uint8_t> out;
        EXPECT_EQ(kv.get_blob("b", &out), XCACHE_OK);
        EXPECT_EQ(out.size(), 256u);
        for (int i = 0; i < 256; i++) EXPECT_EQ(out[i], all[i]);
    }
    cleanup(p);
}

TEST(QaCpp, EmptyStringValue) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_string("e", ""), XCACHE_OK);
        EXPECT_TRUE(kv.exists("e"));
        std::string v;
        EXPECT_EQ(kv.get_string("e", &v), XCACHE_OK);
        EXPECT_TRUE(v.empty());
    }
    cleanup(p);
}

TEST(QaCpp, TombstoneChurn) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        for (int c = 0; c < 50; c++) {
            for (int i = 0; i < 20; i++) kv.put_i64("k" + std::to_string(i), i);
            for (int i = 0; i < 20; i++) kv.remove("k" + std::to_string(i));
        }
        EXPECT_EQ(kv.size(), 0u);
    }
    cleanup(p);
}

TEST(QaCpp, CppEmptyBlob) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        EXPECT_EQ(kv.put_blob("e", nullptr, 0), XCACHE_OK);
        std::vector<uint8_t> v;
        EXPECT_EQ(kv.get_blob("e", &v), XCACHE_OK);
        EXPECT_TRUE(v.empty());
    }
    cleanup(p);
}

TEST(QaCpp, SyncNoCrash) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p);
        kv.put_string("k", "v");
        kv.sync();
    }
    cleanup(p);
}
