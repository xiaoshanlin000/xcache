# xcache

**English** | [中文](./README.md)

A persistent KV storage engine based on mmap, implemented in C++17 with C API. Designed for **local cache, config storage, AI inference KV cache, mobile/embedded**. Not suitable for crash-safe transactional workloads.

## Performance

Benchmark (Apple Silicon M4, Release):

| Operation | Latency | Throughput |
|-----------|---------|------------|
| Put | ~105 ns | 9.5 M/s |
| Get (hit) | ~65 ns | 15 M/s |
| Get (miss) | ~4.4 ns | 225 M/s |
| Exists | ~13.5 ns | 74 M/s |
| Remove | ~62 ns | 16 M/s |

**Mixed throughput** (Put:Get:Exists:Remove = 1:1:1:1, different keys, no contention):

| Scenario | 2 threads | 4 threads | 8 threads |
|----------|-----------|-----------|-----------|
| Put Only | 5.5 M/s | 3.2 M/s | 3.2 M/s |
| Mixed R/W | 9.5 M/s | 9.0 M/s | 8.1 M/s |

## Quick Start

```cpp
#include "xcache/xcache.h"

xcache::XCache kv("/tmp/mydb");

if (kv.put_string("hello", "world") != XCACHE_OK) {
    // write failed (disk full, etc.)
}

std::string v;
if (kv.get_string("hello", &v) == XCACHE_OK) {
    // v → "world"
}

kv.remove("hello");
```

C API: see `xcache/xcache_c.h` (full C bindings with TTL support).

## Use Cases

- **Local cache** — process-level KV cache, data survives restart
- **Config storage** — app configs, feature flags, low-frequency reads/writes
- **AI inference cache** — LLM prompt cache, embedding persistence, token state storage
- **Mobile / Embedded** — zero external deps, cross-platform (iOS/Android/HarmonyOS/Linux/macOS)
- **Multi-process sharing** — read-heavy shared storage, multiple processes can read concurrently

## Not Recommended For

- **✗ Transactions** — single-key put/remove is atomic (CAS), but no multi-key atomic commit or rollback. Using xcache as a relational database? Wrong tool.
- **✗ Zero data loss** — no msync at runtime; process crash + unwritten kernel pages = lost writes. Write ordering (data before index) protects existing data. sync() narrows the window but can't eliminate it. This is the mmap tradeoff.

## Core Design: Write Ordering

xcache's correctness rests on **data before index** — data is written to `.dat` first, then the `.idx` slot is CAS'd to point to it.

```
put:  serialize → alloc(.dat tail) → memcpy → CAS .idx slot
                                           ↑          ↑
                                      step 2 done    step 3 runs
```

Step 2 must complete before step 3 executes. On crash:
- **Before step 3** — `.idx` still points to old data, intact
- **After step 3** — `.idx` points to new data, visible

No path to half-written data exists. This is why xcache **needs no CRC** — write ordering already guarantees you can never read incomplete data.

### Storage Format

| File | Role | Structure |
|------|------|-----------|
| `path.idx` | Hash index table (mmap) | 4KB Header + N × 8B Slot, open addressing + linear probing, initial 64K slots, 1.5x auto-grow at 70% load |
| `path.dat` | Data blocks (chained mmap, append-only) | Records appended sequentially, old data reclaimed by `rebuild()` |

Single block max 1TB (40-bit offset), 256 blocks max, total **256TB**.

### Write / Delete / Maintenance

| Op | Flow |
|----|------|
| put | CAS alloc at `.dat` tail → memcpy data → CAS `.idx` slot |
| remove | CAS slot to tombstone (kTomb), `.dat` untouched, reclaimed by `rebuild()` |
| rehash | Pause writes → new `.idx.tmp` → rehash entries → atomic swap → resume (reads never block) |
| rebuild | Pause writes → new `.idx+.dat.tmp` → copy live entries → rename → resume (reads never block) |

### Lazy Expiry

When `get` / `exists` / `get_type` encounter an expired key, they **automatically CAS the slot to tombstone** (read operations are no longer read-only). Subsequent operations on that key will see it as deleted.

- `scan()` / `get_all_keys()` do **not** trigger lazy expiry — they skip expired keys without modifying the index
- CAS failure (another thread already removed it) is harmless — the next operation will naturally skip it
- rebuild is unaffected — it already filters out expired keys

### Concurrency Model

| Scenario | Mechanism |
|----------|-----------|
| Different keys | Non-blocking, CAS operations |
| Same key put/remove | CAS guarantees integrity |
| rebuild / rehash | Pause writes, reads unaffected |
| Multi-process | Read LOCK_SH / Write LOCK_EX + generation auto-remap; flock overhead reduces single-core throughput to ~60–70% of single-process mode |

## Durability Guarantee

xcache's durability strategy hinges on one tradeoff: **speed means no fsync on every write**.

No msync at runtime — writes go to mmap memory, kernel handles writeback. Write ordering (data before index) guarantees **no data corruption on crash**: data goes to `.dat` first, index writes to `.idx` after — incomplete data is never readable.

| Guarantee | Condition |
|-----------|-----------|
| Existing data safe | Any crash — write ordering prevents corruption |
| No recent writes lost | Normal process exit (destructor auto-syncs) |
| May lose recent writes | Process crash + kernel not flushed (mmap tradeoff) |

Call `sync()` after writes for stronger durability. Destructor auto-syncs, so manual sync before close is rarely needed.

## Lifecycle Management

`sync()` and `rebuild()` are not required for day-to-day use. Understanding them helps you control storage behavior:

| Call | What it does | When to call |
|------|-------------|--------------|
| `sync()` | Flushes `.idx` to disk | After writing important data; destructor syncs automatically — usually not needed before close |
| `rebuild()` | Compacts tombstones and expired keys, reclaims `.dat` space | After deleting many keys or TTL expiry — writes pause during rebuild, reads don't block |

## Behavior Notes

| Behavior | Note |
|----------|------|
| Strict type matching | `put_i64` data requires `get_i64`; `get_string()` returns `XCACHE_TYPE_MISMATCH` |
| `size()` is approximate | doesn't proactively scan for expiry, but read ops trigger lazy expiry auto-cleanup |
| `get_type()` returns error code | `*out` contains type on success; `XCACHE_NOT_FOUND` / `XCACHE_EXPIRED` when missing/expired |
| Repeated `remove()` returns `XCACHE_NOT_FOUND` | not idempotent; also returns `XCACHE_NOT_FOUND` if key was already cleaned by lazy expiry |
| No CRC | write ordering (data before index) guarantees integrity |
| TTL precision | expire_seconds in seconds, minimum 1 second, 0 = no expiry |
| Max key/value size | 4GB each (32-bit length field) |
| Max DB size | 1TB per block (40-bit offset), 256 blocks max, total 256TB |

## Build

```bash
cmake --preset release && cmake --build --preset release
ctest --test-dir build
./build/benchmarks/bench_latency
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `XCACHE_BUILD_TESTS` | ON (desktop) / OFF (mobile) | Build tests |
| `XCACHE_BUILD_BENCHMARKS` | ON (desktop) / OFF (mobile) | Build benchmarks |
| `XCACHE_BUILD_EXAMPLES` | ON (desktop) / OFF (mobile) | Build examples |

Tests/benchmarks/examples default OFF on iOS / Android / HarmonyOS.

### Third-Party Dependencies

| Library | Version | Use | License |
|---------|---------|-----|---------|
| [Google Test](https://github.com/google/googletest) | 1.15.2 | Unit tests | BSD |
| [Google Benchmark](https://github.com/google/benchmark) | 1.9.1 | Benchmarks | Apache 2.0 |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.2 | Hashing | BSD

## API

### C++ (xcache.h)

```cpp
XCache(path, block_size = 256K, multi_process = false, init_slots = 65536);

// write (return XCACHE_OK / XCACHE_NO_SPACE / XCACHE_IO_ERROR / XCACHE_INVALID_ARG)
xcache_error_t put_string(key, value, expire_seconds = 0);
xcache_error_t put_i64(key, int64_t, expire_seconds = 0);
// ... put_i32|f32|f64|bool|blob|vector|set|map (same pattern)

// read (return XCACHE_OK / XCACHE_NOT_FOUND / XCACHE_TYPE_MISMATCH / XCACHE_EXPIRED)
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
void scan(fn);   // fn(key, type) → bool, return true to continue
void sync();
void close();
```

### C (xcache_c.h)

```c
xcache_t* xcache_open(path, block_size);
xcache_t* xcache_open_ex(path, block_size, multi_process);
xcache_t* xcache_open_ex2(path, block_size, multi_process, init_slots);
void      xcache_close(kv);

// write (return xcache_error_t)
xcache_error_t xcache_put_string(kv, key, value);
xcache_error_t xcache_put_string_ex(kv, key, value, expire_seconds);
xcache_error_t xcache_put_i64|i32|f32|f64|bool|blob|...(kv, ...);
xcache_error_t xcache_put_i64_ex|i32_ex|...(kv, ..., expire_seconds);

// read (return xcache_error_t, data via out param)
xcache_error_t xcache_get_string(kv, key, char** out);
xcache_error_t xcache_get_i64(kv, key, int64_t* out);
xcache_error_t xcache_get_i32(kv, key, int32_t* out);
xcache_error_t xcache_get_f64(kv, key, double* out);
xcache_error_t xcache_get_bool(kv, key, int* out);
xcache_error_t xcache_get_blob|vector|set|map(kv, key, xcache_blob_t* out);  // free out->data with xcache_free_blob

xcache_error_t xcache_get_type(kv, key, xcache_value_type_t* out);
int    xcache_exists(kv, key);
xcache_error_t xcache_remove(kv, key);
size_t xcache_size(kv);
xcache_error_t xcache_rebuild(kv);
void   xcache_scan(kv, fn, userdata);    // fn returns 0 to stop

// Memory management: get_string/out->data must be freed
void xcache_free_string(char* s);
void xcache_free_blob(xcache_blob_t b);
```

## Error Handling

All operations return `xcache_error_t`:

| Code | Meaning |
|------|---------|
| `XCACHE_OK` | Success |
| `XCACHE_NOT_FOUND` | Key not found |
| `XCACHE_TYPE_MISMATCH` | Type mismatch |
| `XCACHE_EXPIRED` | Key expired |
| `XCACHE_NO_SPACE` | Disk full |
| `XCACHE_IO_ERROR` | IO error (mmap fail, file corrupt) |
| `XCACHE_INVALID_ARG` | Invalid argument (NULL param, etc.) |

C++ example:

```cpp
std::string val;
if (kv.get_string("hello", &val) == XCACHE_OK) {
    // use val
} else {
    // key not found, type mismatch, or expired
}
```

C example:

```c
xcache_value_type_t type;
xcache_error_t err = xcache_get_type(kv, "hello", &type);
if (err == XCACHE_OK) {
    // key exists, type is the data type
} else if (err == XCACHE_NOT_FOUND) {
    // key not found
} else if (err == XCACHE_EXPIRED) {
    // key expired
}
```

## License

[MIT](./LICENSE)
