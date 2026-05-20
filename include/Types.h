#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

namespace cachepush {
    #define BITMAP 1

    static constexpr uint64_t PAGE_SIZE = 1024;
    static constexpr uint64_t POOL_SIZE = 1024 * 64;

    static constexpr uint64_t SWIZZLE_TAG  = 1ULL << 63;
    static constexpr uint64_t SWIZZLE_HIDE = (1ULL << 63) - 1;

    static constexpr uint64_t MEGA_LEVEL = 4; // 4 as bigger node

    static constexpr int MAX_TREE_DEPTH = 32;
    static const int SLOT_PER_BUCKET = 3;
    
    enum class PageType : uint8_t {
        BTreeInner = 1,
        BTreeLeaf = 2,
    };

    enum class PosState : uint8_t {
        POS_REMOTE = 0,
        POS_COOL = 1,
        POS_HOT = 2,
        POS_LOCAL = 3,
        POS_PIN = 4,
    };

    enum class RPC_Type { LOOKUP, UPDATE, INSERT, DELETE };

    using Key = uint64_t;
    using Value = uint64_t;

} // namespace cachepush
