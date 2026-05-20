#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <utility>

#include "../GlobalAddress.h"
#include "../Types.h"

namespace cachepush {
    struct NodeBase {
        GlobalAddress remote_address;   
        uint64_t bitmap;      // 32B
        NodeBase *parent_ptr; // ptr to parent
        PageType type;        // can remove later since check level -> know Type
        uint8_t count;
        uint8_t level;        // 0 = leaf, 1 = inner node on top of leaf
        uint8_t pos_state;    // 0 = remote, 1 = cool, 2 = hot, 3 = local working set, 4 = pinned (can't sample)
        bool shared;
        bool dirty;
        bool obsolete;
        Key min_limit_;
        Key max_limit_; // 64B

        bool checkObsolete() { return obsolete; }
        bool isShared() { return shared; }
    };

    struct BTreeLeafBase : public NodeBase {
        static constexpr PageType type_marker = PageType::BTreeLeaf;
    };

    template <class Key, class Payload>
    struct BTreeLeaf : public BTreeLeafBase { 
        using KeyValueType = std::pair<Key, Payload>;

        static constexpr uint64_t max_entries =
            (PAGE_SIZE - (sizeof(Key) * 2 + sizeof(uint64_t) * 2) - sizeof(NodeBase)) /
            sizeof(KeyValueType);

        KeyValueType data[max_entries];
        GlobalAddress next_leaf = GlobalAddress::Null();
        uint64_t dummy[2]; // pack for 1-24 granularity

        BTreeLeaf(GlobalAddress remote) {
            remote_address = remote;
            parent_ptr     = nullptr;
            level          = 0;
            count          = 0;
            pos_state      = 0;
            dirty          = true;
            obsolete       = false;
            shared         = false;
            type           = type_marker;
            bitmap         = 0;
            min_limit_     = std::numeric_limits<Key>::min();
            max_limit_     = std::numeric_limits<Key>::max();
        }

        bool isFull() const { return count == max_entries; }

        unsigned lowerBound(Key k) const {
            unsigned lower = 0;
            unsigned upper = count;
            while (lower < upper) {
                const unsigned mid = (upper - lower) / 2 + lower;
                const Key &mid_key = data[mid].first;
                if (k == mid_key) return mid;
                if (k > mid_key) lower = mid + 1;
                else upper = mid;
            }
            return lower;
        }

        bool find(Key k, Value &v) const {
            const unsigned pos = lowerBound(k);
            if (pos < count && data[pos].first == k) {
                v = data[pos].second;
                return true;
            }
            return false;
        }

        uint32_t rangeScan(Key k, uint32_t num, KeyValueType *&a) {
            const unsigned pos = lowerBound(k);
            if (pos < count && data[pos].first == k) {
                const uint32_t remain_count = count - pos;
                const uint32_t copy_count = std::min<uint32_t>(remain_count, num);
                std::memcpy(a, data + pos, copy_count * sizeof(KeyValueType));
                return copy_count;
            }
            return 0;
        }

        bool insert(Key k, Payload p) {
            dirty = true;
            if (count > 0) {
                const unsigned pos = lowerBound(k);
                if (pos < count && data[pos].first == k) {  // upsert
                    data[pos].second = p;
                    return false;
                }
                if (count >= max_entries) {
                    return false;
                }
                std::memmove(reinterpret_cast<void *>(data + pos + 1),
                            reinterpret_cast<void *>(data + pos),
                            sizeof(KeyValueType) * (count - pos));
                data[pos].first = k;
                data[pos].second = p;
            } 
            else {
                data[0].first = k;
                data[0].second = p;
            }
            ++count;
            return true;
        }

        bool remove(Key k) {
            if (!count) 
                return false;
            const unsigned pos = lowerBound(k);
            if (pos < count && data[pos].first == k) {
                dirty = true;
                // move data element
                std::memmove(reinterpret_cast<void *>(data + pos),
                            reinterpret_cast<void *>(data + pos + 1),
                            sizeof(KeyValueType) * (count - pos - 1));
                --count;
                return true;
            }
            return false;
        }

        bool update(Key k, Payload p) {
            const unsigned pos = lowerBound(k);
            if (pos < count && data[pos].first == k) {
                dirty = true;
                data[pos].second = p;
                return true;
            }
            return false;
        }

        void split(Key &sep, BTreeLeaf *newLeaf) {
            newLeaf->count = count - (count / 2);
            count = count - newLeaf->count;
            std::memcpy(reinterpret_cast<void *>(newLeaf->data),
                        reinterpret_cast<void *>(data + count),
                        sizeof(KeyValueType) * newLeaf->count);
            next_leaf = newLeaf->remote_address;
            sep = data[count - 1].first;
            newLeaf->min_limit_ = sep;
            newLeaf->max_limit_ = max_limit_;
            max_limit_ = sep;
            dirty = true;
        }

        bool rangeValid(Key k) const {
            if (std::numeric_limits<Key>::min() == k && k == min_limit_) 
                return true;
            return k > min_limit_ && k <= max_limit_;
        }
    };

    struct BTreeInnerBase : public NodeBase {
        static constexpr PageType type_marker = PageType::BTreeInner;
    };

    template <class Key>
    struct BTreeInner : public BTreeInnerBase {
        static constexpr uint64_t max_entries =
            (PAGE_SIZE - sizeof(NodeBase) - sizeof(uint64_t)) /
            (sizeof(Key) + sizeof(GlobalAddress));

        GlobalAddress children[max_entries];
        Key keys[max_entries];
        uint64_t dummy;

        BTreeInner(uint8_t level_in, GlobalAddress remote) {
            remote_address = remote;
            parent_ptr     = nullptr;
            level          = level_in;
            count          = 0;
            pos_state      = 0;
            dirty          = true;
            obsolete       = false;
            shared         = false;
            type           = type_marker;
            bitmap         = 0;
            min_limit_     = std::numeric_limits<Key>::min();
            max_limit_     = std::numeric_limits<Key>::max();
        }

        bool isFull() const { return count == (max_entries - 1); }

        unsigned lowerBound(Key k) const {
            unsigned lower = 0;
            unsigned upper = count;
            while (lower < upper) {
                const unsigned mid = ((upper - lower) / 2) + lower;
                if (k == keys[mid]) return mid;
                if (k > keys[mid]) lower = mid + 1; 
                else upper = mid;
            }
            return lower;
        }

        int findIdx(uint64_t target_addr) const {
            const int end = count + 1;
            for (int i = 0; i < end; ++i) 
                if (children[i].val == target_addr) 
                    return i;
            return -1;
        }

        void set_bitmap(int idx) { bitmap |= (1ULL << idx); }
        void unset_bitmap(int idx) { bitmap &= ~(1ULL << idx); }
        bool test_bitmap(int idx) const { return (bitmap & (1ULL << idx)) != 0; }

        void split(Key &sep, BTreeInner *newInner) {
            newInner->count = count - (count / 2);
            count = count - newInner->count - 1;
            sep = keys[count];
            std::memcpy(newInner->keys, keys + count + 1,
                        sizeof(Key) * (newInner->count + 1));
            std::memcpy(newInner->children, children + count + 1,
                        sizeof(GlobalAddress) * (newInner->count + 1));
            newInner->min_limit_ = sep;
            newInner->max_limit_ = max_limit_;
            max_limit_ = sep;
            newInner->bitmap = bitmap >> (count + 1);
            bitmap &= ((1ULL << (count + 1)) - 1);
            dirty = true;
        }

        bool rangeValid(Key k) const {
            if (std::numeric_limits<Key>::min() == k && k == min_limit_) return true;
            return k > min_limit_ && k <= max_limit_;
        }

        void insert(Key k, GlobalAddress child) {
            assert(count < max_entries - 1);
            const unsigned pos = lowerBound(k);
            std::memmove(keys + pos + 1, keys + pos, sizeof(Key) * (count - pos + 1));
            std::memmove(children + pos + 1, children + pos,
                        sizeof(GlobalAddress) * (count - pos + 1));
            keys[pos] = k;
            children[pos] = child;
            std::swap(children[pos], children[pos + 1]);
            const uint64_t upper_part = ((bitmap >> (pos + 1)) << (pos + 2));
            bitmap = (bitmap & ((1ULL << (pos + 1)) - 1)) | upper_part;
            if (child.val & SWIZZLE_TAG) 
                set_bitmap(pos + 1);
        
            ++count;
            dirty = true;
        }
    };

    // function to init leaf_node
    template <class Key, class Value>
    inline void initLeaf(BTreeLeaf<Key, Value>* leaf, GlobalAddress remote) {
        std::memset(reinterpret_cast<void *>(leaf), 0, PAGE_SIZE);
        leaf->remote_address = remote;
        leaf->parent_ptr     = nullptr;
        leaf->bitmap         = 0;
        leaf->type           = PageType::BTreeLeaf;
        leaf->level          = 0;
        leaf->count          = 0;
        leaf->pos_state      = 3; // local
        leaf->shared         = false;
        leaf->dirty          = true;
        leaf->obsolete       = false;
        leaf->min_limit_     = std::numeric_limits<Key>::min();
        leaf->max_limit_     = std::numeric_limits<Key>::max();
        leaf->next_leaf      = GlobalAddress::Null();
    }

    // function to init inner_node
    template <class Key>
    inline void initInner(BTreeInner<Key> *inner, GlobalAddress remote, std::uint8_t level) {
        std::memset(reinterpret_cast<void *>(inner), 0, PAGE_SIZE);
        inner->remote_address = remote;
        inner->parent_ptr     = nullptr;
        inner->bitmap         = 0;
        inner->type           = PageType::BTreeInner;
        inner->level          = level;
        inner->count          = 0;
        inner->pos_state      = 3; // local
        inner->shared         = false;
        inner->dirty          = true;
        inner->obsolete       = false;
        inner->min_limit_     = std::numeric_limits<Key>::min();
        inner->max_limit_     = std::numeric_limits<Key>::max();
    }

} // namespace cachepush
