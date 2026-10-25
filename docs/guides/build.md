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
./setup.sh                                    # bootstrap deps, then Release build + tests
```

`setup.sh` configures `build/` as Release with `PULP_BUILD_EXAMPLES=OFF`, so a
later bare reconfigure of the same directory inherits both from the cache. Pass
`./setup.sh --examples`, or `-DPULP_BUILD_EXAMPLES=ON` on a reconfigure, to add
the example projects. To drive the steps yourself instead:

```bash
./setup.sh --deps-only                        # bootstrap deps only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release  # configure
cmake --build build -j8                       # build
ctest --test-dir build --output-on-failure    # test
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `PULP_BUILD_TESTS` | ON | Build test targets |
| `PULP_BUILD_EXAMPLES` | ON | Build the example projects. `setup.sh` configures `OFF` unless `--examples` is passed. |
| `PULP_ENABLE_GPU` | ON | Enable Dawn/Skia GPU rendering |
| `PULP_TEST_PCH` | ON on Clang, OFF elsewhere | Share one precompiled Catch2 + standard-library header across the Catch2 test executables (`pulp-test-pch-cxx20` / `-cxx23` carriers). Suites with ObjC++ sources, per-target `-f`/`-std` options, or `NO_PCH` compile as before; `<build>/pulp-test-pch.tsv` lists every decision and the `test-pch-wiring` ctest verifies it against the generated compile lines. |
| `PULP_ENABLE_PROJECT_PACKAGE` | ON | Build and export the optional project-package component and dependent Timeline authoring tools. `OFF` omits them, so the installed SDK cannot satisfy a request for the `project-package` component. |
| `PULP_ENABLE_JS` | ON | Build the JS scripting + web-compat layer (`pulp::view-script`). `OFF` yields a native-only `pulp::view` with no JS engine — smaller binaries, and the native faithful-vector design-import path still works. |
| `PULP_JS_ENGINE` | auto | JS engine: `auto`/`quickjs` compile QuickJS only; `jsc` adds JavaScriptCore on Apple and makes it the default; `v8` adds the sealed V8 backend and makes it the default (only when `PULP_ENABLE_JS=ON`) |
| `PULP_DEV_SHARED_UI` | OFF | macOS development only: link plug-in modules against one shared Pulp runtime dylib for fast relinks. Never distributable; see [Fast plug-in relinks](#fast-plug-in-relinks-pulp_dev_shared_ui). |
| `PULP_ENABLE_AAX` | OFF | Enable optional AAX format |
| `PULP_ENABLE_ABLETON_LINK` | OFF | Enable the source-build-only desktop Link adapter. Requires a developer-supplied, out-of-tree checkout at `PULP_ABLETON_LINK_SDK_DIR`; its adapter header and target are build-interface-only and never installed or exported. |

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

## Fast plug-in relinks (`PULP_DEV_SHARED_UI`)

A plug-in module normally links every Pulp library and the Skia/Dawn stack
statically, so each format binary is a 30 to 45 MB image, and a one-file edit
relinks and rescans all of them. With `PULP_DEV_SHARED_UI=ON` (macOS only, off
by default) the format-independent Pulp libraries are linked once into
`libpulp-dev-shared-ui.<version>.dylib` in the build tree, and the VST3, CLAP,
AU and Standalone binaries link against it instead. Each module drops to about a
megabyte, its link edge runs only the linker, and its control-shipping scan and
build record move to the `all` build steps that already exist for them.

In an SDK project `pulp dev` and `pulp loop` turn it on (`--no-dev-shared`
keeps static links), `pulp build` turns it off again, and `pulp build
--dev-shared` keeps it on. A non-empty `PULP_DEV_SHARED_UI` environment value
overrides all three. The Pulp source checkout keeps static links unless asked,
because its own tests inspect example and fixture modules as they ship. Switching
costs a relink, not a recompile. A raw CMake build sets it directly:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_DEV_SHARED_UI=ON
```

It is a development-only link mode, by design:

- **It never ships.** Each module loads Pulp from an absolute path inside this
  build tree, so it fails to load on any other machine or after the build
  directory moves. `pulp ship sign`, `package`, `notarize`, `release` and
  `share` refuse a build cache with the option on and any artifact that is, or
  names, the shared runtime; so does
  `tools/scripts/build_combined_installer.sh`. Rebuild with `pulp build` first.
- **What stays static.** The format adapters (`libpulp-format*`,
  `libpulp-standalone*`) call hooks the plug-in defines, such as
  `make_plug_view` and the processor factory, so they stay in each module with
  the VST3 SDK and AudioUnitSDK. The per-binary Objective-C view cluster still
  compiles into each module with its suffixed class names.
- **What differs from a static build.** Every member of each Pulp library is in
  the dylib, so all of their static initializers run when it loads. The
  formats of one project share one copy of Pulp's globals when a host loads
  more than one of them. The dylib carries Pulp's and SDL's fixed-name
  Objective-C classes, so a host that is itself a Pulp app (such as `pulp audio
  render`) prints `Class ... is implemented in both` warnings, and two dev-shared
  projects loaded into one host each bring a copy. A sandboxed host may refuse
  to load code from outside the bundle; use `--no-dev-shared` there.
- **Where it applies.** Plug-ins with the default view stack. `NO_EDITOR`,
  `NATIVE_UI` and control-capable plug-ins keep their static link, as do AUv3,
  AAX and LV2. It needs macOS and a single-configuration Ninja or Unix
  Makefiles generator, because the link rewriting runs as the
  `<LANG>_LINKER_LAUNCHER` of each module
  (`tools/cmake/scripts/pulp_dev_shared_link.sh`). Anywhere else the configure
  warns, records the option as OFF, and links statically.

## Platform-Specific Notes

See [macOS](platform-macos.html), [Windows](platform-windows.html), [Linux](platform-linux.html).
