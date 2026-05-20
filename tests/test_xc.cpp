#include "xcache/xcache_c.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>

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
