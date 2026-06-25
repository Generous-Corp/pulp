# Building Pulp

## Prerequisites

- CMake 3.24+
- C++20 compiler (Clang 15+, GCC 13+, MSVC 2022+)
- git-lfs (for Skia binaries)
- macOS: Xcode Command Line Tools
- Linux: `libasound2-dev` and desktop dev packages
- Windows: Visual Studio 2022+ or Build Tools

## Quick Build

```bash
./setup.sh                                    # bootstrap deps
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release  # configure
cmake --build build -j$(nproc)                # build
ctest --test-dir build --output-on-failure    # test
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `PULP_BUILD_TESTS` | ON | Build test targets |
| `PULP_ENABLE_GPU` | ON | Enable Dawn/Skia GPU rendering |
| `PULP_ENABLE_JS` | ON | Build the JS scripting + web-compat layer (`pulp::view-script`). `OFF` yields a native-only `pulp::view` with no JS engine — smaller binaries, and the native faithful-vector design-import path still works. |
| `PULP_JS_ENGINE` | auto | JS engine: auto, quickjs, jsc, v8 (only when `PULP_ENABLE_JS=ON`) |
| `PULP_ENABLE_AAX` | OFF | Enable optional AAX format |

### Native-only builds (`-DPULP_ENABLE_JS=OFF`)

When `PULP_ENABLE_JS=OFF`, `pulp::view` links only the JS-free `pulp::view-core`;
the scripting layer (`pulp::view-script` — JS engine + web-compat + widget
bridge) is neither built nor linked, so a standalone built on `pulp::view`
carries zero JS-engine symbols. Use it for apps that drive the UI through the
native view API or the faithful-vector design-import lane (native Skia
`draw_svg` + native overlays) rather than JS-scripted UIs. The
`pulp-design-import-standalone` example carries a build-time `nm` assertion
(`tools/cmake/AssertNoJsSymbols.cmake`) that fails the build if a native-only
binary ever links the JS layer. Consumer C++ can gate optional JS use on the
`PULP_ENABLE_JS` compile definition (`PUBLIC` on `pulp::view-core`).

## Platform-Specific Notes

See [macOS](platform-macos.html), [Windows](platform-windows.html), [Linux](platform-linux.html).
