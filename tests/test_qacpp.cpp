#include "xcache/xcache.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <cmath>
#include <set>
#include <limits>

#define TCHECK(expr, msg) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s\n", msg); _fails++; } \
    else { printf("  PASS: %s\n", msg); } \
} while(0)

static int _fails = 0;

static std::string tmp_path() {
    return "/tmp/xf_qa_" + std::to_string(::getpid()) + "_"
         + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".dat").c_str());
    ::unlink((p + ".idx").c_str());
}

int main() {
    printf("=== C++ Edges & Regression ===\n");

    /* 1. Overwrite same key with different types repeatedly */
    {
        printf("\n--- type cycling ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        TCHECK(kv.put_i64("k", 100) == XCACHE_OK, "put_i64");
        TCHECK(kv.put_string("k", "text") == XCACHE_OK, "overwrite with string");
        TCHECK(kv.put_f64("k", 3.14) == XCACHE_OK, "overwrite with f64");
        TCHECK(kv.put_bool("k", true) == XCACHE_OK, "overwrite with bool");
        TCHECK(kv.put_i32("k", -42) == XCACHE_OK, "overwrite with i32");
        int32_t v32;
        TCHECK(kv.get_i32("k", &v32) == XCACHE_OK, "get_i32 after cycling");
        TCHECK(v32 == -42, "i32 value correct after cycling");
        TCHECK(kv.size() == 1u, "size=1 after type cycling");
        kv.close();
        cleanup(p);
    }

    /* 2. Negative values and special floats */
    {
        printf("\n--- numeric edges ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        TCHECK(kv.put_i64("neg", INT64_MIN) == XCACHE_OK, "put_i64 INT64_MIN");
        int64_t v64;
        TCHECK(kv.get_i64("neg", &v64) == XCACHE_OK, "get_i64 INT64_MIN");
        TCHECK(v64 == INT64_MIN, "INT64_MIN roundtrip");

        TCHECK(kv.put_f64("inf", std::numeric_limits<double>::infinity()) == XCACHE_OK, "put_f64 +inf");
        double vf64;
        TCHECK(kv.get_f64("inf", &vf64) == XCACHE_OK, "get_f64 +inf");
        TCHECK(std::isinf(vf64) && vf64 > 0, "+inf roundtrip");

        TCHECK(kv.put_f64("ninf", -std::numeric_limits<double>::infinity()) == XCACHE_OK, "put_f64 -inf");
        TCHECK(kv.get_f64("ninf", &vf64) == XCACHE_OK, "get_f64 -inf");
        TCHECK(std::isinf(vf64) && vf64 < 0, "-inf roundtrip");

        TCHECK(kv.put_f64("nan", std::numeric_limits<double>::quiet_NaN()) == XCACHE_OK, "put_f64 NaN");
        TCHECK(kv.get_f64("nan", &vf64) == XCACHE_OK, "get_f64 NaN");
        TCHECK(std::isnan(vf64), "NaN roundtrip");

        TCHECK(kv.put_f32("f32max", std::numeric_limits<float>::max()) == XCACHE_OK, "put_f32 max");
        float vf32;
        TCHECK(kv.get_f32("f32max", &vf32) == XCACHE_OK, "get_f32 max");
        TCHECK(vf32 == std::numeric_limits<float>::max(), "f32 max roundtrip");
        kv.close();
        cleanup(p);
    }

    /* 3. Remove then put different type → verify type */
    {
        printf("\n--- remove + retype ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        kv.put_string("k", "v");
        kv.remove("k");
        TCHECK(kv.put_i64("k", 42) == XCACHE_OK, "put_i64 after remove");
        xcache_value_type_t t;
        TCHECK(kv.get_type("k", &t) == XCACHE_OK, "get_type after retype");
        TCHECK(t == XCACHE_INT64, "type is XCACHE_INT64 after retype");
        kv.close();
        cleanup(p);
    }

    /* 4. get_all_keys after insert/remove */
    {
        printf("\n--- get_all_keys ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        kv.put_string("a", "1");
        kv.put_string("b", "2");
        kv.put_string("c", "3");
        auto keys = kv.get_all_keys();
        TCHECK(keys.size() == 3u, "get_all_keys after 3 puts");
        kv.remove("b");
        keys = kv.get_all_keys();
        TCHECK(keys.size() == 2u, "get_all_keys after remove");
        bool has_a = false, has_c = false;
        for (auto& k : keys) {
            if (k == "a") has_a = true;
            if (k == "c") has_c = true;
        }
        TCHECK(has_a && has_c, "keys a and c present, b gone");
        kv.close();
        cleanup(p);
    }

    /* 5. scan with stop condition */
    {
        printf("\n--- scan with stop ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        for (int i = 0; i < 100; i++) {
            kv.put_i64("k" + std::to_string(i), i);
        }
        int count = 0;
        kv.scan([&](const std::string&, xcache_value_type_t) {
            count++;
            return count < 10; /* stop after 10 */
        });
        TCHECK(count == 10, "scan stopped after 10 iterations");
        kv.close();
        cleanup(p);
    }

    /* 6. reopen with different init_slots (read-only: respects existing file) */
    {
        printf("\n--- reopen with different init_slots ---\n");
        auto p = tmp_path();
        {
            xcache::XCache kv(p, 256UL << 10, false, 1024);
            for (int i = 0; i < 500; i++) {
                kv.put_i64("r" + std::to_string(i), i);
            }
        }
        {
            xcache::XCache kv(p, 256UL << 10, false, 99999);
            TCHECK(kv.size() == 500u, "reopen with diff init_slots: size preserved");
            int64_t v;
            TCHECK(kv.get_i64("r42", &v) == XCACHE_OK, "reopen: key r42 exists");
            TCHECK(v == 42, "reopen: r42 value correct");
        }
        cleanup(p);
    }

    /* 7. close twice, close+reopen+close */
    {
        printf("\n--- close multiple times ---\n");
        auto p = tmp_path();
        {
            xcache::XCache kv(p);
            kv.put_string("k", "v");
            kv.close();
            kv.close(); /* double close - no crash */
        }
        printf("  PASS: double close (no crash)\n");
        {
            xcache::XCache kv(p);
            kv.close();
            kv.close();
        }
        printf("  PASS: close empty then close again (no crash)\n");
        cleanup(p);
    }

    /* 8. move assignment from closed handle */
    {
        printf("\n--- move semantics ---\n");
        auto p = tmp_path();
        {
            xcache::XCache a(p);
            a.put_string("k", "v");
            a.close();
            xcache::XCache b(std::move(a));
            b.close();
        }
        printf("  PASS: move from closed handle (no crash)\n");
        cleanup(p);
    }

    /* 9. concurrent read during rebuild */
    {
        printf("\n--- concurrent read during rebuild ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        for (int i = 0; i < 5000; i++) {
            kv.put_i64("k" + std::to_string(i), i);
        }

        std::atomic<bool> stop{false};
        std::atomic<int> reads_ok{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; t++) {
            threads.emplace_back([&]() {
                while (!stop.load(std::memory_order_relaxed)) {
                    int64_t v;
                    int idx = rand() % 5000;
                    if (kv.get_i64("k" + std::to_string(idx), &v) == XCACHE_OK) {
                        if (v == (int64_t)idx) reads_ok++;
                    }
                }
            });
        }
        for (int i = 0; i < 5; i++) {
            kv.rebuild();
        }
        stop.store(true);
        for (auto& t : threads) t.join();
        TCHECK(reads_ok.load() > 0, "concurrent reads during rebuild succeeded");
        printf("  reads_ok during rebuild: %d\n", reads_ok.load());
        kv.close();
        cleanup(p);
    }

    /* 10. large number of keys + scan + rebuild + verify */
    {
        printf("\n--- stress: many keys + rebuild ---\n");
        auto p = tmp_path();
        {
            xcache::XCache kv(p);
            int n = 20000;
            for (int i = 0; i < n; i++) {
                kv.put_string("s" + std::to_string(i), "val" + std::to_string(i));
            }
            TCHECK(kv.size() >= (size_t)n, "size after bulk insert");
            kv.rebuild();
            TCHECK(kv.size() >= (size_t)n, "size after rebuild");
            for (int i = 0; i < n; i += 1000) {
                std::string v;
                auto err = kv.get_string("s" + std::to_string(i), &v);
                TCHECK(err == XCACHE_OK, "spot-check after rebuild");
                TCHECK(v == "val" + std::to_string(i), "value intact after rebuild");
            }
        }
        cleanup(p);
    }

    /* 11. put_vector, put_set, put_map roundtrip */
    {
        printf("\n--- vector/set/map operations ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        uint8_t data[] = {1, 2, 3, 4, 5};
        TCHECK(kv.put_vector("vec", data, 5) == XCACHE_OK, "put_vector");
        TCHECK(kv.put_set("set", data, 5) == XCACHE_OK, "put_set");
        TCHECK(kv.put_map("map", data, 5) == XCACHE_OK, "put_map");

        std::vector<uint8_t> vout;
        TCHECK(kv.get_vector("vec", &vout) == XCACHE_OK, "get_vector");
        TCHECK(vout.size() == 5 && vout[0] == 1, "vector data intact");

        TCHECK(kv.get_set("set", &vout) == XCACHE_OK, "get_set");
        TCHECK(vout.size() == 5, "set data intact");

        TCHECK(kv.get_map("map", &vout) == XCACHE_OK, "get_map");
        TCHECK(vout.size() == 5, "map data intact");
        kv.close();
        cleanup(p);
    }

    /* 12. put_blob with binary data containing all bytes */
    {
        printf("\n--- binary blob all bytes ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        std::vector<uint8_t> all_bytes(256);
        for (int i = 0; i < 256; i++) all_bytes[i] = (uint8_t)i;
        TCHECK(kv.put_blob("allbytes", all_bytes.data(), 256) == XCACHE_OK, "put_blob 256 all bytes");
        std::vector<uint8_t> out;
        TCHECK(kv.get_blob("allbytes", &out) == XCACHE_OK, "get_blob 256 all bytes");
        TCHECK(out.size() == 256, "blob size match");
        bool match = true;
        for (int i = 0; i < 256; i++) if (out[i] != all_bytes[i]) match = false;
        TCHECK(match, "blob all bytes roundtrip exact");
        kv.close();
        cleanup(p);
    }

    /* 13. empty string value, exists check */
    {
        printf("\n--- empty string edge ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        TCHECK(kv.put_string("empty", "") == XCACHE_OK, "put empty string");
        TCHECK(kv.exists("empty"), "exists empty string key");
        std::string v;
        TCHECK(kv.get_string("empty", &v) == XCACHE_OK, "get empty string");
        TCHECK(v.empty(), "empty string value is empty");
        kv.close();
        cleanup(p);
    }

    /* 14. many remove + put in loop (tombstone churn) */
    {
        printf("\n--- tombstone churn ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        for (int cycle = 0; cycle < 50; cycle++) {
            for (int i = 0; i < 20; i++) {
                kv.put_i64("churn_" + std::to_string(i), i);
            }
            for (int i = 0; i < 20; i++) {
                kv.remove("churn_" + std::to_string(i));
            }
        }
        TCHECK(kv.size() == 0u, "size=0 after tombstone churn");
        kv.close();
        cleanup(p);
    }

    /* 15. put_blob nullptr+0 len (C++ API allows) */
    {
        printf("\n--- C++ empty blob ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        TCHECK(kv.put_blob("empty_b", nullptr, 0) == XCACHE_OK, "put_blob nullptr+0");
        std::vector<uint8_t> bv;
        TCHECK(kv.get_blob("empty_b", &bv) == XCACHE_OK, "get_blob nullptr+0");
        TCHECK(bv.empty(), "get empty blob yields empty vector");
        kv.close();
        cleanup(p);
    }

    /* 16. sync() call */
    {
        printf("\n--- sync ---\n");
        auto p = tmp_path();
        xcache::XCache kv(p);
        kv.put_string("s", "v");
        kv.sync(); /* no crash, returns void */
        printf("  PASS: sync() no crash\n");
        kv.close();
        cleanup(p);
    }

    printf("\n=== Results: %d failures ===\n", _fails);
    return _fails > 0 ? 1 : 0;
}
