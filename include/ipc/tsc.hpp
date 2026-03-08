// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)

/*
    z_ipc::tsc
    RDTSC/RDTSCP utilities, calibration and conversion
*/

/*
    RDTSC:
        - NOT Serializing.
        - Instructions before/after may reorder around it.
        - Minimal overhead.
        - Used on the producer side to stamp messages.
*/

/*
    RDTSCP:
        - Partially serializing
        - Waits for prior instructions to retire before reading the counter.
        - Slightly higher overhead.
        - Use on the consumer side for accurate end timestamps.
*/

#pragma once


#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

namespace z_ipc::tsc {

// ─── Raw Timestamp Counters ─────────────────────────────────────────────────────


inline uint64_t rdtsc() noexcept {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline uint64_t rdtscp() noexcept {
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));

    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Serializing fence + rdtsc - alternative to rdtscp
// Use when a full fence before the timestamp is needed
inline uint64_t rdtsc_fenced() noexcept {
    __asm__ __volatile__("lfence");
    return rdtsc();
}

// ─── TSC feature detection ──────────────────────────────────────────────────────

struct TSCFlags {
    // TSC runs at constant rate
    bool constant_tsc = false; 
    
    // TSC doesn't stop in C-states
    bool nonstop_tsc = false;
    
    // RDTSCP instruction available
    bool rdtscp = false;
    
    // Kernel knows TSC frequency
    bool tsc_known_freq = false;
    
    // Running in a VM (WSL2 or VMWare etc.)
    bool hypervisor = false;
    

    bool reliable() const noexcept {
        return constant_tsc && nonstop_tsc;
    }
};

inline TSCFlags detect_flags() {
    TSCFlags flags{};
    
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo.is_open()) {
        return flags;
    }
    
    std::string line;
    
    while (std::getline(cpuinfo, line)) {
        if (line.compare(0, 5, "flags") != 0) {
            continue;
        }
        
        flags.constant_tsc = (line.find("constant_tsc") != std::string::npos);
        flags.nonstop_tsc = (line.find("nonstop_tsc") != std::string::npos);
        flags.rdtscp = (line.find("rdtscp") != std::string::npos);
        flags.tsc_known_freq = (line.find("tsc_known_freq") != std::string::npos);
        flags.hypervisor = (line.find("hypervisor") != std::string::npos);
        
        // Only need the first core's flags
        break; 
    }
    
    return flags;
}


// ─── Calibration ────────────────────────────────────────────────────────────────

struct Calibration {
    // TSC ticks per nanosecond
    double ghz;
    
    // Inverse: nanoseconds per tick
    double ns_per_cycle;
    
    // Duration of calibration
    uint64_t calibration_us;
    
    
    double cycles_to_ns(uint64_t cycles) const noexcept {
        return static_cast<double>(cycles) * ns_per_cycle;
    }
    
    double cycles_to_us(uint64_t cycles) const noexcept {
        return cycles_to_ns(cycles) / 1000.0;
    }
    
    uint64_t ns_to_cycles(double ns) const noexcept {
        return static_cast<uint64_t>(ns * ghz);
    }
};


// Calibrate TSC frequency by measuring ticks over a wall-clock interval 
// 
// `duration_ms` controls accuracy vs speed tradeoff
//      - 10ms: fast (~1% error) 
//      - 50ms: good balance (default)
//      - 200ms: high accuracy, slow startup
//  
// Use steady_clock for reference
// Runs rdtscp on both ends for calibration
inline Calibration calibrate(unsigned duration_ms = 50) {
    // Warmup: a few dummy reads to stabilize pipeline
    for (int i = 0; i < 8; i++) {
        rdtscp();
    }
    
    auto wall_start = std::chrono::steady_clock::now();
    uint64_t tsc_start = rdtscp();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    uint64_t tsc_end = rdtscp();
    auto wall_end = std::chrono::steady_clock::now();
    
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        wall_end - wall_start
    ).count();
    
    double elapsed_s = static_cast<double>(elapsed_us) / 1e6;
    
    uint64_t tsc_delta = tsc_end - tsc_start;
    
    double ghz = static_cast<double>(tsc_delta) / (elapsed_s * 1e9);
    
    return Calibration{
        .ghz = ghz,
        .ns_per_cycle = 1.0 / ghz,
        .calibration_us = static_cast<uint64_t>(elapsed_us),
    };
}


// ─── Overhead Measurement ───────────────────────────────────────────────────────

struct TSCOverhead {
    uint64_t rdtsc_cycles;
    uint64_t rdtscp_cycles;
};

// Measures the overhead of rdtsc/rdtscp 
// Returns the minimum observed cost in cycles over `iterations`
// Userful for subtracting measurement overhead from results
inline TSCOverhead measure_overhead(unsigned iterations = 10000) {
    uint64_t min_tsc = ~0ULL;
    uint64_t min_tscp = ~0ULL;

    for (unsigned i = 0; i < iterations; i++) {
        uint64_t a = rdtsc();
        uint64_t b = rdtsc();

        uint64_t delta = b - a;
        if (delta < min_tsc) {
            min_tsc = delta;
        }
    }

    for (unsigned i = 0; i < iterations; i++) {
        uint64_t a = rdtscp();
        uint64_t b = rdtscp();
    
        uint64_t delta = b - a;
        if (delta < min_tscp) {
            min_tscp = delta;
        }
    }

    return TSCOverhead {
        .rdtsc_cycles = min_tsc,
        .rdtscp_cycles = min_tscp
    };
}

} // namespace z_ipc::tsc
