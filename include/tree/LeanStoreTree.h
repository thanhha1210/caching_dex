#pragma once

/* B+tree driver for caching_dex
 *
 * This doesn't apply DEX Leanstore tree:
 *   - OptLock acquire/release (single threaded)
 *   - RPC pushdown
 *   - Shared/private state
 *
 * Storage layer: Backend -> change to DSM later
 * Cache layer: CacheManager (LeanStore-style buffer pool with pointer swizzling)
 */

#include <cassert>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "../GlobalAddress.h"
#include "../Types.h"
#include "../TreeAPI.h"
#include "../cache/BtreeNode.h"
#include "../cache/LeanStoreCache.h"
#include "../cache/NodeWR.h"
#include "../memory/Backend.h"

namespace cachepush {

template <class K, class V>
class LeanStoreTree : public tree_api<K, V> {
public:
    Backend *backend_; // copy of pointer (use for alloc_leaf/inner)
    CacheManager cache; 
    BTreeInner<K> *super_root_; // level = 255 (in heap), identity as root_parent
    GlobalAddress root_; // backend address of current real root

    int height_;
    uint64_t leaf_num_;
    uint64_t inner_num_;

    LeanStoreTree(Backend *backend, uint64_t cache_capacity_pages, double cooling_ratio = 0.1)
        : backend_(backend), cache(backend, cache_capacity_pages, cooling_ratio, /*rpc_rate*/ 0.0, /*admission_rate*/ 1.0, &root_),
          height_(1), leaf_num_(1), inner_num_(0) {
        super_root_ = new BTreeInner<K>(255, GlobalAddress::Null());
        super_root_->pos_state = 2; // set to hot, but not in pool

        // allocate the initial root (a single empty leaf) in the backend.
        root_ = allocate_leaf();
        super_root_->children[0] = root_;
        super_root_->count = 0;

        // bring the root into cache
        (void)load_child(super_root_->children[0], super_root_, 0);
    }

    ~LeanStoreTree() {
        cache.flush_all(); // CacheManager::flush_all writes dirty pages back to backend
        delete super_root_;
    }

    // --- Backend Alloc Helper ---
    GlobalAddress allocate_leaf(K min_lim = std::numeric_limits<K>::min(),
                                K max_lim = std::numeric_limits<K>::max()) {
        auto addr = backend_->alloc(PAGE_SIZE);
        char buf[PAGE_SIZE];
        std::memset(buf, 0, PAGE_SIZE);
        auto leaf = new (buf) BTreeLeaf<K, V>(addr);
        leaf->min_limit_ = min_lim;
        leaf->max_limit_ = max_lim;
        backend_->write(addr, buf, PAGE_SIZE);
        return addr;
    }

    GlobalAddress allocate_inner(uint8_t level,
                                 K min_lim = std::numeric_limits<K>::min(),
                                 K max_lim = std::numeric_limits<K>::max()) {
        auto addr = backend_->alloc(PAGE_SIZE);
        char buf[PAGE_SIZE];
        std::memset(buf, 0, PAGE_SIZE);
        auto inner = new (buf) BTreeInner<K>(level, addr);
        inner->min_limit_ = min_lim;
        inner->max_limit_ = max_lim;
        backend_->write(addr, buf, PAGE_SIZE);
        return addr;
    }

    // --- Cache ---    
    // go from parent->children[child_idx]: 
    // if already swizzled -> return the cached pointer; 
    // otherwise -> load from backend, admit into cache, update parent's bitmap + the slot's swizzle tag.
    NodeBase *load_child(GlobalAddress &child_slot, NodeBase *parent, unsigned child_idx) {
        // in cache -> interpret as NodeBase
        if (child_slot.val & SWIZZLE_TAG) 
            return reinterpret_cast<NodeBase *>(child_slot.val & SWIZZLE_HIDE);
      
        // pin parent so sample_page won't choose & recycle its slot during cache_insert below.
        uint8_t prev_state = 0;
        bool pin = (parent && parent != super_root_);
        if (pin) { 
            prev_state = parent->pos_state; 
            parent->pos_state = 4; 
        }

        // not in cache -> load from backend (raw_remote_read uses global_backend_)
        NodeBase *buf = raw_remote_read(child_slot); 
        if (buf->type == PageType::BTreeLeaf) 
            ++cache.leaf_miss_;
        else 
            ++cache.inner_miss_;
        // put it into cache 
        NodeBase *cached = cache.cache_insert(child_slot, buf, parent);
        // cache_insert pins as pos_state=4, we change to hot so the sampler can later demote it
        cached->pos_state = 2;

        // Maintain parent's swizzle bitmap (super_root_ is level 255 has no real bitmap logic -> skip).
        if (parent && parent->level != 255 && parent->type == PageType::BTreeInner) 
            static_cast<BTreeInner<K> *>(parent)->set_bitmap(child_idx);

        // return to parent prev state
        if (pin) 
            parent->pos_state = prev_state;
        return cached;
    }

    static bool is_full(NodeBase *node) { 
        if (node->type == PageType::BTreeLeaf) 
            return static_cast<BTreeLeaf<K, V> *>(node)->isFull();
        return static_cast<BTreeInner<K> *>(node)->isFull();
    }

    // --- Split Helper ---
    // split child (which lives at parent->children[idx], currently swizzled and in cache). 
    // if parent is super_root_, a new real root is created via make_new_root.
    void split_child(BTreeInner<K> *parent, unsigned idx, NodeBase *child) {
        const bool parent_is_super = (parent == super_root_);
        
        if (child->type == PageType::BTreeLeaf) {
            auto leaf = static_cast<BTreeLeaf<K, V> *>(child);

            // allocate sibling in backend 
            auto sib_addr = backend_->alloc(PAGE_SIZE);
            char buf[PAGE_SIZE];
            std::memset(buf, 0, PAGE_SIZE);
            auto sib = new (buf) BTreeLeaf<K, V>(sib_addr);

            // move the upper half of the keys into the sibling, set sep.
            K sep;
            leaf->split(sep, sib);

            // persist sibling to backend (split() set its min/max).
            backend_->write(sib_addr, buf, PAGE_SIZE);
            leaf->dirty = true;
            ++leaf_num_;

            if (parent_is_super) {
                // the split leaf is currently the root.
                make_new_root(sep, leaf, sib_addr, /*level*/ 1);
            } 
            else {
                parent->insert(sep, sib_addr);
                parent->dirty = true;
                // the new sibling is at parent->children[idx+1] now,
                // bring it into cache so the next walk can swizzle through
                (void)load_child(parent->children[idx + 1], parent, idx + 1);
            }
        } 
        else {
            auto inner = static_cast<BTreeInner<K> *>(child);
            auto sib_addr = backend_->alloc(PAGE_SIZE);
            char buf[PAGE_SIZE];
            std::memset(buf, 0, PAGE_SIZE);
            auto sib = new (buf) BTreeInner<K>(inner->level, sib_addr);

            K sep;
            inner->split(sep, sib);
            // sib may all swizzled child -> admit sib to cache directly.
            // backend will know when we write back when the eviction
            inner->dirty = true;
            ++inner_num_;

            NodeBase *cached_sib = cache.cache_insert(sib_addr, sib, parent);
            cached_sib->pos_state = 2;
            cached_sib->dirty = true; // ensure first eviction writes the page
            // redirect some child to have cached_sib as parent (other half still has inner as parent)
            update_all_parent_ptr(cached_sib); 
            if (parent_is_super) {
                // make_new_root will create a fresh inner above (leftChild,
                // sib) and re-point leftChild & sib's parent_ptr to it.
                make_new_root(sep, inner, sib_addr, inner->level + 1);
            } else {
                // sib_addr now carries SWIZZLE_TAG; BTreeInner::insert sets parent's bitmap bit pos+1 itself
                parent->insert(sep, sib_addr);
                parent->dirty = true;
            }
        }
    }

    // create a new inner above leftChild (already cached) and rightAddr (has SWIZZLE TAG | cache-ptr from caller's split)
    // admit the new root directly into cache 
    void make_new_root(K sep, NodeBase *leftChild, GlobalAddress rightAddr,
                       uint8_t level) {
        auto new_root_addr = backend_->alloc(PAGE_SIZE);

        // build the new root in a scratch buffer with swizzled children
        // pointing at the already-cached leftChild and sib.
        char buf[PAGE_SIZE];
        std::memset(buf, 0, PAGE_SIZE);
        auto new_root_buf = new (buf) BTreeInner<K>(level, new_root_addr);
        new_root_buf->count = 1;
        new_root_buf->keys[0] = sep;
        new_root_buf->children[0].val = reinterpret_cast<uint64_t>(leftChild) | SWIZZLE_TAG;
        new_root_buf->children[1] = rightAddr; // already SWIZZLE-tagged
        new_root_buf->set_bitmap(0);
        if (rightAddr.val & SWIZZLE_TAG) new_root_buf->set_bitmap(1);
        ++inner_num_;

        // point super_root_ at the new backend address, then admit it to cache
        root_ = new_root_addr;
        GlobalAddress new_root_slot = new_root_addr;
        NodeBase *cached_root = cache.cache_insert(new_root_slot, new_root_buf, super_root_);
        cached_root->pos_state = 2;
        cached_root->dirty = true;

        // super_root_'s children[0] now holds the swizzled pointer to cached_root
        super_root_->children[0] = new_root_slot;
        super_root_->bitmap = 0; // super_root_ doesn't use bitmap
        super_root_->count = 0;

        // re-point leftChild and the right sibling at the new root.
        leftChild->parent_ptr = cached_root;
        if (rightAddr.val & SWIZZLE_TAG) {
            auto cached_right = reinterpret_cast<NodeBase *>(rightAddr.val & SWIZZLE_HIDE);
            cached_right->parent_ptr = cached_root;
        }

        height_ = level + 1;
    }

    // --- Tree_API ---
    bool insert(K k, V v) override {
        NodeBase *cur = load_child(super_root_->children[0], super_root_, 0);
        if (is_full(cur)) { // full -> split root first
            split_child(super_root_, 0, cur);
            cur = load_child(super_root_->children[0], super_root_, 0);
        }

        while (cur->type == PageType::BTreeInner) {
            auto inner = static_cast<BTreeInner<K> *>(cur);
            unsigned idx = inner->lowerBound(k);
            NodeBase *child = load_child(inner->children[idx], inner, idx);
            if (is_full(child)) {
                split_child(inner, idx, child); // split & update inner
                idx = inner->lowerBound(k);     // sep was insert, pick again
                child = load_child(inner->children[idx], inner, idx);
            }
            cur = child;
        }

        auto leaf = static_cast<BTreeLeaf<K, V> *>(cur);
        bool inserted = leaf->insert(k, v);
        leaf->dirty = true; // know to write it later
        return inserted;
    }

    bool lookup(K k, V &result) override {
        NodeBase *cur = load_child(super_root_->children[0], super_root_, 0);
        while (cur->type == PageType::BTreeInner) {
            auto inner = static_cast<BTreeInner<K> *>(cur);
            unsigned idx = inner->lowerBound(k);
            cur = load_child(inner->children[idx], inner, idx);
        }
        auto leaf = static_cast<BTreeLeaf<K, V> *>(cur);
        return leaf->find(k, result);
    }

    bool update(K k, V v) override {
        NodeBase *cur = load_child(super_root_->children[0], super_root_, 0);
        while (cur->type == PageType::BTreeInner) {
            auto inner = static_cast<BTreeInner<K> *>(cur);
            unsigned idx = inner->lowerBound(k);
            cur = load_child(inner->children[idx], inner, idx);
        }
        auto leaf = static_cast<BTreeLeaf<K, V> *>(cur);
        bool ok = leaf->update(k, v);
        if (ok) leaf->dirty = true;
        return ok;
    }

    bool remove(K k) override {
        NodeBase *cur = load_child(super_root_->children[0], super_root_, 0);
        while (cur->type == PageType::BTreeInner) {
            auto inner = static_cast<BTreeInner<K> *>(cur);
            unsigned idx = inner->lowerBound(k);
            cur = load_child(inner->children[idx], inner, idx);
        }
        auto leaf = static_cast<BTreeLeaf<K, V> *>(cur);
        bool ok = leaf->remove(k);
        if (ok) leaf->dirty = true;
        return ok;
    }

    int range_scan(K k, uint32_t num, std::pair<K, V> *&result) override {
        NodeBase *cur = load_child(super_root_->children[0], super_root_, 0);
        while (cur->type == PageType::BTreeInner) {
            auto inner = static_cast<BTreeInner<K> *>(cur);
            unsigned idx = inner->lowerBound(k);
            cur = load_child(inner->children[idx], inner, idx);
        }
        auto leaf = static_cast<BTreeLeaf<K, V> *>(cur);
        return static_cast<int>(leaf->rangeScan(k, num, result));
    }

    void clear_statistic() override {
        cache.inner_miss_ = 0;
        cache.leaf_miss_ = 0;
        cache.full_page_miss_ = 0;
        cache.rdma_write = 0;
    }

    void reset_buffer_pool(bool flush_dirty) override {
        cache.reset(flush_dirty);
    }

    void set_rpc_ratio(double ratio) override { cache.set_rpc_ratio(ratio); }
    void set_admission_ratio(double ratio) override { cache.set_admission_ratio(ratio); }
    double get_rpc_ratio() override { return cache.get_rpc_ratio(); }

    void get_statistic() override {
        std::cout << "height=" << height_ << " leaves=" << leaf_num_
                  << " inners=" << inner_num_ << std::endl;
        cache.check_dirty_in_buffer();
    }
};

} // namespace cachepush
