// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)

/*
    z_ipc::SPSCQueue<T>
    Lock free, bounded, single producer single consumer queue
    operates over shared memory 
*/

/*
    Shared memory layout (3 cache lines + data)
    ┌────────────────────────────────────┐ offset 0
    │  Header  (64 B, cache line 0)      │
    │    magic, capacity, mask, slot_sz  │
    ├────────────────────────────────────┤ offset 64
    │  Write cursor (cache line 1)       │ <- producer OWNS this line
    │    uint64_t  (atomic via ref)      │
    ├────────────────────────────────────┤ offset 128
    │  Read cursor  (cache line 2)       │ <- consumer OWNS this line
    │    uint64_t  (atomic via ref)      │
    ├────────────────────────────────────┤ offset 192
    │  Slot array: T[capacity]           │
    └────────────────────────────────────┘
*/

/*
    False Sharing Elimination
    - Producer writes write_cursor (line 1), reads read_cursor (line 2)
    - Consumer writes read_cursor (line 2), reads write_cursor (line 1)
    - Each cursor has its own cache line and no two threads to the same line.
    - This avoids MESI invalidation bouncing.
*/

/*
    Strategy for Atomics
    - Use C++ 20 std::atomic_ref<uint64_t> rather than placing std::atomic<uint64_t> in shared memory
    - The standard only guarantees std::atomic works within a single process.
    - std::atomic_ref applies atomic semantics to a plan uint64_t that lives in any memory region.
*/

/*
    Concurrency Model
    - Exactly one thread/process may call try_push (producer).
    - Exactly one thread/process may call try_pop (consumer).
    - Producer and Consumer may be on different threads/processes.
    - capacity(), size_approx() may be called from any thread.
*/


#pragma once


#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "ipc/common.hpp"


namespace z_ipc {

    template <typename T>
        requires(std::is_trivially_copyable_v<T> && alignof(T) <= kCacheLineSize)

    class SPSCQueue {
    private:

        static_assert(std::atomic_ref<uint64_t>::is_always_lock_free, 
            "Lock-free uint64_t atomics required"
        );

        
        // Constants
        static constexpr uint64_t kMagic = 0x5A49504353505343ULL; // Spells "ZIPCSPSC"
    
        // Layout Offsets
        static constexpr std::size_t kHeaderOffset = 0;
        static constexpr std::size_t kWriteOffset = kCacheLineSize; // 64
        static constexpr std::size_t kReadOffset = kCacheLineSize * 2; // 128
        static constexpr std::size_t kDataOffset = kCacheLineSize * 3; // 192

        // Header
        struct Header {
            uint64_t magic;
            uint64_t capacity;
            uint64_t mask;
            uint64_t slot_size;
            // remaining bytes in the cache line are padding 
        };

        static_assert(sizeof(Header) <= kCacheLineSize);

        // Internal states (NOT IN SHARED MEMORY)
        std::byte* base_ = nullptr;
        uint64_t capacity_ = 0;
        uint64_t mask_ = 0;

        // Cached copies of the remote cursor 
        // This avoids cross-core atmoic loads on the common (non-full/non-empty) path
        // NOT IN THE SHARED MEMORY
        mutable uint64_t cached_write_pos_ = 0; // consumer caches producer's cursor
        mutable uint64_t cached_read_pos_ = 0; // producer caches consumer's cursor


        // Helper Functions
        explicit SPSCQueue(std::byte* base) noexcept : base_{base} {
            auto* hdr = reinterpret_cast<const Header*> (base + kHeaderOffset);
            capacity_ = hdr -> capacity;
            mask_ = hdr -> mask;
        }

        uint64_t* write_cursor_ptr() const noexcept {
            return reinterpret_cast<uint64_t*> (base_ + kWriteOffset);
        }

        uint64_t* read_cursor_ptr() const noexcept {
            return reinterpret_cast<uint64_t*> (base_ + kReadOffset);
        }

        T* slot_ptr(uint64_t seq) const noexcept {
            return reinterpret_cast<T*> (base_ + kDataOffset) + (seq & mask_);
        }

        // Atmoic Reference Wrappers
        std::atomic_ref<uint64_t> write_cursor() const noexcept {
            return std::atomic_ref<uint64_t*> {*write_cursor_ptr()};
        }

        std::atomic_ref<uint64_t> read_cursor() const noexcept {
            return std::atomic_ref<uint64_t*> {*read_cursor_ptr()};
        }

    
    public:

        // Size Calculation

        // Returns the minimum shared memory region size for a queue of given capacity
        static constexpr std::size_t required_size(std::size_t capacity) noexcept {
            return kDataOffset + capacity * sizeof(T);
        }

        // SPSC Factory Methods

        // Initialize a new queue in the region
        // The capacity of the queue must be a power of 2 (positive)
        static SPSCQueue create(void* region, std::size_t region_size, std::size_t capacity) {
            if (!region) {
                throw std::invalid_argument("[SPSCQueue::create]: null region.");
            }

            if (capacity == 0) {
                throw std::invalid_argument("[SPSCQueue::create]: capacity must be > 0.");
            }

            if (!is_power_of_two(capacity)) {
                throw std::invalid_argument("[SPSCQueue::create]: capacity must be a power of two.");
            }

            if (region_size < required_size(capacity)) {
                throw std::invalid_argument(
                "[SPSCQueue::create]: region too small (" + std::to_string(region_size) +
                " < " + std::to_string(required_size(capacity)) + ")");
            }


            auto* base = static_cast<std::byte*> (region);
            // zero the control region (both cursor lines and header)
            std::memset(base, 0, kDataOffset);

            auto* hdr = reinterpret_cast<Header*> (base + kHeaderOffset);
            hdr -> magic = kMagic;
            hdr -> capacity = static_cast<uint64_t> (capacity);
            hdr -> mask = static_cast<uint64_t> (capacity - 1);
            hdr -> slot_size = static_cast<uint64_t> (sizeof(T));
        
            // publish the cursors with atomic_ref
            auto* wp = reinterpret_cast<uint64_t*> (base + kWriteOffset);
            auto* rp = reinterpret_cast<uint64_t*> (base + kReadOffset);
            std::atomic_ref<uint64_t> {*wp}.store(0, std::memory_order_relaxed);
            std::atomic_ref<uint64_t> {*rp}.store(0, std::memory_order_relaxed);
        
            return SPSCQueue{base};
        }

    };
} // namespace z_ipc
