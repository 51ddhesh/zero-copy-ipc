// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include "ipc/shared_region.hpp"    
#include "ipc/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <sys/wait.h>
#include <thread>
#include <vector>

using z_ipc::SharedRegion;
using z_ipc::SPSCQueue;

// ─── TEST SETUP ──────────────────────────────────
class SPSCQueueTest : public ::testing::Test {
    protected:
    static constexpr const char* kShmName = "/z_ipc_spsc_test";
    static constexpr std::size_t kCap = 1024; // 2 ^ 10
    
    void SetUp() override {
        SharedRegion::unlink(kShmName);
    }
    
    void TearDown() override {
        SharedRegion::unlink(kShmName);
    }
    
    // Create a SharedRegion + SPSCQueue<T> at once
    template <typename T> 
    std::pair<SharedRegion, SPSCQueue<T>> make_queue(std::size_t cap = kCap) {
        std::size_t sz = SPSCQueue<T>::required_size(cap);
        auto region = SharedRegion::create(kShmName, sz);
        auto queue = SPSCQueue<T>::create(region.data(), region.size(), cap);
        
        return {std::move(region), std::move(queue)};
    }
};


// ─── Test Basic Functionality ────────────────────

TEST_F(SPSCQueueTest, CreateAndCapacity) {
    auto [region, q] = make_queue<uint64_t>();
    EXPECT_EQ(q.capacity(), kCap);
    EXPECT_EQ(q.size_approx(), 0u);
    EXPECT_TRUE(q.empty_approx());
    EXPECT_FALSE(q.full_approx());
}

TEST_F(SPSCQueueTest, PushPopSingle) {
    auto [region, q] = make_queue<uint64_t>();
    EXPECT_TRUE(q.try_push(42ULL));
    EXPECT_EQ(q.size_approx(), 1u);
    
    uint64_t val = 0;
    EXPECT_TRUE(q.try_pop(val));
    EXPECT_EQ(val, 42ULL);
    EXPECT_TRUE(q.empty_approx());
}

TEST_F(SPSCQueueTest, PushPopOptional) {
    auto [region, q] = make_queue<uint64_t>();
    EXPECT_TRUE(q.try_push(99ULL));
    
    auto opt = q.try_pop();
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(*opt, 99ULL);
    
    auto empty_opt = q.try_pop();
    EXPECT_FALSE(empty_opt.has_value());
}

TEST_F(SPSCQueueTest, PopFromEmptyReturnsFalse) {
    auto [region, q] = make_queue<uint64_t>();
    uint64_t val = 0xDEAD;
    EXPECT_FALSE(q.try_pop(val));
    EXPECT_EQ(val, 0xDEAD);  // unchanged
}

TEST_F(SPSCQueueTest, FIFOOrdering) {
    auto [region, q] = make_queue<uint64_t>();
    
    constexpr std::size_t N = 256;
    for (std::size_t i = 0; i < N; ++i) {
        ASSERT_TRUE(q.try_push(static_cast<uint64_t>(i)));
    }
    
    for (std::size_t i = 0; i < N; ++i) {
        uint64_t val = ~0ULL;
        ASSERT_TRUE(q.try_pop(val));
        ASSERT_EQ(val, static_cast<uint64_t>(i))
        << "FIFO violation at index " << i;
    }
}


// ─── Test Limits of Capacity ─────────────────────

TEST_F(SPSCQueueTest, FillToCapacity) {
    constexpr std::size_t cap = 16;
    auto sz = SPSCQueue<uint64_t>::required_size(cap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q = SPSCQueue<uint64_t>::create(region.data(), region.size(), cap);
    
    // Fill exactly to capacity
    for (std::size_t i = 0; i < cap; ++i) {
        ASSERT_TRUE(q.try_push(static_cast<uint64_t>(i)))
        << "push failed at i=" << i;
    }
    
    EXPECT_TRUE(q.full_approx());
    
    // One more should fail
    EXPECT_FALSE(q.try_push(999ULL));
    
    // Drain and verify
    for (std::size_t i = 0; i < cap; ++i) {
        uint64_t val = ~0ULL;
        ASSERT_TRUE(q.try_pop(val));
        ASSERT_EQ(val, static_cast<uint64_t>(i));
    }
    
    uint64_t tmp;
    EXPECT_TRUE(q.empty_approx());
    EXPECT_FALSE(q.try_pop(tmp));
}

TEST_F(SPSCQueueTest, FillThenDrainRepeated) {
    constexpr std::size_t cap = 8;
    auto sz = SPSCQueue<uint64_t>::required_size(cap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q = SPSCQueue<uint64_t>::create(region.data(), region.size(), cap);
    
    // Repeat fill/drain cycles to exercise wraparound
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (std::size_t i = 0; i < cap; ++i) {
            ASSERT_TRUE(q.try_push(static_cast<uint64_t>(cycle * cap + i)));
        }
        ASSERT_FALSE(q.try_push(0ULL));  // full
        
        for (std::size_t i = 0; i < cap; ++i) {
            uint64_t val = ~0ULL;
            ASSERT_TRUE(q.try_pop(val));
            ASSERT_EQ(val, static_cast<uint64_t>(cycle * cap + i));
        }
        ASSERT_TRUE(q.empty_approx());
    }
}


// ─── Test Index Overflow and Wraparound ──────────

TEST_F(SPSCQueueTest, WraparoundManyMessages) {
    // Push/pop more messages than capacity to exercise index wrapping
    auto [region, q] = make_queue<uint64_t>();
    
    constexpr std::size_t N = kCap * 10;  // 10x capacity
    uint64_t send_seq = 0;
    uint64_t recv_seq = 0;
    
    while (recv_seq < N) {
        // Push a batch
        while (send_seq < N && q.try_push(send_seq)) {
            ++send_seq;
        }
        // Pop a batch
        uint64_t val;
        while (q.try_pop(val)) {
            ASSERT_EQ(val, recv_seq)
            << "sequence mismatch at recv_seq=" << recv_seq;
            ++recv_seq;
        }
    }
    EXPECT_EQ(recv_seq, N);
}


// ─── Test Custom Structs and types ───────────────

struct alignas(8) MarketTick {
    uint64_t timestamp;
    uint32_t symbol_id;
    int32_t  price;
    uint32_t quantity;
    uint32_t flags;
};

static_assert(std::is_trivially_copyable_v<MarketTick>);

TEST_F(SPSCQueueTest, CustomStructType) {
    auto sz     = SPSCQueue<MarketTick>::required_size(kCap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q      = SPSCQueue<MarketTick>::create(region.data(), region.size(), kCap);
    
    MarketTick sent{};
    sent.timestamp = 1234567890ULL;
    sent.symbol_id = 42;
    sent.price     = -100;
    sent.quantity  = 500;
    sent.flags     = 0xFF;
    
    ASSERT_TRUE(q.try_push(sent));
    
    MarketTick received{};
    ASSERT_TRUE(q.try_pop(received));
    
    EXPECT_EQ(received.timestamp, sent.timestamp);
    EXPECT_EQ(received.symbol_id, sent.symbol_id);
    EXPECT_EQ(received.price,     sent.price);
    EXPECT_EQ(received.quantity,  sent.quantity);
    EXPECT_EQ(received.flags,    sent.flags);
}


// ─── Test Zero-copy API (claim/publish and peek/consume)

TEST_F(SPSCQueueTest, ClaimPublish) {
    auto [region, q] = make_queue<uint64_t>();
    
    auto* slot = q.claim();
    ASSERT_NE(slot, nullptr);
    *slot = 0xCAFEBABEULL;
    q.publish();
    
    uint64_t val = 0;
    ASSERT_TRUE(q.try_pop(val));
    EXPECT_EQ(val, 0xCAFEBABEULL);
}

TEST_F(SPSCQueueTest, PeekConsume) {
    auto [region, q] = make_queue<uint64_t>();
    ASSERT_TRUE(q.try_push(0xBEEFULL));
    
    const auto* slot = q.peek();
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(*slot, 0xBEEFULL);
    
    q.consume();
    EXPECT_TRUE(q.empty_approx());
}

TEST_F(SPSCQueueTest, ClaimReturnsNullWhenFull) {
    constexpr std::size_t cap = 4;
    auto sz     = SPSCQueue<uint64_t>::required_size(cap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q      = SPSCQueue<uint64_t>::create(region.data(), region.size(), cap);
    
    for (std::size_t i = 0; i < cap; ++i) {
        auto* s = q.claim();
        ASSERT_NE(s, nullptr);
        *s = i;
        q.publish();
    }
    
    EXPECT_EQ(q.claim(), nullptr);
}

TEST_F(SPSCQueueTest, PeekReturnsNullWhenEmpty) {
    auto [region, q] = make_queue<uint64_t>();
    EXPECT_EQ(q.peek(), nullptr);
}

TEST_F(SPSCQueueTest, ZeroCopyStructRoundtrip) {
    auto sz     = SPSCQueue<MarketTick>::required_size(kCap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q      = SPSCQueue<MarketTick>::create(region.data(), region.size(), kCap);
    
    // Producer: claim + write in-place
    auto* slot = q.claim();
    ASSERT_NE(slot, nullptr);
    slot->timestamp = 111;
    slot->symbol_id = 7;
    slot->price     = 42;
    slot->quantity  = 100;
    slot->flags     = 0;
    q.publish();
    
    // Consumer: peek + read in-place
    const auto* peek_slot = q.peek();
    ASSERT_NE(peek_slot, nullptr);
    EXPECT_EQ(peek_slot->timestamp, 111u);
    EXPECT_EQ(peek_slot->symbol_id, 7u);
    EXPECT_EQ(peek_slot->price,     42);
    EXPECT_EQ(peek_slot->quantity,  100u);
    q.consume();
    
    EXPECT_TRUE(q.empty_approx());
}


// ─── Test open/attach ────────────────────────────

TEST_F(SPSCQueueTest, OpenValidatesMagic) {
    auto region = SharedRegion::create(kShmName, 4096);
    // Don't create a queue - just raw memory (all zeros)
    EXPECT_THROW(
        SPSCQueue<uint64_t>::open(region.data(), region.size()),
        std::runtime_error);
    }
    
TEST_F(SPSCQueueTest, OpenValidatesSlotSize) {
    auto sz     = SPSCQueue<uint64_t>::required_size(kCap);
    auto region = SharedRegion::create(kShmName, sz);
    [[maybe_unused]] auto q      = SPSCQueue<uint64_t>::create(region.data(), region.size(), kCap);
    
    // Try to open as uint32_t - slot size mismatch
    EXPECT_THROW(
        (SPSCQueue<uint32_t>::open(region.data(), region.size())),
        std::runtime_error
    );
}
        
TEST_F(SPSCQueueTest, CreateThenOpenSameType) {
    auto sz     = SPSCQueue<uint64_t>::required_size(kCap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q1     = SPSCQueue<uint64_t>::create(region.data(), region.size(), kCap);
    
    // Push some data through q1
    ASSERT_TRUE(q1.try_push(777ULL));
    
    // Open a second handle to the same memory
    auto q2 = SPSCQueue<uint64_t>::open(region.data(), region.size());
    EXPECT_EQ(q2.capacity(), kCap);
    
    uint64_t val = 0;
    ASSERT_TRUE(q2.try_pop(val));
    EXPECT_EQ(val, 777ULL);
}
        
// ─── Test Error cases ────────────────────────────

TEST_F(SPSCQueueTest, CreateNullRegion) {
    EXPECT_THROW(
        SPSCQueue<uint64_t>::create(nullptr, 4096, 64),
        std::invalid_argument
    );
}

TEST_F(SPSCQueueTest, CreateZeroCapacity) {
    auto region = SharedRegion::create(kShmName, 4096);
    EXPECT_THROW(
        SPSCQueue<uint64_t>::create(region.data(), region.size(), 0),
        std::invalid_argument
    );
}

TEST_F(SPSCQueueTest, CreateNonPowerOfTwo) {
    auto region = SharedRegion::create(kShmName, 4096);
    EXPECT_THROW(
        SPSCQueue<uint64_t>::create(region.data(), region.size(), 100),
        std::invalid_argument
    );
}

TEST_F(SPSCQueueTest, CreateRegionTooSmall) {
    auto region = SharedRegion::create(kShmName, 256);  // too small for 1024 uint64_t
    EXPECT_THROW(
        SPSCQueue<uint64_t>::create(region.data(), region.size(), 1024),
        std::invalid_argument
    );
}


// ─── Test Reset ──────────────────────────────────

TEST_F(SPSCQueueTest, Reset) {
    auto [region, q] = make_queue<uint64_t>();
    
    for (std::size_t i = 0; i < 100; ++i) {
        q.try_push(i);
    }
    EXPECT_FALSE(q.empty_approx());
    
    q.reset();
    EXPECT_TRUE(q.empty_approx());
    EXPECT_EQ(q.size_approx(), 0u);
    
    // Should work normally after reset
    ASSERT_TRUE(q.try_push(42ULL));
    uint64_t val = 0;
    ASSERT_TRUE(q.try_pop(val));
    EXPECT_EQ(val, 42ULL);
}


// ─── Test Concurrency ────────────────────────────

TEST_F(SPSCQueueTest, ConcurrentTwoThreads) {
    auto [region, q_handle] = make_queue<uint64_t>();
    
    constexpr uint64_t N = 1'000'000;
    
    // We need both threads to operate on the same queue.
    // Since SPSCQueue is trivially copyable (just a pointer + cached state),
    // we re-open a second handle for the consumer from the same region.
    
    auto producer_q = SPSCQueue<uint64_t>::open(region.data(), region.size());
    auto consumer_q = SPSCQueue<uint64_t>::open(region.data(), region.size());
    
    std::thread producer([&producer_q]() {
        for (uint64_t i = 0; i < N; ++i) {
            while (!producer_q.try_push(i)) {
                // spin
            }
        }
    });
    
    std::thread consumer([&consumer_q]() {
        uint64_t expected = 0;
        while (expected < N) {
            uint64_t val;
            if (consumer_q.try_pop(val)) {
                ASSERT_EQ(val, expected)
                << "sequence break at expected=" << expected;
                ++expected;
            }
        }
    });
    
    producer.join();
    consumer.join();
}


// ─── Test Cross Processes (fork()) ───────────────

TEST_F(SPSCQueueTest, CrossProcess) {
    constexpr std::size_t cap = 256;
    constexpr uint64_t N      = 10'000;
    
    auto sz     = SPSCQueue<uint64_t>::required_size(cap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q      = SPSCQueue<uint64_t>::create(region.data(), region.size(), cap);
    
    pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork failed: " << strerror(errno);
    
    if (pid == 0) {
        // Child = Producer
        auto child_region = SharedRegion::open(kShmName, sz);
        auto child_q      = SPSCQueue<uint64_t>::open(child_region.data(), child_region.size());
        
        for (uint64_t i = 0; i < N; ++i) {
            while (!child_q.try_push(i)) {
                // spin
            }
        }
        ::_exit(0);
    }
    
    // Parent = Consumer 
    uint64_t expected = 0;
    while (expected < N) {
        uint64_t val;
        if (q.try_pop(val)) {
            ASSERT_EQ(val, expected)
            << "cross-process sequence break at " << expected;
            ++expected;
        }
    }
    
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}


// ─── Test Large Message types ────────────────────

struct alignas(64) BigMessage {
    uint64_t id;
    char payload[256 - sizeof(uint64_t)];
};
static_assert(std::is_trivially_copyable_v<BigMessage>);
static_assert(alignof(BigMessage) <= z_ipc::kCacheLineSize);

TEST_F(SPSCQueueTest, LargeMessageType) {
    constexpr std::size_t cap = 64;
    auto sz     = SPSCQueue<BigMessage>::required_size(cap);
    auto region = SharedRegion::create(kShmName, sz);
    auto q      = SPSCQueue<BigMessage>::create(region.data(), region.size(), cap);

    BigMessage msg{};
    msg.id = 12345;
    std::memset(msg.payload, 'A', sizeof(msg.payload));

    ASSERT_TRUE(q.try_push(msg));

    BigMessage out{};
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.id, 12345u);
    EXPECT_EQ(out.payload[0], 'A');
    EXPECT_EQ(out.payload[sizeof(out.payload) - 1], 'A');
}