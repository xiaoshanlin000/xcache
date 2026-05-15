#include "xcache/xcache.h"
#include <cstdio>
#include <iostream>

int main() {
    const char* path = "/tmp/xcache_demo.dat";

    // Remove stale files from last run
    std::remove((std::string(path) + ".dat").c_str());
    std::remove((std::string(path) + ".idx").c_str());
    std::remove((std::string(path) + ".idx.tmp").c_str());
    {
        xcache::XCache store(path);

        store.put_string("user:42:name", "Alice");
        store.put_string("user:42:email", "alice@example.com");
        store.put_string("config:theme", "dark");
        store.put_string("config:lang", "zh-CN");

        std::cout << "entries after 4 puts: " << store.size() << "\n";

        std::string v;
        if (store.get_string("user:42:name", &v) == XCACHE_OK) {
            std::cout << "user:42:name = " << v << "\n";
        }

        // Overwrite existing key
        store.put_string("config:theme", "light");

        // Explicit sync before close to ensure durability
        store.sync();

        // Remove
        store.remove("config:lang");
        std::cout << "config:lang exists: " << store.exists("config:lang") << "\n";

        // Batch read
        for (auto& k : {"user:42:name", "user:42:email", "config:theme"}) {
            std::string sv;
            std::cout << k << " = ";
            if (store.get_string(k, &sv) == XCACHE_OK)
                std::cout << sv;
            else
                std::cout << "(null)";
            std::cout << "\n";
        }

    }
    std::cout << "store closed. data preserved at " << path << "\n";

    // Re-open and verify persistence
    {
        xcache::XCache store(path);
        std::cout << "re-opened, entries: " << store.size() << "\n";
        std::string tv;
        if (store.get_string("config:theme", &tv) == XCACHE_OK) {
            std::cout << "config:theme = " << tv << "\n";
        }
    }

    std::remove((std::string(path) + ".idx").c_str());
    std::remove((std::string(path) + ".idx.tmp").c_str());
    return 0;
}
