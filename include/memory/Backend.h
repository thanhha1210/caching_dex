#pragma once

// Abstract backend -> later change to RDMA 

#include <cstddef>
#include "../GlobalAddress.h"
#include "../Types.h"

namespace cachepush {
    class Backend {
    public:
        virtual ~Backend() = default;

        // Read 'size' bytes from 'addr' into the caller-owned 'buf'
        virtual bool read(GlobalAddress addr, void *buf, uint64_t size = PAGE_SIZE) = 0;

        // Write 'size' bytes from 'buf' to 'addr'
        virtual bool write(GlobalAddress addr, const void *buf, uint64_t size = PAGE_SIZE) = 0;

        // Allocate 'size' bytes; returns Null on failure
        virtual GlobalAddress alloc(uint64_t size = PAGE_SIZE) = 0;

        // Free a previously-allocated address
        virtual void free(GlobalAddress addr) {}
    };

} // namespace cachepush
