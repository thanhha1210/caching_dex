#pragma once

/* This cache management is specialized for B+-Tree, implement the idea of LeanStore */

#include <atomic>
#include <list>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <inttypes.h>

#include "../tree/HashTable.h"
#include "BtreeNode.h"
#include "../Types.h"
#include "../../util/CacheAllocator.h"

#include "NodeWR.h"


namespace cachepush {
// FIXME(BT): this variable is better to be included in the cache container
thread_local static std::vector<void *> local_page_set;

// FIXME(BT): need a sensitive study for the following parameters
int probing_length = 8;
int num_pages_to_sample = 2;
#define CONCURRENT 1
uint64_t sample_num = 0;
uint64_t total_sample_loop = 0;
uint64_t wrong_page_state = 0;
uint64_t wrong_iteration = 0;
uint64_t wrong_parent = 0;
uint64_t wrong_parent_lock = 0;
uint64_t wrong_parent_check = 0;
uint64_t wrong_evict = 0;
uint64_t one_state = 0;
uint64_t zero_state = 0;
uint64_t three_state = 0;

enum class RPC_type { LOOKUP, UPDATE, INSERT, DELETE };

class CacheManager {
public:
    // Core Data Structure 
    uint64_t capacity;      // Number of pages used in the cache of CPU node
    std::atomic<int> state; // 0 means warm-up phase, also use the page from allocator
                            // 1 means dynamic phase

    // Below is cache statistic 
    uint64_t inner_miss_ = 0;     // inner read miss
    uint64_t leaf_miss_ = 0;      // leaf read miss
    uint64_t full_page_miss_ = 0; // full read miss
    uint64_t rdma_write = 0;

    // NOT Concurrent hash table (cooling_ratio * capacity entry) hash table 
    // of (remote_address -> slot ptr) for pages has pos_state = 1
    HashTable *hash_table_;

    // NOT Concurrent page table - (capacity entry) hash table  
    // Map backend address currently in any slot(hot/ cold) to slot pointer
    PageTable *page_table_;  

    // Limit information
    Key buffer_min_limit_;
    Key buffer_max_limit_;

    GlobalAddress *root_ptr_;

    double rpc_rate_; // the ratio for pushdown
    double admission_rate_;

    CacheManager(Backend *backend, uint64_t cache_capacity, double cooling_ratio,
                 double rpc_rate, double admission_rate, GlobalAddress *root_ptr = nullptr)
        : capacity(cache_capacity), root_ptr_(root_ptr), admission_rate_(admission_rate) {
        // publish the backend so NodeWR.h's free-function read/write helpers
        // (and anyone else who reaches for global_backend_) can find it.
        assert(backend != nullptr);
        global_backend_ = backend;

        rpc_rate_ = std::max<double>(0, std::min<double>(rpc_rate, 0.99)); // no need RPC rate here, just keep as old
        std::cout << "Pushdown (RPC) Rate: " << rpc_rate_ << std::endl;
        std::cout << "Admission Rate: " << admission_rate_ << std::endl;
        state = 0;

        // buffer pool (cache_cap * PAGE_SIZE) chunk of mmap mem, each slot
        // 1KB & can hold NodeBase page
        CacheAllocator::initialize(PAGE_SIZE, cache_capacity * PAGE_SIZE);

        page_table_ = new PageTable(cache_capacity);
        hash_table_ = new HashTable(cache_capacity * cooling_ratio, page_table_);
    }

    // No need these 3 for now
    double get_rpc_ratio() { return 0; }    
    void set_rpc_ratio(double ratio) { rpc_rate_ = ratio; }
    void set_admission_ratio(double ratio) { admission_rate_ = ratio; }

//--- Cache management ---
    // return the swizzled pointer, or nullptr using swizzled bit
    NodeBase *search_in_cache(GlobalAddress global_node) {
        uint64_t node = global_node.val;
        if (node & SWIZZLE_TAG) // have in cache
            return reinterpret_cast<NodeBase *>(node & SWIZZLE_HIDE);
        return nullptr;
    }

    bool is_in_cache(GlobalAddress global_node) {
        uint64_t node = global_node.val;
        if (node & SWIZZLE_TAG) 
            return true;
        return false;
    }

    // insert into cache
    NodeBase *cache_insert(GlobalAddress &global_node, NodeBase *cache_node,
                            NodeBase *parent_ptr, bool add_to_pt = true) {
        void *page = get_empty_page();
        memcpy(reinterpret_cast<char *>(page), reinterpret_cast<char *>(cache_node), PAGE_SIZE);
        auto return_page = reinterpret_cast<NodeBase *>(page);
        return_page->pos_state = 4; // means this page can not be sampled
        return_page->parent_ptr = parent_ptr;
        GlobalAddress snapshot = global_node;
        // FIXME(BT): no need to setup bitmap?
        // add node is swizzled
        global_node.val = reinterpret_cast<uint64_t>(page) | SWIZZLE_TAG;
        // assert(return_page->isLocked()); // TODO - uncomment for concurrent version
        if (add_to_pt) { // insert return page to page_table_
            page_table_->upsert_with_lock(snapshot, page);
        }
        return return_page;
    }

    // HOT => COOLING 
    // actual eviction policy when buffer pool is full & cool tier(HT) has nothing to evict
    void sample_page() {
        int counter = 0;
        bool verbose = false;
        while (true) {
            ++counter;
            if (counter >= 900) verbose = true;
            if (counter >= 1000) {
                exit(0);
                break;
            }
            
            auto page = reinterpret_cast<NodeBase *>(CacheAllocator::random_select());  // uniform rundom slot
            if (page->pos_state != 2) continue; // not hot -> skip

            int idx_in_parent = -1; 
            auto cur_page = recursive_iterate(page, idx_in_parent); //walk down via bitmap
            if (cur_page == nullptr)    // dead_end
                continue;
            auto parent = reinterpret_cast<BTreeInner<Key> *>(cur_page->parent_ptr);
            if (parent && parent->level != 255) {  // normal parent
                assert(cur_page->pos_state == 2);
                
                if (cur_page->parent_ptr != parent) 
                    continue;

                if (idx_in_parent == -1) 
                    idx_in_parent = parent->findIdx(reinterpret_cast<uint64_t>(cur_page) | SWIZZLE_TAG);

                if (idx_in_parent == -1) { // stale parent pointer (not use in this case since 1 thread only)
                    cur_page->parent_ptr = nullptr;
                    ++wrong_parent;
                    continue;
                }

                assert(idx_in_parent != -1);
                assert(check_parent_child_info(parent, cur_page));

                 
                parent->children[idx_in_parent].val = cur_page->remote_address.val;
                parent->unset_bitmap(idx_in_parent);
                cur_page->parent_ptr = nullptr;
                NodeBase *evict_page = nullptr;
                if (cur_page->dirty) // write back
                    remote_write(cur_page->remote_address, cur_page, true);
                
                hash_table_->insert(cur_page->remote_address.val,
                                    reinterpret_cast<void *>(cur_page),
                                    reinterpret_cast<void **>(&evict_page));
                if (evict_page != nullptr) 
                    insert_local_set(reinterpret_cast<uint64_t>(evict_page));
                return;
            } 
            else if (parent == nullptr) { // this was the root (super_root's child) -> no parent fix
                assert(cur_page->parent_ptr == nullptr);
                if (cur_page->dirty) 
                    remote_write(cur_page->remote_address, cur_page, true);
                NodeBase *evict_page = nullptr;
                hash_table_->insert(cur_page->remote_address.val,
                                    reinterpret_cast<void *>(cur_page),
                                    reinterpret_cast<void **>(&evict_page));
                if (evict_page != nullptr) 
                    insert_local_set(reinterpret_cast<uint64_t>(evict_page));
                return;
            }
        }
    }

    // HOT => COOLING
    void sample_multiple_pages(int count) {
        while (count) {
            sample_page(); // Need to write back the dirty data?? 
            --count;
        }
    }

// --- Mini/Full Page management 

    // TODO: add lock later for concurrency
    void insert_local_set(uint64_t addr) {
        auto node = reinterpret_cast<NodeBase *>(addr);
        // assert(node->isLocked()); 
        node->pos_state = 3;
        local_page_set.push_back(reinterpret_cast<void *>(addr));
    }

    void *get_local_page_set() {
        if (local_page_set.empty()) 
            return nullptr;
        auto ret = local_page_set.back();
        local_page_set.pop_back();
        return ret;
    }

    // Strategy to get the empty page from buffer pool
    void *try_get_empty_page() {
        void *page = nullptr;
        while (true) {
            if (state == 1) {   // dynamic -> pool is full, recycle
                if (!local_page_set.empty()) {
                    page = get_local_page_set();
                    break;
                } 
                else { // evict from the cooling table
                    int ret = hash_table_->random_evict_to_remote(&page, probing_length);
                    if (ret == -1) 
                        sample_multiple_pages(num_pages_to_sample);             
                    if (page == nullptr && (!local_page_set.empty())) {
                        page = get_local_page_set();
                        assert(page != nullptr);
                    }
                    break;
                }
            } 
            else { // state == 0 -> warm up - pool has free slot
                bool last_page_flag = false;
                page = CacheAllocator::allocate(last_page_flag);
                // If this is the last page -> inc the state of the buffer pool
                if (last_page_flag) {
                    std::cout << "entering dynamic phase" << std::endl;
                    state.store(1);
                }
                if (page != nullptr) {
                    // dex called setLockState() here to atomically take the
                    // page's OptLock; single-threaded there's nothing to lock.
                    break;
                }
            }
        }

        // Not use lock for now, add later
        // if (page != nullptr) 
        //     assert(reinterpret_cast<NodeBase *>(page)->isLocked());
        return page;
    }

    void *get_empty_page() {
        void *page = nullptr;
        while (true) {
            page = try_get_empty_page();
            if (page != nullptr)
                break;
            }
        return page;
    }

    void swizzling(GlobalAddress &global_addr, NodeBase *parent, unsigned child_idx, NodeBase *child) {
        if (parent) {
            if (!check_parent_child_info(parent, child)) {
                std::cout << "Global addr = " << global_addr << std::endl;
                auto new_child = raw_remote_read(child->remote_address);
                assert(check_parent_child_info(parent, child));
            }
        }

        child->parent_ptr = parent;
        child->pos_state = 2;
        global_addr.val = reinterpret_cast<uint64_t>(child) | SWIZZLE_TAG;
        if (parent) {
            auto inner_parent = reinterpret_cast<BTreeInner<Key> *>(parent);
            inner_parent->set_bitmap(child_idx);
            if (inner_parent->level == 255) {
                inner_parent->children[0].val = reinterpret_cast<uint64_t>(child) | SWIZZLE_TAG;
            }
            assert(check_parent_child_info(parent, child));
        }
    }

    void unswizzling(GlobalAddress &global_addr, NodeBase *parent, unsigned child_idx, NodeBase *child) {
        auto inner_parent = reinterpret_cast<BTreeInner<Key> *>(parent);
        inner_parent->unset_bitmap(child_idx);
        global_addr.val = child->remote_address.val;
        child->parent_ptr = nullptr;
    }

    bool fit_limits(NodeBase *cur_node) {
        if ((cur_node->min_limit_ >= buffer_max_limit_) || (cur_node->max_limit_ < buffer_min_limit_))
            return false;
        return true;
    }

    // Not use for now
    // To test whether the loaded node belongs to this computing node 
    bool sync_or_not(BTreeInner<Key> *inner, uint64_t idx) {
        if (inner->level == 255)
            return true;
        Key min_limit = (idx == 0) ? inner->min_limit_ : inner->keys[idx - 1];
        Key max_limit =  (idx == inner->count) ? inner->max_limit_ : inner->keys[idx];
        if (min_limit >= buffer_min_limit_ && max_limit < buffer_max_limit_) 
            return false;
        return true;
    }

    // Not use now
    void opportunistic_sample() {
        if (state == 1 && local_page_set.empty()) { // start sample (hot -> cold)
            sample_multiple_pages(num_pages_to_sample);
        }
    }

    inline void fill_local_page_set() {
        if (local_page_set.empty()) {
            auto new_page = get_empty_page();
            insert_local_set(reinterpret_cast<uint64_t>(new_page));
        }
    }

    // It relies on the pointer swizzling information
    NodeBase *cache_get(GlobalAddress node, NodeBase *parent, unsigned child_idx,
                        bool &restart, bool &refresh, bool IO_enable) {
        // Start the search in page table
        auto head_page_bucket = page_table_->get_bucket(node);
        // head_page_bucket->lock_.get_lock();
        
        /* TODO: no lock for now -> remove this, change later
            bool lock_success = head_page_bucket->lock_.try_get_lock();
            if (!lock_success) {
            restart = true;
            return nullptr;
            }
        */
        auto target_page = page_table_->get(head_page_bucket, node);
        // 1.0 Cold => Hot
        // cache miss
        if (target_page == nullptr) {
            page_table_->insert_io_flag(head_page_bucket, node); // TODO: Add IO flag
            // head_page_bucket->lock_.release_lock();
            if (!IO_enable)
                return nullptr;
            restart = true;
            NodeBase *return_page = nullptr;
            cold_to_hot(node, reinterpret_cast<void **>(&return_page), parent, child_idx, refresh);
            return return_page;
        } 
        
        // 2.0 Cool => hot or Hot => hot
        // Get the exclusive lock of this node and then get the exclusive lock of this parent
        // TODO: change to lock version later, for now just no lock
        /* // whole code of this sec is locked version
        auto target_node = reinterpret_cast<NodeBase *>(target_page);
        bool exclusive_success =
            get_exclusive_node(GlobalAddress(node), target_node);
        if (!exclusive_success) {
        restart = true;
        head_page_bucket->lock_.release_lock();
        return nullptr;
        }
        assert(target_node->isLocked());

        // Using range to check
        if (!new_check_limit_match(parent, target_node, child_idx)) {
        assert(parent->isShared() || target_node->isShared());
        if (target_node->isShared()) {
            // Check whether it is outdated
            auto remote_target_node = reinterpret_cast<BTreeInner<Key> *>(
                raw_remote_read(target_node->remote_address));
            check_global_conflict(remote_target_node, target_node->front_version,
                                restart);
            if (restart) {
            target_node->obsolete = true;
            // remove the outdated page from the page table
            auto flag = page_table_->remove(head_page_bucket, node, target_page);
            assert(flag == true);
            head_page_bucket->lock_.release_lock();
            target_node->pos_state = 2;
            target_node->writeUnlock();
            refresh = true;
            return nullptr;
            }
        }

        if (parent->isShared()) {
            auto remote_parent_node = reinterpret_cast<BTreeInner<Key> *>(
                raw_remote_read(parent->remote_address));
            check_global_conflict(remote_parent_node, parent->front_version,
                                restart);
            if (restart) {
            // std::cout << "Refresh because parent is not obsolete" << std::endl;
            head_page_bucket->lock_.release_lock();
            target_node->pos_state = 2;
            target_node->writeUnlock();
            refresh = true;
            return nullptr;
            }
        }
        }
        // Do the swizzling
        head_page_bucket->lock_.release_lock();
        return target_node;
    }
        */

        // cache hit
        auto target_node = reinterpret_cast<NodeBase *>(target_page);
        // optional for range validation
        if (!new_check_limit_match(parent, target_node, child_idx)) {
            target_node->obsolete = true;
            page_table_->remove(head_page_bucket, node, target_page);
            refresh = true;
            return nullptr;
        }
        return target_node;
    }
    
    // -1 means failure and retry
    // 0 means cold to hot succeeds
    // 1 means one-sided update succeeds
    int cold_to_hot_with_admission(GlobalAddress global_node, void **ret_page, NodeBase *parent, unsigned child_idx,
                                    bool &refresh, Key k, Value &result, bool &success, RPC_type rpc_type) {
        static thread_local std::mt19937 *generator = nullptr;
        if (!generator)
            generator = new std::mt19937(clock() + pthread_self());
        static thread_local std::uniform_int_distribution<uint64_t> distribution(0, 9999);
        auto idx = distribution(*generator);
        uint64_t admission_idx = 10000 * admission_rate_;
        if (state == 1 && idx >= admission_idx) {
        // Just read from remote and return to the application
        int ret = 1;
        switch (rpc_type) {
        case RPC_type::LOOKUP: {
            auto buffer_page = raw_remote_read(global_node);
            auto cur_leaf = reinterpret_cast<BTreeLeaf<Key, Value> *>(buffer_page);
            if (!cur_leaf->rangeValid(k)) 
                ret = -1;
            else 
                success = cur_leaf->find(k, result);
            break;
        }

        case RPC_type::UPDATE: {
            auto buffer_page = raw_remote_read(global_node);
            auto cur_leaf = reinterpret_cast<BTreeLeaf<Key, Value> *>(buffer_page);
            if (!cur_leaf->rangeValid(k)) 
                ret = -1;
            else {
                success = cur_leaf->update(k, result);
                if (success)
                    remote_write(global_node, buffer_page, true, true);
            }
            break;
        }

        case RPC_type::INSERT: {
            auto buffer_page = raw_remote_read(global_node);
            auto cur_leaf = reinterpret_cast<BTreeLeaf<Key, Value> *>(buffer_page);
            if ((!cur_leaf->rangeValid(k)) || (cur_leaf->count == cur_leaf->max_entries)) 
                ret = -1;
            else {
                success = cur_leaf->insert(k, result);
                remote_write(global_node, buffer_page, true, true);
            }
            break;
        }

        case RPC_type::DELETE: {
            auto buffer_page = raw_remote_read(global_node);
            auto cur_leaf = reinterpret_cast<BTreeLeaf<Key, Value> *>(buffer_page);
            if (!cur_leaf->rangeValid(k)) 
                ret = -1;
            else {
                success = cur_leaf->remove(k);
                if (success)
                    remote_write(global_node, buffer_page, true, true);
            }
            break;
        }

        default:
            ret = -1;
            break;
        }
        bool flag = page_table_->remove_with_lock(global_node, reinterpret_cast<void *>(IO_FLAG));
        assert(flag == true);
        return ret;
        }

        return cold_to_hot(global_node, ret_page, parent, child_idx, refresh);
    }

    // -1 means failure and retry
    // 0 means cold to hot succeeds
    // 1 means one-sided update succeeds
    int cold_to_hot_with_admission_for_scan(GlobalAddress global_node, void **ret_page, NodeBase *parent,
                                            unsigned child_idx, bool &refresh, Key k,
                                            std::pair<Key, Value> *&kv_buffer, int &scan_num, Key &max_key) {
        static thread_local std::mt19937 *generator = nullptr;
        if (!generator)
            generator = new std::mt19937(clock() + pthread_self());
        static thread_local std::uniform_int_distribution<uint64_t> distribution( 0, 9999);
        auto idx = distribution(*generator);
        uint64_t admission_idx = 10000 * admission_rate_;
        if (idx >= admission_idx) {
            // Just read from remote and return to the application
            int ret = 1;
            auto buffer_page = raw_remote_read(global_node);
            auto cur_leaf = reinterpret_cast<BTreeLeaf<Key, Value> *>(buffer_page);
            if (!cur_leaf->rangeValid(k)) 
                ret = -1; 
            else {
                scan_num = cur_leaf->rangeScan(k, scan_num, kv_buffer);
                max_key = cur_leaf->max_limit_;
            }

            bool flag = page_table_->remove_with_lock(global_node, reinterpret_cast<void *>(IO_FLAG));
            assert(flag == true);
            return ret;
        }
        return cold_to_hot(global_node, ret_page, parent, child_idx, refresh);
    }

    // -1 means failure and retry, 0 means cold to hot succeeds.
    // RPC pushdown is not supported in the local-memory build (no DSM to push
    // operations across); this stub keeps the API stable for a future BTree
    // driver and just delegates to cold_to_hot.
    int cold_to_hot_with_rpc(GlobalAddress global_node, void **ret_page,
                             NodeBase *parent, unsigned child_idx, bool &refresh,
                             Key k, Value &result, bool &success,
                             RPC_type rpc_type) {
        (void)k; (void)result; (void)success; (void)rpc_type;
        return cold_to_hot(global_node, ret_page, parent, child_idx, refresh);
    }

    // -1 means failure and retry
    // 0 means cold to hot succeeds
    int cold_to_hot(GlobalAddress global_node, void **ret_page, NodeBase *parent,
                    unsigned child_idx, bool &refresh) {
        // No sync read is needed because leaf nodes are exclusive
        bool sync_read = sync_or_not(reinterpret_cast<BTreeInner<Key> *>(parent), child_idx);
        auto ret = simple_cold_to_hot(global_node, ret_page, parent, child_idx, refresh, sync_read);
        if (ret == 0) {
            void *old_flag = nullptr;
            auto flag = page_table_->update_with_lock(global_node, reinterpret_cast<void *>(*ret_page), &old_flag);
            assert(flag == true);
            assert(reinterpret_cast<uint64_t>(old_flag) == IO_FLAG);
        } 
        else {
            auto flag = page_table_->remove_with_lock(global_node, reinterpret_cast<void *>(IO_FLAG));
            assert(flag == true);
        }
        return ret;
    }

    // TODO: add lock later
    bool get_exclusive_node(GlobalAddress node, NodeBase *target_node) {
        if (target_node->pos_state == 1) {
            // How to enfore that page table is indexing the up-to-date page?
            auto promote_succes = hash_table_->try_promote_using_value(node.val, target_node);
            return promote_succes;
        } 

        else if (target_node->pos_state == 2) {
            // bool needRestart = false;
            // target_node->writeLockOrRestart(needRestart);
            // if (needRestart) {
            //     return false;
            // }

            assert(target_node->pos_state == 2);
            auto target_parent = reinterpret_cast<BTreeInner<Key> *>(target_node->parent_ptr);
            if (target_parent != nullptr) {
                assert(target_parent->pos_state == 2);
                // target_parent->writeLockOrRestart(needRestart);
                // if (needRestart) {
                //     target_node->writeUnlock();
                //     return false;
                // }

                // The following may happen because of SMO
                // if (target_node->parent_ptr != target_parent) {
                //     target_parent->writeUnlock();
                //     target_node->writeUnlock();
                //     return false;
                // }

                // assert(check_parent_child_info(target_parent, target_node));
                if (!check_parent_child_info(target_parent, target_node)) {
                    std::cout << "There is a BUGGGGGGGGGG!!!!!!" << std::endl;
                    while (true);
                }

                auto idx_in_parent = target_parent->findIdx(
                                        reinterpret_cast<uint64_t>(target_node) | SWIZZLE_TAG);
                assert(idx_in_parent != -1);
                unswizzling(target_parent->children[idx_in_parent], target_parent, idx_in_parent, target_node);
                // target_parent->writeUnlock();
            }
            // assert(target_node->isLocked());
            return true;
        }

        return false;
    }

    // TODO: add lock later
    void replace_child(BTreeInner<Key> *parent, NodeBase *cur_node, NodeBase *new_node) {
        int idx_in_parent = -1;
        if (parent->level == 255) 
            idx_in_parent = 0;
        else 
            idx_in_parent = parent->findIdx(reinterpret_cast<uint64_t>(cur_node) | SWIZZLE_TAG);
        
        assert(idx_in_parent != -1);
        cur_node->parent_ptr = nullptr;
        new_node->parent_ptr = parent;
        parent->children[idx_in_parent].val = reinterpret_cast<uint64_t>(new_node) | SWIZZLE_TAG;
    }

    // Lock of cur_node and parent have been acquired, remote_cur_node is in read buffer
    // TODO: add lock later
    void updatest_replace(NodeBase *cur_node, BTreeInner<Key> *parent, NodeBase *remote_cur_node) {
        fill_local_page_set();
        GlobalAddress remote_addr = cur_node->remote_address;
        auto head_page_bucket = page_table_->get_bucket(remote_addr);
        // head_page_bucket->lock_.get_lock();
        auto target_page = page_table_->get(head_page_bucket, remote_addr);
        auto target_node = reinterpret_cast<NodeBase *>(target_page);

        // Case 1
        if (target_node == nullptr) {
        // This case may happen if the up-to-date cur_node has been evicted from the buffer pool
            NodeBase *new_cur_node = reinterpret_cast<NodeBase *>(get_local_page_set());
            assert(new_cur_node != nullptr);
            buffer_to_cache(new_cur_node, remote_cur_node);
            replace_child(parent, cur_node, new_cur_node);
            auto mem_addr = reinterpret_cast<void *>(new_cur_node);
            auto insert_success = page_table_->insert(head_page_bucket, remote_addr, mem_addr);
            assert(insert_success == true);
            // Check parent child relation
            assert(check_parent_child_info(parent, new_cur_node));
            new_cur_node->pos_state = 2;
            // new_cur_node->writeUnlock();
        } 

        else {
            assert(target_node == cur_node);
            // Directly do the replacement
            NodeBase *new_cur_node =reinterpret_cast<NodeBase *>(get_local_page_set());
            assert(new_cur_node != nullptr);
            buffer_to_cache(new_cur_node, remote_cur_node);
            replace_child(parent, cur_node, new_cur_node);
            auto mem_addr = reinterpret_cast<void *>(new_cur_node);
            void *old_val = nullptr;
            auto update_success = page_table_->update(head_page_bucket, remote_addr, mem_addr, &old_val);
            assert(update_success == true);
            assert(old_val == target_page);
            cur_node->obsolete = true;
            assert(check_parent_child_info(parent, new_cur_node));
            new_cur_node->pos_state = 2;
        }
        
        // else if (target_node->front_version > cur_node->front_version) {
        // // FIXME(BT): this should never happen?
        // // cur_node is not up_to_date "cur_node", so we should first move the
        // // target node to make it attach to the parent
        // auto success = get_exclusive_node(remote_addr, target_node);
        // if (success) {
        //     // if (!check_limit_match(parent, target_node)) {
        //     if (target_node->min_limit_ == cur_node->min_limit_ &&
        //         target_node->max_limit_ == cur_node->max_limit_) {
        //     replace_child(parent, cur_node, target_node);
        //     assert(check_parent_child_info(parent, target_node));
        //     }
        //     target_node->pos_state = 2;
        //     target_node->writeUnlock();
        // }
        // }
        // head_page_bucket->lock_.release_lock();
        return;
    }

    bool remote_to_cache(void *cache_page, GlobalAddress global_node, bool sync_read) {
        NodeBase *buffer_page = nullptr;
        if (sync_read) {
            buffer_page = opt_remote_read(global_node);
            if (buffer_page == nullptr)
                return false;
        } 
        else 
            buffer_page = raw_remote_read(global_node);
        
        memcpy(reinterpret_cast<char *>(cache_page),
            reinterpret_cast<char *>(buffer_page), PAGE_SIZE);
        return true;
    }
    // TODO: add lock later
    void buffer_to_cache(NodeBase *cache_page, NodeBase *cur_node) {
        memcpy(reinterpret_cast<char *>(cache_page),
            reinterpret_cast<char *>(cur_node), PAGE_SIZE);
        // assert(cache_page->isLocked());
        assert(cache_page->pos_state != 2);
    }

    // -1 means failure and retry, 0 means succeeds
    // TODO: add lock later
    int simple_cold_to_hot(GlobalAddress node, void **ret_page, NodeBase *parent,
                            unsigned child_idx, bool &refresh, bool sync_read) {
        void *page = try_get_empty_page();
        if (page == nullptr) 
            return -1;
        assert(reinterpret_cast<NodeBase *>(page)->pos_state != 2);
        
        bool IO_success = remote_to_cache(page, node, sync_read);
        auto return_page = reinterpret_cast<NodeBase *>(page);
        assert(parent->level != 255);
        if ((!IO_success) || (!fit_limits(return_page)) || (!new_check_limit_match(parent, return_page, child_idx))) {
            insert_local_set(reinterpret_cast<uint64_t>(return_page));
            refresh = true;
            return -1;
        }

        if (return_page->remote_address != node) {
            std::cout << "Remote node is incorrect when reading it" << std::endl;
            while (true);
        }

        assert(return_page->pos_state == 0);
        // assert(return_page->isLocked());
        assert(return_page->parent_ptr == nullptr);
        *ret_page = return_page;
        return 0;
    }

    void reset(bool flush_dirty) {
        if (flush_dirty)
            flush_all();
        state.store(0);
        inner_miss_ = 0;
        leaf_miss_ = 0;
        full_page_miss_ = 0;
        rdma_write = 0;

        CacheAllocator::reset();
        hash_table_->reset();
        page_table_->reset();
    }

    void flush_all() {
        inner_miss_ = 0;
        leaf_miss_ = 0;
        // Flush where are dirty in allocator
        uint64_t start = CacheAllocator::instance_->base_address;
        uint64_t page_num = CacheAllocator::instance_->page_num_;

        for (uint64_t i = 0; i < page_num; ++i) {
            auto cur_page = reinterpret_cast<NodeBase *>(start + i * PAGE_SIZE);
            // First fully unswizzle its children
            if (cur_page->dirty) {
                auto page_buffer = get_scratch_page();
                memcpy(page_buffer, cur_page, PAGE_SIZE);
                auto buffer_page = reinterpret_cast<NodeBase *>(page_buffer);
                fully_unswizzle(buffer_page);
                remote_write(buffer_page->remote_address, buffer_page, true);
                cur_page->dirty = false;
            }
        }
    }

    void check_dirty_in_buffer() {
        uint64_t start = CacheAllocator::instance_->base_address;
        uint64_t page_num = CacheAllocator::instance_->page_num_;
        uint64_t dirty_page = 0;

        for (uint64_t i = 0; i < page_num; ++i) {
            auto cur_page = reinterpret_cast<NodeBase *>(start + i * PAGE_SIZE);
            if (cur_page->dirty) 
                dirty_page++; 
        }
        std::cout << "The buffer pool has " << dirty_page << " dirty pages" << std::endl;
    }

    void statistic_in_buffer() {
        uint64_t start = CacheAllocator::instance_->base_address;
        uint64_t page_num = CacheAllocator::instance_->page_num_;
        int remote_state = 0;
        int cooling_state = 0;
        int hot_state = 0;
        int local_work_page = 0;
        int hot_leaf = 0;
        int hot_inner = 0;
        int cooling_leaf = 0;
        int cooling_inner = 0;
        int hot_mini = 0;
        int cooling_mini = 0;
        int local_mini = 0;
        uint64_t hot_mini_records = 0;
        uint64_t cooling_mini_records = 0;

        for (uint64_t i = 0; i < page_num * 2; ++i) {
        auto cur_page = reinterpret_cast<NodeBase *>(start + i * PAGE_SIZE / 2);
        switch (cur_page->pos_state) {
        case 0:
            remote_state++;
            break;
        case 1:
            cooling_state++;
            if (cur_page->type == PageType::BTreeInner) {
                cooling_inner++;
                ++i;
            } 
            else if (cur_page->type == PageType::BTreeLeaf) {
                cooling_leaf++;
                ++i;
            } 
            else {
                cooling_mini++;
                cooling_mini_records += cur_page->count;
            }
            break;
        case 2:
            hot_state++;
            if (cur_page->type == PageType::BTreeInner) {
                hot_inner++;
                ++i;
            } 
            else if (cur_page->type == PageType::BTreeLeaf) {
                hot_leaf++;
                ++i;
            } 
            else {
                hot_mini++;
                hot_mini_records += cur_page->count;
            }
            break;
        case 3:
            local_work_page++;
            ++i;
        default:
            break;
        }
        }
        std::cout << "#hot inner = " << hot_inner << std::endl;
        std::cout << "#hot leaf = " << hot_leaf << std::endl;
        std::cout << "#hot mini = " << hot_mini << std::endl;

        std::cout << "remote_state: " << remote_state << std::endl;
        std::cout << "cooling_state: " << cooling_state << std::endl;
        std::cout << "hot_state: " << hot_state << std::endl;
        std::cout << "local_work_page: " << local_work_page << std::endl;
        std::cout << "local_mini_page: " << local_mini << std::endl;

        std::cout << "hot leaf/mini ratio = "
                  << static_cast<double>(hot_leaf + hot_mini / 2) / static_cast<double>(page_num)
                  << std::endl;
        std::cout << "#cooling inner = " << cooling_inner << std::endl;
        std::cout << "#cooling leaf = " << cooling_leaf << std::endl;
        std::cout << "#cooling mini = " << cooling_mini << std::endl;
        std::cout << "cooling leaf ratio = "
                  << static_cast<double>(cooling_leaf + cooling_mini / 2) / static_cast<double>(page_num)
                  << std::endl;
    }
};

} // namespace cachepush
