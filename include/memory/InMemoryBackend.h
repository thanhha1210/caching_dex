#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "Backend.h"

namespace cachepush {

class InMemoryBackend : public Backend {
private:
    uint64_t capacity_bytes_;
    uint64_t cur_offset_;
    std::vector<char> store_;
public:
    explicit InMemoryBackend(uint64_t page_nums)
        : capacity_bytes_(page_nums * PAGE_SIZE), cur_offset_(PAGE_SIZE), store_(page_nums * PAGE_SIZE, 0) {}

    bool read(GlobalAddress addr, void *buf, uint64_t size) override {
        if (addr.offset + size > capacity_bytes_) 
            return false;
        std::memcpy(buf, store_.data() + addr.offset, size);
        return true;
    }
    bool write(GlobalAddress addr, const void *buf, uint64_t size) override {
        if (addr.offset + size > capacity_bytes_) 
            return false;
        std::memcpy(store_.data() + addr.offset, buf, size);
        return true;
    }
    GlobalAddress alloc(uint64_t size) override {
        const uint64_t rounded = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
        if (cur_offset_ + rounded > capacity_bytes_) 
            return GlobalAddress::Null();
        GlobalAddress out(0, cur_offset_);
        cur_offset_ += rounded;
        return out;
    }

    char *raw_base() { return store_.data(); }
    const char *raw_base() const { return store_.data(); }


};

} // namespace cachepush
