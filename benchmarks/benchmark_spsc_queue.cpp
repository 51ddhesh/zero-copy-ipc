// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)

/*
    z_ipc Single Producer Single Consumer Queue benchmark
    
    Measures
        1. Throughput (million msgs/sec)
        2. Latency distribution using RDTSC cycle counts
*/

/*
    Usage:
        ./benchmark_spsc_queue [num_messages] [queue_capacity]

    Defaults:
        10M messages
        64K capacity
*/


#include "ipc/common.hpp"   
#include "ipc/shared_region.hpp"
#include "ipc/spsc_queue.hpp"

#include <vector>
#include <thread>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <numeric>
#include <pthread.h>
#include <string>

using z_ipc::SharedRegion;
using z_ipc::SPSCQueue;
using z_ipc::rdtsc;
using z_ipc::rdtscp;



// ─── Configuration ─────────────────────────────────────────────────────────────
static constexpr const char* kShmName = "/z_ipc_benchmark_spsc";
static constexpr std::size_t kDefaultN = 10'000'000;
static constexpr std::size_t kDefaultCap = 65536; // 64K


// ─── Benchmark Message ─────────────────────────────────────────────────────────
// Kept small (16 bytes) to measure queue overhead, not memcpy

struct BenchmarkMsg {
    uint64_t sequence;
    uint64_t tsc_publish; // RDTSC at push time
};

static_assert(sizeof(BenchmarkMsg) == 16);
static_assert(std::is_trivially_copyable_v<BenchmarkMsg>);



// ─── Utility ───────────────────────────────────────────────────────────────────


/// Creates thread affinity -> pin or bind a specific execution thread to a designated CPU core
static bool pin_thread(int cpu) {
    // Create a bitmask to represent a set of CPUs
    cpu_set_t cpuset;
    // Initialize the bitmask by setting it to zero 
    CPU_ZERO(&cpuset);
    // Modify the bitmask to the specific `cpu`
    CPU_SET(cpu, &cpuset);
    // apply the mask to current thread 
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
}


static bool check_tsc_flags() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    
    if (!cpuinfo) {
        return false;
    }
    
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.find("flags") != std::string::npos) {
            bool constant = line.find("constant_tsc") != std::string::npos;
            bool nonstop = line.find("nonstop_tsc") != std::string::npos;
            
            return constant && nonstop;
        }
    }
    
    return false;
}

static double estimate_tsc_freq_ghz() {
    // Calibrate
    // measure tsc ticks over a known wall-clock interval
    
    auto t0 = std::chrono::steady_clock::now();
    auto c0 = rdtscp();
    
    // Busy-wait for ~50 ms
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto c1 = rdtscp();
    auto t1 = std::chrono::steady_clock::now();
    
    double seconds = std::chrono::duration<double> (t1 - t0).count();
    double ticks = static_cast<double>(c1 - c0);
    
    return ticks / seconds / 1e9;
}



// ─── Latency Statistics ────────────────────────────────────────────────────────

struct LatencyStats {
    uint64_t min_cycles;
    uint64_t median_cycles;
    uint64_t p99_cycles;
    uint64_t p999_cycles;
    uint64_t max_cycles;
    double mean_cycles;
    double stddev_cycles;
};

static LatencyStats compute_stats(std::vector<uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    std::size_t n = samples.size();
    
    double sum = 0;
    for (auto s : samples) {
        sum += static_cast<double>(s);
    }
    
    double mean = sum / static_cast<double>(n);
    
    double var_sum = 0;
    for (auto s : samples) {
        double d = static_cast<double>(s) - mean;
        var_sum += d * d;
    }
    
    double stddev = std::sqrt(var_sum / static_cast<double>(n));
    
    return LatencyStats {
        .min_cycles = samples[0],
        .median_cycles = samples[n / 2],
        .p99_cycles = samples[static_cast<std::size_t>(n * 0.99)],
        .p999_cycles = samples[static_cast<std::size_t>(n * 0.999)],
        .max_cycles = samples[n - 1],
        .mean_cycles = mean,
        .stddev_cycles = stddev
    };
}

// static void print_stats(const LatencyStats& s, double ghz) {
//     auto to_ns = [ghz](double cycles) { return cycles / ghz; };
    
//     std::printf("\n╔══════════════════════════════════════════╗\n");
//     std::printf("║       SPSC Latency Distribution          ║\n");
//     std::printf("╠══════════════════════════════════════════╣\n");
//     std::printf("║  min    : %8lu cycles  (%7.1f ns)    ║\n", s.min_cycles,    to_ns(s.min_cycles));
//     std::printf("║  mean   : %8.0f cycles  (%7.1f ns)    ║\n", s.mean_cycles,   to_ns(s.mean_cycles));
//     std::printf("║  median : %8lu cycles  (%7.1f ns)    ║\n", s.median_cycles, to_ns(s.median_cycles));
//     std::printf("║  p99    : %8lu cycles  (%7.1f ns)    ║\n", s.p99_cycles,    to_ns(s.p99_cycles));
//     std::printf("║  p999   : %8lu cycles  (%7.1f ns)    ║\n", s.p999_cycles,   to_ns(s.p999_cycles));
//     std::printf("║  max    : %8lu cycles  (%7.1f ns)    ║\n", s.max_cycles,    to_ns(s.max_cycles));
//     std::printf("║  stddev : %8.0f cycles  (%7.1f ns)    ║\n", s.stddev_cycles, to_ns(s.stddev_cycles));
//     std::printf("╚══════════════════════════════════════════╝\n");
// }

static void print_stats(const LatencyStats& s, double ghz) {
    auto to_ns = [ghz](double cycles) { return cycles / ghz; };

    // Helper to format unsigned integers safely
    auto format_u =[](const char* label, uint64_t cycles, double ns) {
        char buf[128];
        // Cast to unsigned long long for cross-platform %llu safety
        std::snprintf(buf, sizeof(buf), "%-6s : %8llu cycles  (%7.1f ns)", 
                      label, static_cast<unsigned long long>(cycles), ns);
        return std::string(buf);
    };

    // Helper to format doubles (mean/stddev)
    auto format_f =[](const char* label, double cycles, double ns) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%-6s : %8.0f cycles  (%7.1f ns)", 
                      label, cycles, ns);
        return std::string(buf);
    };

    // 1. Pre-format all rows
    std::vector<std::string> rows = {
        format_u("min",    s.min_cycles,    to_ns(s.min_cycles)),
        format_f("mean",   s.mean_cycles,   to_ns(s.mean_cycles)),
        format_u("median", s.median_cycles, to_ns(s.median_cycles)),
        format_u("p99",    s.p99_cycles,    to_ns(s.p99_cycles)),
        format_u("p999",   s.p999_cycles,   to_ns(s.p999_cycles)),
        format_u("max",    s.max_cycles,    to_ns(s.max_cycles)),
        format_f("stddev", s.stddev_cycles, to_ns(s.stddev_cycles))
    };

    // 2. Determine the maximum width required
    std::string title = "SPSC Latency Distribution";
    std::size_t max_len = title.length();
    for (const auto& row : rows) {
        if (row.length() > max_len) {
            max_len = row.length();
        }
    }

    // Add 4 characters of breathing room inside the box
    std::size_t inner_width = max_len + 4; 

    // Helper to print standard borders
    auto print_border = [inner_width](const char* left, const char* mid, const char* right) {
        std::printf("%s", left);
        for (std::size_t i = 0; i < inner_width; ++i) std::printf("%s", mid);
        std::printf("%s\n", right);
    };

    // 3. Render the table
    std::printf("\n");
    print_border("╔", "═", "╗");

    // Center the title dynamically
    std::size_t title_pad_left = (inner_width - title.length()) / 2;
    std::size_t title_pad_right = inner_width - title.length() - title_pad_left;
    std::printf("║%*s%s%*s║\n", (int)title_pad_left, "", title.c_str(), (int)title_pad_right, "");

    print_border("╠", "═", "╣");

    // Print rows dynamically padded to the right
    for (const auto& row : rows) {
        // We use 2 spaces on the left, so subtract 2 from remaining right padding
        std::size_t right_pad = inner_width - 2 - row.length();
        std::printf("║  %s%*s║\n", row.c_str(), (int)right_pad, "");
    }

    print_border("╚", "═", "╝");
    std::printf("\n");
}

// ─── Main Benchmark ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::size_t num_messages = kDefaultN;
    std::size_t queue_cap = kDefaultCap;
    
    
    if (argc > 1) {
        num_messages = static_cast<std::size_t>(std::atoi(argv[1]));
    }
    
    if (argc > 2) {
        queue_cap = static_cast<std::size_t>(std::atoi(argv[2]));
    }
    
    // Ensure power of two
    if (!z_ipc::is_power_of_two(queue_cap)) {
        std::fprintf(stderr, "[SPSCQueue Benchmark]: Error: Capacity must be a power of two\n");
        return 1;
    }

    std::printf("z_ipc SPSC Benchmark\n");
    std::printf("────────────────────────────────────────────────────────────────────────\n");
    std::printf("Messages: %zu\n", num_messages);
    std::printf("Queue Capacity: %zu\n", queue_cap);
    std::printf("Message size: %zu bytes\n", sizeof(BenchmarkMsg));
    std::printf("Shared Memory: %zu bytes (%.2f MB)\n", 
        SPSCQueue<BenchmarkMsg>::required_size(queue_cap),
        SPSCQueue<BenchmarkMsg>::required_size(queue_cap) / (1024.0 * 1024.0)
    );
    
    std::printf("────────────────────────────────────────────────────────────────────────\n");
    std::printf("Checking TSC flags...\n");
    if (check_tsc_flags()) {
        std::printf("TSC: constant_tsc + nonstop_tsc found\n");
    } else {
        std::printf("TSC: WARNING. constant_tsc/nonstop_tsc not found\n");
        std::printf("              Latency likely to be unreliable...\n");
        
    }
    
    double tsc_ghz = estimate_tsc_freq_ghz();
    std::printf("TSC Frequency: %.3f GHz\n", tsc_ghz);
    std::printf("────────────────────────────────────────────────────────────────────────\n");
    
    
    // ─── Setup shared memory and queue ─────────────────────────────────────────────
    SharedRegion::unlink(kShmName);
    auto sz = SPSCQueue<BenchmarkMsg>::required_size(queue_cap);
    auto region = SharedRegion::create(kShmName, sz);
    region.prefault();
    
    [[maybe_unused]] auto queue = SPSCQueue<BenchmarkMsg>::create(region.data(), region.size(), queue_cap);
    
    // Storage for latency samples (filled by consumer)
    std::vector<uint64_t> latencies(num_messages);
    
    
    // ─── Determine CPU Cores ───────────────────────────────────────────────────────
    // Use CPU Core 1 and Core 3 
    int producer_cpu = 1;
    int consumer_cpu = 3;
    
    long n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    
    if (n_cpus < 4) {
        producer_cpu = 0;
        consumer_cpu = (n_cpus > 1) ? 1 : 0;
    }


    // ─── Consumer Thread ───────────────────────────────────────────────────────────
    // Start the consumer thread first so that it's ready when producer begins
    
    auto consumer_q = SPSCQueue<BenchmarkMsg>::open(region.data(), region.size());
    
    std::thread consumer([&consumer_q, &latencies, num_messages, consumer_cpu]() {
        pin_thread(consumer_cpu);
        
        for (std::size_t i = 0; i < num_messages; i++) {
            BenchmarkMsg msg{};
            while (!consumer_q.try_pop(msg)) {
                // spin
            }
            
            uint64_t now = rdtscp();
            latencies[i] = now - msg.tsc_publish;
        }
    });
    
    // briefly pause to let consumer thread start and pin
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // ─── Producer Thread ───────────────────────────────────────────────────────────
    // Runs on this thread
    auto producer_q = SPSCQueue<BenchmarkMsg>::open(region.data(), region.size());
    pin_thread(producer_cpu);

    auto wall_start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < num_messages; ++i) {
        BenchmarkMsg msg;
        msg.sequence    = i;
        msg.tsc_publish = rdtsc();  // non-serializing — fast
        while (!producer_q.try_push(msg)) {
            // spin
        }
    }

    consumer.join();
    auto wall_end = std::chrono::steady_clock::now();

    double elapsed_s = std::chrono::duration<double>(wall_end - wall_start).count();
    double msg_per_s = static_cast<double>(num_messages) / elapsed_s;

    std::printf("Throughput\n");
    std::printf("  Elapsed  : %.3f s\n", elapsed_s);
    std::printf("  Messages : %zu\n", num_messages);
    std::printf("  Rate     : %.2f M msg/sec\n", msg_per_s / 1e6);
    std::printf("  Bandwidth: %.2f MB/sec\n",
        msg_per_s * sizeof(BenchmarkMsg) / (1024.0 * 1024.0)
    );

    auto stats = compute_stats(latencies);
    print_stats(stats, tsc_ghz);

    SharedRegion::unlink(kShmName);

    return 0;
} 
