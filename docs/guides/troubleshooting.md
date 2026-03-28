# Troubleshooting

Common issues and solutions when building and using Pulp.

## First-Time Setup

### `cmake -B build` fails immediately

**Symptom:** CMake errors during configure, missing targets or directories.

**Fix:** Use `setup.sh` instead of raw CMake:
```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
./setup.sh
```

`setup.sh` handles all prerequisites: git-lfs, external SDK cloning, configure, build, and test.

### Skia headers are git-lfs pointers

**Symptom:** C++ compiler errors mentioning `version https://git-lfs.github.com/spec/v1` in header files, or Skia `.a` files that are only a few hundred bytes.

**Cause:** git-lfs wasn't installed or initialized before cloning.

**Fix:**
```bash
brew install git-lfs    # macOS
# or: sudo apt install git-lfs    # Linux

git lfs install
git lfs pull
```

Verify: check that files in `external/skia-build/` are actual binaries (multiple MB), not text pointers.

### External SDK symlinks are broken

**Symptom:** `external/vst3sdk` or `external/AudioUnitSDK` are broken symlinks.

**Fix:**
```bash
rm external/vst3sdk external/AudioUnitSDK  # remove broken links
./setup.sh                                  # re-clone properly
```

Or clone manually:
```bash
git clone --depth 1 --recursive --branch v3.7.12_build_20 \
    https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk

# macOS only:
git clone --depth 1 \
    https://github.com/apple/AudioUnitSDK.git external/AudioUnitSDK
```

## Environment Issues

### `pulp doctor` reports failures

Run `pulp doctor --fix` to auto-resolve what it can:

```bash
./build/tools/cli/pulp doctor --fix
```

Each failed check includes a fix command. Common ones:

| Check | Fix |
|-------|-----|
| C++20 compiler missing | `xcode-select --install` (macOS) or `sudo apt install g++-13` (Linux) |
| CMake too old | `brew upgrade cmake` or install from cmake.org |
| git-lfs missing | `brew install git-lfs && git lfs install` |
| VST3 SDK missing | `git clone --depth 1 --recursive https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk` |
| ALSA headers missing | `sudo apt install libasound2-dev` (Linux) |

### No C++20 support

**Symptom:** Compiler errors about `std::format`, `<concepts>`, designated initializers, or similar C++20 features.

**Fix:** Pulp requires Clang 15+, GCC 13+, or MSVC 2022+.

- **macOS:** Update Xcode Command Line Tools: `softwareupdate --install -a`
- **Linux:** `sudo apt install g++-13` or `sudo apt install clang-15`
- **Windows:** Install Visual Studio Build Tools 2022+ with "Desktop development with C++" workload

## Build Issues

### Build fails after switching branches

**Fix:** Clean and reconfigure:
```bash
pulp clean
pulp build
```

Or manually:
```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### FetchContent download failures

**Symptom:** CMake hangs or fails downloading CLAP or Catch2.

**Fix:** Check your internet connection. If behind a proxy, set `http_proxy` and `https_proxy` environment variables. If the download server is down, you can point to a local cache:

```bash
cmake -S . -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
```

This requires that `build/_deps/` already has the sources from a previous successful configure.

### Tests fail with AudioWorkgroup timeout

**Symptom:** `AudioWorkgroup` test times out intermittently.

**Cause:** Known flaky test related to macOS audio workgroup scheduling. Not a real failure.

**Fix:** Exclude it:
```bash
ctest --test-dir build --output-on-failure --exclude-regex AudioWorkgroup
```

## Plugin Issues

### Plugin doesn't appear in DAW

**Causes:**
1. Plugin wasn't installed to the system folder
2. DAW hasn't rescanned
3. Plugin format isn't supported by the DAW

**Fix:**
```bash
pulp build
pulp validate    # verify plugin loads correctly
pulp install     # copy to system plugin folders
```

Then rescan in your DAW. System plugin folders:
- **AU:** `~/Library/Audio/Plug-Ins/Components/`
- **VST3:** `~/Library/Audio/Plug-Ins/VST3/`
- **CLAP:** `~/Library/Audio/Plug-Ins/CLAP/`

### auval fails

**Symptom:** `auval -v aufx` returns errors.

**Common causes:**
- Missing Audio Unit properties
- Incorrect channel configuration
- State serialization issues

**Fix:** Run `pulp validate` for detailed output, then check the specific AU adapter code.

## Getting Help

- Run `pulp doctor` to diagnose environment issues
- Check the [getting started guide](getting-started.html) for setup instructions
- File issues at the project's GitHub repository
