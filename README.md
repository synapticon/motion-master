# Motion Master

Next-generation motion control software featuring a simplified design, streamlined HTTP API, and true real-time support. See [NEXTGEN.md](NEXTGEN.md) for the full design specification.

## Prerequisites

- CMake 4.0+
- Ninja
- GCC / Clang with C++23 support (or MSVC on Windows)
- Git

## Getting Started

```bash
git clone <repo-url>
cd motion-master
git submodule update --init --recursive

./tools/configure.sh
./tools/build.sh
```

The `motion-master` binary lands in `build/x64-linux-debug/apps/motion_master/`.

## Usage

```
motion-master [OPTIONS]

  -h, --help                    Print this help message and exit
      --version                 Display program version and exit
  -c, --config TEXT:FILE        Path to JSON config file
  -p, --port UINT [8443]        HTTP/WebSocket port
  -d, --driver TEXT [soem]      Fieldbus driver: soem, spoe, igh
  -l, --log-level TEXT [info]   Log level: trace, debug, info, warn, error
```

## Developer Scripts

All scripts default to the `x64-linux-debug` preset. Pass a preset name as the first argument to override (e.g. `./tools/build.sh x64-linux-release`).

| Script | Description |
|---|---|
| `./tools/configure.sh` | Run CMake configure |
| `./tools/build.sh` | Build all targets |
| `./tools/test.sh` | Run tests |
| `./tools/format.sh` | Auto-format all sources with clang-format |
| `./tools/lint.sh` | Run cpplint (`pip install cpplint` if missing) |
| `./tools/cppcheck.sh` | Run cppcheck static analysis |
| `./tools/clean.sh` | Remove the build directory |

## Dependencies

Managed via [vcpkg](https://vcpkg.io). No manual installation needed — vcpkg downloads and builds everything on first configure.

| Package | Purpose |
|---|---|
| CLI11 | Command-line argument parsing |
| spdlog | Structured logging |
| nlohmann-json | JSON config file parsing |
| neargye-semver | Semantic versioning |
| GTest | Unit testing |

## Platform Support

| Platform | Status |
|---|---|
| Linux x86-64 | Primary target |
| Linux ARM | Planned |
| Windows x64 | Planned |
