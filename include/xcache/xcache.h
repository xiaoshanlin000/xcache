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

#ifndef XCACHE_XCACHE_H_
#define XCACHE_XCACHE_H_

#include "xcache/xcache_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xcache {

class XCache {
    struct Impl;

  public:
    explicit XCache(const std::string& path,
                    std::size_t block_size = 256UL << 10,
                    bool multi_process = false,
                    uint32_t init_slots = 65536);

    ~XCache();

    XCache(const XCache&) = delete;
    XCache& operator=(const XCache&) = delete;
    XCache(XCache&&) noexcept;
    XCache& operator=(XCache&&) noexcept;

    xcache_error_t put_string(const std::string& key, const std::string& value,
                              std::uint32_t expire_seconds = 0);

    xcache_error_t put_i64(const std::string& key, std::int64_t value,
                           std::uint32_t expire_seconds = 0);

    xcache_error_t put_i32(const std::string& key, std::int32_t value,
                           std::uint32_t expire_seconds = 0);

    xcache_error_t put_f32(const std::string& key, float value,
                           std::uint32_t expire_seconds = 0);

    xcache_error_t put_f64(const std::string& key, double value,
                           std::uint32_t expire_seconds = 0);

    xcache_error_t put_bool(const std::string& key, bool value,
                            std::uint32_t expire_seconds = 0);

    xcache_error_t put_blob(const std::string& key, const void* data, std::size_t len,
                            std::uint32_t expire_seconds = 0);

    xcache_error_t put_vector(const std::string& key, const void* data, std::size_t len,
                              std::uint32_t expire_seconds = 0);

    xcache_error_t put_set(const std::string& key, const void* data, std::size_t len,
                           std::uint32_t expire_seconds = 0);

    xcache_error_t put_map(const std::string& key, const void* data, std::size_t len,
                           std::uint32_t expire_seconds = 0);

    xcache_error_t get_string(const std::string& key, std::string* out) const;

    xcache_error_t get_i64(const std::string& key, std::int64_t* out) const;

    xcache_error_t get_i32(const std::string& key, std::int32_t* out) const;

    xcache_error_t get_f32(const std::string& key, float* out) const;

    xcache_error_t get_f64(const std::string& key, double* out) const;

    xcache_error_t get_bool(const std::string& key, bool* out) const;

    xcache_error_t get_blob(const std::string& key, std::vector<std::uint8_t>* out) const;

    xcache_error_t get_vector(const std::string& key, std::vector<std::uint8_t>* out) const;

    xcache_error_t get_set(const std::string& key, std::vector<std::uint8_t>* out) const;

    xcache_error_t get_map(const std::string& key, std::vector<std::uint8_t>* out) const;

    xcache_error_t get_type(const std::string& key, xcache_value_type_t* out) const;

    bool exists(const std::string& key) const;

    xcache_error_t remove(const std::string& key);

    std::size_t size() const;

    std::vector<std::string> get_all_keys() const;

    void sync();

    void close();

    xcache_error_t rebuild();

    // 遍历全部有效 key(跳过过期/已删除)。回调返回 false 提前停止。
    // 注意:回调内不要调用 put/remove/rebuild 等写操作——它们可能触发
    // rehash/rebuild,而后者要等当前读操作排空,会造成死锁。
    using ScanFn = std::function<bool(const std::string& key, xcache_value_type_t type)>;

    void scan(const ScanFn& fn) const;

  private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace xcache

#endif  // XCACHE_XCACHE_H_
