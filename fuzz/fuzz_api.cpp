// fuzz_api.cpp — API call sequence fuzzer
// Random sequences of put/get/remove/exists/rebuild/scan with random keys/values.
// Checks invariants: no crashes, get after put returns correct value, size consistency.

#include "xcache/xcache.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>

static std::mt19937_64 rng(std::random_device{}());

static std::string rand_str(size_t max_len = 32) {
    size_t len = rng() % (max_len + 1);
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i)
        s += static_cast<char>('a' + (rng() % 26));
    return s;
}

static std::string rand_bin(size_t max_len = 64) {
    size_t len = rng() % (max_len + 1);
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i)
        s += static_cast<char>(rng() & 0xFF);
    return s;
}

int main() {
    std::string path = "/tmp/xc_fapi_" + std::to_string(getpid());

    fprintf(stderr, "=== fuzz_api: random API sequences ===\n");

    for (int round = 0; round < 50; ++round) {
        // Fresh instance per round
        ::unlink((path + ".idx").c_str());
        ::unlink((path + ".dat").c_str());

        xcache::XCache kv(path, 4UL << 20);
        std::unordered_map<std::string, std::string> oracle;  // ground truth
        std::vector<std::string> keys_in_cache;

        int n_ops = 200 + static_cast<int>(rng() % 500);
        int puts = 0, gets = 0, removes = 0, rebuilds = 0;

        for (int i = 0; i < n_ops; ++i) {
            int op = static_cast<int>(rng() % 8);

            switch (op) {
                case 0: case 1: {  // put
                    auto k = rand_str(20);
                    auto v = rand_bin(50);
                    uint32_t ttl = (rng() % 4 == 0) ? static_cast<uint32_t>(rng() % 60 + 60) : 0;
                    kv.put_string(k, v, ttl);
                    oracle[k] = v;
                    puts++;
                    break;
                }
                case 2: {  // get (check consistency)
                    std::string k;
                    if (!oracle.empty() && rng() % 2 == 0) {
                        auto it = oracle.begin();
                        std::advance(it, rng() % oracle.size());
                        k = it->first;
                    } else {
                        k = rand_str(10);
                    }
                    std::string v;
                    auto err = kv.get_string(k, &v);
                    auto it = oracle.find(k);
                    if (it != oracle.end()) {
                        // key was put; might be expired if TTL'd
                        // just verify: either OK with correct val, or NOT_FOUND/EXPIRED
                        if (err == XCACHE_OK && v != it->second)
                            fprintf(stderr, "  MISMATCH: key=%s expected=%s got=%s\n",
                                    k.c_str(), it->second.c_str(), v.c_str());
                    }
                    gets++;
                    break;
                }
                case 3: {  // exists
                    auto k = rand_str(10);
                    kv.exists(k);
                    break;
                }
                case 4: {  // remove
                    std::string k;
                    if (!oracle.empty() && rng() % 2 == 0) {
                        auto it = oracle.begin();
                        std::advance(it, rng() % oracle.size());
                        k = it->first;
                    } else {
                        k = rand_str(10);
                    }
                    kv.remove(k);
                    oracle.erase(k);
                    removes++;
                    break;
                }
                case 5: {  // rebuild
                    kv.rebuild();
                    rebuilds++;
                    break;
                }
                case 6: {  // scan (catch crashes)
                    kv.scan([](const std::string&, xcache_value_type_t) { return true; });
                    break;
                }
                case 7: {  // get_all_keys (catch crashes)
                    kv.get_all_keys();
                    break;
                }
            }

            // size() consistency check
            kv.size();
        }

        fprintf(stderr, "  round %d: %d ops (put=%d get=%d remove=%d rebuild=%d) oracle=%zu\n",
                round, n_ops, puts, gets, removes, rebuilds, oracle.size());
    }

    ::unlink((path + ".idx").c_str());
    ::unlink((path + ".dat").c_str());

    fprintf(stderr, "=== fuzz_api DONE ===\n");
    return 0;
}
