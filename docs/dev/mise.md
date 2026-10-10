# mise developer-tool profile

Pulp has an optional mise profile for contributors and fleet images on macOS,
Linux, and Windows. It gives local development a repeatable selection for the
host tools that are otherwise discovered through Homebrew, apt, winget, or the
system `PATH`.

The profile is deliberately layered over Pulp's existing contracts.
`tools/motion/visual/requirements.lock` remains the authority for Python
packages. Android SDK/NDK, the Gradle wrapper, Xcode and Apple SDKs, Rust's
`rust-toolchain.toml`, and the prebuilt Skia/Dawn/V8 artifacts remain managed by
their existing provisioning paths.

To opt in on a trusted checkout:

```sh
mise trust
mise run visual-deps
mise run visual-check
```

On Windows, use the explicitly named `visual-deps-windows` and
`visual-check-windows` tasks because the system interpreter is normally exposed
as `python` rather than `python3`.

Configure CMake with the interpreter selected by the profile when you want the
visual CTest lane to use it:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$(mise which python)"
```

The visual task delegates to Pulp's existing configured-interpreter installer,
which installs the existing hash-pinned requirements and handles PEP 668,
wheelhouses, and retries. It does not upgrade packages implicitly. Configure a
Release build first (or set `PULP_BUILD_DIR`) so the installer can use the same
Python that CTest launches. If the dependencies are absent, the required
`visual-python-deps-present` test continues to fail closed and names the
missing distributions.

Automatic updates are a reviewed maintenance operation. Do not enable mise's
global `auto_update` or `auto_install` settings for CI, release, or governed
build jobs. A future host-tool profile must use a generated cross-platform
`mise.lock`, pass local replay checks, then run the normal gates. Fleet images
may preinstall exact locked tools and publish their image digest plus tool
manifest; they must retain the Homebrew/apt/winget fallback until a platform
canary is green.

The profile is not used by iOS device builds, Android SDK provisioning, or
release packaging. Those lanes retain their platform-specific toolchain and
artifact provenance requirements.
