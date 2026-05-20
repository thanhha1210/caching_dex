#pragma once

/* This is a page table implementation (not concurrent);
Used to enhance the feature of leanstore => to give the ground truth of page ID
*/

#include <algorithm>
#include <atomic>
#include <bits/hash_bytes.h>
#include <cstdint>
#include <ctime>
#include <random>
#include "../cache/BtreeNode.h"

namespace cachepush {
static const int num_entry_in_page_bucket = 3;
// It should be cache-line based (64 bytes)
const uint64_t IO_FLAG = (1ULL << 63); // to know if a load in progress (not use here since not concurrent)

struct PageFrame { // 16B (8 + 8)
    GlobalAddress page_id_;
    void *buffer_page_;  // buffer_page_ = nullptr => this page_frame is free
};

struct PageBucket { // exactly 64 Byte
    //Lock32 lock_;
    uint32_t count_;   // 4B
    uint32_t padding;  // use instead of Lock (4B)
    PageBucket *next_; // 8B
    PageFrame page_frame_[num_entry_in_page_bucket]; // 16 * 3
};

// hash table
class PageTable {
public:
    uint64_t bucket_num_;
    uint64_t entry_num_;
    PageBucket *table_;

    PageTable(uint64_t entry_num) : entry_num_(entry_num) {
        bucket_num_ = ((entry_num % num_entry_in_page_bucket) == 0)
                        ? (entry_num / num_entry_in_page_bucket)
                        : (entry_num / num_entry_in_page_bucket + 1);
        assert(sizeof(PageBucket) == 64);
        posix_memalign(reinterpret_cast<void **>(&table_), 64,  bucket_num_ * sizeof(PageBucket));
        memset(reinterpret_cast<void *>(table_), 0, bucket_num_ * sizeof(PageBucket));
        std::cout << "entry_num: " << entry_num_ << std::endl;
        std::cout << "bucket_num: " << bucket_num_ << std::endl;
    }

    void traverse_and_delete(PageBucket *cur_bucket) {
        if (cur_bucket == nullptr)
            return;
        traverse_and_delete(cur_bucket->next_);
        free(cur_bucket);
    }

    void reset() {
        // Loop the page table to clear all the overflowed buckets
        for (uint64_t i = 0; i < bucket_num_; ++i) {
            auto cur_bucket = table_ + i;
            traverse_and_delete(cur_bucket->next_);
        }
        memset(reinterpret_cast<void *>(table_), 0, bucket_num_ * sizeof(PageBucket));
    }

    size_t hash(const void *_ptr, size_t _len, size_t _seed = static_cast<size_t>(0xc70f6907UL)) {
        return std::_Hash_bytes(_ptr, _len, _seed);
    }

    PageBucket *get_bucket(GlobalAddress key) { 
        auto hash_val = hash(reinterpret_cast<void *>(&(key.val)), sizeof(GlobalAddress)); // hash the val of key
        auto bucket_idx = hash_val % bucket_num_;
        return (table_ + bucket_idx);
    }

    // initial add a page
    // add (key -> cache_slot); return false if key already there
    bool insert(PageBucket *head_bucket, GlobalAddress key, void *&value) {
        auto cur_bucket = head_bucket;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if ((cur_frame[i].page_id_.val == key.val) &&  (cur_frame[i].buffer_page_ != nullptr)) {
                    // return false;
                    std::cout << "Key value = " << key.val << std::endl;
                    std::cout << "K in table value = " << cur_frame[i].page_id_.val << std::endl;
                    value = cur_frame[i].buffer_page_;
                    return false;
                }
            }
            cur_bucket = cur_bucket->next_;
        }

        cur_bucket = head_bucket;
        PageBucket *last_bucket = nullptr;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if (cur_frame[i].buffer_page_ == nullptr) { // any frame is free -> insert into
                    cur_frame[i].page_id_ = key;
                    cur_frame[i].buffer_page_ = value;
                    cur_bucket->count_++;
                    return true;
                }
            }
            last_bucket = cur_bucket;
            cur_bucket = cur_bucket->next_;
        }

        // otherwise, assign a new PageBucket, insert into that
        assert(last_bucket != nullptr);
        posix_memalign(reinterpret_cast<void **>(&(last_bucket->next_)), 64, sizeof(PageBucket));
        cur_bucket = last_bucket->next_;    
        memset(reinterpret_cast<void *>(cur_bucket), 0, sizeof(PageBucket));
        cur_bucket->page_frame_[0].page_id_ = key;
        cur_bucket->page_frame_[0].buffer_page_ = value;
        cur_bucket->count_++;
        return true;
    }

    bool insert_io_flag(PageBucket *head_bucket, GlobalAddress key) {
        void *value = reinterpret_cast<void *>(IO_FLAG);
        return insert(head_bucket, key, value);
    }

    // TODO: Although the name is with lock, just use no lock for now
    bool insert_with_lock(GlobalAddress key, void *&value) {
        auto head_bucket = get_bucket(key);
        // head_bucket->lock_.get_lock();
        auto ret = insert(head_bucket, key, value);
        // head_bucket->lock_.release_lock();
        return ret;
    }

    // TODO: Just with flag
    bool insert_io_flag_with_lock(GlobalAddress key) {
        auto head_bucket = get_bucket(key);
        // head_bucket->lock_.get_lock();
        auto ret = insert_io_flag(head_bucket, key);
        // head_bucket->lock_.release_lock();
        return ret;
    }

    // if key exist -> change old pointer with new, value hold old slot; otherwise insert
    bool upsert(PageBucket *head_bucket, GlobalAddress key, void *&value) {
        auto cur_bucket = head_bucket;
        // update
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if ((cur_frame[i].page_id_.val == key.val) && (cur_frame[i].buffer_page_ != nullptr)) {
                    auto ret = cur_frame[i].buffer_page_;
                    cur_frame[i].buffer_page_ = value;
                    value = ret;
                    return false;
                }
            }
            cur_bucket = cur_bucket->next_;
        }

        // insert
        cur_bucket = head_bucket;
        PageBucket *last_bucket = nullptr;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if (cur_frame[i].buffer_page_ == nullptr) {
                    cur_frame[i].page_id_ = key;
                    cur_frame[i].buffer_page_ = value;
                    auto target_node = reinterpret_cast<NodeBase *>(value);
                    if (target_node->remote_address != key) {
                        std::cout << "PageTable::upsert mismatch: key=" << key
                                  << " value->remote_address=" << target_node->remote_address
                                  << " value=" << value
                                  << " pos_state=" << (int)target_node->pos_state
                                  << " dirty=" << target_node->dirty
                                  << std::endl;
                    }
                    assert(target_node->remote_address == key);
                    cur_bucket->count_++;
                    return true;
                }
            }
            last_bucket = cur_bucket;
            cur_bucket = cur_bucket->next_;
        }

        assert(last_bucket != nullptr);
        posix_memalign(reinterpret_cast<void **>(&(last_bucket->next_)), 64, sizeof(PageBucket));
        cur_bucket = last_bucket->next_;
        memset(reinterpret_cast<void *>(cur_bucket), 0, sizeof(PageBucket));
        cur_bucket->page_frame_[0].page_id_ = key;
        cur_bucket->page_frame_[0].buffer_page_ = value;
        cur_bucket->count_++;
        return true;
    }

    // TODO : no lock for now
    bool upsert_with_lock(GlobalAddress key, void *&value) {
        auto head_bucket = get_bucket(key);
        // head_bucket->lock_.get_lock();
        auto ret = upsert(head_bucket, key, value);
        // head_bucket->lock_.release_lock();
        return ret;
    }

    // use when hash_table evict a cool page
    // remove (key,val) entry; return false if (key,val) not match what is there
    bool remove(PageBucket *head_bucket, GlobalAddress key, void *value) {
        auto cur_bucket = head_bucket;
        PageBucket *prev_bucket = nullptr;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if ((cur_frame[i].page_id_.val == key.val) &&  (cur_frame[i].buffer_page_ == value)) {
                    cur_frame[i].buffer_page_ = nullptr;
                    cur_bucket->count_--;
                    if (cur_bucket->count_ == 0 && prev_bucket != nullptr) {
                        prev_bucket->next_ = cur_bucket->next_;
                        free(cur_bucket);
                    }
                    return true;
                }
            }
            prev_bucket = cur_bucket;
            cur_bucket = cur_bucket->next_;
        }
        return false;
    }

    // TODO: no lock for now
    bool remove_with_lock(GlobalAddress key, void *value) {
        auto head_bucket = get_bucket(key);
        // head_bucket->lock_.get_lock();
        auto ret = remove(head_bucket, key, value);
        // head_bucket->lock_.release_lock();
        return ret;
    }

    bool remove(PageBucket *head_bucket, GlobalAddress key) {
        auto cur_bucket = head_bucket;
        PageBucket *prev_bucket = nullptr;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if (cur_frame[i].page_id_.val == key.val) {
                    cur_frame[i].buffer_page_ = nullptr;
                    cur_bucket->count_--;
                    if (cur_bucket->count_ == 0 && prev_bucket != nullptr) {
                        prev_bucket->next_ = cur_bucket->next_;
                        free(cur_bucket);
                    }
                    return true;
                }
            }
            prev_bucket = cur_bucket;
            cur_bucket = cur_bucket->next_;
        }

        return false;
    }

    // TODO: is using get instead of get_with_lock, reverse to get_with_lock later for concurrency
    bool check_and_remove(GlobalAddress key) {
        auto head_bucket = get_bucket(key);
        // auto page_ptr = get_with_lock(head_bucket, key);
        auto page_ptr = get(head_bucket, key);
        if (page_ptr == nullptr)
            return false;

        // change the way of locking
        // head_bucket->lock_.get_lock();
        auto ret = remove(head_bucket, key);
        // head_bucket->lock_.release_lock();
        return ret;
    }

    // return cache_lot if found, null_ptr if not
    void *get(PageBucket *head_bucket, GlobalAddress key) {
        auto cur_bucket = head_bucket;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) 
                if ((cur_frame[i].page_id_.val == key.val) && (cur_frame[i].buffer_page_ != nullptr)) 
                    return cur_frame[i].buffer_page_; 
            cur_bucket = cur_bucket->next_;
        }
        return nullptr;
    }

    /* Not use for now - TODO: add for concurrency later
    void *get_with_lock(GlobalAddress key) {
        auto head_bucket = get_bucket(key);
        // Below is the lock-based version
        // head_bucket->lock_.get_lock();
        // auto ret = get(head_bucket, key);
        // head_bucket->lock_.release_lock();
        // return ret;
        void *ret = nullptr;
        while (true) {
        uint32_t version = 0;
        auto flag = head_bucket->lock_.test_lock_set(version);
        if (flag)
            continue;
        ret = get(head_bucket, key);
        flag = head_bucket->lock_.test_lock_version_change(version);
        if (flag)
            continue;
        break;
        }
        return ret;
    }

    void *get_with_lock(page_bucket *head_bucket, GlobalAddress key) {
        void *ret = nullptr;
        while (true) {
        uint32_t version = 0;
        auto flag = head_bucket->lock_.test_lock_set(version);
        if (flag)
            continue;
        ret = get(head_bucket, key);
        flag = head_bucket->lock_.test_lock_version_change(version);
        if (flag)
            continue;
        break;
        }
        return ret;
    }
    */

    bool update(PageBucket *head_bucket, GlobalAddress key, void *new_value, void **old_value) {
        auto cur_bucket = head_bucket;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if ((cur_frame[i].page_id_.val == key.val) && (cur_frame[i].buffer_page_ != nullptr)) {
                    *old_value = cur_frame[i].buffer_page_;
                    cur_frame[i].buffer_page_ = new_value;
                    auto target_node = reinterpret_cast<NodeBase *>(new_value);
                    if (target_node->remote_address != key) {
                        std::cout << "New node's remote addr is not what we want!!!"  << std::endl;
                        while (true) ;
                    }
                    assert(target_node->remote_address == key);
                    return true;
                }
            }
            cur_bucket = cur_bucket->next_;
        }
        return false;
    }
    // TODO: add lock later
    bool update_with_lock(GlobalAddress key, void *new_value, void **old_value) {
        auto head_bucket = get_bucket(key);
        // head_bucket->lock_.get_lock();
        auto ret = update(head_bucket, key, new_value, old_value);
        // head_bucket->lock_.release_lock();
        return ret;
    }

    bool RMW(PageBucket *head_bucket, GlobalAddress key, void *new_value, void *old_value) {
        auto cur_bucket = head_bucket;
        while (cur_bucket) {
            auto cur_frame = cur_bucket->page_frame_;
            for (int i = 0; i < num_entry_in_page_bucket; ++i) {
                if ((cur_frame[i].page_id_.val == key.val) && (cur_frame[i].buffer_page_ == old_value)) {
                    cur_frame[i].buffer_page_ = new_value;
                    return true;
                }
            }
            cur_bucket = cur_bucket->next_;
        }
        return false;
    }

    // only update the value when the old value matches
    // TODO: add lock later
    bool RMW_with_lock(GlobalAddress key, void *new_value, void *old_value) {
        auto head_bucket = get_bucket(key);
        auto ret = RMW(head_bucket, key, new_value, old_value);
        return ret;
    }
};

} // namespace cachepush
