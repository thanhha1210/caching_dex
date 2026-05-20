#pragma once

/* A cache implementation*/
#include <algorithm>
#include <atomic>
#include <bits/hash_bytes.h>
#include <cstdint>
#include <ctime>
#include <random>

#include "PageTable.h"
#include "../cache/BtreeNode.h"
#include "../cache/NodeWR.h"


namespace cachepush {
#define PAGE_TABLE 1

const uint64_t ADDRESS_HIDE = (1ULL << 48) - 1;
const uint64_t NUM_ENTRIES = 6;
const int CACHE_LINE_SIZE = 64;

struct Frame { // 8B
    uint64_t page_; // Stores the cache memory address (FP top 16 bits | slot_ptr low 48 bits)
    // uint64_t epoch_; // Used in the future to implement concurrency stuff
};


struct Bucket { // 64 byte = one cache line
    int count;  // 4B
    int padding; // 4B
    Frame frame_[NUM_ENTRIES]; // 48B
    uint64_t cacheline_pad_; // 8B (instead of Lock lock_ to keep Bucket 64B)

    // Match key
    inline int match(uint64_t key, uint64_t FP) {
        for (int i = 0; i < count; ++i) {
            uint64_t cur_val = frame_[i].page_ >> 48;
            if (cur_val == FP && reinterpret_cast<NodeBase *>(frame_[i].page_ & ADDRESS_HIDE)->remote_address.val == key) 
                return i;
        }
        return -1;
    }

    inline int match_value(uint64_t FP, void *value) {
        for (int i = 0; i < count; ++i) {
            uint64_t cur_val = frame_[i].page_ >> 48;
            if (cur_val == FP && reinterpret_cast<void *>(frame_[i].page_ & ADDRESS_HIDE) == value) 
                return i;
        }
        return -1;
    }

    inline void remove(int idx) {
        frame_[idx].page_ = 0;
        for (int i = idx; i < count - 1; ++i) 
            frame_[i] = frame_[i + 1];
        --count;
    }

    // -1 = eviction fail,  0 = evict success
    inline int evict_last(void **address_ptr) {
        if (count == 0) 
            return -1;
        *address_ptr = reinterpret_cast<void *>(frame_[count - 1].page_ & ADDRESS_HIDE);
        assert(reinterpret_cast<NodeBase *>(*address_ptr)->dirty == false);
        --count;
        return 0;
    }

    bool is_full() { return (count == NUM_ENTRIES); }

    // insert always succeed because of no epoch
    void insert(uint64_t key, uint64_t payload, void **evict_page) {
        if (count == NUM_ENTRIES) {
            auto ret = evict_last(evict_page);
            assert(ret == 0);
        }
        for (int i = count - 1; i >= 0; --i) 
            frame_[i + 1] = frame_[i];
        frame_[0].page_ = payload;
        // frame_[0].epoch_ = epoch;
        ++count;
    }
};

class HashTable {
public: 
    uint64_t entry_num_;
    uint64_t bucket_num_;
    uint64_t total_num_ = 0; // total kv num
    Bucket *table_;
    PageTable *page_table_;
    std::default_random_engine gen_;
    std::uniform_int_distribution<uint32_t> dist_;
    

    size_t hash(const void *_ptr, size_t _len, size_t _seed = static_cast<size_t>(0xc70f6907UL)) {
        return std::_Hash_bytes(_ptr, _len, _seed);
    }

    HashTable(uint64_t entry_num, PageTable *buffer_page_table = nullptr)
        : entry_num_(entry_num), 
          dist_(0, (((entry_num % NUM_ENTRIES) == 0) ? (entry_num / NUM_ENTRIES) : (entry_num / NUM_ENTRIES + 1)) - 1) {
        // gen_.seed((uint32_t)time(NULL));
        gen_.seed((uint32_t)0xc70f6907);
        bucket_num_ = ((entry_num % NUM_ENTRIES) == 0) ? (entry_num / NUM_ENTRIES) : (entry_num / NUM_ENTRIES + 1);
        posix_memalign(reinterpret_cast<void **>(&table_), CACHE_LINE_SIZE, bucket_num_ * sizeof(Bucket));
        
        std::cout << "Bucket size = " << sizeof(Bucket) << std::endl;
        assert((sizeof(Bucket) % CACHE_LINE_SIZE) == 0);
        std::cout << "Entry_num: " << entry_num << std::endl;
        std::cout << "Bucket_num: " << bucket_num_ << std::endl;
        memset(reinterpret_cast<void *>(table_), 0, bucket_num_ * sizeof(Bucket));
        total_num_ = 0;
        page_table_ = buffer_page_table;
    }

    void reset() {
        memset(reinterpret_cast<void *>(table_), 0, bucket_num_ * sizeof(Bucket));
    }

    // Get bucket just based on key only, other is ref to be changed
    Bucket *get_bucket(uint64_t key, void *value, uint64_t &payload) {  
        auto hash_val = hash(&key, sizeof(key));
        auto bucket_idx = hash_val % bucket_num_;
        auto FP = (hash_val >> 49) | (1ULL << 15);
        payload = (FP << 48) | (reinterpret_cast<uint64_t>(value) & ADDRESS_HIDE);
        return (table_ + bucket_idx);
    }

    Bucket *get_bucket(uint64_t key, uint64_t &FP) {
        auto hash_val = hash(&key, sizeof(key));
        auto bucket_idx = hash_val % bucket_num_;
        FP = (hash_val >> 49) | (1ULL << 15);
        // payload = (FP << 48) | (reinterpret_cast<uint64_t>(value) & ADDRESS_HIDE);
        return (table_ + bucket_idx);
    }

    // TODO: add lock later, remote_write
    // used by leanstore_cache.sample_page demotes a hot page
    // if not full -> place new entries at 0, shift other by 1
    // if full -> return oldest entry via evict_page
    void insert(uint64_t key, void *value, void **evict_page) {
        auto hash_val = hash(&key, sizeof(key));
        auto bucket_idx = hash_val % bucket_num_;
        auto FP = (hash_val >> 49) | (1ULL << 15);
        Bucket *cur_bucket = table_ + bucket_idx;

        auto mem_node = reinterpret_cast<NodeBase *>(value);
        if (mem_node->dirty) {
            remote_write(mem_node->remote_address, mem_node, true); // ??? change to ...
        }
        //cur_bucket->lock_.get_lock();
        auto payload = (FP << 48) | (reinterpret_cast<uint64_t>(value) & ADDRESS_HIDE);
        cur_bucket->insert(key, payload, evict_page);
        mem_node->pos_state = 1;
        if ((*evict_page) != nullptr) {
            auto mem_page = reinterpret_cast<NodeBase *>(*evict_page);
            auto flag = page_table_->remove_with_lock(mem_page->remote_address, *evict_page);
        // if (!flag) {
        //   assert(mem_page->type == PageType::BTreeInner);
        //   auto mem_inner = reinterpret_cast<BTreeInner<Key> *>(mem_page);
        //   assert(mem_inner->isShared());
        // }
        }
        // cur_bucket->lock_.release_lock();
    }

    /*
  // Get a free slot in target bucket
  // 0 means success
  // -1 means failure
  int get_lock_if_free(uint64_t key, void **evict_page, uint64_t &hash_val) {
    hash_val = hash(&key, sizeof(key));
    auto bucket_idx = hash_val % bucket_num_;
    bucket *cur_bucket = table_ + bucket_idx;
    cur_bucket->lock_.get_lock();
    if (!cur_bucket->is_full()) {
      return 0;
    }

    auto success = cur_bucket->evict_last(evict_page);
    if (success == 0) {
      auto mem_page = reinterpret_cast<NodeBase *>(*evict_page);
      auto flag =
          page_table_->remove_with_lock(mem_page->remote_address,
          *evict_page);
      if (!flag) {
        assert(mem_page->type == PageType::BTreeInner);
        auto mem_inner = reinterpret_cast<BTreeInner<Key> *>(mem_page);
        assert(mem_inner->isShared());
      }
      return 0;
    }
    cur_bucket->lock_.release_lock();
    return -1;
  }

  void insert_without_lock(uint64_t key, void *value, uint64_t hash_val) {
    auto bucket_idx = hash_val % bucket_num_;
    auto FP = (hash_val >> 49) | (1ULL << 15);
    bucket *cur_bucket = table_ + bucket_idx;

    assert(!cur_bucket->is_full());
    auto payload =
        (FP << 48) | (reinterpret_cast<uint64_t>(value) & ADDRESS_HIDE);
    void **evict_page = nullptr;
    auto ret =
        cur_bucket->insert(key, payload, cache_allocator::GetCurrentEpoch(),
                           evict_page); // Evict the pa0ge with current epoch
    assert(evict_page == nullptr);
    assert(ret == 0);

    auto mem_node = reinterpret_cast<NodeBase *>(value);
    mem_node->pos_state = 1;
    if (mem_node->dirty) {
      remote_write(mem_node->remote_address, mem_node, true);
    }
    cur_bucket->lock_.release_lock();
  }
*/
    
    // Not use here. TODO: add lock later
    bool try_promote(uint64_t key, void **page_ptr) {
        auto hash_val = hash(&key, sizeof(key));
        auto bucket_idx = hash_val % bucket_num_;
        auto FP = (hash_val >> 49) | (1ULL << 15);

        Bucket *cur_bucket = table_ + bucket_idx;
        // if (!cur_bucket->lock_.try_get_lock())
        //    return false;

        auto slot_idx = cur_bucket->match(key, FP);
        if (slot_idx == -1) {
        //    cur_bucket->lock_.release_lock();
            return false;
        }

        *page_ptr = reinterpret_cast<void *>(cur_bucket->frame_[slot_idx].page_ & ADDRESS_HIDE);
        cur_bucket->remove(slot_idx);
        // cur_bucket->lock_.release_lock();
        return true;
    }
    
    // TODO: add lock later
    // find (key, val) in the bucket & remove it, caller change slot back to hot
    bool try_promote_using_value(uint64_t key, void *page) {
        auto hash_val = hash(&key, sizeof(key));
        auto bucket_idx = hash_val % bucket_num_;
        auto FP = (hash_val >> 49) | (1ULL << 15);

        Bucket *cur_bucket = table_ + bucket_idx;
        // if (!cur_bucket->lock_.try_get_lock())
            // return false;

        auto slot_idx = cur_bucket->match_value(FP, page);
        if (slot_idx == -1) {
            // cur_bucket->lock_.release_lock();
            return false;
        }

        cur_bucket->remove(slot_idx);
        // cur_bucket->lock_.release_lock();
        return true;
    }

    // not use now 
    // check if (key, page_ptr) pair currently in the cool tier
    // if yes -> return true, otherwise -> return false
    bool check_existence(uint64_t key, void *page_ptr) {
        auto hash_val = hash(&key, sizeof(key));
        auto bucket_idx = hash_val % bucket_num_; // which bucket the key maps to
        auto FP = (hash_val >> 49) | (1ULL << 15);

        Bucket *cur_bucket = table_ + bucket_idx;
        auto slot_idx = cur_bucket->match(key, FP);
        if (slot_idx == -1)
            return false;
        auto target_page = reinterpret_cast<void *>( cur_bucket->frame_[slot_idx].page_ & ADDRESS_HIDE);
        if (page_ptr != target_page)
            return false;
        return true;
    }

    // TODO: no need to use for now
    void release_target_lock(uint64_t key) {
        auto hash_val = hash(&key, sizeof(key));
        auto bucket_idx = hash_val % bucket_num_;
        Bucket *cur_bucket = table_ + bucket_idx;
        // cur_bucket->lock_.release_lock();
    }

    // used by leanstorecache.try_get_empty_page() need a free slot
    // -1 means find nothing, 0 means find an evict page in this page
    int random_evict_to_remote(void **page_ptr, int probing_length) {
        auto idx = random_bucket_idx();
        Bucket *cur_bucket;
        uint64_t target_bucket = std::min(bucket_num_, idx + probing_length);
        
        /* This is original for -> below, I will remove lock & indef
        for (uint64_t i = idx; i < target_bucket; ++i) {
        cur_bucket = table_ + i;
        if (cur_bucket->lock_.try_get_lock()) {
            auto success = cur_bucket->evict_last(page_ptr);
            if (success == 0) {
    #ifdef PAGE_TABLE
            auto mem_page = reinterpret_cast<NodeBase *>(*page_ptr);
            auto flag = page_table_->remove_with_lock(mem_page->remote_address,
                                                        *page_ptr);
            // It can be also obsolete pages caused by RPC
            // if (!flag) {
            //   assert(mem_page->type == PageType::BTreeInner);
            //   auto mem_inner = reinterpret_cast<BTreeInner<Key> *>(mem_page);
            //   assert(mem_inner->isShared());
            // }
    #endif
            cur_bucket->lock_.release_lock();
            return 0;
            }
            cur_bucket->lock_.release_lock();
        }
        }
        */  

        for (uint64_t i = idx; i < target_bucket; ++i) {
            cur_bucket = table_ + i;
            auto success = cur_bucket->evict_last(page_ptr);
            if (success == 0) {
                auto mem_page = reinterpret_cast<NodeBase *>(*page_ptr);
                auto flag = page_table_->remove_with_lock(mem_page->remote_address, *page_ptr);
            return 0;
            }
        }
        // Find nothing
        return -1;
    }

    inline uint64_t random_bucket_idx() {
        static thread_local std::mt19937 *generator = nullptr;
        if (!generator)
            generator = new std::mt19937(clock() + pthread_self());
        static thread_local std::uniform_int_distribution<uint64_t> distribution(0, bucket_num_ - 1);
        auto idx = distribution(*generator);
        // int idx = rand() % bucket_num_;
        // auto idx = dist_(gen_);
        assert(idx < bucket_num_);
        return idx;
    }
};

} // namespace cachepush
