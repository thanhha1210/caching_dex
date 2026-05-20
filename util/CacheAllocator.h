/*Allocator for the concurrent cache*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <vector>

#include <inttypes.h>
#include <memory.h>
#include <pthread.h>
#include <sys/mman.h>

// Here we need to add a wrapper to invoke epoch managerment

namespace cachepush {
// Plain anonymous mmap. The dex original used MAP_HUGETLB + numa_*
// (requires root / libhugetlbfs / libnuma). For  single-threaded local-memory,
// just use simple mmap
inline void *hugePageAlloc(size_t size) {
    void *res = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        perror("mmap allocation failed");
        return nullptr;
    }
    return res;
}

class CacheAllocator {
public:
    uint64_t base_address;
    uint64_t alloc_unit_;
    uint64_t page_num_;
    std::atomic<uint64_t> head_idx_; // head index of cache pool
    std::uniform_int_distribution<uint32_t> dist_;

    static CacheAllocator *instance_;
    // static uint32_t seed_; // seed
    // static std::default_random_engine gen_;
    // EpochManager epoch_manager_{};

    CacheAllocator(uint64_t alloc_unit, uint64_t cache_size) // cache size in B
        : alloc_unit_(alloc_unit), dist_(0, (cache_size / alloc_unit) - 1) {
        
        std::cout << "Cache size(MB) = " << cache_size / (1024.0 * 1024.0) << std::endl; 
        
        void *pool = nullptr;
        pool = hugePageAlloc(cache_size);
        if (pool == nullptr) {
            throw std::bad_alloc();
        }

        printf("-----Cache initial addr = 0x%" PRIx64 "\n", reinterpret_cast<uint64_t>(pool));
        base_address = reinterpret_cast<uint64_t>(pool);
        memset(reinterpret_cast<void *>(base_address), 0, cache_size);
        page_num_ = cache_size / alloc_unit_;
        std::cout << "page num = " << page_num_ << std::endl;
        std::cout << "alloc_unit = " << alloc_unit_ << std::endl;
        head_idx_ = 0;
    }

    // destroy cache 
    ~CacheAllocator() {
        if (base_address != 0) {
            munmap(reinterpret_cast<void *>(base_address), page_num_ * alloc_unit_);
        }
    }

    static void initialize(uint64_t alloc_unit, uint64_t cache_size) {
        if (instance_ != nullptr)
            return;
        instance_ = new CacheAllocator(alloc_unit, cache_size);
        // instance_->epoch_manager_.Initialize();
    }

    static void reset() {
        if (instance_ == nullptr) 
            return;
        instance_->head_idx_ = 0;
        memset(reinterpret_cast<void *>(instance_->base_address), 0, instance_->page_num_ * instance_->alloc_unit_);
    }

    static void *allocate(bool &last_page_flag) {
        if (instance_ == nullptr) {
            std::cout << "The External Memory is NOT initialized now!" << std::endl;
            return nullptr;
        }
        if (instance_->head_idx_ >= instance_->page_num_)
            return nullptr;
        auto cur_idx = instance_->head_idx_.fetch_add(1);
        if (cur_idx >= instance_->page_num_)
            return nullptr;
        auto ret = reinterpret_cast<void *>(instance_->base_address + cur_idx * instance_->alloc_unit_);
        if (cur_idx == instance_->page_num_ - 1)
            last_page_flag = true;
        return ret;
    }

    // not use for now - since just local mem
    static bool is_peer(uint64_t left_pair, uint64_t right_pair) {
        if (left_pair > right_pair) {
            std::swap(left_pair, right_pair);
        }
        auto left_id = (left_pair - instance_->base_address) / instance_->alloc_unit_;
        auto right_id = (right_pair - instance_->base_address) / instance_->alloc_unit_;
        return (left_id == right_id);
    }

    // not use for now - since just local mem
    static uint64_t get_peer_addr(uint64_t page_addr) {
        // auto page_id = page_addr / instance_->alloc_unit_;
        uint64_t ret = 0;
        if (page_addr % instance_->alloc_unit_ == 0) // return the middle addr
            ret = page_addr + (instance_->alloc_unit_ / 2); 
        else // return the initial addr
            ret = page_addr - (instance_->alloc_unit_ / 2);
        return ret;
    }

    // Randomly select a page
    static void *random_select() {
        static thread_local std::mt19937 *generator = nullptr;
        if (!generator) {
            generator = new std::mt19937(clock() + pthread_self());
        }
        static thread_local std::uniform_int_distribution<uint32_t> distribution(0, instance_->page_num_ - 1);

        auto idx = distribution(*generator);
        if (idx > instance_->head_idx_ || idx >= instance_->page_num_) {
            idx = idx % (std::min(instance_->head_idx_.load(), instance_->page_num_));
        }
        return reinterpret_cast<void *>(instance_->base_address + idx * instance_->alloc_unit_);
    }

    static void free() {
        // TODO (BT)
        if (instance_ == nullptr)
            return;
        delete instance_;
        instance_ = nullptr;
    }
};

CacheAllocator *CacheAllocator::instance_ = nullptr;

} // namespace cachepush
