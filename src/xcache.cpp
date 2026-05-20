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

static uint64_t encode_slot(uint64_t bid, uint64_t off) {
    return ((bid << 40) | off) << 1 | 1;
}
static void decode_slot(uint64_t s, uint64_t& bid, uint64_t& off) {
    auto v = s >> 1; bid = v >> 40; off = v & 0xFFFFFFFFFF;
}
static uint64_t blk_sz_of(uint64_t r)  { return r >> 32; }
static uint64_t blk_used_of(uint64_t r) { return r & 0xFFFFFFFF; }
static uint64_t blk_encode_r(uint64_t sz, uint64_t used) {
    return (sz << 32) | used;
}
static uint64_t align8(uint64_t v) { return (v + 7) & ~7ull; }

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
};

// ── implementation ───────────────────────────────────────────────

struct XCache::Impl {
    mutable void*  idx_map_      = nullptr;
    mutable int    idx_fd_       = -1;
    int    dat_fd_       = -1;
    void*  blk_maps_[kMaxBlks] = {};
    size_t num_blks_     = 0;
    mutable std::atomic<int32_t> active_ops_{0};
    mutable std::atomic<bool>    pause_flag_{false};
    std::mutex  grow_mtx_;
    std::mutex  rehash_mtx_;
    std::string base_path_;
    std::string idx_path_;
    std::string dat_path_;
    bool    multi_process_ = false;
    mutable uint64_t last_generation_ = 0;
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
            xc_lock_sh(idx_fd_);
        }
        active_ops_.fetch_add(1, std::memory_order_acquire);
        if (multi_process_) {
            remap_if_stale();
        }
    }

    void exit_read() const {
        active_ops_.fetch_sub(1, std::memory_order_release);
        if (multi_process_) {
            xc_unlock(idx_fd_);
        }
    }

    void enter_write() const {
        if (multi_process_) {
            xc_lock_ex(idx_fd_);
        }
        active_ops_.fetch_add(1, std::memory_order_acquire);
        if (pause_flag_.load(std::memory_order_acquire)) {
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
            xc_unlock(idx_fd_);
        }
    }

    // ── multi-process remap ───────────────────────────────────────

    void remap_if_stale() const {
        auto gen = hdr()->generation.load(std::memory_order_acquire);
        if (gen == last_generation_) {
            return;
        }

        struct stat st;
        if (::fstat(idx_fd_, &st) != 0) { return; }

        int new_fd = ::open(idx_path_.c_str(), O_RDWR);
        if (new_fd < 0) { return; }

        void* new_map = mmap(nullptr, static_cast<size_t>(st.st_size),
                             PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
        if (new_map == MAP_FAILED) { ::close(new_fd); return; }
        hint_hugepage(new_map, static_cast<size_t>(st.st_size));

        auto old_map = idx_map_;
        auto old_fd  = idx_fd_;
        auto old_sz  = idx_sz();

        idx_map_ = new_map;
        idx_fd_  = new_fd;
        const_cast<Impl*>(this)->last_generation_ = gen;

        munmap(old_map, old_sz);
        ::close(old_fd);
    }

    // ── pack / read / write ────────────────────────────────────────

    static size_t packed_size(const std::string& key, size_t val_size) {
        size_t ksz = key.size();
        size_t kp  = (ksz + 3) & ~3ull;
        return (4 + kp + 4 + 4 + 8 + val_size + 7) & ~7ull;
    }

    void write_record(uint64_t bid, uint64_t off,
                      const std::string& key, xcache_value_type_t type,
                      const void* data, size_t size,
                      uint64_t expire_at) const {
        auto p = static_cast<char*>(blk_maps_[bid]) + off;
        uint32_t ksz = static_cast<uint32_t>(key.size());
        uint32_t kp  = (ksz + 3) & ~3u;
        uint32_t vsz = static_cast<uint32_t>(size);
        size_t o = 0;
        std::memcpy(p + o, &ksz, 4); o += 4;
        std::memcpy(p + o, key.data(), ksz); o += kp;  // NOLINT: binary key with explicit size, not a C string
        uint32_t vt = static_cast<uint32_t>(type);
        std::memcpy(p + o, &vt, 4); o += 4;
        std::memcpy(p + o, &vsz, 4); o += 4;
        std::memcpy(p + o, &expire_at, 8); o += 8;
        std::memcpy(p + o, data, size);
    }

    Record read_record(uint64_t bid, uint64_t off) const {
        if (bid >= num_blks_) { return {std::string{}, XCACHE_TEXT, std::string{}, true}; }
        if (!blk_maps_[bid] || blk_maps_[bid] == MAP_FAILED) {
            return {std::string{}, XCACHE_TEXT, std::string{}, true};
        }
        auto raw = blk_alloc(bid).raw.load(std::memory_order_relaxed);
        auto bsz = blk_sz_of(raw);
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        if (off + 4 > bsz) { return {std::string{}, XCACHE_TEXT, std::string{}, true}; }
        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        auto kp = static_cast<size_t>((ksz + 3) & ~3u);
        if (kp < ksz) {
            return {std::string{}, XCACHE_TEXT, std::string{}, true};
        }
        if (off + 4 + kp + 4 + 4 + 8 > bsz) {
            return {std::string{}, XCACHE_TEXT, std::string{}, true};
        }
        std::string key(p0 + 4, ksz);

        auto p = p0 + 4 + kp;
        uint32_t vt; std::memcpy(&vt, p, 4); p += 4;
        uint32_t vsz; std::memcpy(&vsz, p, 4); p += 4;
        if (off + 4 + kp + 4 + 4 + 8 + static_cast<size_t>(vsz) > bsz) {
            return {std::string{}, XCACHE_TEXT, std::string{}, true};
        }
        uint64_t expire_at; std::memcpy(&expire_at, p, 8); p += 8;
        if (expire_at != 0 && static_cast<uint64_t>(std::time(nullptr)) > expire_at) {
            return {std::move(key), static_cast<xcache_value_type_t>(vt), std::string{}, true};
        }
        std::string val(p, vsz);
        return {std::move(key), static_cast<xcache_value_type_t>(vt), std::move(val)};
    }

    std::string read_key(uint64_t bid, uint64_t off) const {
        if (bid >= num_blks_) { return {}; }
        if (!blk_maps_[bid] || blk_maps_[bid] == MAP_FAILED) { return {}; }
        auto raw = blk_alloc(bid).raw.load(std::memory_order_relaxed);
        auto bsz = blk_sz_of(raw);
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        if (off + 4 > bsz) { return {}; }
        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        auto kp = static_cast<size_t>((ksz + 3) & ~3u);
        if (kp < ksz) { return {}; }
        if (off + 4 + kp + 4 + 4 + 8 > bsz) { return {}; }
        return std::string(p0 + 4, ksz);
    }

    bool is_expired(uint64_t bid, uint64_t off) const {
        if (bid >= num_blks_) { return true; }
        if (!blk_maps_[bid] || blk_maps_[bid] == MAP_FAILED) { return true; }
        auto raw = blk_alloc(bid).raw.load(std::memory_order_relaxed);
        auto bsz = blk_sz_of(raw);
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        if (off + 4 > bsz) { return true; }
        uint32_t ksz; std::memcpy(&ksz, p0, 4);
        auto kp = static_cast<size_t>((ksz + 3) & ~3u);
        if (kp < ksz) { return true; }
        if (off + 4 + kp + 4 + 4 + 8 > bsz) { return true; }
        auto p = p0 + 4 + kp + 4 + 4;
        uint64_t expire_at; std::memcpy(&expire_at, p, 8);
        return expire_at != 0 && static_cast<uint64_t>(std::time(nullptr)) > expire_at;
    }

    std::pair<std::string, xcache_value_type_t> read_key_type(uint64_t bid, uint64_t off) const {
        if (bid >= num_blks_) { return {{}, XCACHE_TEXT}; }
        if (!blk_maps_[bid] || blk_maps_[bid] == MAP_FAILED) { return {{}, XCACHE_TEXT}; }
        auto raw = blk_alloc(bid).raw.load(std::memory_order_relaxed);
        auto bsz = blk_sz_of(raw);
        auto p0 = static_cast<const char*>(blk_maps_[bid]) + off;
        if (off + 4 > bsz) { return {{}, XCACHE_TEXT}; }
        uint32_t ksz;
        std::memcpy(&ksz, p0, 4);
        auto kp = static_cast<size_t>((ksz + 3) & ~3u);
        if (kp < ksz) { return {{}, XCACHE_TEXT}; }
        if (off + 4 + kp + 4 + 4 + 8 > bsz) { return {{}, XCACHE_TEXT}; }
        std::string key(p0 + 4, ksz);
        auto p = p0 + 4 + kp;
        uint32_t vt; std::memcpy(&vt, p, 4);
        return {std::move(key), static_cast<xcache_value_type_t>(vt)};
    }

    // ── typed get helpers ─────────────────────────────────────────

    template <typename F>
    xcache_error_t key_get_typed(const std::string& key,
                                  xcache_value_type_t expected,
                                  F&& parse,
                                  decltype(parse(std::declval<Record>()))* out) const {
        if (!idx_map_) { return XCACHE_IO_ERROR; }
        enter_read();
        using T = decltype(parse(std::declval<Record>()));
        auto h  = XXH3_64bits(key.data(), key.size());
        auto N  = hdr()->num_slots;
        for (size_t i = 0; i < N; ++i) {
            auto& s = slot((h + i) % N);
            auto  v = s.state.load(std::memory_order_acquire);
            if (v == kEmpty) { exit_read(); return XCACHE_NOT_FOUND; }
            if (v == kTomb) { continue; }
            uint64_t bid, off;
            decode_slot(v, bid, off);
            auto [k, vt] = read_key_type(bid, off);
            if (k != key) { continue; }
            if (vt != expected) { exit_read(); return XCACHE_TYPE_MISMATCH; }
            auto rec = read_record(bid, off);
            if (rec.expired) { exit_read(); return XCACHE_EXPIRED; }
            *out = T(parse(std::move(rec)));
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
        if (!idx_map_) { return XCACHE_IO_ERROR; }
        if (size > UINT32_MAX) { return XCACHE_INVALID_ARG; }
        enter_write();
        auto expire_at = expire_seconds ? static_cast<uint64_t>(std::time(nullptr)) + expire_seconds : 0ULL;
        auto tot = packed_size(key, size);
        auto pos = alloc_data(tot);
        if (!pos) { exit_write(); return XCACHE_NO_SPACE; }
        auto [bid, off] = *pos;
        write_record(bid, off, key, type, data, size, expire_at);
        if (!try_insert(key, bid, off)) {
            exit_write();
            auto ok = rehash() && try_insert(key, bid, off);
            enter_write();
            if (!ok) { exit_write(); return XCACHE_NO_SPACE; }
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
                    if (v == kEmpty || v == kTomb) {
                        hd->num_entries.fetch_add(1, std::memory_order_relaxed);
                    }
                    return true;
                }
                --i;
                continue;
            }
            uint64_t eb, eo;
            decode_slot(v, eb, eo);
            if (read_key(eb, eo) == key) {
                if (s.state.compare_exchange_strong(
                        v, dv, std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    return true;
                }
                --i;
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
        auto nb = h->num_blocks.load(std::memory_order_acquire);
        if (nb >= h->max_blocks) { return false; }
        if (blk_used_of(blk_alloc(nb).raw.load(std::memory_order_acquire)) != 0) {
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

    bool rehash() {
        std::lock_guard<std::mutex> lk(rehash_mtx_);
        auto* h = hdr();
        auto ne = h->num_entries.load(std::memory_order_relaxed);
        if (ne <= static_cast<size_t>(static_cast<double>(h->num_slots) * kLoadMax)) {
            return true;
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
        if (fd < 0) { pause_flag_.store(false); return false; }
        if (::ftruncate(fd, static_cast<off_t>(is2)) != 0) {
            ::close(fd); ::unlink(tmp.c_str()); pause_flag_.store(false); return false;
        }
        void* nm = mmap(nullptr, is2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (nm == MAP_FAILED) { ::unlink(tmp.c_str()); pause_flag_.store(false); return false; }
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
                    break;
                }
            }
        }

        nh->generation.store(
            h->generation.load(std::memory_order_relaxed) + 1,
            std::memory_order_release);

        void* old_map = idx_map_;
        auto old_fd = idx_fd_;
        auto old_isz = h->idx_size;
        idx_map_ = nm;

        // post-swap drain — wait for readers that grabbed old pointer
        while (active_ops_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }

        if (multi_process_) {
            int new_fd = ::open(tmp.c_str(), O_RDWR);
            if (new_fd >= 0) { idx_fd_ = new_fd; }
            else { idx_fd_ = -1; }
        }

        munmap(old_map, old_isz);
        ::close(old_fd);
        if (!multi_process_) { idx_fd_ = -1; }

        if (::rename(tmp.c_str(), idx_path_.c_str()) != 0) {
            pause_flag_.store(false, std::memory_order_release);
            return false;
        }

        pause_flag_.store(false, std::memory_order_release);
        last_generation_ = nh->generation.load(std::memory_order_relaxed);
        return true;
    }

    // ── file open / close ─────────────────────────────────────────

    void open_file(const std::string& path, size_t block_size, bool multi) {
        multi_process_ = multi;
        base_path_ = path;
        idx_path_  = path + ".idx";
        dat_path_  = path + ".dat";
        ::unlink((idx_path_ + ".tmp").c_str());

        idx_fd_ = ::open(idx_path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (idx_fd_ < 0) { return; }
        dat_fd_ = ::open(dat_path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (dat_fd_ < 0) { ::close(idx_fd_); idx_fd_ = -1; return; }

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
            uint64_t off = 0;
            for (uint64_t i = 0; i < nb; ++i) {
                uint64_t bsz;
                if (::pread(dat_fd_, &bsz, sizeof(bsz), static_cast<off_t>(off))
                    != static_cast<ssize_t>(sizeof(bsz))) {
                    close_file(); return;
                }
                blk_maps_[i] = mmap(nullptr, bsz, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, dat_fd_, static_cast<off_t>(off));
                if (blk_maps_[i] == MAP_FAILED) { close_file(); return; }
                off += bsz;
            }
            num_blks_ = nb;
            last_generation_ = h->generation.load(std::memory_order_relaxed);
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
            if (is_expired(bid, off)) { continue; }
            keys.push_back(read_key(bid, off));
        }
        return keys;
    }

    void ms_sync() {
        if (!idx_map_) { return; }
        msync(idx_map_, idx_sz(), MS_SYNC);
    }

    void close_file() {
        if (!idx_map_ || idx_map_ == MAP_FAILED) {
            if (idx_fd_ >= 0) { ::close(idx_fd_); idx_fd_ = -1; }
            if (dat_fd_ >= 0) { ::close(dat_fd_); dat_fd_ = -1; }
            idx_map_ = nullptr;
            num_blks_ = 0;
            return;
        }
        for (size_t i = 0; i < num_blks_; ++i) {
            if (blk_maps_[i] && blk_maps_[i] != MAP_FAILED) {
                auto raw = blk_alloc(i).raw.load(std::memory_order_relaxed);
                munmap(blk_maps_[i], blk_sz_of(raw));
                blk_maps_[i] = nullptr;
            }
        }
        if (idx_map_ != MAP_FAILED) { munmap(idx_map_, idx_sz()); }
        idx_map_ = nullptr;
        if (idx_fd_ >= 0) { ::close(idx_fd_); idx_fd_ = -1; }
        if (dat_fd_ >= 0) { ::close(dat_fd_); dat_fd_ = -1; }
        num_blks_ = 0;
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
    return impl_->put_typed(key, XCACHE_BLOB, data,
                            len, expire_seconds);
}
xcache_error_t XCache::put_vector(const std::string& key, const void* data, size_t len,
                        uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_VECTOR, data,
                            len, expire_seconds);
}
xcache_error_t XCache::put_set(const std::string& key, const void* data, size_t len,
                      uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_SET, data,
                            len, expire_seconds);
}
xcache_error_t XCache::put_map(const std::string& key, const void* data, size_t len,
                      uint32_t expire_seconds) {
    return impl_->put_typed(key, XCACHE_MAP, data,
                            len, expire_seconds);
}

// ── get / typed get ──────────────────────────────────────────────

xcache_error_t XCache::get_string(const std::string& key, std::string* out) const {
    return impl_->key_get_typed(
        key, XCACHE_TEXT, [](const Record& r) -> std::string {
            return r.raw_val;
        }, out);
}

xcache_error_t XCache::get_i64(const std::string& key, int64_t* out) const {
    return impl_->key_get_typed(
        key, XCACHE_INT64, [](const Record& r) -> int64_t {
            int64_t v; std::memcpy(&v, r.raw_val.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_i32(const std::string& key, int32_t* out) const {
    return impl_->key_get_typed(
        key, XCACHE_INT32, [](const Record& r) -> int32_t {
            int32_t v; std::memcpy(&v, r.raw_val.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_f32(const std::string& key, float* out) const {
    return impl_->key_get_typed(
        key, XCACHE_FLOAT, [](const Record& r) -> float {
            float v; std::memcpy(&v, r.raw_val.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_f64(const std::string& key, double* out) const {
    return impl_->key_get_typed(
        key, XCACHE_DOUBLE, [](const Record& r) -> double {
            double v; std::memcpy(&v, r.raw_val.data(), sizeof(v)); return v;
        }, out);
}

xcache_error_t XCache::get_bool(const std::string& key, bool* out) const {
    return impl_->key_get_typed(
        key, XCACHE_BOOLEAN, [](const Record& r) -> bool {
            return r.raw_val[0] != 0;
        }, out);
}

xcache_error_t XCache::get_blob(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_BLOB, [](const Record& r) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(r.raw_val.data());
            return std::vector<uint8_t>(p, p + r.raw_val.size());
        }, out);
}

xcache_error_t XCache::get_vector(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_VECTOR, [](const Record& r) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(r.raw_val.data());
            return std::vector<uint8_t>(p, p + r.raw_val.size());
        }, out);
}

xcache_error_t XCache::get_set(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_SET, [](const Record& r) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(r.raw_val.data());
            return std::vector<uint8_t>(p, p + r.raw_val.size());
        }, out);
}

xcache_error_t XCache::get_map(const std::string& key, std::vector<uint8_t>* out) const {
    return impl_->key_get_typed(
        key, XCACHE_MAP, [](const Record& r) -> std::vector<uint8_t> {
            auto p = reinterpret_cast<const uint8_t*>(r.raw_val.data());
            return std::vector<uint8_t>(p, p + r.raw_val.size());
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
        auto [k, vt] = I.read_key_type(bid, off);
        if (k == key) {
            if (I.is_expired(bid, off)) { I.exit_read(); return XCACHE_EXPIRED; }
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
        if (I.read_key(bid, off) == key) { found = !I.is_expired(bid, off); break; }
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
        if (I.read_key(bid, off) == key) {
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

    std::lock_guard<std::mutex> lk(I.rehash_mtx_);
    I.pause_flag_.store(true, std::memory_order_release);
    while (I.active_ops_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    auto blk_sz = I.hdr()->default_block_size;
    auto multi  = I.multi_process_;

    auto tmp_path = I.base_path_ + ".rebuild";
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
                if (tmp.put_typed(rec.key, rec.type, rec.raw_val.data(),
                                   rec.raw_val.size()) != XCACHE_OK) {
                    ok = false;
                    break;
                }
            }
        }
        tmp.close_file();
    }

    if (!ok) {
        ::unlink((tmp_path + ".idx").c_str());
        ::unlink((tmp_path + ".dat").c_str());
        I.pause_flag_.store(false, std::memory_order_release);
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

    // rename first — old inode stays alive via I's existing fd+mmap
    if (::rename((tmp_path + ".idx").c_str(), I.idx_path_.c_str()) != 0) {
        ::unlink((tmp_path + ".dat").c_str());
        I.pause_flag_.store(false, std::memory_order_release);
        return XCACHE_IO_ERROR;
    }
    if (::rename((tmp_path + ".dat").c_str(), I.dat_path_.c_str()) != 0) {
        ::rename(I.idx_path_.c_str(), (tmp_path + ".idx").c_str());
        I.pause_flag_.store(false, std::memory_order_release);
        return XCACHE_IO_ERROR;
    }

    I.open_file(I.base_path_, blk_sz, multi);

    // drain readers that may still hold old pointer
    while (I.active_ops_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // cleanup old — same style as rehash
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
    return XCACHE_OK;
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
        if (I.is_expired(bid, off)) { continue; }
        auto [k, vt] = I.read_key_type(bid, off);
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
