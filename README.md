## Zero Copy Inter-Process Communication

WIP...

---

### Run Tests

- Test the shared region

```bash
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/tests/test_shared_region
```


