// MIT License
//
// Copyright (c) 2026 xiaoshanlin000
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "xcache/xcache_c.h"
#include "xcache/xcache.h"
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {

static xcache_t* open_impl(const char* path, size_t block_size, int multi_process, uint32_t init_slots) {
    if (!path) { return nullptr; }
    if (block_size == 0) { block_size = 256UL << 10; }
    if (init_slots == 0) { init_slots = 65536; }
    try {
        auto* kv = new (std::nothrow) xcache::XCache(path, block_size, multi_process != 0, init_slots);
        return reinterpret_cast<xcache_t*>(kv);
    } catch (...) {
        return nullptr;
    }
}

xcache_t* xcache_open(const char* path, size_t block_size) {
    return open_impl(path, block_size, false, 65536);
}

xcache_t* xcache_open_ex(const char* path, size_t block_size, int multi_process) {
    return open_impl(path, block_size, multi_process, 65536);
}

xcache_t* xcache_open_ex2(const char* path, size_t block_size, int multi_process, uint32_t init_slots) {
    return open_impl(path, block_size, multi_process, init_slots);
}

void xcache_sync(xcache_t* kv) {
    if (!kv) { return; }
    try {
        reinterpret_cast<xcache::XCache*>(kv)->sync();
    } catch (...) {
    }
}

void xcache_close(xcache_t* kv) {
    try {
        delete reinterpret_cast<xcache::XCache*>(kv);
    } catch (...) {
    }
}

xcache_error_t xcache_put_string(xcache_t* kv, const char* key, const char* value) {
    if (!kv || !key || !value) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_string(key, value);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_string_ex(xcache_t* kv, const char* key, const char* value, uint32_t expire_seconds) {
    if (!kv || !key || !value) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_string(key, value, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_i64(xcache_t* kv, const char* key, int64_t value) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_i64(key, value);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_i64_ex(xcache_t* kv, const char* key, int64_t value, uint32_t expire_seconds) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_i64(key, value, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_i32(xcache_t* kv, const char* key, int32_t value) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_i32(key, value);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_i32_ex(xcache_t* kv, const char* key, int32_t value, uint32_t expire_seconds) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_i32(key, value, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_f32(xcache_t* kv, const char* key, float value) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_f32(key, value);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_f32_ex(xcache_t* kv, const char* key, float value, uint32_t expire_seconds) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_f32(key, value, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_f64(xcache_t* kv, const char* key, double value) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_f64(key, value);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_f64_ex(xcache_t* kv, const char* key, double value, uint32_t expire_seconds) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_f64(key, value, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_bool(xcache_t* kv, const char* key, int value) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_bool(key, value != 0);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_bool_ex(xcache_t* kv, const char* key, int value, uint32_t expire_seconds) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_bool(key, value != 0, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_blob(xcache_t* kv, const char* key, const void* data, size_t len) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_blob(key, data, len);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_blob_ex(xcache_t* kv, const char* key, const void* data, size_t len, uint32_t expire_seconds) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_blob(key, data, len, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_vector(xcache_t* kv, const char* key, const void* data, size_t len) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_vector(key, data, len);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_vector_ex(xcache_t* kv, const char* key, const void* data, size_t len, uint32_t expire_seconds) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_vector(key, data, len, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_set(xcache_t* kv, const char* key, const void* data, size_t len) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_set(key, data, len);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_set_ex(xcache_t* kv, const char* key, const void* data, size_t len, uint32_t expire_seconds) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_set(key, data, len, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_map(xcache_t* kv, const char* key, const void* data, size_t len) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_map(key, data, len);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_put_map_ex(xcache_t* kv, const char* key, const void* data, size_t len, uint32_t expire_seconds) {
    if (!kv || !key || !data) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->put_map(key, data, len, expire_seconds);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_get_string(xcache_t* kv, const char* key, char** out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        std::string tmp;
        auto err = reinterpret_cast<xcache::XCache*>(kv)->get_string(key, &tmp);
        if (err != XCACHE_OK) { return err; }
        char* s = static_cast<char*>(std::malloc(tmp.size() + 1));
        if (!s) { return XCACHE_NO_SPACE; }
        std::memcpy(s, tmp.data(), tmp.size());
        s[tmp.size()] = '\0';
        *out = s;
        return XCACHE_OK;
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_get_i64(xcache_t* kv, const char* key, int64_t* out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->get_i64(key, out);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_get_i32(xcache_t* kv, const char* key, int32_t* out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->get_i32(key, out);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_get_f32(xcache_t* kv, const char* key, float* out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->get_f32(key, out);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_get_f64(xcache_t* kv, const char* key, double* out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->get_f64(key, out);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_get_bool(xcache_t* kv, const char* key, int* out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        bool tmp;
        auto err = reinterpret_cast<xcache::XCache*>(kv)->get_bool(key, &tmp);
        if (err != XCACHE_OK) { return err; }
        *out = tmp ? 1 : 0;
        return XCACHE_OK;
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

static xcache_error_t get_blob_impl(xcache_t* kv, const char* key,
                                     xcache_error_t (xcache::XCache::*get_fn)(const std::string&, std::vector<uint8_t>*) const,
                                     xcache_blob_t* out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        std::vector<uint8_t> tmp;
        auto err = (reinterpret_cast<xcache::XCache*>(kv)->*get_fn)(key, &tmp);
        if (err != XCACHE_OK) { return err; }
        auto* p = static_cast<unsigned char*>(std::malloc(tmp.size()));
        if (!p && tmp.size() > 0) { return XCACHE_NO_SPACE; }
        if (tmp.size() > 0) {
            std::memcpy(p, tmp.data(), tmp.size());
        }
        out->data = p;
        out->len  = tmp.size();
        return XCACHE_OK;
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

xcache_error_t xcache_get_blob(xcache_t* kv, const char* key, xcache_blob_t* out) {
    return get_blob_impl(kv, key, &xcache::XCache::get_blob, out);
}

xcache_error_t xcache_get_vector(xcache_t* kv, const char* key, xcache_blob_t* out) {
    return get_blob_impl(kv, key, &xcache::XCache::get_vector, out);
}

xcache_error_t xcache_get_set(xcache_t* kv, const char* key, xcache_blob_t* out) {
    return get_blob_impl(kv, key, &xcache::XCache::get_set, out);
}

xcache_error_t xcache_get_map(xcache_t* kv, const char* key, xcache_blob_t* out) {
    return get_blob_impl(kv, key, &xcache::XCache::get_map, out);
}

xcache_error_t xcache_get_type(xcache_t* kv, const char* key, xcache_value_type_t* out) {
    if (!kv || !key || !out) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->get_type(key, out);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

int xcache_exists(xcache_t* kv, const char* key) {
    if (!kv || !key) { return 0; }
    try {
        return static_cast<int>(reinterpret_cast<xcache::XCache*>(kv)->exists(key));
    } catch (...) {
        return 0;
    }
}

xcache_error_t xcache_remove(xcache_t* kv, const char* key) {
    if (!kv || !key) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->remove(key);
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

size_t xcache_size(xcache_t* kv) {
    if (!kv) { return 0; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->size();
    } catch (...) {
        return 0;
    }
}

xcache_error_t xcache_rebuild(xcache_t* kv) {
    if (!kv) { return XCACHE_INVALID_ARG; }
    try {
        return reinterpret_cast<xcache::XCache*>(kv)->rebuild();
    } catch (...) {
        return XCACHE_IO_ERROR;
    }
}

void xcache_scan(xcache_t* kv, xcache_scan_fn fn, void* userdata) {
    if (!kv || !fn) { return; }
    try {
        reinterpret_cast<xcache::XCache*>(kv)->scan(
            [fn, userdata](const std::string& key, xcache_value_type_t type) {
                return fn(key.c_str(), type, userdata) != 0;
            });
    } catch (...) {
    }
}

void xcache_free_string(char* s) {
    try {
        std::free(s);
    } catch (...) {
    }
}

void xcache_free_blob(xcache_blob_t b) {
    try {
        std::free(b.data);
    } catch (...) {
    }
}

}  // extern "C"
