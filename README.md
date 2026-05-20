# xcache

[English](./README_en.md) | **中文**

基于 mmap 的持久化 KV 存储引擎，C++17 实现，附带 C API。适合**本地缓存、配置存储、AI 推理 KV 缓存、移动端嵌入式场景**，不适合需要严格 crash 安全的事务型场景。

## 性能

Benchmark (Apple Silicon M4, Release)：

| 操作 | 延迟 | 吞吐 |
|------|------|------|
| Put | ~105 ns | 9.5 M/s |
| Get (hit) | ~65 ns | 15 M/s |
| Get (miss) | ~4.4 ns | 225 M/s |
| Exists | ~13.5 ns | 74 M/s |
| Remove | ~62 ns | 16 M/s |

**混合读写吞吐**（Put:Get:Exists:Remove = 1:1:1:1，不同 key 无冲突）：

| 场景 | 2 线程 | 4 线程 | 8 线程 |
|------|--------|--------|--------|
| 纯 Put | 5.5 M/s | 3.2 M/s | 3.2 M/s |
| 混合读写 | 9.5 M/s | 9.0 M/s | 8.1 M/s |

## Quick Start

```cpp
#include "xcache/xcache.h"

xcache::XCache kv("/tmp/mydb");

if (kv.put_string("hello", "world") != XCACHE_OK) {
    // 写入失败（磁盘满等）
}

std::string v;
if (kv.get_string("hello", &v) == XCACHE_OK) {
    // v → "world"
}

kv.remove("hello");
```

C API 见 `xcache/xcache_c.h`（提供完整 C 绑定，支持 TTL）。

## 适用场景

- **本地缓存** — 进程级 KV 缓存，重启后数据自动恢复
- **配置存储** — 应用配置、特征开关等低频读写场景
- **AI 推理缓存** — LLM prompt 缓存、embedding 持久化、token state 存储
- **移动端 / 嵌入式** — 零外部依赖，跨平台（iOS/Android/鸿蒙/Linux/macOS）
- **多进程共享** — 读多写少的共享存储，多进程可同时读取

## 不适用的场景

- **✗ 事务** — 单 key 的 put/remove 是原子的（CAS 保证），但没有多 key 原子提交和回滚。需要将 xcache 当作关系数据库使用？不合适。
- **✗ 零数据丢失** — 运行时无 msync，进程 crash 加内核未刷盘会丢最后几笔写入。写顺序保证（data before index）已有数据不会被破坏。调用 sync() 可缩小丢失窗口，但无法根除。这是 mmap 方案的设计取舍。

## 核心设计：写入顺序保证

xcache 的正确性基石是 **data before index**——数据先写入 `.dat`，确认写完后才修改 `.idx` 索引指向它。

```
put:  serialize → alloc(.dat 尾部) → memcpy → CAS 改 .idx slot
                                          ↑          ↑
                                    第 2 步完成    第 3 步才执行
```

第 2 步完成前第 3 步永远不会执行。任何时候 crash：
- **第 3 步前 crash** — `.idx` 仍指向旧数据，旧数据完好
- **第 3 步后 crash** — `.idx` 已指向新数据，新数据可见

不存在指向半写入数据的路径。这是 xcache **不需要 CRC** 的原因——写序保证已经堵死了读到不完整数据的可能。

### 存储格式

| 文件 | 作用 | 结构 |
|------|------|------|
| `path.idx` | 哈希索引表（mmap） | 4KB Header + N × 8B Slot，开放定址 + 线性探测，初始 64K slots，70% 负载自动 1.5x 扩容 |
| `path.dat` | 数据块（链式 mmap，追加写入） | 记录顺序追加，旧数据占用的空间由 `rebuild()` 回收 |

单 block 最大 1TB（40 位偏移），最多 256 个 block，总容量上限 **256TB**。

### 写入 / 删除 / 维护

| 操作 | 流程 |
|------|------|
| put | CAS 在 `.dat` 尾部分配空间 → memcpy 写入数据 → CAS 修改 `.idx` slot |
| remove | CAS 将 slot 标记为墓碑（kTomb），`.dat` 数据不动，`rebuild()` 回收空间 |
| rehash | 暂停写 → 新建 `.idx.tmp` → 重插所有 entry → 原子切换 → 恢复写（读全程不阻塞） |
| rebuild | 暂停写 → 新建 `.idx+.dat.tmp` → 拷贝有效 entry → rename 替换 → 恢复写（读全程不阻塞） |

### 并发模型

| 场景 | 机制 |
|------|------|
| 不同 key 读写 | 互不阻塞，CAS 原子操作 |
| 同 key put/remove | CAS 保证操作完整 |
| rebuild / rehash | 暂停写，读不受影响 |
| 多进程 | 读 LOCK_SH / 写 LOCK_EX + generation 版本号自动 remap；多进程模式每次操作有 flock 开销，单核吞吐约为单进程的 60–70% |

## 持久化保证

xcache 的持久化策略围绕一个核心权衡：**要速度就不能每次写都刷盘**。

运行时不做 msync，数据写入 mmap 内存后由内核脏页回写机制处理。写顺序保证（data before index）确保 **crash 不会损坏已有数据**——data 先落 `.dat`，index 后改 `.idx`，任何时候崩溃都不会读到半写入的数据。

| 保证程度 | 条件 |
|----------|------|
| 已有数据安全 | 任何时候 crash，已有数据不会损坏（写顺序保证） |
| 不丢最近写入 | 进程正常退出（析构函数自动 sync 兜底） |
| 可能丢最近写入 | 进程 crash + 内核未刷盘（mmap 方案的设计取舍） |

需要更强制持久化时在写入后调 `sync()`。析构函数自动 sync，所以 close 前一般不用手动调。

## 生命周期管理

`sync()` 和 `rebuild()` 都不是日常必调操作，了解它们能帮你更好地掌控存储行为：

| 操作 | 做什么 | 何时调用 |
|------|--------|----------|
| `sync()` | 将 `.idx` 刷到磁盘 | 写入重要数据后调用确保持久化；close 前一般不需调（析构函数自动 sync 兜底） |
| `rebuild()` | 压缩墓碑和过期 key，回收 `.dat` 空间 | 删除大量 key 或 TTL 过期后想释放磁盘空间时调用；重建期间写暂停，读不阻塞 |

## 需要注意的行为

| 行为 | 说明 |
|------|------|
| 类型严格匹配 | `put_i64` 写入的数据只能用 `get_i64` 读取，`get_string()` 返回 `XCACHE_TYPE_MISMATCH` |
| `size()` 不精确 | 不检查 TTL，可能包含已过期但未 rebuild 的 key |
| `get_type()` 返回 error code | 成功时 `*out` 包含类型；`XCACHE_NOT_FOUND` / `XCACHE_EXPIRED` 表示不存在或已过期 |
| 重复 `remove()` 返回 `XCACHE_NOT_FOUND` | 非幂等操作，删除已删除或已过期的 key 返回 `XCACHE_NOT_FOUND` |
| 无 CRC 校验 | 写顺序（data before index）保证数据完整性，不额外校验 |
| TTL 精度 | expire_seconds 单位为秒，最小 1 秒，0=永不过期 |
| 单 key 上限 | key 最大 4GB（4 字节长度字段），value 最大 4GB（uint32_t 字段上限） |
| 总容量上限 | 单个 block 最大 1TB（40 位偏移），最多 256 个 block，总计 256TB |

## 构建

```bash
cmake --preset release && cmake --build --preset release
ctest --test-dir build              # 跑测试
./build/benchmarks/bench_latency    # 跑 benchmark
```

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `XCACHE_BUILD_TESTS` | ON (桌面) / OFF (移动端) | 构建测试 |
| `XCACHE_BUILD_BENCHMARKS` | ON (桌面) / OFF (移动端) | 构建 benchmark |
| `XCACHE_BUILD_EXAMPLES` | ON (桌面) / OFF (移动端) | 构建示例 |

iOS / Android / 鸿蒙 上 test/benchmark/example 默认关闭。

### 第三方依赖

| 库 | 版本 | 用途 | 协议 |
|---|---|---|---|
| [Google Test](https://github.com/google/googletest) | 1.15.2 | 单元测试 | BSD |
| [Google Benchmark](https://github.com/google/benchmark) | 1.9.1 | 性能测试 | Apache 2.0 |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.2 | 哈希计算 | BSD

## API

类型定义统一在 `xcache_types.h` 中（C 和 C++ 共用）：

| C 类型 | C++ 别名 | 说明 |
|--------|----------|------|
| `xcache_error_t` | — | 错误码：`XCACHE_OK`, `XCACHE_NOT_FOUND`, `XCACHE_TYPE_MISMATCH`, `XCACHE_EXPIRED`, `XCACHE_NO_SPACE`, `XCACHE_IO_ERROR`, `XCACHE_INVALID_ARG` |
| `xcache_value_type_t` | — | 数据类型标签：`XCACHE_TEXT`, `XCACHE_INT64` 等 |
| `xcache_blob_t` | — | 二进制数据：`{data, len}` |

所有操作通过返回值报告结果，数据通过 out 参数传递。

> ⚠️ 注意：`exists()` 和 `size()` 不返回错误码，它们只是查询不存在错误状态。

### C++ (xcache.h)

```cpp
XCache(path, block_size = 256K, multi_process = false, init_slots = 65536);

// 写（返回 XCACHE_OK / XCACHE_NO_SPACE / XCACHE_IO_ERROR / XCACHE_INVALID_ARG）
xcache_error_t put_string(key, value, expire_seconds = 0);
xcache_error_t put_i64(key, int64_t, expire_seconds = 0);
// ... put_i32|f32|f64|bool|blob|vector|set|map 同上

// 读（返回 XCACHE_OK / XCACHE_NOT_FOUND / XCACHE_TYPE_MISMATCH / XCACHE_EXPIRED）
xcache_error_t get_string(key, string* out);
xcache_error_t get_i64(key, int64_t* out);
xcache_error_t get_i32(key, int32_t* out);
xcache_error_t get_f32(key, float* out);
xcache_error_t get_f64(key, double* out);
xcache_error_t get_bool(key, bool* out);
xcache_error_t get_blob|vector|set|map(key, vector<uint8_t>* out);

xcache_error_t get_type(key, xcache_value_type_t* out);
bool exists(key);
xcache_error_t remove(key);      // XCACHE_OK / XCACHE_NOT_FOUND
size_t size();
vector<string> get_all_keys();
xcache_error_t rebuild();
void scan(fn);   // fn(key, type) → bool，返回 true 继续
void sync();
void close();
```

### C (xcache_c.h)

```c
xcache_t* xcache_open(path, block_size);
xcache_t* xcache_open_ex(path, block_size, multi_process);
xcache_t* xcache_open_ex2(path, block_size, multi_process, init_slots);
void      xcache_close(kv);

// 写（返回 xcache_error_t）
xcache_error_t xcache_put_string(kv, key, value);
xcache_error_t xcache_put_string_ex(kv, key, value, expire_seconds);
xcache_error_t xcache_put_i64|i32|f32|f64|bool|blob|...(kv, ...);
xcache_error_t xcache_put_i64_ex|i32_ex|...(kv, ..., expire_seconds);

// 读（返回 xcache_error_t，数据通过 out 参数）
xcache_error_t xcache_get_string(kv, key, char** out);
xcache_error_t xcache_get_i64(kv, key, int64_t* out);
xcache_error_t xcache_get_i32(kv, key, int32_t* out);
xcache_error_t xcache_get_f32(kv, key, float* out);
xcache_error_t xcache_get_f64(kv, key, double* out);
xcache_error_t xcache_get_bool(kv, key, int* out);
xcache_error_t xcache_get_blob|vector|set|map(kv, key, xcache_blob_t* out);  // out->data 需 xcache_free_blob

xcache_error_t xcache_get_type(kv, key, xcache_value_type_t* out);
int    xcache_exists(kv, key);
xcache_error_t xcache_remove(kv, key);
size_t xcache_size(kv);
xcache_error_t xcache_rebuild(kv);
void   xcache_scan(kv, fn, userdata);    // fn 返回 0 停止

// 内存管理：get_string 成功时 *out 指向 malloc 内存，必须 free
void xcache_free_string(char* s);
void xcache_free_blob(xcache_blob_t b);
```

## 错误处理

所有操作通过 `xcache_error_t` 返回值报告结果：

| 返回值 | 含义 |
|--------|------|
| `XCACHE_OK` | 成功 |
| `XCACHE_NOT_FOUND` | key 不存在 |
| `XCACHE_TYPE_MISMATCH` | 类型不匹配 |
| `XCACHE_EXPIRED` | key 已过期 |
| `XCACHE_NO_SPACE` | 磁盘空间不足 |
| `XCACHE_IO_ERROR` | IO 错误 |
| `XCACHE_INVALID_ARG` | 参数错误（NULL 等） |

C++ 示例如下：

```cpp
std::string val;
if (kv.get_string("hello", &val) == XCACHE_OK) {
    // 使用 val
} else {
    // key 不存在、类型不匹配或已过期
}
```

C 示例如下：

```c
xcache_value_type_t type;
xcache_error_t err = xcache_get_type(kv, "hello", &type);
if (err == XCACHE_OK) {
    // key 存在，type 为数据类型
} else if (err == XCACHE_NOT_FOUND) {
    // key 不存在
} else if (err == XCACHE_EXPIRED) {
    // key 已过期
}
```

## License

[MIT](./LICENSE)
