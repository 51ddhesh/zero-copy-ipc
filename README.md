## Zero Copy Inter-Process Communication

WIP...

---

### Run Tests

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

### NOTICE
This project is licensed under the **Boost Software License 1.0 (BSL 1)**. See the [LICENSE](./LICENSE) file for the full text, or visit the official [Boost Software License page](https://www.boost.org/LICENSE_1_0.txt).

