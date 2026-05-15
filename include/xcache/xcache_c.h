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

#ifndef XCACHE_XCACHE_C_H_
#define XCACHE_XCACHE_C_H_

#include "xcache/xcache_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xcache_s xcache_t;

// ══════════════════════════════════════════════════════════════════
//
//  打开 / 关闭
//
// ══════════════════════════════════════════════════════════════════

xcache_t* xcache_open(const char* path, size_t block_size);

xcache_t* xcache_open_ex(const char* path, size_t block_size, int multi_process);

xcache_t* xcache_open_ex2(const char* path, size_t block_size, int multi_process, uint32_t init_slots);

void xcache_sync(xcache_t* kv);

void xcache_close(xcache_t* kv); 

xcache_error_t xcache_put_string(xcache_t* kv, const char* key, const char* value);

xcache_error_t xcache_put_string_ex(xcache_t* kv, const char* key, const char* value,
                                    uint32_t expire_seconds);

xcache_error_t xcache_put_i64(xcache_t* kv, const char* key, int64_t value);

xcache_error_t xcache_put_i64_ex(xcache_t* kv, const char* key, int64_t value,
                                 uint32_t expire_seconds);

xcache_error_t xcache_put_i32(xcache_t* kv, const char* key, int32_t value);

xcache_error_t xcache_put_i32_ex(xcache_t* kv, const char* key, int32_t value,
                                 uint32_t expire_seconds);

xcache_error_t xcache_put_f32(xcache_t* kv, const char* key, float value);

xcache_error_t xcache_put_f32_ex(xcache_t* kv, const char* key, float value,
                                 uint32_t expire_seconds);

xcache_error_t xcache_put_f64(xcache_t* kv, const char* key, double value);

xcache_error_t xcache_put_f64_ex(xcache_t* kv, const char* key, double value,
                                 uint32_t expire_seconds);

xcache_error_t xcache_put_bool(xcache_t* kv, const char* key, int value);

xcache_error_t xcache_put_bool_ex(xcache_t* kv, const char* key, int value,
                                  uint32_t expire_seconds);

xcache_error_t xcache_put_blob(xcache_t* kv, const char* key, const void* data, size_t len);

xcache_error_t xcache_put_blob_ex(xcache_t* kv, const char* key, const void* data,
                                  size_t len, uint32_t expire_seconds);

xcache_error_t xcache_put_vector(xcache_t* kv, const char* key, const void* data, size_t len);

xcache_error_t xcache_put_vector_ex(xcache_t* kv, const char* key, const void* data,
                                    size_t len, uint32_t expire_seconds);

xcache_error_t xcache_put_set(xcache_t* kv, const char* key, const void* data, size_t len);

xcache_error_t xcache_put_set_ex(xcache_t* kv, const char* key, const void* data,
                                 size_t len, uint32_t expire_seconds);

xcache_error_t xcache_put_map(xcache_t* kv, const char* key, const void* data, size_t len);

xcache_error_t xcache_put_map_ex(xcache_t* kv, const char* key, const void* data,
                                 size_t len, uint32_t expire_seconds);
 
xcache_error_t xcache_get_string(xcache_t* kv, const char* key, char** out);

xcache_error_t xcache_get_i64(xcache_t* kv, const char* key, int64_t* out);

xcache_error_t xcache_get_i32(xcache_t* kv, const char* key, int32_t* out);

xcache_error_t xcache_get_f32(xcache_t* kv, const char* key, float* out);

xcache_error_t xcache_get_f64(xcache_t* kv, const char* key, double* out);

xcache_error_t xcache_get_bool(xcache_t* kv, const char* key, int* out);

xcache_error_t xcache_get_blob(xcache_t* kv, const char* key, xcache_blob_t* out);

xcache_error_t xcache_get_vector(xcache_t* kv, const char* key, xcache_blob_t* out);

xcache_error_t xcache_get_set(xcache_t* kv, const char* key, xcache_blob_t* out);

xcache_error_t xcache_get_map(xcache_t* kv, const char* key, xcache_blob_t* out); 

xcache_error_t xcache_get_type(xcache_t* kv, const char* key, xcache_value_type_t* out);

int xcache_exists(xcache_t* kv, const char* key);

xcache_error_t xcache_remove(xcache_t* kv, const char* key);

size_t xcache_size(xcache_t* kv);

xcache_error_t xcache_rebuild(xcache_t* kv);

typedef int (*xcache_scan_fn)(const char* key, xcache_value_type_t type, void* userdata);

void xcache_scan(xcache_t* kv, xcache_scan_fn fn, void* userdata);

void xcache_free_string(char* s);

void xcache_free_blob(xcache_blob_t b);

#ifdef __cplusplus
}
#endif

#endif /* XCACHE_XCACHE_C_H_ */
