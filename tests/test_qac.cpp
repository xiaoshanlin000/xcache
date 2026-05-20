#include "xcache/xcache_c.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string tmp_path() {
    return "/tmp/xc_qa_" + std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".idx").c_str());
    ::unlink((p + ".dat").c_str());
}

TEST(XCapiNullSafety, NullHandle) {
    EXPECT_NE(xcache_put_string(NULL, "k", "v"), XCACHE_OK);
    EXPECT_NE(xcache_put_i64(NULL, "k", 42), XCACHE_OK);
    EXPECT_NE(xcache_put_f64(NULL, "k", 1.0), XCACHE_OK);
    EXPECT_NE(xcache_put_bool(NULL, "k", 1), XCACHE_OK);
    EXPECT_NE(xcache_put_blob(NULL, "k", "x", 1), XCACHE_OK);

    char* tmp = NULL;
    EXPECT_NE(xcache_get_string(NULL, "k", &tmp), XCACHE_OK);
    int64_t i64 = 0;
    EXPECT_NE(xcache_get_i64(NULL, "k", &i64), XCACHE_OK);
    xcache_value_type_t tt;
    EXPECT_NE(xcache_get_type(NULL, "k", &tt), XCACHE_OK);
    EXPECT_EQ(xcache_exists(NULL, "k"), 0);
    EXPECT_NE(xcache_remove(NULL, "k"), XCACHE_OK);
    EXPECT_EQ(xcache_size(NULL), 0u);
    xcache_close(NULL);
    EXPECT_NE(xcache_rebuild(NULL), XCACHE_OK);
}

TEST(XCapiNullSafety, NullPath) {
    EXPECT_EQ(xcache_open(NULL, 0), nullptr);
    EXPECT_EQ(xcache_open_ex(NULL, 0, 0), nullptr);
    EXPECT_EQ(xcache_open_ex2(NULL, 0, 0, 1024), nullptr);
}

TEST(XCapiNullSafety, NullKey) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_NE(xcache_put_string(kv, NULL, "v"), XCACHE_OK);
    EXPECT_NE(xcache_put_i64(kv, NULL, 42), XCACHE_OK);
    EXPECT_NE(xcache_put_blob(kv, NULL, "x", 1), XCACHE_OK);
    EXPECT_NE(xcache_get_string(kv, NULL, NULL), XCACHE_OK);
    EXPECT_EQ(xcache_exists(kv, NULL), 0);
    EXPECT_NE(xcache_remove(kv, NULL), XCACHE_OK);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiNullSafety, NullOutParam) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string(kv, "k", "v"), XCACHE_OK);
    EXPECT_NE(xcache_get_string(kv, "k", NULL), XCACHE_OK);
    EXPECT_NE(xcache_get_type(kv, "k", NULL), XCACHE_OK);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiNullSafety, PutBlobNullData) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_NE(xcache_put_blob(kv, "b1", NULL, 100), XCACHE_OK);
    EXPECT_EQ(xcache_put_blob(kv, "b2", NULL, 0), XCACHE_OK);
    EXPECT_EQ(xcache_put_blob(kv, "b3", (const uint8_t*)"data", 4), XCACHE_OK);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, GetStringReturnsMallocdMemory) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string(kv, "k", "hello"), XCACHE_OK);
    char* v = NULL;
    EXPECT_EQ(xcache_get_string(kv, "k", &v), XCACHE_OK);
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "hello");
    xcache_free_string(v);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, GetStringMissing) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    char* tmp = NULL;
    EXPECT_NE(xcache_get_string(kv, "nope", &tmp), XCACHE_OK);
    EXPECT_EQ(tmp, nullptr);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, TypeMismatch) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_i64(kv, "k", 42), XCACHE_OK);
    xcache_blob_t b;
    EXPECT_NE(xcache_get_blob(kv, "k", &b), XCACHE_OK);
    int64_t v;
    EXPECT_EQ(xcache_get_i64(kv, "k", &v), XCACHE_OK);
    EXPECT_EQ(v, 42);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, BlobRoundtrip) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_blob(kv, "b", (const uint8_t*)"blobdata", 8), XCACHE_OK);
    xcache_blob_t out;
    out.data = NULL; out.len = 0;
    EXPECT_EQ(xcache_get_blob(kv, "b", &out), XCACHE_OK);
    ASSERT_EQ(out.len, 8u);
    ASSERT_NE(out.data, nullptr);
    EXPECT_EQ(std::memcmp(out.data, "blobdata", 8), 0);
    xcache_free_blob(out);

    EXPECT_NE(xcache_get_blob(kv, "b", NULL), XCACHE_OK);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, ExistsBehaviour) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string(kv, "k", "v"), XCACHE_OK);
    EXPECT_EQ(xcache_exists(kv, "k"), 1);
    EXPECT_EQ(xcache_exists(kv, "nope"), 0);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, LargeTTL) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string_ex(kv, "k", "v", 365U * 86400), XCACHE_OK);
    EXPECT_EQ(xcache_exists(kv, "k"), 1);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, Rebuild) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string(kv, "a", "1"), XCACHE_OK);
    EXPECT_EQ(xcache_put_string(kv, "b", "2"), XCACHE_OK);
    EXPECT_EQ(xcache_rebuild(kv), XCACHE_OK);
    EXPECT_EQ(xcache_exists(kv, "a"), 1);
    EXPECT_EQ(xcache_exists(kv, "b"), 1);

    xcache_close(kv);
    cleanup(p);
}

TEST(XCapiEdgeCases, ReopenPersistence) {
    auto p = tmp_path();
    {
        auto* kv = xcache_open(p.c_str(), 0);
        ASSERT_NE(kv, nullptr);
        EXPECT_EQ(xcache_put_string(kv, "k", "v"), XCACHE_OK);
        xcache_close(kv);
    }
    {
        auto* kv = xcache_open(p.c_str(), 0);
        ASSERT_NE(kv, nullptr);
        EXPECT_EQ(xcache_exists(kv, "k"), 1);
        char* v = NULL;
        EXPECT_EQ(xcache_get_string(kv, "k", &v), XCACHE_OK);
        ASSERT_NE(v, nullptr);
        EXPECT_STREQ(v, "v");
        xcache_free_string(v);
        xcache_close(kv);
    }
    cleanup(p);
}

TEST(XCapiEdgeCases, Size) {
    auto p = tmp_path();
    auto* kv = xcache_open(p.c_str(), 0);
    ASSERT_NE(kv, nullptr);

    EXPECT_EQ(xcache_put_string(kv, "a", "1"), XCACHE_OK);
    EXPECT_EQ(xcache_put_string(kv, "b", "2"), XCACHE_OK);
    EXPECT_EQ(xcache_put_string(kv, "c", "3"), XCACHE_OK);
    EXPECT_GE(xcache_size(kv), 3u);

    xcache_close(kv);
    cleanup(p);
}
