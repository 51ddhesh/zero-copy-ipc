// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)


/*
    z_ipc/common.hpp
    Shared common utils
*/

#pragma once


#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace z_ipc {

// Hardware Constants
// x86_64 cache line. Two atomics on the same cache line -> false sharing
inline constexpr std::size_t kCacheLineSize = 64;

// Page utilities
// Query once from the OS and cache it
inline std::size_t page_size() noexcept {
    static const std::size_t ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return ps;
}

// Round n up to the nearest multiple of the system page size
inline std::size_t align_to_page(std::size_t n) noexcept {
    const std::size_t ps = page_size();
    return (n + ps - 1) & ~(ps - 1);
}

// Arithmetic helper
inline constexpr bool is_power_of_two(std::size_t n) noexcept {
    return n > 0 && (n & (n - 1)) == 0;
}

} // namespace z_ipc
