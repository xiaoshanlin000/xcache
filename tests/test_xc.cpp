#include "xcache/xcache_c.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static std::string tmp_path() {
    return "/tmp/xc_" + std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".idx").c_str());
    ::unlink((p + ".dat").c_str());
}

TEST(XCTest, NullSafety) {
    EXPECT_NE(xcache_put_string(NULL, "k", "v"), XCACHE_OK);
    EXPECT_NE(xcache_put_string(NULL, NULL, NULL), XCACHE_OK);
    char* tmp = NULL;
    EXPECT_NE(xcache_get_string(NULL, "k", &tmp), XCACHE_OK);
    EXPECT_FALSE(xcache_exists(NULL, "k"));
    EXPECT_NE(xcache_remove(NULL, "k"), XCACHE_OK);
    EXPECT_EQ(xcache_size(NULL), 0u);
    xcache_close(NULL);
}

TEST(XCTest, NullKeyOnValidHandle) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_NE(xcache_put_string(kv, NULL, "v"), XCACHE_OK);
    EXPECT_NE(xcache_put_string(kv, NULL, NULL), XCACHE_OK);
    char* tmp = NULL;
    EXPECT_NE(xcache_get_string(kv, NULL, &tmp), XCACHE_OK);
    EXPECT_FALSE(xcache_exists(kv, NULL));
    EXPECT_NE(xcache_remove(kv, NULL), XCACHE_OK);
    xcache_value_type_t tt;
    EXPECT_NE(xcache_get_type(kv, NULL, &tt), XCACHE_OK);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCTest, PutGetRemove) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string(kv, "hello", "world"), XCACHE_OK);
    EXPECT_EQ(xcache_size(kv), 1u);

    char* v = NULL;
    EXPECT_EQ(xcache_get_string(kv, "hello", &v), XCACHE_OK);
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "world");
    xcache_free_string(v);

    EXPECT_TRUE(xcache_exists(kv, "hello"));
    EXPECT_FALSE(xcache_exists(kv, "nope"));

    EXPECT_EQ(xcache_remove(kv, "hello"), XCACHE_OK);
    EXPECT_FALSE(xcache_exists(kv, "hello"));

    xcache_close(kv);
    cleanup(p);
}

TEST(XCTest, BulkInsertThenReopen) {
    auto p = tmp_path();
    int n = 5000;
    {
        auto* kv = xcache_open(p.c_str(), 0);
        ASSERT_NE(kv, nullptr);
        for (int i = 0; i < n; ++i) {
            auto k = "k" + std::to_string(i);
            auto val = "v" + std::to_string(i);
            EXPECT_EQ(xcache_put_string(kv, k.c_str(), val.c_str()), XCACHE_OK);
        }
        EXPECT_EQ(xcache_size(kv), (size_t)n);
        xcache_close(kv);
    }
    {
        auto* kv = xcache_open(p.c_str(), 0);
        ASSERT_NE(kv, nullptr);
        EXPECT_EQ(xcache_size(kv), (size_t)n);
        char* v = NULL;
        EXPECT_EQ(xcache_get_string(kv, "k42", &v), XCACHE_OK);
        ASSERT_NE(v, nullptr);
        EXPECT_STREQ(v, "v42");
        xcache_free_string(v);
        xcache_close(kv);
    }
    cleanup(p);
}

// ── typed put/get ────────────────────────────────────────────────

TEST(XCTest, TypedInt64) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_i64(kv, "k", 42), XCACHE_OK);
    EXPECT_EQ(xcache_put_i64(kv, "neg", -1), XCACHE_OK);

    int64_t v;
    EXPECT_EQ(xcache_get_i64(kv, "k", &v), XCACHE_OK);
    EXPECT_EQ(v, 42);
    int64_t neg_v;
    EXPECT_EQ(xcache_get_i64(kv, "neg", &neg_v), XCACHE_OK);
    EXPECT_EQ(neg_v, -1);

    // type mismatch: i64 stored, string requested
    char* s = NULL;
    EXPECT_NE(xcache_get_string(kv, "k", &s), XCACHE_OK);

    xcache_value_type_t t;
    EXPECT_EQ(xcache_get_type(kv, "k", &t), XCACHE_OK);
    EXPECT_EQ(t, XCACHE_INT64);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCTest, TypedFloatDoubleBool) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_f32(kv, "f32", 3.14f), XCACHE_OK);
    EXPECT_EQ(xcache_put_f64(kv, "f64", 2.718281828459045), XCACHE_OK);
    EXPECT_EQ(xcache_put_bool(kv, "btrue", 1), XCACHE_OK);
    EXPECT_EQ(xcache_put_bool(kv, "bfalse", 0), XCACHE_OK);

    float fv;
    EXPECT_EQ(xcache_get_f32(kv, "f32", &fv), XCACHE_OK);
    EXPECT_FLOAT_EQ(fv, 3.14f);

    double dv;
    EXPECT_EQ(xcache_get_f64(kv, "f64", &dv), XCACHE_OK);
    EXPECT_DOUBLE_EQ(dv, 2.718281828459045);

    int bv;
    EXPECT_EQ(xcache_get_bool(kv, "btrue", &bv), XCACHE_OK);
    EXPECT_EQ(bv, 1);
    EXPECT_EQ(xcache_get_bool(kv, "bfalse", &bv), XCACHE_OK);
    EXPECT_EQ(bv, 0);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCTest, TypedBlob) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    unsigned char data[] = {0x00, 0xFF, 0xAB, 0xCD};
    EXPECT_EQ(xcache_put_blob(kv, "b", data, sizeof(data)), XCACHE_OK);

    xcache_blob_t out = {NULL, 0};
    EXPECT_EQ(xcache_get_blob(kv, "b", &out), XCACHE_OK);
    ASSERT_EQ(out.len, 4u);
    EXPECT_EQ(out.data[0], 0x00);
    EXPECT_EQ(out.data[3], 0xCD);
    xcache_free_blob(out);

    xcache_value_type_t t;
    EXPECT_EQ(xcache_get_type(kv, "b", &t), XCACHE_OK);
    EXPECT_EQ(t, XCACHE_BLOB);

    xcache_close(kv);
    cleanup(p);
}

// ── TTL ──────────────────────────────────────────────────────────

TEST(XCTest, TtlExpires) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    xcache_put_string_ex(kv, "forever", "permanent", 0);
    xcache_put_string_ex(kv, "gone", "temporary", 1);

    EXPECT_TRUE(xcache_exists(kv, "forever"));
    EXPECT_TRUE(xcache_exists(kv, "gone"));

    std::this_thread::sleep_for(std::chrono::seconds(2));

    EXPECT_TRUE(xcache_exists(kv, "forever"));
    EXPECT_FALSE(xcache_exists(kv, "gone"));

    char* v = NULL;
    EXPECT_EQ(xcache_get_string(kv, "forever", &v), XCACHE_OK);
    EXPECT_STREQ(v, "permanent");
    xcache_free_string(v);

    char* v2 = NULL;
    EXPECT_NE(xcache_get_string(kv, "gone", &v2), XCACHE_OK);

    xcache_close(kv);
    cleanup(p);
}

// ── rebuild ──────────────────────────────────────────────────────

TEST(XCTest, RebuildCompacts) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    for (int i = 0; i < 100; ++i) {
        auto k = "k" + std::to_string(i);
        auto v = "v" + std::to_string(i);
        EXPECT_EQ(xcache_put_string(kv, k.c_str(), v.c_str()), XCACHE_OK);
    }
    for (int i = 0; i < 50; ++i) {
        auto k = "k" + std::to_string(i);
        EXPECT_EQ(xcache_remove(kv, k.c_str()), XCACHE_OK);
    }
    EXPECT_EQ(xcache_size(kv), 50u);
    EXPECT_EQ(xcache_rebuild(kv), XCACHE_OK);
    EXPECT_EQ(xcache_size(kv), 50u);

    // verify survivors
    for (int i = 50; i < 100; ++i) {
        auto k = "k" + std::to_string(i);
        char* v = NULL;
        EXPECT_EQ(xcache_get_string(kv, k.c_str(), &v), XCACHE_OK);
        auto expected = "v" + std::to_string(i);
        EXPECT_STREQ(v, expected.c_str());
        xcache_free_string(v);
    }

    xcache_close(kv);
    cleanup(p);
}

// ── scan ─────────────────────────────────────────────────────────

static int scan_count_fn(const char*, xcache_value_type_t, void* userdata) {
    auto* count = static_cast<int*>(userdata);
    ++(*count);
    return 1;
}

static int scan_stop_early_fn(const char*, xcache_value_type_t, void* userdata) {
    auto* count = static_cast<int*>(userdata);
    ++(*count);
    return 0;  // stop
}

TEST(XCTest, ScanEntries) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    xcache_put_string(kv, "a", "1");
    xcache_put_string(kv, "b", "2");
    xcache_put_string(kv, "c", "3");

    int count = 0;
    xcache_scan(kv, scan_count_fn, &count);
    EXPECT_EQ(count, 3);

    int early = 0;
    xcache_scan(kv, scan_stop_early_fn, &early);
    EXPECT_EQ(early, 1);

    xcache_close(kv);
    cleanup(p);
}

// ── edge ─────────────────────────────────────────────────────────

TEST(XCTest, RemoveNonExistent) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_NE(xcache_remove(kv, "nope"), XCACHE_OK);
    EXPECT_EQ(xcache_size(kv), 0u);
    EXPECT_EQ(xcache_put_string(kv, "k", "v"), XCACHE_OK);
    EXPECT_EQ(xcache_size(kv), 1u);
    EXPECT_EQ(xcache_remove(kv, "k"), XCACHE_OK);
    EXPECT_NE(xcache_remove(kv, "k"), XCACHE_OK);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCTest, EmptyKey) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string(kv, "", "empty_key"), XCACHE_OK);
    char* v = NULL;
    EXPECT_EQ(xcache_get_string(kv, "", &v), XCACHE_OK);
    EXPECT_STREQ(v, "empty_key");
    xcache_free_string(v);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCTest, GetTypeNotFound) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    xcache_value_type_t t;
    EXPECT_NE(xcache_get_type(kv, "nope", &t), XCACHE_OK);
    EXPECT_FALSE(xcache_exists(kv, "nope"));

    xcache_close(kv);
    cleanup(p);
}
