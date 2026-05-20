#include "xcache/xcache_c.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(expr, msg) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("  PASS: %s\n", msg); } \
} while(0)

int main() {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/xc_qa_%d_%d", getpid(), rand());

    printf("=== C API Null-Safety Tests ===\n");

    printf("\n--- NULL handle ---\n");
    CHECK(xcache_put_string(NULL, "k", "v") != XCACHE_OK, "put_string NULL handle");
    CHECK(xcache_put_i64(NULL, "k", 42) != XCACHE_OK, "put_i64 NULL handle");
    CHECK(xcache_put_f64(NULL, "k", 1.0) != XCACHE_OK, "put_f64 NULL handle");
    CHECK(xcache_put_bool(NULL, "k", 1) != XCACHE_OK, "put_bool NULL handle");
    CHECK(xcache_put_blob(NULL, "k", "x", 1) != XCACHE_OK, "put_blob NULL handle");

    char* tmp = NULL;
    CHECK(xcache_get_string(NULL, "k", &tmp) != XCACHE_OK, "get_string NULL handle");
    int64_t i64 = 0;
    CHECK(xcache_get_i64(NULL, "k", &i64) != XCACHE_OK, "get_i64 NULL handle");
    xcache_value_type_t tt;
    CHECK(xcache_get_type(NULL, "k", &tt) != XCACHE_OK, "get_type NULL handle");
    CHECK(xcache_exists(NULL, "k") == 0, "exists NULL handle returns 0");
    CHECK(xcache_remove(NULL, "k") != XCACHE_OK, "remove NULL handle");
    CHECK(xcache_size(NULL) == 0, "size NULL handle returns 0");
    xcache_close(NULL);
    printf("  PASS: close NULL handle (no crash)\n");
    CHECK(xcache_rebuild(NULL) != XCACHE_OK, "rebuild NULL handle");

    printf("\n--- NULL path ---\n");
    xcache_t* kv_null = xcache_open(NULL, 0);
    CHECK(kv_null == NULL, "xcache_open(NULL, 0) returns NULL");
    kv_null = xcache_open_ex(NULL, 0, 0);
    CHECK(kv_null == NULL, "xcache_open_ex(NULL, 0, 0) returns NULL");
    kv_null = xcache_open_ex2(NULL, 0, 0, 1024);
    CHECK(kv_null == NULL, "xcache_open_ex2(NULL, 0, 0, 1024) returns NULL");

    printf("\n--- Open valid handle ---\n");
    xcache_t* kv = xcache_open(path, 0);
    assert(kv);

    printf("\n--- NULL key (valid handle) ---\n");
    CHECK(xcache_put_string(kv, NULL, "v") != XCACHE_OK, "put_string NULL key");
    CHECK(xcache_put_string(kv, NULL, NULL) != XCACHE_OK, "put_string NULL key+val");
    CHECK(xcache_put_i64(kv, NULL, 42) != XCACHE_OK, "put_i64 NULL key");
    CHECK(xcache_put_blob(kv, NULL, "x", 1) != XCACHE_OK, "put_blob NULL key");

    tmp = NULL;
    CHECK(xcache_get_string(kv, NULL, &tmp) != XCACHE_OK, "get_string NULL key");
    CHECK(xcache_exists(kv, NULL) == 0, "exists NULL key");
    CHECK(xcache_remove(kv, NULL) != XCACHE_OK, "remove NULL key");
    CHECK(xcache_get_type(kv, NULL, &tt) != XCACHE_OK, "get_type NULL key");

    printf("\n--- NULL out parameter (valid handle) ---\n");
    CHECK(xcache_put_string(kv, "null_out_test", "test_value") == XCACHE_OK,
          "put_string for null_out test");
    CHECK(xcache_get_string(kv, "null_out_test", NULL) != XCACHE_OK,
          "get_string NULL out param");
    CHECK(xcache_get_type(kv, "null_out_test", NULL) != XCACHE_OK,
          "get_type NULL out param");

    printf("\n--- put_blob edge cases ---\n");
    CHECK(xcache_put_blob(kv, "bad_blob", NULL, 100) != XCACHE_OK,
          "put_blob NULL data, len=100 -> INVALID_ARG");
    CHECK(xcache_put_blob(kv, "good_blob", (const uint8_t*)"blobdata", 8) == XCACHE_OK,
          "put_blob valid data");

    xcache_blob_t blob_out;
    blob_out.data = NULL; blob_out.len = 0;
    CHECK(xcache_get_blob(kv, "good_blob", &blob_out) == XCACHE_OK,
          "get_blob valid data");
    CHECK(blob_out.len == 8, "blob length matches");
    CHECK(memcmp(blob_out.data, "blobdata", 8) == 0, "blob content matches");
    xcache_free_blob(blob_out);

    CHECK(xcache_get_blob(kv, "good_blob", NULL) != XCACHE_OK,
          "get_blob NULL blob out param");

    printf("\n--- get_string: allocated memory ---\n");
    CHECK(xcache_put_string(kv, "alloc_test", "hello_alloc") == XCACHE_OK,
          "put_string alloc_test");
    char* v = NULL;
    CHECK(xcache_get_string(kv, "alloc_test", &v) == XCACHE_OK,
          "get_string alloc_test");
    CHECK(v != NULL && strcmp(v, "hello_alloc") == 0,
          "get_string returns correct content");
    xcache_free_string(v);

    printf("\n--- exists edge cases ---\n");
    CHECK(xcache_exists(kv, "alloc_test") == 1, "exists true on valid key");
    CHECK(xcache_exists(kv, "no_such_key_ever") == 0, "exists false on missing");
    CHECK(xcache_exists(kv, "") == 0, "exists false on empty key (no such key)");

    printf("\n--- get_string: missing key ---\n");
    tmp = NULL;
    CHECK(xcache_get_string(kv, "never_added", &tmp) != XCACHE_OK,
          "get_string missing key");
    CHECK(tmp == NULL, "get_string missing: output stays NULL");

    printf("\n--- type mismatch ---\n");
    CHECK(xcache_put_i64(kv, "int_key", 42) == XCACHE_OK, "put_i64");
    xcache_blob_t mb;
    CHECK(xcache_get_blob(kv, "int_key", &mb) != XCACHE_OK,
          "get_blob on int key -> type mismatch");
    int64_t mi64;
    CHECK(xcache_get_i64(kv, "int_key", &mi64) == XCACHE_OK, "get_i64 on int key works");

    printf("\n--- TTL edge cases ---\n");
    CHECK(xcache_put_string_ex(kv, "ttl_large", "val", 365U * 86400) == XCACHE_OK,
          "put_string_ex with large TTL (365 days)");
    CHECK(xcache_put_string_ex(kv, "ttl_zero", "val", 0) == XCACHE_OK,
          "put_string_ex with zero TTL (no expiry)");

    printf("\n--- rebuild ---\n");
    CHECK(xcache_rebuild(kv) == XCACHE_OK, "rebuild succeeds");

    printf("\n--- close + reopen persistence ---\n");
    xcache_close(kv);
    kv = xcache_open(path, 0);
    CHECK(kv != NULL, "reopen after close succeeds");
    CHECK(xcache_exists(kv, "alloc_test") == 1, "data persists after close");
    CHECK(xcache_exists(kv, "good_blob") == 1, "blob data persists");
    CHECK(xcache_exists(kv, "int_key") == 1, "int data persists");

    v = NULL;
    CHECK(xcache_get_string(kv, "alloc_test", &v) == XCACHE_OK, "re-read string after reopen");
    CHECK(v != NULL && strcmp(v, "hello_alloc") == 0, "string content intact after reopen");
    xcache_free_string(v);

    printf("\n--- size ---\n");
    size_t sz = xcache_size(kv);
    CHECK(sz >= 5, "size returns reasonable value");
    printf("  size = %zu\n", sz);

    xcache_close(kv);
    char idx[160], dat[160];
    snprintf(idx, sizeof(idx), "%s.idx", path);
    snprintf(dat, sizeof(dat), "%s.dat", path);
    unlink(idx); unlink(dat);

    printf("\n=== Results: %d failures ===\n", failures);
    return failures > 0 ? 1 : 0;
}
