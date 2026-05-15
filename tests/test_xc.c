#include "xcache/xcache_c.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/xc_%d_%d", getpid(), rand());

    /* NULL 参数测试 */
    assert(xcache_put_string(NULL, "k", "v") != XCACHE_OK);
    assert(xcache_put_string(NULL, NULL, NULL) != XCACHE_OK);
    char* tmp = NULL; assert(xcache_get_string(NULL, "k", &tmp) != XCACHE_OK);
    assert(!xcache_exists(NULL, "k"));
    assert(xcache_remove(NULL, "k") != XCACHE_OK);
    assert(xcache_size(NULL) == 0);
    xcache_close(NULL);  /* 不应 crash */

    xcache_t* kv = xcache_open(path, 0);
    assert(kv);

    /* NULL key 参数测试（valid handle） */
    assert(xcache_put_string(kv, NULL, "v") != XCACHE_OK);
    assert(xcache_put_string(kv, NULL, NULL) != XCACHE_OK);
    assert(xcache_get_string(kv, NULL, &tmp) != XCACHE_OK);
    assert(!xcache_exists(kv, NULL));
    assert(xcache_remove(kv, NULL) != XCACHE_OK);
    xcache_value_type_t tt; assert(xcache_get_type(kv, NULL, &tt) != XCACHE_OK);

    assert(xcache_put_string(kv, "hello", "world") == XCACHE_OK);
    assert(xcache_size(kv) == 1);

    char* v = NULL;
    assert(xcache_get_string(kv, "hello", &v) == XCACHE_OK && strcmp(v, "world") == 0);
    xcache_free_string(v);

    assert(xcache_exists(kv, "hello"));
    assert(!xcache_exists(kv, "nope"));

    assert(xcache_remove(kv, "hello") == XCACHE_OK);
    assert(!xcache_exists(kv, "hello"));

    int n = 5000;
    for (int i = 0; i < n; ++i) {
        char k[32], val[32];
        snprintf(k, sizeof(k), "k%d", i);
        snprintf(val, sizeof(val), "v%d", i);
        assert(xcache_put_string(kv, k, val) == XCACHE_OK);
    }
    assert(xcache_size(kv) == (size_t)n);

    xcache_close(kv);

    /* reopen */
    kv = xcache_open(path, 0);
    assert(kv);
    assert(xcache_size(kv) == (size_t)n);
    v = NULL; assert(xcache_get_string(kv, "k42", &v) == XCACHE_OK && strcmp(v, "v42") == 0);
    xcache_free_string(v);

    xcache_close(kv);

    /* cleanup */
    char idx[160], dat[160];
    snprintf(idx, sizeof(idx), "%s.idx", path);
    snprintf(dat, sizeof(dat), "%s.dat", path);
    unlink(idx); unlink(dat);

    printf("C API: ok\n");
    return 0;
}
