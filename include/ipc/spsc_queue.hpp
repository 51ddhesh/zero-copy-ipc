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

    
    // ─── Constants ───────────────────────────────────────
    static constexpr uint64_t kMagic = 0x5A49504353505343ULL; // Spells "ZIPCSPSC"

    // Layout Offsets
    static constexpr std::size_t kHeaderOffset = 0;
    static constexpr std::size_t kWriteOffset = kCacheLineSize; // 64
    static constexpr std::size_t kReadOffset = kCacheLineSize * 2; // 128
    static constexpr std::size_t kDataOffset = kCacheLineSize * 3; // 192

    // ─── Header ──────────────────────────────────────────
    struct Header {
        uint64_t magic;
        uint64_t capacity;
        uint64_t mask;
        uint64_t slot_size;
        // remaining bytes in the cache line are padding 
    };

    static_assert(sizeof(Header) <= kCacheLineSize);

    // ─── Internal states (NOT IN SHARED MEMORY) ──────────
    std::byte* base_ = nullptr;
    uint64_t capacity_ = 0;
    uint64_t mask_ = 0;

    // Cached copies of the remote cursor 
    // This avoids cross-core atmoic loads on the common (non-full/non-empty) path
    // NOT IN THE SHARED MEMORY
    mutable uint64_t cached_write_pos_ = 0; // consumer caches producer's cursor
    mutable uint64_t cached_read_pos_ = 0; // producer caches consumer's cursor


    // ─── Helper Functions ────────────────────────────────
    
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
    
    // ─── Atomic Reference Wrappers ───────────────────────
    // These functions create short lived atomic_ref objects on the stack
    // On x86_64 arch, atomic loads/stores of aligned uint64_t compile to 
    // plan MOV instructions (zero overhead)
    
    std::atomic_ref<uint64_t> write_cursor() const noexcept {
        return std::atomic_ref<uint64_t> {*write_cursor_ptr()};
    }
    
    std::atomic_ref<uint64_t> read_cursor() const noexcept {
        return std::atomic_ref<uint64_t> {*read_cursor_ptr()};
    }


public:
    
    // ─── Size calculation ────────────────────────────────
    
    /// Returns the minimum shared memory region size for a queue of given capacity
    static constexpr std::size_t required_size(std::size_t capacity) noexcept {
        return kDataOffset + capacity * sizeof(T);
    }
    
    // ─── Factory Methods ─────────────────────────────────
    
    /// Initialize a new queue in the region
    /// The capacity of the queue must be a power of 2 (positive)
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
                " < " + std::to_string(required_size(capacity)) + ")"
            );
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
        
    /// Attach to an exisiting queue in `region`
    /// Validate magic and slot size to catch type mismatches
    static SPSCQueue open(void* region, std::size_t region_size) {
        if (!region) {
            throw std::invalid_argument("[SPSCQueue::open]: null region");
        }
        
        auto* base = static_cast<std::byte*> (region);
        auto* hdr = reinterpret_cast<const Header*> (base + kHeaderOffset);
        
        if (hdr -> magic != kMagic) {
            throw std::runtime_error(
                "[SPSCQueue::open]: bad magic (not an SPSC Queue?)"
            );
        }
        
        if (hdr -> slot_size != sizeof(T)) {
            throw std::runtime_error(
                "[SPSCQueue::open]: slot size mismatch. Expected" + std::to_string(sizeof(T)) + ". Got "
                + std::to_string(hdr -> slot_size) + "."
            );
        }
        
        if (!is_power_of_two(static_cast<std::size_t> (hdr -> capacity))) {
            throw std::runtime_error(
                "[SPSCQueue::open]: capacity is not a power of two."
            );
        }
        
        if (region_size < required_size(static_cast<std::size_t> (hdr -> capacity))) {
            throw std::runtime_error(
                "[SPSCQueue::open]: region too small for stored capacity"
            );
        }
        
        return SPSCQueue{base};
    }
        
    
    // ─── Producer API ────────────────────────────────────
    
    /// Try to enqueue a copy of `val`
    /// Returns true on success, false if the queue is full
    ///
    /// Memory Ordering:
    /// - We `memcpy` data into the slot before advancing the write_cursor.
    /// - The release store on write_cursor ensures that the slot write is 
    ///   visible to the consumer before it sees the new cursor.
    /// - The consumer's acquire load of write_cursor pairs with this release, 
    ///   forming a synchronizes-with relationship.
    
    bool try_push(const T& val) noexcept {
        const uint64_t wp = write_cursor().load(std::memory_order_relaxed);
        const uint64_t next = wp + 1;
        
        // Fast path: check cache read position
        if (next - cached_read_pos_ > capacity_) {
            // Slow path: the queue might be full, re-read the actual cursor
            cached_read_pos_ = read_cursor().load(std::memory_order_acquire);
            if (next - cached_read_pos_ > capacity_) {
                return false; // the queue is full
            }
        }
        
        // Write data and then publish
        std::memcpy(slot_ptr(wp), &val, sizeof(T));
        write_cursor().store(next, std::memory_order_release);
        return true;
    }
    
    
    /// Try to dequeue into `out`
    /// Returns true on success, false if the queue is empty
    ///
    /// Memory Ordering
    /// - We acquire-load write_cursor to see the producer's latest publish.
    /// - We memcpy data out of the slot BEFORE advancing read_cursor.
    /// - The release store on read_cursor ensures the producer sees
    ///   that we've finished reading before it overwrites the slot.
    
    bool try_pop(T& out) noexcept {
        const uint64_t rp = read_cursor().load(std::memory_order_relaxed);
        
        // check the cached write position
        if (rp == cached_write_pos_) {
            // check the slow path as well since the queue might be empty
            cached_write_pos_ = write_cursor().load(std::memory_order_acquire);
            if (rp == cached_write_pos_) {
                return false; // empty queue
            }
        }
        
        // Read the data and publish 
        std::memcpy(&out, slot_ptr(rp), sizeof(T));
        read_cursor().store(rp + 1, std::memory_order_release);
        
        return true;
    }
    
    /// use `std::optional<T>`
    std::optional<T> try_pop() noexcept {
        T val;
        if (try_pop(val)) {
            return val;
        }
        
        return std::nullopt;
    }
    
    
    // ─── Zero-Copy Producer API ──────────────────────────
    
    /// Claim a slot for writing. 
    /// Returns `nullptr` if the queue is full.
    /// ! MUST call `publish()` after writing to the returned pointer
    /// No other `try_push()` or `claim()` may be made before `publish()`. 
    T* claim() noexcept {
        const uint64_t wp = write_cursor().load(std::memory_order_relaxed);
        const uint64_t next = wp + 1;
        
        if (next - cached_read_pos_ > capacity_) {
            cached_read_pos_ = read_cursor().load(std::memory_order_acquire);
            if (next - cached_read_pos_ > capacity_) {
                return nullptr;
            }
        }
        
        return slot_ptr(wp);
    }
    
    /// Publish a slot previously obtained via `claim()`
    /// ! MUST be called exactly once after each successful `claim()`
    void publish() noexcept {
        const uint64_t wp = write_cursor().load(std::memory_order_relaxed);
        write_cursor().store(wp + 1, std::memory_order_release);
    }
    
    
    // ─── Zero-Copy Consumer API ──────────────────────────
    
    /// Peek at the front element without consuming it
    /// Returns `nullptr` if queue is empty
    /// ! MUST call `consume()` after reading from the returned pointer
    /// No other `try_pop()` or `peek()` call may be made before `consume()`
    const T* peek() noexcept {
        const uint64_t rp = read_cursor().load(std::memory_order_relaxed);
        
        if (rp == cached_write_pos_) {
            cached_write_pos_ = write_cursor().load(std::memory_order_acquire);
            if (rp == cached_write_pos_) {
                return nullptr; // empty queue
            }
        }
        
        return slot_ptr(rp);
    }
    
    /// Consume the element previously peeked at.
    /// ! Must be called exactly once after each successful peek().
    void consume() noexcept {
        const uint64_t rp = read_cursor().load(std::memory_order_relaxed);
        read_cursor().store(rp + 1, std::memory_order_release);
    }
    
    
    // ─── Observers ───────────────────────────────────────
    
    std::size_t capacity() const noexcept {
        return static_cast<std::size_t> (capacity_);
    }
    
    /// Approximate number of elements in the queue
    /// ### CAUTION
    /// #### This is not linearizable - use only for debugging and monitoring
    std::size_t size_approx() const noexcept {
        const uint64_t wp = write_cursor().load(std::memory_order_relaxed);
        const uint64_t rp = read_cursor().load(std::memory_order_relaxed);
        
        return static_cast<std::size_t> (wp - rp);
    } 
    
    bool empty_approx() const noexcept {
        return size_approx() == 0;
    }
    
    bool full_approx() const noexcept {
        return size_approx() >= capacity_;
    }


    // ─── Reset (test/debug only) NOT THREADSAFE ──────────

    void reset() noexcept {
        write_cursor().store(0, std::memory_order_relaxed);
        read_cursor().store(0, std::memory_order_relaxed);
        cached_write_pos_ = 0;
        cached_read_pos_  = 0;
    }
};

} // namespace z_ipc
