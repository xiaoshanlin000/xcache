// Regression tests for the review fixes:
//   1. rebuild() pointer-swap race with concurrent readers (pause_reads)
//   2. put_typed rehash-retry outside the write protection window
//   3. remap_if_stale fstat size mismatch (crash on remap after rehash)
//   4. multi-process protocol: lock file, EX held across rehash/rebuild,
//      generation monotonic bump on rebuild, .dat remap on inode change
#include "xcache/xcache.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

static std::string tmp_path() {
    return "/tmp/xcf_" + std::to_string(::getpid()) + "_" + std::to_string(std::rand());
}
static void cleanup(const std::string& p) {
    ::unlink((p + ".dat").c_str());
    ::unlink((p + ".idx").c_str());
    ::unlink((p + ".lock").c_str());
    ::unlink((p + ".rebuild.idx").c_str());
    ::unlink((p + ".rebuild.dat").c_str());
    ::unlink((p + ".rebuild.lock").c_str());
}

// ── fix 1: rebuild 换指针瞬间,并发读者不得读到错配的槽位/数据块 ──

TEST(XConcurrencyFix, RebuildUnderConcurrentReaders) {
    auto p = tmp_path();
    {
        xcache::XCache kv(p, 65536, false, 1024);
        constexpr int nkeys = 200;
        for (int i = 0; i < nkeys; ++i) {
            kv.put_string("k" + std::to_string(i), "v" + std::to_string(i));
        }

        std::atomic<bool> stop{false};
        std::atomic<int64_t> reads_ok{0};
        std::atomic<int64_t> reads_bad{0};   // 错误值或非常规错误码
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t) {
            readers.emplace_back([&, t] {
                int64_t i = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    auto k = "k" + std::to_string(i % nkeys);
                    std::string v;
                    auto err = kv.get_string(k, &v);
                    if (err == XCACHE_OK) {
                        if (v == "v" + std::to_string(i % nkeys)) {
                            reads_ok.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            reads_bad.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        reads_bad.fetch_add(1, std::memory_order_relaxed);
                    }
                    if ((i & 63) == 0 && t == 0) {
                        // 额外覆盖 scan / size 读路径
                        kv.scan([](const std::string&, xcache_value_type_t) { return true; });
                        kv.size();
                    }
                    ++i;
                }
            });
        }

        for (int r = 0; r < 20; ++r) {
            ASSERT_EQ(kv.rebuild(), XCACHE_OK);
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : readers) t.join();

        EXPECT_EQ(reads_bad.load(), 0);
        EXPECT_GT(reads_ok.load(), 0);
        EXPECT_EQ(kv.size(), static_cast<size_t>(nkeys));
        for (int i = 0; i < nkeys; ++i) {
            std::string v;
            EXPECT_EQ(kv.get_string("k" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "v" + std::to_string(i));
        }
    }
    cleanup(p);
}

// ── fix 2: 表满 → try_insert 失败 → rehash → 重试必须回到写保护区 ──

TEST(XConcurrencyFix, RehashRetryUnderConcurrentWriters) {
    auto p = tmp_path();
    {
        // 极小 init_slots → 频繁 rehash;多线程 put 持续制造 try_insert 失败回退路径
        xcache::XCache kv(p, 1UL << 20, false, 64);
        constexpr int nthreads = 6;
        constexpr int per_thread = 4000;
        std::atomic<int> put_fail{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < nthreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < per_thread; ++i) {
                    auto key = "t" + std::to_string(t) + "_" + std::to_string(i);
                    if (kv.put_string(key, "v") != XCACHE_OK) {
                        put_fail.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        // 与 rebuild 竞争(rehash 与 rebuild 的 pause/换指针路径交叉)
        std::thread rebuilder([&] {
            for (int r = 0; r < 3; ++r) {
                kv.rebuild();
                std::this_thread::sleep_for(5ms);
            }
        });
        for (auto& t : threads) t.join();
        rebuilder.join();

        EXPECT_EQ(put_fail.load(), 0);
        for (int t = 0; t < nthreads; ++t) {
            for (int i = 0; i < per_thread; ++i) {
                std::string v;
                EXPECT_EQ(kv.get_string("t" + std::to_string(t) + "_" + std::to_string(i), &v),
                          XCACHE_OK);
            }
        }
        EXPECT_EQ(kv.size(), static_cast<size_t>(nthreads * per_thread));
    }
    cleanup(p);
}

// ── fix 3+4: 双进程同时写 + 交叉 rehash + rebuild,任何进程都不丢写、不错读 ──

TEST(XConcurrencyFix, ForkMultiProcessRehashAndRebuild) {
    auto p = tmp_path();
    constexpr int N = 2000;

    // 父进程忽略 SIGPIPE:子进程提前失败时 write 返回 EPIPE,便于诊断子进程退出码
    ::signal(SIGPIPE, SIG_IGN);

    // 父进程准备初始数据(小 init_slots → 多进程侧也会频繁 rehash)
    {
        xcache::XCache kv(p, 1UL << 20, true, 64);
        for (int i = 0; i < N; ++i) {
            ASSERT_EQ(kv.put_string("pa" + std::to_string(i), "pva" + std::to_string(i)), XCACHE_OK);
        }
    }

    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        // ── 子进程 ──
        ::close(fds[1]);
        xcache::XCache kv(p, 1UL << 20, true, 64);
        for (int i = 0; i < N; ++i) {
            if (kv.put_string("pc" + std::to_string(i), "pvc" + std::to_string(i)) != XCACHE_OK) {
                _exit(1);  // put 失败(不应发生)
            }
            if (i % 50 == 0) {
                std::string v;
                if (kv.get_string("pa" + std::to_string(i % N), &v) != XCACHE_OK ||
                    v != "pva" + std::to_string(i % N)) {
                    _exit(2);  // 父进程数据读不到/读错(remap 问题)
                }
            }
        }
        // 等父进程 rebuild 完成
        char c;
        if (::read(fds[0], &c, 1) != 1) { _exit(3); }
        // rebuild 后验证:自己的 key 必须仍在(跨进程 remap,含 .dat 换 inode)
        for (int i = 0; i < N; ++i) {
            std::string v;
            if (kv.get_string("pc" + std::to_string(i), &v) != XCACHE_OK ||
                v != "pvc" + std::to_string(i)) {
                _exit(4);
            }
        }
        kv.close();
        _exit(0);
    }

    // ── 父进程 ──
    ::close(fds[0]);
    int put_fail = 0;
    {
        xcache::XCache kv(p, 1UL << 20, true, 64);
        for (int i = 0; i < N; ++i) {
            if (kv.put_string("pb" + std::to_string(i), "pvb") != XCACHE_OK) {
                ++put_fail;
            }
        }
        // 子进程可能仍在写;rebuild 通过 EX 锁 + pause 等待其完成
        EXPECT_EQ(kv.rebuild(), XCACHE_OK);
        char c = 'x';
        ::write(fds[1], &c, 1);
    }
    EXPECT_EQ(put_fail, 0);

    int status;
    ::waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status)) << "child exited abnormally (signal "
                                   << (WIFSIGNALED(status) ? WTERMSIG(status) : -1) << ")";
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child exit code: " << WEXITSTATUS(status);

    // 父进程 reopen 验证全部数据(含子进程在 rebuild 前写入的 c_i)
    {
        xcache::XCache kv(p, 1UL << 20, true, 64);
        for (int i = 0; i < N; ++i) {
            std::string v;
            EXPECT_EQ(kv.get_string("pa" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "pva" + std::to_string(i));
            EXPECT_EQ(kv.get_string("pb" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "pvb");
            EXPECT_EQ(kv.get_string("pc" + std::to_string(i), &v), XCACHE_OK);
            EXPECT_EQ(v, "pvc" + std::to_string(i));
        }
        EXPECT_EQ(kv.size(), static_cast<size_t>(3 * N));
    }
    cleanup(p);
}
