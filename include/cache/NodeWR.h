#pragma once

/* Node read/write helpers for the cache layer.
 *
 * Replaces the original DSM-backed implementation with the local-memory
 * Backend abstraction 
 * 
 */

#include <cassert>
#include <cstring>
#include <iostream>

#include "../GlobalAddress.h"
#include "../Types.h"
#include "../memory/Backend.h"
#include "BtreeNode.h"

namespace cachepush {

// change to DSM later
inline Backend *global_backend_ = nullptr; // set by CacheManager

// --- Scratch page buffers ---
// Per-thread scratch buffer, they are distinct since 
// single op can use both read/write of the concurrently

inline char *get_scratch_page() {
    thread_local char buffer[PAGE_SIZE];
    return buffer;
}

inline char *get_sibling_scratch_page() {
    thread_local char buffer[PAGE_SIZE];
    return buffer;
}

inline char *get_write_scratch_page() {
    thread_local char buffer[PAGE_SIZE];
    return buffer;
}


//--- Backend IO ---
inline char *raw_remote_read(GlobalAddress global_node, size_t read_size) {
    char *page_buffer = get_scratch_page();
    global_backend_->read(global_node, page_buffer, read_size);
    return page_buffer;
}

inline NodeBase *raw_remote_read(GlobalAddress global_node) {
    char *page_buffer = get_scratch_page();
    global_backend_->read(global_node, page_buffer, PAGE_SIZE);
    return reinterpret_cast<NodeBase *>(page_buffer);
}

// in single-threaded in-memory mode => just raw read for now
// kept for API (for RDMA change later)
inline NodeBase *opt_remote_read(GlobalAddress global_node) {
    return raw_remote_read(global_node);
}

inline NodeBase *remote_consistent_read(GlobalAddress global_node) {
    return raw_remote_read(global_node);
}

inline void remote_write(GlobalAddress global_node, NodeBase *mem_node,
                         bool clear_lock = true, bool without_copy = false) {
    (void)clear_lock; // no lock bits on the non-concurrent NodeBase
    char *page_buffer = nullptr;
    mem_node->dirty = false;

    if (without_copy) {
        page_buffer = reinterpret_cast<char *>(mem_node);
    } 
    else {
        // create buffer, write into iy
        page_buffer = get_write_scratch_page();
        if (page_buffer != reinterpret_cast<char *>(mem_node)) {
            std::memcpy(page_buffer, mem_node, PAGE_SIZE);
        }
    }

    auto flushed_page = reinterpret_cast<NodeBase *>(page_buffer);
    flushed_page->parent_ptr = nullptr;
    flushed_page->pos_state = 0;

    if (flushed_page->remote_address != global_node) {
        std::cout << "Remote write page addr is not consistent!!!" << std::endl;
        while (true) ;
    }

    global_backend_->write(global_node, page_buffer, PAGE_SIZE);
}


// --- Helpers ---
inline void get_node_statistic() {
    std::cout << "NodeBase size = " << sizeof(NodeBase) << std::endl;
    std::cout << "BTreeLeaf size = " << sizeof(BTreeLeaf<Key, Value>) << std::endl;
    std::cout << "BTreeInner size = " << sizeof(BTreeInner<Key>) << std::endl;
}

inline uint64_t murmur64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53L;
    k ^= k >> 33;
    return k;
}

inline void update_all_parent_ptr(NodeBase *mem_node) {
    if (mem_node->type == PageType::BTreeLeaf) return;

    auto inner = reinterpret_cast<BTreeInner<Key> *>(mem_node);
    // loop through child -> set parent_ptr to parent
    for (int i = 0; i < (inner->count + 1); ++i) {
        if (inner->children[i].val & SWIZZLE_TAG) {
            auto mem_child = reinterpret_cast<NodeBase *>(inner->children[i].val & SWIZZLE_HIDE);
            mem_child->parent_ptr = mem_node;
        }
    }
}

// walk the swizzled portion of the tree starting at mem_node, picking a child at each level randomly. 
// returns the deepest swizzled descendant whose pos_state is still HOT (2) 
// if can't find -> return nullptr
inline NodeBase *recursive_iterate_without_bitmap(NodeBase *mem_node, int &idx_in_parent) {
    while (mem_node->type == PageType::BTreeInner) {
        auto inner = static_cast<BTreeInner<Key> *>(mem_node);
        uint64_t hash_k = murmur64(reinterpret_cast<uint64_t>(mem_node));
        auto inner_count = inner->count;
        int start_idx = hash_k % (inner_count + 1);

        NodeBase *next_node = nullptr;
        for (int i = 0; i < inner_count + 1; i++) {
            int next_child_idx = (start_idx + i) % (inner_count + 1);
            auto node_val = inner->children[next_child_idx].val;
            if (node_val & SWIZZLE_TAG) {
                idx_in_parent = next_child_idx;
                next_node = reinterpret_cast<NodeBase *>(node_val & SWIZZLE_HIDE);
                break;
            }
        }
        if (next_node == nullptr || next_node == mem_node) 
            break;
        mem_node = next_node;
    }

    if (mem_node->pos_state != 2) 
        return nullptr;

    if (mem_node->type == PageType::BTreeInner) {
        auto inner = static_cast<BTreeInner<Key> *>(mem_node);
        for (int i = 0; i < inner->count + 1; ++i) {
            if (inner->children[i].val & SWIZZLE_TAG) 
                return nullptr;
        }
    }
    return mem_node;
}


// same as recursive_iterate_without_bitmap but uses the bitmap to find
// swizzled children. Lock/version checks dropped.
inline NodeBase *recursive_iterate(NodeBase *mem_node, int &idx_in_parent) {
    // ensure sample target always has bitmap == 0 (leaf/ inner has all child unswizzled)
    while (mem_node->bitmap != 0) {  // has swizzled child
        // pick a child via hash + closet_set_bit
        auto inner = static_cast<BTreeInner<Key> *>(mem_node);
        uint64_t hash_k = murmur64(reinterpret_cast<uint64_t>(mem_node));
        int idx = hash_k % (inner->count + 1); 

        int next_idx = inner->closest_set(idx); 
        if (next_idx == -1) 
            return nullptr;
        idx_in_parent = next_idx;

        auto next_val = inner->children[next_idx].val;
        assert((next_val & SWIZZLE_TAG) != 0);
        mem_node = reinterpret_cast<NodeBase *>(next_val & SWIZZLE_HIDE);
    } 
    
    if ((mem_node->bitmap != 0) || (mem_node->pos_state != 2)) 
        return nullptr;
    return mem_node;
}

inline void fully_unswizzle(NodeBase *mem_node) {
    if (mem_node->type == PageType::BTreeLeaf) return;

    auto inner = reinterpret_cast<BTreeInner<Key> *>(mem_node);
    for (int i = 0; i < inner->count + 1; ++i) {
        if (inner->children[i].val & SWIZZLE_TAG) {
            assert(inner->bitmap & (1ULL << i));
            auto child = reinterpret_cast<NodeBase *>(inner->children[i].val & SWIZZLE_HIDE);
            inner->children[i] = child->remote_address;
        }
    }
    inner->bitmap = 0;
}

// check whether the min/max limits match
inline bool check_limit_match(NodeBase *parent, NodeBase *child,
                              bool using_swizzle = false, bool verbose = false) {
    auto inner = reinterpret_cast<BTreeInner<Key> *>(parent);
    int idx = -1;
    if (using_swizzle)
        idx = inner->findIdx(reinterpret_cast<uint64_t>(child) | SWIZZLE_TAG);
    else 
        idx = inner->findIdx(child->remote_address.val);
    if (verbose) 
        std::cout << "The idx = " << idx << std::endl;
    if (idx == -1) 
        return false;

    Key min_limit = (idx == 0) ? inner->min_limit_ : inner->keys[idx - 1];
    Key max_limit = (idx == inner->count) ? inner->max_limit_ : inner->keys[idx];
    if (verbose) {
        std::cout << "Parent min limit = " << parent->min_limit_
                  << ", max limit = " << parent->max_limit_ << std::endl;
        std::cout << "Parent level = " << static_cast<int>(parent->level) << std::endl;
        std::cout << "Min limit in parent = " << min_limit
                  << ", max_limit in parent = " << max_limit << std::endl;
        std::cout << "Child level = " << static_cast<int>(child->level) << std::endl;
        std::cout << "child min limit = " << child->min_limit_
                  << ", max limit = " << child->max_limit_ << std::endl;
    }
    return (min_limit == child->min_limit_ && max_limit == child->max_limit_);
}

inline bool new_check_limit_match(NodeBase *parent, NodeBase *child, unsigned idx) {
    auto inner = reinterpret_cast<BTreeInner<Key> *>(parent);
    Key min_limit = (idx == 0) ? inner->min_limit_ : inner->keys[idx - 1];
    Key max_limit = (idx == inner->count) ? inner->max_limit_ : inner->keys[idx];
    return (min_limit == child->min_limit_ && max_limit == child->max_limit_);
}

inline bool check_parent_child_info(NodeBase *parent, NodeBase *child) {
    if (parent->level == 255) 
        return true;
    bool verbose = false;
    auto parent_level = static_cast<int>(parent->level);
    auto child_level = static_cast<int>(child->level);
    if (parent_level != child_level + 1) 
        verbose = true;
    if (child->min_limit_ < parent->min_limit_ || child->max_limit_ > parent->max_limit_) {
        verbose = true;
    }
    if (!(check_limit_match(parent, child, true) || check_limit_match(parent, child, false))) {
        verbose = true;
    }

    if (verbose) {
        if (child->parent_ptr == parent) 
            std::cout << "Parent is child's parent" << std::endl;
        else 
            std::cout << "Parent is not child's parent" << std::endl;
        
        std::cout << "Parent level = " << parent_level << std::endl;
        std::cout << "Child level = " << child_level << std::endl;
        std::cout << "parent min limit = " << parent->min_limit_
                  << "; parent max limit = " << parent->max_limit_ << std::endl;
        std::cout << "child min limit = " << child->min_limit_
                  << "; child max limit = " << child->max_limit_ << std::endl;
        
        if (parent->type == PageType::BTreeLeaf) 
            std::cout << "The parent is a leaf node." << std::endl;
        else if (parent->type == PageType::BTreeInner) 
            std::cout << "The parent is a inner node." << std::endl;
        
        if (child->type == PageType::BTreeLeaf) 
            std::cout << "The child is a leaf node." << std::endl;
        else if (child->type == PageType::BTreeInner) 
            std::cout << "The child is a inner node." << std::endl;
        
        std::cout << "1. --------------------------------" << std::endl;
        check_limit_match(parent, child, true, true);
        std::cout << "2. --------------------------------" << std::endl;
        check_limit_match(parent, child, false, true);
    }
    return !verbose;
}

} // namespace cachepush
