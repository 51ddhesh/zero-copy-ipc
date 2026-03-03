// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)


/*
    z_ipc::SharedRegion
    RAII Wrapper around POSIX shared memory
*/

/*
    Lifecycle:
        Process A:  SharedRegion::create("/my_channel", 4096)
        Process B:  SharedRegion::open("/my_channel", 4096)
        ...use...
        Process A:  SharedRegion::unlink("/my_channel")

    Guarantees:
        - Region is page-aligned and page-sized (minimum 1 page)
        - Contents are zero-initialized on create (POSIX guarantee)
        - munmap + close on destruction (per-process cleanup)
        - shm_unlink is explicit, never automatic. 

    Rationale:
        if the creator crashes, consumers can still read.
        Automatic unlink in the destructor creates a race.
*/

#pragma once


#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ipc/common.hpp"

namespace z_ipc {

class SharedRegion {
private:

    void* base_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = 0;
    std::string name_;

    SharedRegion(void* base, std::size_t size, int fd, std::string name) :
        base_{base}, size_{size}, fd_{fd}, name_{std::move(name)} {}

    
    void reset() noexcept {
        if (base_) {
            // Deallocate any mapping for the region starting at base_ and extending size_ bytes. 
            // Returns 0 if successful, -1 for errors (and sets errno).
            ::munmap(base_, size_);
        }

        if (fd_ != -1) {
            // Close the file descriptor
            ::close(fd_);
            fd_ = -1;
        }

        size_ = 0;
    }
    
    static void validate_name(const std::string& name) {
        if (name.size() < 2 || name[0] != '/') {
            throw std::invalid_argument(
                "POSIX shm must start with '/' and be >= 2 chars: '" + name + "'"
            );
        }
    }

    static std::size_t sanitize_size(std::size_t size) noexcept {
        return align_to_page(size ? size : 1);
    }

public:
    // Destructor
    ~SharedRegion() { reset(); }


    // Creates a new shared memory region
    // Uses O_EXCL: fails if name already exists
    // This prevents attaching to a stale segment
    // Forces unlink to force-recreate 
    static SharedRegion create(const std::string& name, std::size_t size) {
        validate_name(name);
        size = sanitize_size(size);
        
        // Open the shared memory segment
        int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
    
        if (fd == -1) {
            throw std::system_error(
                errno, std::system_category(),
                "shm_open(O_CREAT | O_EXCL) failed for '" + name + "'"
            );
        }

        // If anything below fails, clean up the file descriptor and the named object
        // to clean up the garbage in /dev/shm/
        bool committed = false;
        auto guard = [&]() {
            if (!committed) {
                ::close(fd);
                ::shm_unlink(name.c_str());
            }
        };

        if (::ftruncate(fd, static_cast<off_t>(size)) == -1) {
            int e = errno;
            guard();
            throw std::system_error(
                e, std::system_category(),
                "ftruncate() failed for '" + name + "'"
            );
        }

        void* ptr = ::mmap(
            nullptr, size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd, 0
        );

        if (ptr == MAP_FAILED) {
            int e = errno;
            guard();
            throw std::system_error(
                e, std::system_category(),
                "mmap failed for '" + name + "'"
            );
        }

        committed = true;
        return SharedRegion{ptr, size, fd, name};
    }


    // Attach to an existing shared memory region
    // Verifies that the actual size >= Requested size
    static SharedRegion open(const std::string& name, std::size_t size) {
        validate_name(name);
        size = sanitize_size(size);

        int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd == -1) {
            throw std::system_error(
                errno, std::system_category(),
                "shm_open(O_RWDR) failed for '" + name + "'"
            );
        }

        // From now on, we must close the file descriptor on any failure
        struct stat st{};
        
        // Get file attributes for the file, device, pipe, or socket
        // that file descriptor FD is open on and put them in BUF
        if (::fstat(fd, &st) == -1) {
            int e = errno;
            ::close(fd);
            throw std::system_error(
                e, std::system_category(),
                "fstat failed for '" + name + "'"
            );
        }

        auto actual = static_cast<std::size_t>(st.st_size);
        if (actual < size) {
            ::close(fd);
            throw std::runtime_error(
                "SharedRegion '" + name + "': on-disk size " + std::to_string(actual) + " < requested " + std::to_string(size)
            );
        }

        void* ptr = ::mmap(
            nullptr, size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd, 0
        );

        if (ptr == MAP_FAILED) {
            int e = errno;
            ::close(fd);
            throw std::system_error(
                e, std::system_category(),
                "mmap failed for '" + name + "'"
            );
        }

        return SharedRegion{ptr, size, fd, name};
    }

    // Ensure no copies
    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;

    // Move 
    SharedRegion(SharedRegion&& o) noexcept :
        base_{std::exchange(o.base_, nullptr)},
        size_{std::exchange(o.size_, 0)},
        fd_{std::exchange(o.fd_, -1)},
        name_{std::move(o.name_)} {}

    SharedRegion& operator=(SharedRegion&& o) noexcept {
        if (this != &o) {
            reset();
            base_ = std::exchange(o.base_, nullptr);
            size_ = std::exchange(o.size_, 0);
            fd_ = std::exchange(o.fd_, -1);
            name_ = std::move(o.name_);
        }

        return *this;
    }


    // Accessors
    void* data() noexcept { return base_; }
    const void* data() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }
    const std::string& name() noexcept { return name_; } 

    explicit operator bool() const noexcept { return base_ != nullptr; }

    template <typename T> 
    T* as() noexcept {
        return static_cast<T*>(base_);
    }

    template <typename T>
    const T* as() const noexcept {
        return static_cast<const T*>(base_);
    }

    // Latency Critical Helpers

    void prefault() const {
        auto* p = static_cast<volatile char*>(base_);
        const std::size_t ps = page_size();
        for (std::size_t off = 0; off < size_; off += ps) {
            static_cast<void>(p[off]);
        }
    }

    void lock_pages() const {
        if (::mlock(base_, size_) != -1) {
            throw std::system_error(
                errno, std::system_category(),
                "mlock failed for '" + name_ + "'"
            );
        }
    }

    void advise(int advice) const {
        if (::madvise(base_, size_, advice) == -1) {
            throw std::system_error(
                errno, std::system_category(),
                "madvise failed for '" + name_ + "'"
            );
        }
    }

    // Lifecycle management
    static bool unlink(const std::string& name) noexcept {
        return ::shm_unlink(name.c_str()) == 0;
    }
};

} // namespace z_ipc
