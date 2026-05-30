// fuzz_corrupt.cpp — corrupted file handling fuzzer
// Exercises open_file / read paths with random binary .idx / .dat content.
// Uses fork() per case — SIGBUS from mmap beyond EOF won't kill the fuzzer.

#include "xcache/xcache.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <signal.h>

static std::mt19937_64 rng(std::random_device{}());

static void random_bytes(const std::string& path, size_t sz) {
    std::vector<uint8_t> buf(sz);
    for (auto& b : buf) b = static_cast<uint8_t>(rng());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(sz));
}

static void do_one(const std::string& base, size_t idx_sz, size_t dat_sz, size_t block_size) {
    auto idx_path = base + ".idx";
    auto dat_path = base + ".dat";

    random_bytes(idx_path, idx_sz);
    random_bytes(dat_path, dat_sz);

    // open — must not crash
    xcache::XCache kv(base, block_size > 0 ? block_size : 256UL << 10);
    std::string v;
    kv.get_string("hello", &v);
    kv.get_string("", &v);
    kv.put_string("k", "v");
    kv.scan([](const std::string&, xcache_value_type_t) { return true; });
    kv.get_all_keys();
    kv.exists("any");
    kv.rebuild();
    kv.size();
    // destructor calls sync+close
}

int main() {
    std::string base = "/tmp/xc_fc_" + std::to_string(getpid());

    size_t sizes[] = {0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256,
                      511, 512, 1023, 1024, 2047, 2048, 4095, 4096, 4097, 8192,
                      16384, 32768, 65536, 131072};
    size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);
    size_t blk_sizes[] = {1, 4, 64, 256, 512, 4096, 65536, 256UL << 10, 1UL << 20};

    fprintf(stderr, "=== fuzz_corrupt: %zu idx * %zu dat * %zu blk ===\n",
            nsizes, nsizes, sizeof(blk_sizes) / sizeof(blk_sizes[0]));

    int crashes = 0, passes = 0;
    for (int n = 0; n < 1000; ++n) {
        auto idx_sz = sizes[rng() % nsizes];
        auto dat_sz = sizes[rng() % nsizes];
        auto blk_sz = blk_sizes[rng() % (sizeof(blk_sizes) / sizeof(blk_sizes[0]))];

        pid_t pid = fork();
        if (pid == 0) {
            do_one(base, idx_sz, dat_sz, blk_sz);
            _exit(0);
        }

        int status;
        waitpid(pid, &status, 0);

        if (WIFSIGNALED(status)) {
            crashes++;
            fprintf(stderr, "  CRASH iter=%d sig=%d (%s) idx=%zu dat=%zu blk=%zu\n",
                    n, WTERMSIG(status), strsignal(WTERMSIG(status)),
                    idx_sz, dat_sz, blk_sz);
        } else {
            passes++;
        }

        ::unlink((base + ".idx").c_str());
        ::unlink((base + ".dat").c_str());
        ::unlink((base + ".idx.tmp").c_str());
        ::unlink((base + ".rebuild.idx").c_str());
        ::unlink((base + ".rebuild.dat").c_str());

        if (n % 500 == 499)
            fprintf(stderr, "  iter %d: ok=%d crash=%d\n", n + 1, passes, crashes);
    }

    fprintf(stderr, "=== fuzz_corrupt DONE: %d passes, %d crashes ===\n", passes, crashes);
    return crashes > 0 ? 1 : 0;
}
