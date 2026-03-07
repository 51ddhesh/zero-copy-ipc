## Zero Copy Inter-Process Communication

WIP...

---

### Run Tests and Benchmarks

- Build
```bash
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

- Test the shared region

```bash
./build/tests/test_shared_region
```

- Test the Single Producer Single Consumer Queue (`SPSCQueue`)
```bash
./build/tests/test_spsc_queue
```
- Run the SPSC Benchmark
```bash
./build/benchmarks/benchmark_spsc_queue
```

### Results

- v1

```
z_ipc SPSC Benchmark
────────────────────────────────────────────────────────────────────────
Messages: 10000000
Queue Capacity: 65536
Message size: 16 bytes
Shared Memory: 1048768 bytes (1.00 MB)
────────────────────────────────────────────────────────────────────────
Checking TSC flags...
TSC: constant_tsc + nonstop_tsc found
TSC Frequency: 3.094 GHz
────────────────────────────────────────────────────────────────────────
Throughput
  Elapsed  : 0.355 s
  Messages : 10000000
  Rate     : 28.19 M msg/sec
  Bandwidth: 430.11 MB/sec

╔════════════════════════════════════════════╗
║         SPSC Latency Distribution          ║
╠════════════════════════════════════════════╣
║  min    :      248 cycles  (   80.2 ns)    ║
║  mean   :  7156577 cycles  (2313130.9 ns)  ║
║  median :  7193364 cycles  (2325021.2 ns)  ║
║  p99    :  8950630 cycles  (2893000.3 ns)  ║
║  p999   : 10491904 cycles  (3391167.0 ns)  ║
║  max    : 10530406 cycles  (3403611.6 ns)  ║
║  stddev :   779666 cycles  (252001.8 ns)   ║
╚════════════════════════════════════════════╝
```

- The results are from Ryzen 7 7435HS, running on WSL2 Ubuntu.
- Expect a 2x to 3x speed-up on native Linux

### NOTICE
This project is licensed under the **Boost Software License 1.0 (BSL 1)**. See the [LICENSE](./LICENSE) file for the full text, or visit the official [Boost Software License page](https://www.boost.org/LICENSE_1_0.txt).

