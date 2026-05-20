#pragma once

#include <cstdint>
#include <cstdio>

namespace cachepush {

    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t admits = 0;
        uint64_t evicts = 0;
        uint64_t writebacks = 0;
        uint64_t allocs = 0;
        uint64_t rescues = 0;   // COOL -> HOT on cache hit
        uint64_t demotions = 0;   // HOT  -> COOL by sampler tick
        uint64_t ticks = 0;   // number of sampler runs

        void reset() { *this = Stats{}; }

        double hitRate() const {
            const uint64_t total = hits + misses;
            return total ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
        }

        void print(const char *tag = "cache") const {
            printf("[%s] hits=%lu misses=%lu admits=%lu evicts=%lu "
                        "writebacks=%lu allocs=%lu rescues=%lu demotions=%lu "
                        "ticks=%lu hit_rate=%.4f\n",
                        tag, hits, misses, admits, evicts, writebacks, allocs,
                        rescues, demotions, ticks, hitRate());
        }
    };

} // namespace cachepush
