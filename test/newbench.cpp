// Args are read from argv (positional, all optional):
//   newbench [key_space] [op_num] [read_ratio] [insert_ratio] [update_ratio]
//            [delete_ratio] [range_ratio] [zipfian] [cache_mb] [scan_len]
//
// Defaults is read and update workload
//   key_space=1M  op_num=1M  reads=50  inserts=0  updates=50  deletes=0
//   range=0  zipfian=0.99  cache_mb=16  scan_len=100


#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "memory/InMemoryBackend.h"
#include "tree/LeanStoreTree.h"
#include "uniform.h"
#include "uniform_generator.h"
#include "zipf.h"

using namespace cachepush;

namespace {

struct Args { 
    uint64_t key_space = 1000000;
    uint64_t op_num = 1000000;
    uint32_t read_ratio = 50;
    uint32_t insert_ratio = 0;
    uint32_t update_ratio = 50;
    uint32_t delete_ratio = 0;
    uint32_t range_ratio  = 0;
    double zipfian = 0.99;
    uint64_t cache_mb = 16;
    uint32_t scan_len = 100;
};

void parse_args(int argc, char **argv, Args &k) {
    if (argc != 11 && argc != 1) {
        std::cout << "argc = " << argc << "\n";
        std::cout << "Usage: ./benchmark key_space op_num read_ratio insert_ratio"
                     "update_ratio delete_ratio range_ratio zipfian cache_mb scan_len" << "\n";
        exit(-1);
    }
    if (argc == 11) {
        k.key_space = atoi(argv[1]);
        k.op_num =  atoi(argv[2]);
        k.read_ratio = atoi(argv[3]);
        k.insert_ratio = atoi(argv[4]);
        k.update_ratio = atoi(argv[5]);
        k.delete_ratio = atoi(argv[6]);
        k.range_ratio = atoi(argv[7]);
        k.zipfian = atoi(argv[8]);
        k.cache_mb = atoi(argv[9]);
        k.scan_len = atoi(argv[10]);
    }
}

void print_Args(const Args &k) {
    std::cout << "key_space = " << k.key_space << "\n"
              << "op_num = " << k.op_num << "\n"
              << "read_ratio = " << k.read_ratio << "%\n"
              << "insert_ratio = " << k.insert_ratio << "%\n"
              << "update_ratio = " << k.update_ratio << "%\n"
              << "delete_ratio = " << k.delete_ratio << "%\n"
              << "range_ratio  = " << k.range_ratio << "%\n"
              << "zipfian = " << k.zipfian << "\n"
              << "cache_mb = " << k.cache_mb << "\n"
              << "scan_len = " << k.scan_len << "\n";
}

// Pick the next operation type from the ratio mix.
enum class Op : uint8_t { READ, INSERT, UPDATE, DELETE, RANGE };

Op pick_op(uint32_t roll, const Args &k) {
    uint32_t r = roll % 100;
    uint32_t acc = k.read_ratio;
    if (r < acc) return Op::READ;
        acc += k.insert_ratio;
    if (r < acc) return Op::INSERT;
        acc += k.update_ratio;
    if (r < acc) return Op::UPDATE;
        acc += k.delete_ratio;
    if (r < acc) return Op::DELETE;
    return Op::RANGE;
}

} // namespace

int main(int argc, char **argv) {
    Args k;
    parse_args(argc, argv, k);

    uint32_t total = k.read_ratio + k.insert_ratio + k.update_ratio +
                     k.delete_ratio + k.range_ratio;
    if (total != 100) {
        std::cerr << "ratios must sum to 100 (got " << total << ")\n";
        return 1;
    }

    print_Args(k);

    // Backend big enough to hold the worst case
    const uint64_t backend_pages = std::max<uint64_t>(64ULL * 1024, k.key_space / 8 + 1024);
    const uint64_t cache_pages = (k.cache_mb * 1024ULL * 1024ULL) / PAGE_SIZE;
    InMemoryBackend backend(backend_pages);

    // A B+tree page (1 KB) fits ~30 64-bit KV pairs -> working set of 
    // n keys needs n/30 leaf pages.
    const uint64_t est_working_set = (k.key_space / 30) + 64;
    LeanStoreTree<Key, Value> tree(&backend, cache_pages);

    // --- 1. Bulk-load: insert key_space keys with values = key + 1  
    auto t0 = std::chrono::steady_clock::now();
    uniform_key_generator_t bulk_gen(k.key_space);
    uint64_t inserted = 0;

    for (uint64_t i = 1; i <= k.key_space; ++i)   // insert into tree sequentially
        if (tree.insert(i, i + 1)) 
            inserted++;
    
    auto t1 = std::chrono::steady_clock::now();
    double bulk_secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "bulk-load: inserted " << inserted << " keys in "
              << bulk_secs << "s (" << (inserted / bulk_secs / 1e6) << " Mops/s)\n";
    tree.get_statistic();

    // --- 2. Mixed workload --- 
    zipf_gen_state zstate;
    mehcached_zipf_init(&zstate, k.key_space, k.zipfian, 0xC70F6907ULL);
    UniformRandom op_picker(0x1234ABCDULL);

    uint64_t cnt_read = 0, cnt_insert = 0, cnt_update = 0, cnt_delete = 0, cnt_range = 0;
    uint64_t hit_read = 0, hit_update = 0, hit_delete = 0;
    uint64_t scanned_total = 0;
    
    std::vector<std::pair<Key, Value>> scan_buf(k.scan_len);

    // insert keys outside the bulk range so they don't collide.
    uint64_t insert_cursor = k.key_space + 1;

    tree.clear_statistic();
    // Snapshot cache miss counters around the mixed phase so we can report
    // how often the walk had to cold-load from Backend.
    const uint64_t leaf_miss_before  = tree.cache.leaf_miss_;
    const uint64_t inner_miss_before = tree.cache.inner_miss_;

    auto bench_t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < k.op_num; ++i) {
        // Zipfian draw into [0, key_space)
        uint64_t zk = mehcached_zipf_next(&zstate);
        Key key = static_cast<Key>(zk + 1);   // shift to [1, key_space]
        Op op = pick_op(op_picker.next_uint32(), k);

        switch (op) {
        case Op::READ: {
            Value v = 0;
            if (tree.lookup(key, v)) ++hit_read;
            ++cnt_read;
            break;
        }
        case Op::UPDATE: {
            if (tree.update(key, key + 7)) ++hit_update;
            ++cnt_update;
            break;
        }
        case Op::INSERT: {
            tree.insert(insert_cursor++, insert_cursor);
            ++cnt_insert;
            break;
        }
        case Op::DELETE: {
            if (tree.remove(key)) ++hit_delete;
            ++cnt_delete;
            break;
        }
        case Op::RANGE: {
            std::pair<Key, Value> *ptr = scan_buf.data();
            int got = tree.range_scan(key, k.scan_len, ptr);
            scanned_total += static_cast<uint64_t>(got);
            ++cnt_range;
            break;
        }
        }
    }
    auto bench_t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(bench_t1 - bench_t0).count();

    std::cout << "----\n";
    std::cout << "mixed workload: " << k.op_num << " ops in " << secs
              << "s -> " << (k.op_num / secs / 1e6) << " Mops/s\n";
    std::cout << "  reads="   << cnt_read   << " (hit " << hit_read   << ")\n";
    std::cout << "  updates=" << cnt_update << " (hit " << hit_update << ")\n";
    std::cout << "  inserts=" << cnt_insert << "\n";
    std::cout << "  deletes=" << cnt_delete << " (hit " << hit_delete << ")\n";
    std::cout << "  ranges="  << cnt_range
              << " (avg scanned " << (cnt_range ? scanned_total / cnt_range : 0)
              << ")\n";
    const uint64_t leaf_misses  = tree.cache.leaf_miss_  - leaf_miss_before;
    const uint64_t inner_misses = tree.cache.inner_miss_ - inner_miss_before;
    const uint64_t total_misses = leaf_misses + inner_misses;
    std::cout << "  cache:  leaf_miss=" << leaf_misses
              << " inner_miss=" << inner_misses
              << " total=" << total_misses
              << " misses/op=" << (k.op_num ? (double)total_misses / k.op_num : 0.0)
              << "\n";
    std::cout << "----\n";
    tree.get_statistic();
    
    return 0;
}
