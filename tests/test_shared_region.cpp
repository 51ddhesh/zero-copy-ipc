// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include "ipc/shared_region.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <sys/wait.h>

using z_ipc::SharedRegion;


class SharedRegionTest : public ::testing::Test {
protected:
    static constexpr const char* kName  = "/z_ipc_test_a";
    static constexpr const char* kName2 = "/z_ipc_test_b";

    void SetUp() override {
        // Remove leftovers from a prior crashed run.
        SharedRegion::unlink(kName);
        SharedRegion::unlink(kName2);
    }

    void TearDown() override {
        SharedRegion::unlink(kName);
        SharedRegion::unlink(kName2);
    }
};

TEST_F(SharedRegionTest, CreateAndDestroy) {
    {
        auto r = SharedRegion::create(kName, 4096);
        EXPECT_NE(r.data(), nullptr);
        EXPECT_GE(r.size(), 4096u);
        EXPECT_EQ(r.name(), kName);
        EXPECT_TRUE(static_cast<bool>(r));
    }
    // Mapping is gone, but the named object persists until unlink.
    // The TearDown will clean it.
}


TEST_F(SharedRegionTest, ZeroInitialized) {
    auto r = SharedRegion::create(kName, 4096);
    auto* p = static_cast<const uint8_t*>(r.data());
    for (std::size_t i = 0; i < r.size(); ++i) {
        ASSERT_EQ(p[i], 0) << "byte " << i << " not zero";
    }
}

TEST_F(SharedRegionTest, ReadWrite) {
    auto r = SharedRegion::create(kName, 8192);
    auto* p = static_cast<uint8_t*>(r.data());

    for (std::size_t i = 0; i < r.size(); ++i) {
        p[i] = static_cast<uint8_t>(i & 0xFF);
    }
    for (std::size_t i = 0; i < r.size(); ++i) {
        ASSERT_EQ(p[i], static_cast<uint8_t>(i & 0xFF))
            << "mismatch at byte " << i;
    }
}


TEST_F(SharedRegionTest, CreateThenOpen) {
    auto creator = SharedRegion::create(kName, 4096);

    constexpr const char kMsg[] = "hello z_ipc";
    std::memcpy(creator.data(), kMsg, sizeof(kMsg));

    auto joiner = SharedRegion::open(kName, 4096);
    EXPECT_EQ(std::memcmp(joiner.data(), kMsg, sizeof(kMsg)), 0);
}


TEST_F(SharedRegionTest, SharedVisibility) {
    auto a = SharedRegion::create(kName, 4096);
    auto b = SharedRegion::open(kName, 4096);

    // Write through mapping A
    auto* pa = a.as<uint64_t>();
    pa[0] = 0xCAFEBABE;

    // Read through mapping B — same physical page
    auto* pb = b.as<volatile uint64_t>();
    EXPECT_EQ(pb[0], 0xCAFEBABE);
}


TEST_F(SharedRegionTest, CrossProcess) {
    auto region = SharedRegion::create(kName, 4096);
    auto* data  = region.as<volatile uint64_t>();
    data[0] = 0;  // parent writes initial value

    pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork failed: " << strerror(errno);

    if (pid == 0) {
        // Child process
        // Open an independent mapping to the same segment.
        auto child_region = SharedRegion::open(kName, 4096);
        auto* child_data  = child_region.as<volatile uint64_t>();
        child_data[0] = 0xDEADBEEFull;
        child_data[1] = 42;
        ::_exit(0);
    }

    // Parent process
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    // Parent sees child's writes.
    EXPECT_EQ(data[0], 0xDEADBEEFull);
    EXPECT_EQ(data[1], 42u);
}


TEST_F(SharedRegionTest, DuplicateCreateFails) {
    auto r = SharedRegion::create(kName, 4096);
    EXPECT_THROW(SharedRegion::create(kName, 4096), std::system_error);
}

TEST_F(SharedRegionTest, OpenNonexistentFails) {
    EXPECT_THROW(
        SharedRegion::open("/z_ipc_does_not_exist", 4096),
        std::system_error);
}


TEST_F(SharedRegionTest, SizeAlignedToPage) {
    auto r = SharedRegion::create(kName, 1);  // ask for 1 byte
    EXPECT_GE(r.size(), z_ipc::page_size());
    EXPECT_EQ(r.size() % z_ipc::page_size(), 0u);
}

TEST_F(SharedRegionTest, ZeroSizeGetsOnePage) {
    auto r = SharedRegion::create(kName, 0);
    EXPECT_EQ(r.size(), z_ipc::page_size());
}


TEST_F(SharedRegionTest, MoveConstruct) {
    auto original = SharedRegion::create(kName, 4096);
    void* ptr  = original.data();
    auto  sz   = original.size();

    auto moved = std::move(original);

    EXPECT_EQ(moved.data(), ptr);
    EXPECT_EQ(moved.size(), sz);
    EXPECT_TRUE(static_cast<bool>(moved));

    // Moved-from object is empty.
    EXPECT_EQ(original.data(), nullptr);  // NOLINT (use-after-move is intentional)
    EXPECT_EQ(original.size(), 0u);       // NOLINT
    EXPECT_FALSE(static_cast<bool>(original)); // NOLINT
}

TEST_F(SharedRegionTest, MoveAssign) {
    auto a = SharedRegion::create(kName, 4096);
    auto b = SharedRegion::create(kName2, 8192);
    void* b_ptr = b.data();

    a = std::move(b);
    // a's old mapping is released (munmap'd).
    // a now owns b's mapping.
    EXPECT_EQ(a.data(), b_ptr);
    EXPECT_FALSE(static_cast<bool>(b)); // NOLINT
}

TEST_F(SharedRegionTest, TypedAccess) {
    struct Header {
        uint64_t magic;
        uint64_t version;
        uint64_t count;
    };

    auto r = SharedRegion::create(kName, sizeof(Header));
    auto* h = r.as<Header>();
    h -> magic   = 0x5A495043;  // "ZIPC"
    h -> version = 1;
    h -> count   = 999;

    // const access
    const auto& cr = r;
    const auto* ch = cr.as<Header>();
    EXPECT_EQ(ch -> magic,   0x5A495043u);
    EXPECT_EQ(ch -> version, 1u);
    EXPECT_EQ(ch -> count,   999u);
}

TEST_F(SharedRegionTest, NameMustStartWithSlash) {
    EXPECT_THROW(SharedRegion::create("no_slash", 4096),
                 std::invalid_argument);
}

TEST_F(SharedRegionTest, NameTooShort) {
    EXPECT_THROW(SharedRegion::create("/", 4096),
                 std::invalid_argument);
}

TEST_F(SharedRegionTest, UnlinkThenRecreate) {
    {
        auto r = SharedRegion::create(kName, 4096);
        r.as<uint64_t>()[0] = 111;
    }

    EXPECT_TRUE(SharedRegion::unlink(kName));

    // Now we can create again with the same name.
    auto r2 = SharedRegion::create(kName, 4096);
    // Fresh region -> zero-initialized, old value is gone.
    EXPECT_EQ(r2.as<uint64_t>()[0], 0u);
}

TEST_F(SharedRegionTest, PrefaultLargeRegion) {
    // 16 MB - enough pages to be meaningful
    auto r = SharedRegion::create(kName, 16 * 1024 * 1024);
    EXPECT_NO_THROW(r.prefault());
}