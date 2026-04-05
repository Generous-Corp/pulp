---
name: build
description: Build the Pulp project (configure + compile)
---

Build the Pulp project using the `pulp` CLI.

Run: `./build/tools/cli/pulp build`

If the CLI binary doesn't exist yet, configure and build it first:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Then run the full build via the CLI.

If the build fails, read the error output carefully and fix the issue. Common problems:
- Missing dependencies: run `./setup.sh` or `pulp doctor --fix`
- Stale CMake cache: run `pulp clean` then rebuild
- Skia/Dawn not found: run `pulp cache fetch skia`
