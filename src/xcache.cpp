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

#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include "xcache/xcache.h"

#include <cstring>
#include <ctime>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ── huge page hint (Linux only) ──────────────────────────────
#if defined(__linux__)
#include <sys/mman.h>
static void hint_hugepage(void* addr, size_t sz) {
    ::madvise(addr, sz, MADV_HUGEPAGE);
}
#else
static void hint_hugepage(void*, size_t) {}
#endif

// ── platform file lock abstraction ─────────────────────────────

#if defined(__OHOS__)

// HarmonyOS: no flock, use fcntl-based POSIX locking
static void xc_lock_ex(int fd) {
    struct flock fl{};
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    ::fcntl(fd, F_SETLKW, &fl);
}
static void xc_lock_sh(int fd) {
    struct flock fl{};
    fl.l_type   = F_RDLCK;
    fl.l_whence = SEEK_SET;
    ::fcntl(fd, F_SETLKW, &fl);
}
static void xc_unlock(int fd) {
    struct flock fl{};
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    ::fcntl(fd, F_SETLK, &fl);
}

#elif defined(__unix__) || defined(__linux__) || defined(__APPLE__)

#include <sys/file.h>
static void xc_lock_ex(int fd) { ::flock(fd, LOCK_EX); }
static void xc_lock_sh(int fd) { ::flock(fd, LOCK_SH); }
static void xc_unlock(int fd) { ::flock(fd, LOCK_UN); }

#else

// unknown platform — locking is a no-op
static void xc_lock_ex(int) {}
static void xc_lock_sh(int) {}
static void xc_unlock(int) {}

#endif

namespace xcache {

// ── constants ────────────────────────────────────────────────────

static constexpr uint32_t kMagic     = 0x58434146;
static constexpr uint32_t kVersion   = 1;
static constexpr size_t   kHdrSz     = 4096;
static constexpr size_t   kMaxBlks   = 256;
static constexpr size_t   kInitSlotsDef = 64UL * 1024;
static constexpr double   kLoadMax   = 0.70;
static constexpr uint64_t kEmpty     = 0;
static constexpr uint64_t kTomb      = 1;

// Slot layout (64 bits): [ bid(24) | off(40) | tag(1) ]
// tag=1 → valid slot; kEmpty(0) and kTomb(1) have tag=0 (sentinels).
// bid: block index (≤kMaxBlks=256, encoding allows up to 24 bits)
// off: byte offset within block (up to 1 TB)
static uint64_t encode_slot(uint64_t bid, uint64_t off) {
    return ((bid << 40) | off) << 1 | 1;
}
static void decode_slot(uint64_t s, uint64_t& bid, uint64_t& off) {
    auto v = s >> 1; bid = v >> 40; off = v & 0xFFFFFFFFFF;
}
// BlkAlloc raw layout: [ block_size(32) | used_bytes(32) ]
static uint64_t blk_sz_of(uint64_t r)  { return r >> 32; }
static uint64_t blk_used_of(uint64_t r) { return r & 0xFFFFFFFF; }
static uint64_t blk_encode_r(uint64_t sz, uint64_t used) {
    return (sz << 32) | used;
}
static uint64_t align8(uint64_t v) { return (v + 7) & ~7ull; }
static uint32_t pad4(uint32_t n)  { return (n + 3) & ~3u; }

// ── on-disk structures ───────────────────────────────────────────

struct Header {
    uint32_t magic;
    uint32_t version;
    uint64_t hdr_size;
    uint64_t idx_size;
    uint64_t num_slots;
    uint64_t slots_off;
    uint64_t blk_alloc_off;
    uint64_t default_block_size;
    std::atomic<uint64_t> num_blocks;
    uint64_t max_blocks;
    std::atomic<uint64_t> data_file_size;
    std::atomic<uint64_t> num_entries;
    std::atomic<uint64_t> generation;
    char _pad[4000];
};
static_assert(sizeof(Header) == kHdrSz);

struct Slot {
    std::atomic<uint64_t> state;
};

struct BlkAlloc {
    std::atomic<uint64_t> raw{0};
};

struct Record {
    std::string key;
    xcache_value_type_t type;
    std::string raw_val;
    bool expired = false;
    uint64_t expire_at = 0;
};

// ── implementation ───────────────────────────────────────────────

struct XCache::Impl {
    struct DeferredMunmap { void* addr; size_t size; int fd; };
    mutable std::vector<DeferredMunmap> deferred_munmaps_;
    mutable std::mutex deferred_mtx_;

    mutable void*  idx_map_      = nullptr;
    mutable int    idx_fd_       = -1;
    mutable uint64_t idx_ino_ = 0;  // 当前映射的 .idx inode(remap 检测基准)
    mutable uint64_t idx_dev_ = 0;
    mutable int    dat_fd_       = -1;
    int    lock_fd_      = -1;  // path.lock:跨进程 flock 载体(永不 rename,锁不随 inode 失效)
    mutable void*  epoch_map_ = nullptr;  // lock 文件前 4 字节:mmap 的 epoch 计数器
    mutable uint32_t cached_epoch_ = 0;   // 本进程上次确认的 epoch
    mutable void*  blk_maps_[kMaxBlks] = {};
    mutable size_t num_blks_     = 0;
    mutable std::atomic<int32_t> active_ops_{0};
    mutable std::atomic<bool>    pause_flag_{false};       // 冻结写(rehash/rebuild 构建期)
    mutable std::atomic<bool>    pause_reads_flag_{false}; // 冻结读(rebuild 换指针瞬间)
    std::mutex  grow_mtx_;
    std::mutex  rehash_mtx_;
    std::string base_path_;
    std::string idx_path_;
    std::string dat_path_;
    bool    multi_process_ = false;
    uint64_t init_slots_ = kInitSlotsDef;

    Header* hdr() const { return static_cast<Header*>(idx_map_); }
    size_t  idx_sz() const { return hdr()->idx_size; }

    Slot* slots() const {
        return reinterpret_cast<Slot*>(
            static_cast<char*>(idx_map_) + hdr()->slots_off);
    }
    Slot& slot(size_t i) const { return slots()[i]; }

    BlkAlloc* blk_allocs() const {
        return reinterpret_cast<BlkAlloc*>(
            static_cast<char*>(idx_map_) + hdr()->blk_alloc_off);
    }
    BlkAlloc& blk_alloc(size_t i) const { return blk_allocs()[i]; }

    // ── enter / exit ─────────────────────────────────────────────

    void enter_read() const {
        if (multi_process_) {
            xc_lock_sh(lock_fd_);
        }
        // 换指针(rebuild)期间冻结新读者;冻结前已进入的读者由 active_ops_ 排空
        while (true) {
            active_ops_.fetch_add(1, std::memory_order_acquire);
            if (!pause_reads_flag_.load(std::memory_order_acquire)) { break; }
            active_ops_.fetch_sub(1, std::memory_order_release);
            while (pause_reads_flag_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        if (multi_process_) {
            remap_if_stale();
        }
    }

    void exit_read() const {
        active_ops_.fetch_sub(1, std::memory_order_release);
        if (multi_process_) {
            xc_unlock(lock_fd_);
        }
        process_deferred_munmaps();
    }

    void enter_write() const {
        if (multi_process_) {
            xc_lock_ex(lock_fd_);
        }
        active_ops_.fetch_add(1, std::memory_order_acquire);
        while (pause_flag_.load(std::memory_order_acquire)) {
            active_ops_.fetch_sub(1, std::memory_order_release);
            while (pause_flag_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            active_ops_.fetch_add(1, std::memory_order_acquire);
        }
        if (multi_process_) {
            remap_if_stale();
        }
    }

    void exit_write() const {
        active_ops_.fetch_sub(1, std::memory_order_release);
        if (multi_process_) {
            xc_unlock(lock_fd_);
        }
        process_deferred_munmaps();
    }

    // ── deferred munmap cleanup ──────────────────────────────────

    void process_deferred_munmaps() const {
        if (deferred_munmaps_.empty()) return;
        if (active_ops_.load(std::memory_order_acquire) > 0) return;
        std::lock_guard<std::mutex> lk(deferred_mtx_);
        if (active_ops_.load(std::memory_order_acquire) > 0) return; // double-check
        for (auto& d : deferred_munmaps_) {
            if (d.addr && d.addr != MAP_FAILED) munmap(d.addr, d.size);
            if (d.fd >= 0) ::close(d.fd);
        }
        deferred_munmaps_.clear();
    }

    // ── multi-process remap ───────────────────────────────────────

    // 从 dat_fd 偏移 0 起按块链布局读取各块大小并全部映射。
    // 任一环节失败则回滚已映射的块并返回 false(调用方保持旧映射)。
    bool map_all_blocks(int dat_fd, void** out_maps, size_t* out_szs,
                        uint64_t num_blocks, size_t* out_n) const {
        uint64_t off = 0;
        size_t n = 0;
        for (uint64_t i = 0; i < num_blocks; ++i) {
            uint64_t bsz;
            if (::pread(dat_fd, &bsz, sizeof(bsz), static_cast<off_t>(off))
                != static_cast<ssize_t>(sizeof(bsz))) {
                break;
            }
            void* m = mmap(nullptr, static_cast<size_t>(bsz),
                           PROT_READ | PROT_WRITE, MAP_SHARED, dat_fd,
                           static_cast<off_t>(off));
            if (m == MAP_FAILED) { break; }
            out_maps[n] = m;
            out_szs[n]  = static_cast<size_t>(bsz);
            ++n;
            off += bsz;
        }
        if (n != static_cast<size_t>(num_blocks)) {
            for (size_t i = 0; i < n; ++i) { munmap(out_maps[i], out_szs[i]); }
            return false;
        }
        *out_n = n;
        return true;
    }

    // 在 idx_fd_ 被重新赋值后刷新 inode 缓存(remap 检测基准)
    void update_idx_inode() const {
        struct stat st;
        if (idx_fd_ >= 0 && ::fstat(idx_fd_, &st) == 0) {
            idx_ino_ = static_cast<uint64_t>(st.st_ino);
            idx_dev_ = static_cast<uint64_t>(st.st_dev);
        }
    }

    // lock 文件前 4 字节的 epoch 计数器:rehash/rebuild 换文件后 +1。
    // 每 op 只需一次 mmap 原子 load(fast path 零系统调用),变化时才走 stat+remap。
    // 用 4 字节原子:32 位平台(如 ARMv7)上 4 字节原子保证 lock-free,
    // 8 字节 atomic 不保证,退化为进程内 mutex 会让跨进程协议静默失效。
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "epoch requires lock-free 32-bit atomics");
    uint32_t epoch() const {
        return static_cast<const std::atomic<uint32_t>*>(epoch_map_)->load(
            std::memory_order_acquire);
    }
    void bump_epoch() {
        if (epoch_map_) {
            static_cast<std::atomic<uint32_t>*>(epoch_map_)->fetch_add(
                1, std::memory_order_release);
        }
    }
    void cache_epoch() const {
        if (epoch_map_) { cached_epoch_ = epoch(); }
    }

    // generation 变化 → 重新映射 idx;若 .dat 同时换了 inode(rebuild 会替换
    // 两个文件),则数据块一并重映射。整个 remap 要么全部成功,要么保持旧映射,
    // 避免出现"新 idx + 旧 .dat"的错配。在 flock 保护下执行,文件视图稳定。
    void remap_if_stale() const {
        if (idx_fd_ < 0) { return; }

        // fast path:epoch 未变 → 没有进程 rehash/rebuild 过,文件未换。
        // 仅一次 mmap 原子 load,零系统调用。
        if (epoch_map_ && epoch() == cached_epoch_) { return; }

        // epoch 变化:慢路径——stat 确认 path 上的 inode 是否真的换了
        // (本进程自己的 rehash 也会 bump epoch,但本进程的映射已是新 inode)。
        struct stat st;
        if (::stat(idx_path_.c_str(), &st) != 0) { return; }
        if (static_cast<uint64_t>(st.st_ino) == idx_ino_ &&
            static_cast<uint64_t>(st.st_dev) == idx_dev_) {
            cache_epoch();  // 文件未换,刷新缓存
            return;
        }

        int new_fd = ::open(idx_path_.c_str(), O_RDWR);
        if (new_fd < 0) { return; }

        if (::fstat(new_fd, &st) != 0) { ::close(new_fd); return; }

        void* new_map = mmap(nullptr, static_cast<size_t>(st.st_size),
                             PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
        if (new_map == MAP_FAILED) { ::close(new_fd); return; }
        hint_hugepage(new_map, static_cast<size_t>(st.st_size));

        // ── .dat 是否也被替换 ──
        int  new_dat_fd   = -1;
        void* new_blks[kMaxBlks] = {};
        size_t new_blk_szs[kMaxBlks] = {};
        size_t new_num_blks = 0;
        bool dat_changed = false;
        if (dat_fd_ >= 0) {
            bool dat_checked = false;
            struct stat dst;
            if (::fstat(dat_fd_, &dst) == 0) {
                int dfd = ::open(dat_path_.c_str(), O_RDWR);
                if (dfd >= 0) {
                    struct stat dst2;
                    if (::fstat(dfd, &dst2) != 0 ||
                        dst.st_ino != dst2.st_ino || dst.st_dev != dst2.st_dev) {
                        dat_changed = true;
                    }
                    if (!dat_changed) { ::close(dfd); }
                    dat_checked = true;
                }
            }
            if (!dat_checked) {
                // 无法确认 .dat 是否被替换:放弃整个 remap,保持全部旧映射。
                // 否则"新 idx + 旧 .dat"错配,后续读全错。
                ::munmap(new_map, static_cast<size_t>(st.st_size));
                ::close(new_fd);
                return;
            }
            if (dat_changed) {
                auto* nh = static_cast<Header*>(new_map);
                auto nb = nh->num_blocks.load(std::memory_order_relaxed);
                int dfd = ::open(dat_path_.c_str(), O_RDWR);
                if (dfd < 0 ||
                    !map_all_blocks(dfd, new_blks, new_blk_szs, nb, &new_num_blks)) {
                    if (dfd >= 0) { ::close(dfd); }
                    ::munmap(new_map, static_cast<size_t>(st.st_size));
                    ::close(new_fd);
                    return;  // 保持旧映射,下次 op 再试
                }
                new_dat_fd = dfd;
            }
        }

        // 旧 idx / 旧块映射延迟回收(等 active_ops_ 排空后再 munmap/close)
        auto old_map    = idx_map_;
        auto old_fd     = idx_fd_;
        auto old_sz     = idx_sz();
        auto old_dat_fd = dat_fd_;
        {
            std::lock_guard<std::mutex> lk(deferred_mtx_);
            deferred_munmaps_.push_back({old_map, old_sz, old_fd});
            if (dat_changed) {
                for (size_t i = 0; i < num_blks_; ++i) {
                    if (blk_maps_[i] && blk_maps_[i] != MAP_FAILED) {
                        auto raw = blk_alloc(i).raw.load(std::memory_order_relaxed);
                        deferred_munmaps_.push_back({blk_maps_[i], blk_sz_of(raw), -1});
                    }
                }
                deferred_munmaps_.push_back({nullptr, 0, old_dat_fd});  // 仅关 fd
            }
        }
        idx_map_ = new_map;
        idx_fd_  = new_fd;
        update_idx_inode();
        if (dat_changed) {
            std::memset(blk_maps_, 0, sizeof(blk_maps_));
            std::memcpy(blk_maps_, new_blks, sizeof(new_blks));
            num_blks_ = new_num_blks;
            dat_fd_   = new_dat_fd;
        }
        cache_epoch();
        process_deferred_munmaps();
    }

    // ── pack / read / write ────────────────────────────────────────

    // On-disk record layout (all fields naturally aligned after padding):
    //   [ ksz(4) | key(ksz) | pad→4B | type(4) | vsz(4) | expire_at(8) | value(vsz) ]
    // ksz = raw key length; vsz = raw value length; expire_at=0 means no TTL.
    static uint32_t packed_size(const std::string& key, uint32_t val_size) {
        uint32_t ksz = static_cast<uint32_t>(key.size());
        uint32_t kp  = pad4(ksz);
        return (4 + kp + 4 + 4 + 8 + val_size + 7) & ~7u;
    }

    void write_record(uint64_t bid, uint64_t off,
                      const std::string& key, xcache_value_type_t type,
                      const void* data, uint32_t size,
                      uint64_t expire_at) const {
        auto p = static_cast<char*>(blk_maps_[bid]) + off;
        uint32_t ksz = static_cast<uint32_t>(key.size());
        uint32_t kp  = pad4(ksz);
        size_t o = 0;
        std::memcpy(p + o, &ksz, 4); o += 4;
        std::memcpy(p + o, key.data(), ksz); o += kp;  // NOLINT: binary key with explicit size, not a C string
        uint32_t vt = static_cast<uint32_t>(type);
        std::memcpy(p + o, &vt, 4); o += 4;
        std::memcpy(p + o, &size, 4); o += 4;
        std::memcpy(p + o, &expire_at, 8); o += 8;
        std::memcpy(p + o, data, size);
    }

    Record read_record(uint64_t bid, uint64_t off) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;

        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        auto kp = pad4(ksz);
        std::string key(p0 + 4, ksz);

        auto p = p0 + 4 + kp;
        uint32_t vt; std::memcpy(&vt, p, 4); p += 4;
        uint32_t vsz; std::memcpy(&vsz, p, 4); p += 4;
        uint64_t expire_at; std::memcpy(&expire_at, p, 8); p += 8;
        if (expire_at != 0 && static_cast<uint64_t>(std::time(nullptr)) > expire_at) {
            return {std::move(key), static_cast<xcache_value_type_t>(vt), std::string{}, true, expire_at};
        }
        std::string val(p, vsz);
        return {std::move(key), static_cast<xcache_value_type_t>(vt), std::move(val), false, expire_at};
    }

    std::string read_key(uint64_t bid, uint64_t off) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        return std::string(p0 + 4, ksz);
    }

    bool is_expired(uint64_t bid, uint64_t off) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        uint32_t ksz; std::memcpy(&ksz, p0, 4);
        auto kp = pad4(ksz);
        auto p = p0 + 4 + kp + 4 + 4;
        uint64_t expire_at; std::memcpy(&expire_at, p, 8);
        return expire_at != 0 && static_cast<uint64_t>(std::time(nullptr)) > expire_at;
    }

    std::pair<std::string, xcache_value_type_t> read_key_type(uint64_t bid, uint64_t off) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        auto kp = pad4(ksz);
        std::string key(p0 + 4, ksz);
        auto p = p0 + 4 + kp;
        uint32_t vt; std::memcpy(&vt, p, 4);
        return {std::move(key), static_cast<xcache_value_type_t>(vt)};
    }

    // ── zero-allocation key access ───────────────────────────────

    bool key_equals(uint64_t bid, uint64_t off, const std::string& key) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        if (ksz != static_cast<uint32_t>(key.size())) { return false; }
        return std::memcmp(p0 + 4, key.data(), ksz) == 0;
    }

    bool key_equals_and_type(uint64_t bid, uint64_t off, const std::string& key,
                             xcache_value_type_t* out_type) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        if (ksz != static_cast<uint32_t>(key.size())) { return false; }
        if (std::memcmp(p0 + 4, key.data(), ksz) != 0) { return false; }
        auto kp = pad4(ksz);
        uint32_t vt;
        std::memcpy(&vt, p0 + 4 + kp, 4);
        *out_type = static_cast<xcache_value_type_t>(vt);
        return true;
    }

    // Read value + expiry, skipping key and type (already validated).
    // Returns {raw_val, expired}.
    std::pair<std::string, bool> read_value(uint64_t bid, uint64_t off,
                                             uint32_t key_size) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        auto kp = pad4(key_size);
        auto p = p0 + 4 + kp + 4;  // skip ksz header + padded key + type
        uint32_t vsz; std::memcpy(&vsz, p, 4); p += 4;
        uint64_t expire_at; std::memcpy(&expire_at, p, 8); p += 8;
        if (expire_at != 0 && static_cast<uint64_t>(std::time(nullptr)) > expire_at) {
            return {std::string{}, true};
        }
        return {std::string(p, vsz), false};
    }

    bool is_expired_at(uint64_t bid, uint64_t off, uint32_t key_size) const {
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        auto kp = pad4(key_size);
        auto p = p0 + 4 + kp + 4 + 4;  // skip ksz + padded key + type + vsz
        uint64_t expire_at; std::memcpy(&expire_at, p, 8);
        return expire_at != 0 && static_cast<uint64_t>(std::time(nullptr)) > expire_at;
    }

    // ── typed get helpers ─────────────────────────────────────────

    template <typename F>
    xcache_error_t key_get_typed(const std::string& key,
                                  xcache_value_type_t expected,
                                  F&& parse,
                                  decltype(parse(std::declval<std::string>()))* out) const {
        if (!idx_map_) { return XCACHE_IO_ERROR; }
        enter_read();
        using T = decltype(parse(std::declval<std::string>()));
        auto h  = XXH3_64bits(key.data(), key.size());
        auto N  = hdr()->num_slots;
        for (size_t i = 0; i < N; ++i) {
            auto& s = slot((h + i) % N);
            auto  v = s.state.load(std::memory_order_acquire);
            if (v == kEmpty) { exit_read(); return XCACHE_NOT_FOUND; }
            if (v == kTomb) { continue; }
            uint64_t bid, off;
            decode_slot(v, bid, off);
            xcache_value_type_t vt;
            if (!key_equals_and_type(bid, off, key, &vt)) { continue; }
            if (vt != expected) { exit_read(); return XCACHE_TYPE_MISMATCH; }
            auto [val, expired] = read_value(bid, off, static_cast<uint32_t>(key.size()));
            if (expired) {
                // lazy expiry: tombstone the slot so subsequent ops treat it as deleted.
                // CAS may fail if another thread already removed it — harmless.
                if (s.state.compare_exchange_strong(v, kTomb, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
                    hdr()->num_entries.fetch_sub(1, std::memory_order_relaxed);
                }
                exit_read(); return XCACHE_EXPIRED;
            }
            *out = T(parse(std::move(val)));
            exit_read();
            return XCACHE_OK;
        }
        exit_read();
        return XCACHE_NOT_FOUND;
    }

    xcache_error_t put_typed(const std::string& key,
                               xcache_value_type_t type,
                               const void* data,
                               size_t size,
                               uint32_t expire_seconds = 0) {
        if (size > UINT32_MAX) { return XCACHE_INVALID_ARG; }
        if (!idx_map_) { return XCACHE_IO_ERROR; }
        enter_write();
        auto expire_at = expire_seconds ? static_cast<uint64_t>(std::time(nullptr)) + expire_seconds : 0ULL;
        auto  vsz  = static_cast<uint32_t>(size);
        auto  tot  = packed_size(key, vsz);
        auto pos = alloc_data(tot);
        if (!pos) { exit_write(); return XCACHE_NO_SPACE; }
        auto [bid, off] = *pos;
        // data-before-index: write record to .dat first, then CAS the .idx slot.
        // crash between these two steps leaves the old slot intact (no dangling pointer).
        write_record(bid, off, key, type, data, vsz, expire_at);
        if (!try_insert(key, bid, off)) {
            exit_write();
            // rehash 须在写保护区外执行(它要等 active_ops_ 排空,不能自等);
            // 重试的 try_insert 必须回到保护区内,否则可能撞上另一线程 rehash 的 munmap。
            bool ok = rehash();
            enter_write();
            if (!ok || !try_insert(key, bid, off)) {
                // Rollback: decrement the block's used counter
                auto& ba = blk_alloc(bid);
                auto raw = ba.raw.load(std::memory_order_acquire);
                auto bsz = blk_sz_of(raw);
                auto used = blk_used_of(raw);
                auto rollback = used >= static_cast<uint64_t>(tot) ? used - static_cast<uint64_t>(tot) : 0;
                ba.raw.store(blk_encode_r(bsz, rollback), std::memory_order_release);
                exit_write();
                return XCACHE_NO_SPACE;
            }
        }
        auto* hd = hdr();
        auto  ne = hd->num_entries.load(std::memory_order_relaxed);
        if (ne > static_cast<size_t>(static_cast<double>(hd->num_slots) * kLoadMax)) {
            exit_write();
            rehash();
            enter_write();
        }
        exit_write();
        return XCACHE_OK;
    }

    // ── insert ────────────────────────────────────────────────────

    bool try_insert(const std::string& key, uint64_t bid, uint64_t off) {
        auto h  = XXH3_64bits(key.data(), key.size());
        auto dv = encode_slot(bid, off);
        auto* hd = hdr();
        auto  N  = hd->num_slots;
        for (size_t i = 0; i < N; ++i) {
            auto& s = slot((h + i) % N);
            auto  v = s.state.load(std::memory_order_acquire);
            if (v == kEmpty || v == kTomb) {
                uint64_t exp = v;
                if (s.state.compare_exchange_strong(
                        exp, dv, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    hd->num_entries.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                continue;
            }
            uint64_t eb, eo;
            decode_slot(v, eb, eo);
            if (key_equals(eb, eo, key)) {
                if (s.state.compare_exchange_strong(
                        v, dv, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    return true;
                }
                continue;
            }
        }
        return false;
    }

    // ── data allocator ────────────────────────────────────────────

    struct BlockPos { uint64_t bid; uint64_t off; };

    std::optional<BlockPos> alloc_data(uint64_t need) {
        auto* h = hdr();
        while (true) {
            auto nb = h->num_blocks.load(std::memory_order_acquire);
            if (nb == 0) { return {}; }
            for (uint64_t i = nb; i > 0;) {
                --i;
                auto& ba = blk_alloc(i);
                auto  raw = ba.raw.load(std::memory_order_acquire);
                auto  bsz = blk_sz_of(raw);
                auto  used = blk_used_of(raw);
                if (used == 0) { continue; }
                if (used + need > bsz) { continue; }
                auto nr = blk_encode_r(bsz, used + need);
                if (ba.raw.compare_exchange_strong(
                        raw, nr, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    return BlockPos{i, used};
                }
            }
            if (nb >= h->max_blocks) { return {}; }
            if (!create_block(need)) { return {}; }
        }
    }

    bool create_block(uint64_t need) {
        std::lock_guard<std::mutex> lk(grow_mtx_);
        auto* h = hdr();
        auto nb = h->num_blocks.load(std::memory_order_relaxed);
        if (nb >= h->max_blocks) { return false; }
        if (blk_used_of(blk_alloc(nb).raw.load(std::memory_order_relaxed)) != 0) {
            h->num_blocks.store(nb + 1, std::memory_order_release);
            return true;
        }
        auto sz   = std::max<uint64_t>(h->default_block_size, (need + 4095) & ~4095ull);
        auto foff = h->data_file_size.load(std::memory_order_relaxed);
        if (::ftruncate(dat_fd_, static_cast<off_t>(foff + sz)) != 0) { return false; }
        void* m = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                       MAP_SHARED, dat_fd_, static_cast<off_t>(foff));
        if (m == MAP_FAILED) {
            ::ftruncate(dat_fd_, static_cast<off_t>(foff));  // rollback
            return false;
        }
        blk_maps_[nb] = m;
        std::memcpy(m, &sz, sizeof(sz));
        blk_alloc(nb).raw.store(blk_encode_r(sz, 8), std::memory_order_release);
        h->num_blocks.store(nb + 1, std::memory_order_release);
        h->data_file_size.store(foff + sz, std::memory_order_release);
        if (nb + 1 > num_blks_) { num_blks_ = nb + 1; }
        return true;
    }

    // ── rehash ────────────────────────────────────────────────────

    // Pause writes → drain active ops → build new idx in .tmp →
    // swap pointers → drain readers still on old pointer → rename .tmp → unpause.
    // Reads are never blocked (old idx stays mmap'd until drain completes).
    // 多进程模式:整个构建+切换+rename 持有 EX 锁,防止其他进程在构建期间写入旧表
    // (那些写入会在 generation 提升、对方 remap 后被丢弃)。
    bool rehash() {
        std::lock_guard<std::mutex> lk(rehash_mtx_);
        auto* h = hdr();
        auto ne = h->num_entries.load(std::memory_order_relaxed);
        if (ne <= static_cast<size_t>(static_cast<double>(h->num_slots) * kLoadMax)) {
            return true;
        }

        if (multi_process_) { xc_lock_ex(lock_fd_); }
        auto unlock_multi = [this]() {
            if (multi_process_ && lock_fd_ >= 0) { xc_unlock(lock_fd_); }
        };
        if (multi_process_) {
            // 持锁后先同步到最新文件:本进程的映射可能已落后于其他进程的
            // rehash/rebuild。基于过期映射构建会生成引用旧 .dat 偏移的新表,
            // rename 后与磁盘上(可能已被替换的).dat 错配,后续打开全部读到错值。
            remap_if_stale();
            h = hdr();  // remap 可能更换了映射
            ne = h->num_entries.load(std::memory_order_relaxed);
            if (ne <= static_cast<size_t>(static_cast<double>(h->num_slots) * kLoadMax)) {
                unlock_multi();
                return true;  // 同步后负载已达标(其他进程刚 rehash 过)
            }
        }

        auto  ns2 = std::max<uint64_t>(init_slots_ * 2, h->num_slots * 3 / 2);

        pause_flag_.store(true, std::memory_order_release);
        while (active_ops_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }

        auto ba2 = align8(kHdrSz + ns2 * sizeof(Slot));
        auto is2 = align8(ba2 + kMaxBlks * sizeof(BlkAlloc));

        auto tmp = idx_path_ + ".tmp";
        int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) { pause_flag_.store(false); unlock_multi(); return false; }
        if (::ftruncate(fd, static_cast<off_t>(is2)) != 0) {
            ::close(fd); ::unlink(tmp.c_str()); pause_flag_.store(false);
            unlock_multi(); return false;
        }
        void* nm = mmap(nullptr, is2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (nm == MAP_FAILED) {
            ::unlink(tmp.c_str()); pause_flag_.store(false);
            unlock_multi(); return false;
        }
        std::memset(nm, 0, is2);
        hint_hugepage(nm, is2);

        std::memcpy(nm, idx_map_, kHdrSz);
        auto* nh = static_cast<Header*>(nm);
        nh->idx_size      = is2;
        nh->num_slots     = ns2;
        nh->slots_off     = kHdrSz;
        nh->blk_alloc_off = ba2;

        std::memcpy(static_cast<char*>(nm) + ba2,
                    static_cast<char*>(idx_map_) + h->blk_alloc_off,
                    kMaxBlks * sizeof(BlkAlloc));

        auto  old_slots = h->num_slots;
        auto* new_slots = reinterpret_cast<Slot*>(static_cast<char*>(nm) + kHdrSz);
        uint64_t entry_count = 0;
        for (uint64_t i = 0; i < old_slots; ++i) {
            auto& os = slot(i);
            auto  v  = os.state.load(std::memory_order_relaxed);
            if (v == kEmpty || v == kTomb) { continue; }
            uint64_t bid, off;
            decode_slot(v, bid, off);
            auto k = read_key(bid, off);
            auto hv = XXH3_64bits(k.data(), k.size());
            for (uint64_t j = 0; j < ns2; ++j) {
                auto& ns = new_slots[(hv + j) % ns2];
                if (ns.state.load(std::memory_order_relaxed) == kEmpty) {
                    ns.state.store(v, std::memory_order_relaxed);
                    ++entry_count;
                    break;
                }
            }
        }
        nh->num_entries.store(entry_count, std::memory_order_relaxed);

        nh->generation.store(
            h->generation.load(std::memory_order_relaxed) + 1,
            std::memory_order_release);

        void* old_map = idx_map_;
        auto old_fd = idx_fd_;
        auto old_isz = h->idx_size;

        // 换指针对读者同样是发布:冻结新读者并排空在飞操作后再写 idx_map_。
        // (与 rebuild 一致;否则 idx_map_ 的 plain 写与读者读之间没有 happens-before,
        // 严格 C++ 内存模型下是 data race——硬件上不出错,TSAN 会报。)
        pause_reads_flag_.store(true, std::memory_order_release);
        while (active_ops_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
        idx_map_ = nm;
        pause_reads_flag_.store(false, std::memory_order_release);

        // post-swap drain — wait for readers that grabbed old pointer
        while (active_ops_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }

        if (multi_process_) {
            int new_fd = ::open(tmp.c_str(), O_RDWR);
            if (new_fd >= 0) { idx_fd_ = new_fd; update_idx_inode(); }
        }

        pause_flag_.store(false, std::memory_order_release);

        if (::rename(tmp.c_str(), idx_path_.c_str()) != 0) {
            // 落盘失败(磁盘错误 / 临时文件被并发清理):内存新表保留(仅本进程可见,
            // 下次打开会加载旧文件),旧映射推迟回收;调用方按 NO_SPACE 回滚。
            ::unlink(tmp.c_str());
            {
                std::lock_guard<std::mutex> lk(deferred_mtx_);
                deferred_munmaps_.push_back({old_map, old_isz, old_fd});
            }
            process_deferred_munmaps();
            unlock_multi();
            return false;
        }

        // rename 成功:确保 idx_fd_ 指向新 inode(单进程模式或上述重开失败时按路径重开)
        if (idx_fd_ == old_fd) {
            int nfd = ::open(idx_path_.c_str(), O_RDWR);
            if (nfd >= 0) { idx_fd_ = nfd; update_idx_inode(); }
        }
        munmap(old_map, old_isz);
        if (idx_fd_ != old_fd) { ::close(old_fd); }
        bump_epoch();  // 换文件完成,通知其他进程 remap(仍在持锁)
        unlock_multi();
        return true;
    }

    // ── file open / close ─────────────────────────────────────────

    // lock_held:调用方已持有 EX 锁(rebuild 换文件时传入),本函数不再加锁/解锁。
    // 失败路径统一走 close_file()(关闭 lock_fd_ 即释放 flock);成功路径释放
    // 本次持有的锁(lock_held 时由调用方负责)。
    void open_file(const std::string& path, size_t block_size, bool multi,
                   bool lock_held = false) {
        multi_process_ = multi;
        base_path_ = path;
        idx_path_  = path + ".idx";
        dat_path_  = path + ".dat";
        auto lock_path = path + ".lock";

        if (lock_fd_ < 0) {
            lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
            if (lock_fd_ < 0) { return; }
        }
        if (multi_process_ && !lock_held) {
            xc_lock_ex(lock_fd_);  // 打开/初始化期间独占:串行化并发初始化与临时文件清理
        }

        if (multi_process_ && !epoch_map_) {
            // epoch 计数器:lock 文件前 4 字节,mmap 共享(多进程可见)
            if (::ftruncate(lock_fd_, 4) != 0) { close_file(); return; }
            epoch_map_ = mmap(nullptr, 4, PROT_READ | PROT_WRITE,
                              MAP_SHARED, lock_fd_, 0);
            if (epoch_map_ == MAP_FAILED) { epoch_map_ = nullptr; close_file(); return; }
        }
        cache_epoch();

        ::unlink((idx_path_ + ".tmp").c_str());

        idx_fd_ = ::open(idx_path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (idx_fd_ < 0) { close_file(); return; }
        update_idx_inode();
        dat_fd_ = ::open(dat_path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (dat_fd_ < 0) { close_file(); return; }

        struct stat st;
        if (::fstat(idx_fd_, &st) != 0) { close_file(); return; }

        if (st.st_size == 0) {
            auto ns  = init_slots_;
            auto ba_off = align8(kHdrSz + ns * sizeof(Slot));
            auto isz    = align8(ba_off + kMaxBlks * sizeof(BlkAlloc));

            if (::ftruncate(idx_fd_, static_cast<off_t>(isz)) != 0) { close_file(); return; }
            idx_map_ = mmap(nullptr, isz, PROT_READ | PROT_WRITE,
                            MAP_SHARED, idx_fd_, 0);
            if (idx_map_ == MAP_FAILED) { close_file(); return; }
            std::memset(idx_map_, 0, isz);
            hint_hugepage(idx_map_, isz);

            if (::ftruncate(dat_fd_, static_cast<off_t>(block_size)) != 0) { close_file(); return; }
            blk_maps_[0] = mmap(nullptr, block_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, dat_fd_, 0);
            if (blk_maps_[0] == MAP_FAILED) { close_file(); return; }
            std::memset(blk_maps_[0], 0, block_size);
            std::memcpy(blk_maps_[0], &block_size, sizeof(block_size));
            num_blks_ = 1;

            auto* h = hdr();
            h->magic               = kMagic;
            h->version             = kVersion;
            h->hdr_size            = kHdrSz;
            h->idx_size            = isz;
            h->num_slots           = ns;
            h->slots_off           = kHdrSz;
            h->blk_alloc_off       = ba_off;
            h->default_block_size  = block_size;
            h->num_blocks.store(1, std::memory_order_relaxed);
            h->max_blocks          = kMaxBlks;
            h->data_file_size.store(block_size, std::memory_order_relaxed);
            h->num_entries.store(0, std::memory_order_relaxed);
            h->generation.store(0, std::memory_order_relaxed);
            blk_alloc(0).raw.store(blk_encode_r(block_size, 8),
                                   std::memory_order_relaxed);
        } else {
            auto fsz = static_cast<size_t>(st.st_size);
            idx_map_ = mmap(nullptr, fsz, PROT_READ | PROT_WRITE,
                            MAP_SHARED, idx_fd_, 0);
            if (idx_map_ == MAP_FAILED) { close_file(); return; }
            hint_hugepage(idx_map_, fsz);
            auto* h = hdr();
            if (h->magic != kMagic || h->version > kVersion) { close_file(); return; }

            auto nb = h->num_blocks.load(std::memory_order_relaxed);
            size_t blk_szs[kMaxBlks] = {};
            size_t nblocks = 0;
            if (!map_all_blocks(dat_fd_, blk_maps_, blk_szs, nb, &nblocks)) {
                std::memset(blk_maps_, 0, sizeof(blk_maps_));
                close_file(); return;
            }
            num_blks_ = nblocks;
        }

        // 成功:释放本次持有的锁(lock_held 时由调用方负责)
        if (multi_process_ && !lock_held) {
            xc_unlock(lock_fd_);
        }
    }

    std::vector<std::string> get_all_keys() const {
        std::vector<std::string> keys;
        auto N = hdr()->num_slots;
        for (size_t i = 0; i < N; ++i) {
            auto v = slot(i).state.load(std::memory_order_relaxed);
            if (v == kEmpty || v == kTomb) { continue; }
            uint64_t bid, off;
            decode_slot(v, bid, off);
            auto [k, vt] = read_key_type(bid, off);
            (void)vt;
            if (is_expired_at(bid, off, static_cast<uint32_t>(k.size()))) { continue; }
            keys.push_back(std::move(k));
        }
        return keys;
    }

    void ms_sync() {
        if (!idx_map_) { return; }
        // data-before-index 的持久化版本:先落 .dat 再落 .idx,
        // 崩溃时 idx 不会指向未落盘的记录。
        for (size_t i = 0; i < num_blks_; ++i) {
            if (blk_maps_[i] && blk_maps_[i] != MAP_FAILED) {
                auto raw = blk_alloc(i).raw.load(std::memory_order_relaxed);
                msync(blk_maps_[i], blk_sz_of(raw), MS_SYNC);
            }
        }
        msync(idx_map_, idx_sz(), MS_SYNC);
    }

    void close_file() {
        for (size_t i = 0; i < num_blks_; ++i) {
            if (blk_maps_[i] && blk_maps_[i] != MAP_FAILED) {
                auto raw = blk_alloc(i).raw.load(std::memory_order_relaxed);
                munmap(blk_maps_[i], blk_sz_of(raw));
                blk_maps_[i] = nullptr;
            }
        }
        if (idx_map_ && idx_map_ != MAP_FAILED) {
            munmap(idx_map_, idx_sz());
        }
        idx_map_ = nullptr;
        if (idx_fd_ >= 0) { ::close(idx_fd_); idx_fd_ = -1; }
        if (dat_fd_ >= 0) { ::close(dat_fd_); dat_fd_ = -1; }
        if (epoch_map_) { munmap(epoch_map_, 4); epoch_map_ = nullptr; }
        if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }  // 关闭即释放 flock
        num_blks_ = 0;
        process_deferred_munmaps();
    }
};

// ── public API ───────────────────────────────────────────────────

XCache::XCache(const std::string& path, size_t block_size, bool multi_process, uint32_t init_slots)
    : impl_(std::make_unique<Impl>()) {
    impl_->init_slots_ = init_slots;
    impl_->open_file(path, block_size, multi_process);
}

XCache::~XCache() {
    if (impl_) { impl_->ms_sync(); impl_->close_file(); }
}

XCache::XCache(XCache&& o) noexcept : impl_(std::move(o.impl_)) {}

XCache& XCache::operator=(XCache&& o) noexcept {
    if (this != &o) {
        if (impl_) { impl_->close_file(); }
        impl_ = std::move(o.impl_);
    }
    return *this;
}

// ── put ──────────────────────────────────────────────────────────

xcache_error_t XCache::put_string(const std::string& key, const std::string& value,
                        uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_TEXT, value.data(),
                            value.size(), expire_seconds);
}

xcache_error_t XCache::put_i64(const std::string& key, int64_t value,
                     uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_INT64, &value, sizeof(value), expire_seconds);
}
xcache_error_t XCache::put_i32(const std::string& key, int32_t value,
                     uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_INT32, &value, sizeof(value), expire_seconds);
}
xcache_error_t XCache::put_f32(const std::string& key, float value,
                     uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_FLOAT, &value, sizeof(value), expire_seconds);
}
xcache_error_t XCache::put_f64(const std::string& key, double value,
                     uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_DOUBLE, &value, sizeof(value), expire_seconds);
}
xcache_error_t XCache::put_bool(const std::string& key, bool value,
                      uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_BOOLEAN, &value, sizeof(value), expire_seconds);
}
xcache_error_t XCache::put_blob(const std::string& key, const void* data, size_t len,
                      uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_BLOB, data, len, expire_seconds);
}
xcache_error_t XCache::put_vector(const std::string& key, const void* data, size_t len,
                        uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_VECTOR, data, len, expire_seconds);
}
xcache_error_t XCache::put_set(const std::string& key, const void* data, size_t len,
                     uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_SET, data, len, expire_seconds);
}
xcache_error_t XCache::put_map(const std::string& key, const void* data, size_t len,
                     uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_MAP, data, len, expire_seconds);
}

// ── get / typed get ──────────────────────────────────────────────

xcache_error_t XCache::get_string(const std::string& key, std::string* out) const {
    return impl_->key_get_typed(
        key, XCACHE_TEXT, [](std::string raw) -> std::string {
            return raw;
        }, out);
}

xcache_error_t XCache::get_i64(const std::string& key, int64_t* out) const {
    return impl_->key_get_typed(
        key, XCACHE_INT64, [](std::string raw) -> int64_t {
            int64_t v; std::memcpy(&v, raw.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_i32(const std::string& key, int32_t* out) const {
    return impl_->key_get_typed(
        key, XCACHE_INT32, [](std::string raw) -> int32_t {
            int32_t v; std::memcpy(&v, raw.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_f32(const std::string& key, float* out) const {
    return impl_->key_get_typed(
        key, XCACHE_FLOAT, [](std::string raw) -> float {
            float v; std::memcpy(&v, raw.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_f64(const std::string& key, double* out) const {
    return impl_->key_get_typed(
        key, XCACHE_DOUBLE, [](std::string raw) -> double {
            double v; std::memcpy(&v, raw.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_bool(const std::string& key, bool* out) const {
    return impl_->key_get_typed(
        key, XCACHE_BOOLEAN, [](std::string raw) -> bool {
            return !raw.empty() && raw[0] != 0;
        }, out);
}

xcache_error_t XCache::get_blob(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_BLOB, [](std::string raw) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(raw.data());
            return std::vector<uint8_t>(p, p + raw.size());
        }, out);
}

xcache_error_t XCache::get_vector(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_VECTOR, [](std::string raw) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(raw.data());
            return std::vector<uint8_t>(p, p + raw.size());
        }, out);
}

xcache_error_t XCache::get_set(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_SET, [](std::string raw) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(raw.data());
            return std::vector<uint8_t>(p, p + raw.size());
        }, out);
}

xcache_error_t XCache::get_map(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_MAP, [](std::string raw) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(raw.data());
            return std::vector<uint8_t>(p, p + raw.size());
        }, out);
}

xcache_error_t XCache::get_type(const std::string& key, xcache_value_type_t* out) const {
    auto& I = *impl_;
    if (!I.idx_map_) { return XCACHE_IO_ERROR; }
    I.enter_read();
    auto h  = XXH3_64bits(key.data(), key.size());
    auto N  = I.hdr()->num_slots;
    for (size_t i = 0; i < N; ++i) {
        auto& s = I.slot((h + i) % N);
        auto  v = s.state.load(std::memory_order_acquire);
        if (v == kEmpty) { break; }
        if (v == kTomb) { continue; }
        uint64_t bid, off;
        decode_slot(v, bid, off);
        xcache_value_type_t vt;
        if (I.key_equals_and_type(bid, off, key, &vt)) {
            if (I.is_expired(bid, off)) {
                // lazy expiry
                if (s.state.compare_exchange_strong(v, kTomb, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
                    I.hdr()->num_entries.fetch_sub(1, std::memory_order_relaxed);
                }
                I.exit_read(); return XCACHE_EXPIRED;
            }
            *out = vt;
            I.exit_read();
            return XCACHE_OK;
        }
    }
    I.exit_read();
    return XCACHE_NOT_FOUND;
}

// ── basic ops ────────────────────────────────────────────────────

bool XCache::exists(const std::string& key) const {
    auto& I = *impl_;
    if (!I.idx_map_) { return false; }
    I.enter_read();
    auto h = XXH3_64bits(key.data(), key.size());
    auto N = I.hdr()->num_slots;
    bool found = false;
    for (size_t i = 0; i < N; ++i) {
        auto& s = I.slot((h + i) % N);
        auto  v = s.state.load(std::memory_order_acquire);
        if (v == kEmpty) { break; }
        if (v == kTomb) { continue; }
        uint64_t bid, off;
        decode_slot(v, bid, off);
        if (I.key_equals(bid, off, key)) {
            if (I.is_expired(bid, off)) {
                // lazy expiry: same CAS pattern as key_get_typed
                if (s.state.compare_exchange_strong(v, kTomb, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
                    I.hdr()->num_entries.fetch_sub(1, std::memory_order_relaxed);
                }
                found = false;
            } else {
                found = true;
            }
            break;
        }
    }
    I.exit_read();
    return found;
}

xcache_error_t XCache::remove(const std::string& key) {
    auto& I = *impl_;
    if (!I.idx_map_) { return XCACHE_IO_ERROR; }
    I.enter_write();
    auto h = XXH3_64bits(key.data(), key.size());
    auto N = I.hdr()->num_slots;
    for (size_t i = 0; i < N; ++i) {
        auto& s = I.slot((h + i) % N);
        auto  v = s.state.load(std::memory_order_acquire);
        if (v == kEmpty) { I.exit_write(); return XCACHE_NOT_FOUND; }
        if (v == kTomb) { continue; }
        uint64_t bid, off;
        decode_slot(v, bid, off);
        if (I.key_equals(bid, off, key)) {
            if (s.state.compare_exchange_strong(
                    v, kTomb, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                I.hdr()->num_entries.fetch_sub(1, std::memory_order_relaxed);
                I.exit_write(); return XCACHE_OK;
            }
            continue;
        }
    }
    I.exit_write(); return XCACHE_NOT_FOUND;
}

size_t XCache::size() const {
    auto& I = *impl_;
    if (!I.idx_map_) { return 0; }
    I.enter_read();
    size_t n = 0;
    auto N = I.hdr()->num_slots;
    for (size_t i = 0; i < N; ++i) {
        auto v = I.slot(i).state.load(std::memory_order_relaxed);
        if (v != kEmpty && v != kTomb) { ++n; }
    }
    I.exit_read();
    return n;
}

void XCache::sync() {
    if (impl_) { impl_->ms_sync(); }
}

void XCache::close() {
    if (impl_) { impl_->close_file(); }
}

xcache_error_t XCache::rebuild() {
    auto& I = *impl_;
    if (!I.idx_map_) { return XCACHE_IO_ERROR; }

    // 与 rehash 互斥:两者都会整体替换表/文件,并发执行会互相破坏
    // (rename 竞争 + 映射错配)。put 的 rehash 回退路径在 rebuild 期间会等这把锁。
    std::lock_guard<std::mutex> lk(I.rehash_mtx_);

    // 多进程:整个 rebuild(含构建与换文件)持有 EX 锁,防止其他进程在构建期间写入
    // 旧表(那些写入会在文件被替换后丢失)。持锁后先同步到最新文件(本进程可能已
    // 落后于其他进程的 rehash/rebuild)。
    if (I.multi_process_) {
        xc_lock_ex(I.lock_fd_);
        I.remap_if_stale();
    }
    auto unlock_multi = [&I]() {
        if (I.multi_process_ && I.lock_fd_ >= 0) { xc_unlock(I.lock_fd_); }
    };

    I.pause_flag_.store(true, std::memory_order_release);
    while (I.active_ops_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    auto blk_sz = I.hdr()->default_block_size;
    auto multi  = I.multi_process_;

    auto tmp_path = I.base_path_ + ".rebuild";
    // 清理上次崩溃(如 kill -9)残留的临时库:残留库 magic 合法,会被 open_file
    // 当作已有库加载,里面的旧 key 会"复活"并混入重建结果。
    ::unlink((tmp_path + ".idx").c_str());
    ::unlink((tmp_path + ".dat").c_str());
    ::unlink((tmp_path + ".lock").c_str());
    bool ok = false;
    {
        Impl tmp;
        tmp.open_file(tmp_path, blk_sz, multi);
        if (tmp.idx_map_) {
            ok = true;
            auto* h = I.hdr();
            for (size_t i = 0; i < h->num_slots; ++i) {
                auto v = I.slot(i).state.load(std::memory_order_relaxed);
                if (v == kEmpty || v == kTomb) { continue; }
                uint64_t bid, off;
                decode_slot(v, bid, off);
                if (I.is_expired(bid, off)) { continue; }
                auto rec = I.read_record(bid, off);
                if (rec.expired) { continue; }  // TTL may expire between check and read
                // Preserve remaining TTL
                auto expire_seconds = rec.expire_at ? static_cast<uint32_t>(
                    rec.expire_at > static_cast<uint64_t>(std::time(nullptr))
                    ? rec.expire_at - static_cast<uint64_t>(std::time(nullptr))
                    : 0ULL) : 0U;
                if (tmp.put_typed(rec.key, rec.type, rec.raw_val.data(),
                                   rec.raw_val.size(), expire_seconds) != XCACHE_OK) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                // 新库 generation 从旧值递增(而非重置为 0):换文件后磁盘上的
                // generation 单调增长,便于外部工具识别文件代次;跨进程 remap
                // 检测由 path.lock 的 epoch + inode 比较承担,不再依赖它。
                tmp.hdr()->generation.store(
                    I.hdr()->generation.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
            }
        }
        tmp.close_file();
    }

    if (!ok) {
        ::unlink((tmp_path + ".idx").c_str());
        ::unlink((tmp_path + ".dat").c_str());
        ::unlink((tmp_path + ".lock").c_str());
        I.pause_flag_.store(false, std::memory_order_release);
        unlock_multi();
        return XCACHE_NO_SPACE;
    }

    // save old state before swap (same style as rehash)
    auto* old_idx_map   = I.idx_map_;
    auto  old_idx_fd    = I.idx_fd_;
    auto  old_dat_fd    = I.dat_fd_;
    auto  old_idx_sz    = I.idx_sz();
    auto  old_num_blks  = I.num_blks_;
    void* old_blk_maps[kMaxBlks];
    std::memcpy(static_cast<void*>(old_blk_maps), static_cast<const void*>(I.blk_maps_), sizeof(old_blk_maps));
    size_t old_blk_szs[kMaxBlks];
    for (size_t i = 0; i < old_num_blks; ++i) {
        old_blk_szs[i] = blk_sz_of(
            I.blk_alloc(i).raw.load(std::memory_order_relaxed));
    }

    // ── 换指针:先冻结新读者并排空在飞操作,再替换映射 ──
    // 读者在整个 rebuild 期间不阻塞,仅在指针切换瞬间自旋等待(微秒级)。
    I.pause_reads_flag_.store(true, std::memory_order_release);
    while (I.active_ops_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // rename first — old inode stays alive via I's existing fd+mmap
    if (::rename((tmp_path + ".idx").c_str(), I.idx_path_.c_str()) != 0 ||
        ::rename((tmp_path + ".dat").c_str(), I.dat_path_.c_str()) != 0) {
        // 换文件失败(若第一个 rename 已成功则为半换状态,不可恢复):清理临时文件,
        // 恢复读写后报 IO 错误。旧映射保持有效,内存数据不丢;下次打开可能拿到
        // 半换文件,这属于磁盘故障范畴。
        ::unlink((tmp_path + ".idx").c_str());
        ::unlink((tmp_path + ".dat").c_str());
        ::unlink((tmp_path + ".lock").c_str());
        I.pause_reads_flag_.store(false, std::memory_order_release);
        I.pause_flag_.store(false, std::memory_order_release);
        unlock_multi();
        return XCACHE_IO_ERROR;
    }

    I.open_file(I.base_path_, blk_sz, multi, /*lock_held=*/true);
    ::unlink((tmp_path + ".lock").c_str());  // .idx/.dat 已被 rename 移走,只剩 lock 残留

    // 读者恢复(此后进入的读者都看到新映射)
    I.pause_reads_flag_.store(false, std::memory_order_release);

    // cleanup old — same style as rehash (旧映射此刻已无人持有)
    for (size_t i = 0; i < old_num_blks; ++i) {
        if (old_blk_maps[i] && old_blk_maps[i] != MAP_FAILED) {
            ::munmap(old_blk_maps[i], old_blk_szs[i]);
        }
    }
    if (old_idx_map && old_idx_map != MAP_FAILED) {
        ::munmap(old_idx_map, old_idx_sz);
    }
    if (old_idx_fd >= 0)  { ::close(old_idx_fd); }
    if (old_dat_fd >= 0)  { ::close(old_dat_fd); }

    I.pause_flag_.store(false, std::memory_order_release);
    I.bump_epoch();  // 换文件完成,通知其他进程 remap(仍在持锁)
    unlock_multi();
    return I.idx_map_ ? XCACHE_OK : XCACHE_IO_ERROR;
}

void XCache::scan(const ScanFn& fn) const {
    auto& I = *impl_;
    if (!I.idx_map_) { return; }
    I.enter_read();
    auto N = I.hdr()->num_slots;
    for (size_t i = 0; i < N; ++i) {
        auto v = I.slot(i).state.load(std::memory_order_relaxed);
        if (v == kEmpty || v == kTomb) { continue; }
        uint64_t bid, off;
        decode_slot(v, bid, off);
        auto [k, vt] = I.read_key_type(bid, off);
        if (I.is_expired_at(bid, off, static_cast<uint32_t>(k.size()))) { continue; }
        if (!fn(k, vt)) { break; }
    }
    I.exit_read();
}

std::vector<std::string> XCache::get_all_keys() const {
    auto& I = *impl_;
    if (!I.idx_map_) { return {}; }
    I.enter_read();
    auto keys = I.get_all_keys();
    I.exit_read();
    return keys;
}

}  // namespace xcache
