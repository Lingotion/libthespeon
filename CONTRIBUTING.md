# Contributing

See [`src/core/README.md`](src/core/README.md) for how `core` is laid out.

## Tests

Unit tests for `core` live in [`tests/`](tests/) and use GoogleTest:

```bash
cmake --build build --target thespeon_tests
ctest --test-dir build --output-on-failure
```

Coverage uses Clang's source-based instrumentation (`THESPEON_COVERAGE=ON`) and reports on `src/core` only. The `coverage` target runs the tests, prints a summary, and writes an HTML report to `build-coverage/coverage/html/`:

```bash
cmake --preset coverage                 # macOS, Linux (clang)
cmake --build --preset coverage

cmake --preset coverage-windows         # Visual Studio with the ClangCL toolset
cmake --build --preset coverage-windows
```

On Linux, select clang with `CC=clang CXX=clang++`. On Windows, install the "C++ Clang tools for Windows" Visual Studio component; `llvm-cov` and `llvm-profdata` are found beside `clang-cl`.

## Profiling

```bash
cmake --preset profile
cmake --build --preset profile --parallel
tracy-capture -o run.tracy &
./build-profile/thespeon bench --input-file input.json --repeats 5
tracy-profiler run.tracy
```

Build `tracy-capture` and `tracy-profiler` from the same Tracy release as the client (0.14.1); mismatched versions refuse to connect. The Statistics window lists per-zone call count and total, mean, and max time. Each `bench` synthesis is marked as a `synthesis` frame.

Code is instrumented only through `THESPEON_PROFILE_*` ([`src/core/utils/profile.h`](src/core/utils/profile.h)) and `METAGRAPH_PROFILE_*` (`metagraph-cpp/src/metagraph_profile.h`). To change backend, add a branch to both headers and the new value to `THESPEON_PROFILER` in this project and `METAGRAPH_PROFILER` in `metagraph-cpp`, which fetches the backend.