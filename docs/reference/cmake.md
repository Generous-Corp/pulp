# CMake Reference

Source: `tools/cmake/PulpUtils.cmake`

## pulp_add_plugin

**Status**: usable

Create plugin targets for multiple formats from a single declaration.

```cmake
pulp_add_plugin(<target>
    FORMATS         <format-list>
    PLUGIN_NAME     <string>
    BUNDLE_ID       <string>
    MANUFACTURER    <string>
    VERSION         <string>
    CATEGORY        <Effect|Instrument|MidiEffect>
    PLUGIN_CODE     <4-char>
    MANUFACTURER_CODE <4-char>
    AAX_PRODUCT_CODE <4-char>
    AAX_NATIVE_CODE <4-char>
    SOURCES         <file-list>
    PROCESSOR_FACTORY <function-name>
    UI_SCRIPT       <path>
    CONTENT_CAPABILITIES <capability-list>
    CONTENT_KINDS   <presets|themes|samples|wavetables...>
    CONTENT_HOT_RELOAD_KINDS <presets|themes|samples|wavetables...>
    CONTENT_MANUAL_RESCAN_KINDS <presets|themes|samples|wavetables...>
)
```

### Parameters

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `FORMATS` | Yes | -- | Space-separated list: `VST3`, `AU`, `AUv3`, `CLAP`, `LV2`, `AAX`, `Standalone` |
| `PLUGIN_NAME` | No | `<target>` | Display name for the plugin |
| `BUNDLE_ID` | No | -- | Reverse-DNS identifier (e.g., `com.company.plugin`) |
| `MANUFACTURER` | No | `"Unknown"` | Manufacturer name |
| `VERSION` | No | `"1.0.0"` | Plugin version string |
| `CATEGORY` | No | `"Effect"` | Plugin category: `Effect`, `Instrument`, or `MidiEffect` |
| `PLUGIN_CODE` | AU/AUv3 only | -- | 4-character AU plugin code |
| `MANUFACTURER_CODE` | AU/AUv3/AAX only | -- | 4-character manufacturer code used by AU, AUv3, and AAX |
| `AAX_PRODUCT_CODE` | AAX only | -- | 4-character stable AAX product identifier |
| `AAX_NATIVE_CODE` | AAX only | -- | 4-character stable AAX Native identifier |
| `SOURCES` | No | -- | Source files for the core object library |
| `PROCESSOR_FACTORY` | No | -- | Factory function name (for generated entry points) |
| `UI_SCRIPT` | No | -- | Path to a JavaScript UI entry file. Pulp applies it to created format targets via `PULP_UI_SCRIPT_PATH`; the Standalone macOS lane owns runtime validation for live JS/theme reload. |
| `CONTENT_CAPABILITIES` | No | -- | Runtime content capabilities the plugin actually consumes, such as `content.presets.v1`. Use this when the plugin can load installed content packs. Must be paired with `CONTENT_KINDS`. |
| `CONTENT_KINDS` | No | -- | Content kinds accepted by the plugin: `presets`, `themes`, `samples`, `wavetables`. This lets users and agents reject mismatched packs before install. Must be paired with `CONTENT_CAPABILITIES`. |
| `CONTENT_HOT_RELOAD_KINDS` | No | -- | Accepted content kinds the plugin can refresh immediately after install/update. Each value must also appear in `CONTENT_KINDS`. |
| `CONTENT_MANUAL_RESCAN_KINDS` | No | -- | Accepted content kinds that require an in-app rescan action but not a full restart. Each value must also appear in `CONTENT_KINDS`. |

When `CONTENT_CAPABILITIES` and `CONTENT_KINDS` are present,
`pulp_add_plugin()` generates a `pulp.plugin-runtime.json` resource containing
the `pulp.plugin-runtime.v1` manifest. Reload-policy fields are optional:
accepted kinds listed in `CONTENT_HOT_RELOAD_KINDS` can update immediately,
accepted kinds listed in `CONTENT_MANUAL_RESCAN_KINDS` should surface an in-app
rescan action, and all other accepted kinds are restart-required in content
preview/install UX. VST3/AU-style desktop bundles receive the manifest under
`Contents/Resources/`; AUv3/iOS flat bundles receive it under `Resources/`;
LV2 bundles receive it at the `.lv2` bundle root; single file non-bundle formats
receive a sibling `<plugin-stem>.pulp.plugin-runtime.json` sidecar. Validate the emitted artifact with
`ValidationHarness::validate_plugin_runtime_manifest(...)`.

Reviewed UI kits are consumed explicitly after `pulp kit apply` has generated
`cmake/pulp-kits.cmake`. Include that file, declare the plugin, then attach the
chosen kit script:

```cmake
include(cmake/pulp-kits.cmake OPTIONAL)

pulp_add_plugin(MyPlugin
    FORMATS CLAP Standalone
    ...)

pulp_use_kit_ui(MyPlugin pulp_kit_dev_pulp_fixtures_basic_ui_kit)
```

`pulp_use_kit_ui(<plugin-target> <kit-target> [SCRIPT <exported-path>] [TOKENS <exported-path>])`
reads the reviewed kit target's `PULP_UI_SCRIPTS`, `PULP_DESIGN_TOKENS`, and
`PULP_ASSETS` properties. The selected script is applied to existing format
targets through `PULP_UI_SCRIPT_PATH`; the selected token/theme file is applied
through `PULP_UI_THEME_PATH`; declared asset directories are applied through
`PULP_UI_ASSET_ROOTS`. Scripted editors can then use `__loadAssetSync__` with
relative paths into those reviewed asset roots. Kits with multiple scripts or
token files require the matching `SCRIPT` / `TOKENS` argument so the project
makes the choice intentionally. The helper does not apply kits automatically
and does not run package code.

### Created Targets

| Target | Type | Description |
|--------|------|-------------|
| `${target}_Core` | OBJECT or INTERFACE | Shared processor code |
| `${target}_VST3` | MODULE | VST3 bundle (`.vst3`) |
| `${target}_AU` | MODULE | AU v2 component (`.component`), macOS only |
| `${target}_AUv3Framework` | SHARED FRAMEWORK | AUv3 implementation framework, macOS only |
| `${target}_AUv3` | EXECUTABLE/BUNDLE | AUv3 app extension (`.appex`), Apple platforms only |
| `${target}_AUv3Host` | EXECUTABLE/BUNDLE | macOS container app embedding the AUv3 extension |
| `${target}_CLAP` | MODULE | CLAP bundle (`.clap`) |
| `${target}_LV2` | MODULE | LV2 bundle (`.lv2`) |
| `${target}_AAX` | MODULE | AAX Native bundle (`.aaxplugin`), macOS/Windows with developer-supplied SDK |
| `${target}_Standalone` | EXECUTABLE | Standalone app |
| `pulp-install-${target}` | CUSTOM | Copies built plugins to system folders |

### File Conventions

Each format target looks for an entry-point source file by convention:

| Format | Expected file |
|--------|--------------|
| VST3 | `vst3_entry.cpp` |
| CLAP | `clap_entry.cpp` |
| AU v2 | `au_v2_entry.cpp` |
| AUv3 | `au_v3_entry.cpp` |
| LV2 | `lv2_entry.cpp` |
| AAX | `aax_entry.cpp` |
| Standalone | `main.cpp` |

Info.plist files can be provided per-plugin or generated from templates:

| Format | Custom plist | Template |
|--------|-------------|----------|
| VST3 | `Info.plist.vst3` | `tools/cmake/PulpInfoPlist.vst3.in` |
| AU | `Info.plist.au` | `tools/cmake/PulpInfoPlist.au.in` |
| AAX | generated | `tools/cmake/PulpInfoPlist.aax.in` |

### Format Availability

Format targets are only created when the corresponding SDK is available:

- VST3: requires `PULP_HAS_VST3` (set when `external/vst3sdk/` exists)
- AU: requires `APPLE` and `PULP_HAS_AUSDK` (set when `external/AudioUnitSDK/` exists)
- AUv3: requires `APPLE`, `PLUGIN_CODE`, and `MANUFACTURER_CODE`; macOS creates a framework, `.appex`, and container `.app`, while iOS creates the `.appex`
- CLAP: requires `PULP_HAS_CLAP` (fetched via FetchContent)
- LV2: requires `PULP_HAS_LV2` (set when LV2 headers are available)
- AAX: requires `APPLE` or `WIN32`, `PULP_ENABLE_AAX=ON`, and `PULP_AAX_SDK_DIR` pointing to a developer-supplied out-of-tree AAX SDK
- Standalone: always available

On Linux and Ubuntu, requesting `AAX` in `FORMATS` is a configure-time error.

### Install Target

`pulp-install-${target}` copies supported built bundles to platform-standard locations:

| Platform | VST3 | CLAP | AU | AUv3 | AAX |
|----------|------|------|----|------|-----|
| macOS | `~/Library/Audio/Plug-Ins/VST3/` | `~/Library/Audio/Plug-Ins/CLAP/` | `~/Library/Audio/Plug-Ins/Components/` | `~/Applications/` plus PlugInKit registration | `/Library/Application Support/Avid/Audio/Plug-Ins/` |
| Windows | `%COMMONPROGRAMFILES%/VST3/` | `%COMMONPROGRAMFILES%/CLAP/` | -- | -- | `%COMMONPROGRAMFILES%/Avid/Audio/Plug-Ins/` |
| Linux | `~/.vst3/` | `~/.clap/` | -- | -- | -- |

LV2 build output is created under `build/LV2/<plugin>.lv2/`; `pulp-install-${target}`
does not currently copy LV2 bundles to a user plugin folder.

## pulp_add_app

**Status**: partial

Create a standalone application target.

```cmake
pulp_add_app(<target>
    APP_NAME    <string>
    BUNDLE_ID   <string>
    VERSION     <string>
)
```

Creates a basic executable target with compile definitions for the app name and bundle ID. Minimal functionality compared to `pulp_add_plugin()`.

On Apple platforms the target is created as a bundle-capable executable so it
can accept `pulp_app_icon(...)`.

## pulp_app_icon

**Status**: usable

Attach an icon source to an application or standalone target.

Targets build normally if you never call `pulp_app_icon(...)`. The helper is an
optional overlay, so removing the call cleanly removes the custom-icon
behavior.

```cmake
pulp_app_icon(<target>
    SOURCE       assets/app-icon.png
    MACOS        assets/app-icon-mac.png
    WINDOWS      assets/app-icon-win.png
    IOS          assets/app-icon-ios.png
    ANDROID      assets/app-icon-android.png
    LINUX        assets/app-icon-linux.png
    DEBUG_ICON   assets/app-icon-debug.png
    RELEASE_ICON assets/app-icon-release.png
)
```

### Parameters

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `SOURCE` | Yes, unless a platform override is present | -- | Default PNG used when a platform-specific override is absent; must be at least 1024×1024 |
| `MACOS` | No | `SOURCE` | macOS override |
| `WINDOWS` | No | `SOURCE` | Windows override |
| `IOS` | No | `SOURCE` | iOS override, recorded for downstream packaging |
| `ANDROID` | No | `SOURCE` | Android override |
| `LINUX` | No | `SOURCE` | Linux override |
| `DEBUG_ICON` | No | -- | Debug-build variant, used when the active configure/build type is Debug |
| `RELEASE_ICON` | No | -- | Release-build variant |

### Current Platform Outputs

- macOS: generates `AppIcon.icns` and bundles it into the app
- Windows: generates an `.ico` plus `.rc` and links it into the executable
- Android: writes launcher PNGs under `android/app/src/main/res-generated/`
- Linux: records the selected PNG as the target's canonical app-icon asset via the `PULP_LINUX_APP_ICON` target property
- iOS: records the selected PNG as `PULP_IOS_APP_ICON` and warns that asset-catalog emission is not implemented

The selected source must be a PNG that is at least 1024×1024. Smaller inputs
fail at configure time so a release doesn’t silently ship blurry assets.

Example:

```cmake
pulp_add_plugin(MyPlugin
    FORMATS Standalone
    PLUGIN_NAME "MyPlugin"
    BUNDLE_ID "com.example.myplugin"
)

pulp_app_icon(MyPlugin_Standalone
    SOURCE assets/app-icon.png
)
```

## pulp_add_binary_data

**Status**: usable

Embed binary assets as C++ arrays.

```cmake
pulp_add_binary_data(MyResources
    SOURCES   logo.png preset.json font.ttf
    NAMESPACE myresources
)
```

Generates a static library with:
```cpp
namespace myresources {
    extern const unsigned char logo_png[];
    extern const std::size_t logo_png_size;
}
```

### Parameters

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `SOURCES` | Yes | -- | Files to embed |
| `NAMESPACE` | No | `<target>` | C++ namespace for the generated symbols |

### Implementation notes

The function delegates the byte-to-C-array encoding to the
`tools/cmake/scripts/encode_binary_data.py` helper through
`add_custom_command(OUTPUT … DEPENDS …)`. As a result:

- Encoding runs at **build time**, not configure time. Editing a source
  asset and re-running `cmake --build` regenerates the embedded `.cpp`
  automatically — no `cmake -S/-B` reconfigure required.
- Encoding is fast: a 1 MB asset finishes in well under a second. The older
  in-CMake hex loop was effectively O(n²) and could pin a single reconfigure
  at 100 % CPU for 10–22 minutes.
- Python (`find_package(Python3 COMPONENTS Interpreter REQUIRED)`) is the
  only added build-time dependency; Python is already required elsewhere
  in the Pulp build.

## pulp_add_cargo_staticlib

**Status**: experimental

Build a Rust (Cargo) `staticlib` and expose it as an `IMPORTED` static-library
target, for an opt-in [native-component](native-components.md) DSP core written in
Rust (or any language that compiles to a C-ABI staticlib). Available via
`include(PulpCargoStaticlib)`. It requires a Rust toolchain (`cargo` + `rustc`,
install from [rustup.rs](https://rustup.rs)) and is intended for the opt-in lane
gated behind `PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS` — **OFF by default, so a
default Pulp build never needs Rust**.

```cmake
include(PulpCargoStaticlib)

pulp_add_cargo_staticlib(
    NAME      MyGain_Core               # IMPORTED target name to create
    MANIFEST  ${CMAKE_CURRENT_SOURCE_DIR}/rust-core/Cargo.toml
    LIB_NAME  my_gain_core              # crate's [lib] name → libmy_gain_core.a
    PROFILE   release                   # optional: dev (default) | release
    FEATURES  simd                      # optional: forwarded as --features
)

# Link it into your component target; a direct symbol reference pulls the
# archive into the final module.
target_link_libraries(MyPlugin_Core PRIVATE MyGain_Core)
```

### Parameters

| Parameter  | Required | Default | Description |
|------------|----------|---------|-------------|
| `NAME`     | Yes | -- | Name of the `IMPORTED` target to create |
| `MANIFEST` | Yes | -- | Absolute path to the crate's `Cargo.toml` |
| `LIB_NAME` | Yes | -- | The crate's `[lib] name` (the archive is `lib<LIB_NAME>.a`) |
| `PROFILE`  | No  | `dev` | `dev` or `release` cargo profile |
| `FEATURES` | No  | -- | Cargo features, forwarded to `--features` |

### Implementation notes

- The archive is built `-C relocation-model=pic -C panic=abort` (PIC so it links
  into Pulp's globally-PIC build; `panic=abort` so a Rust panic can never unwind
  across the `extern "C"` boundary — also pinned in the crate's `Cargo.toml` as
  belt-and-suspenders).
- The custom command `DEPENDS` on a `CONFIGURE_DEPENDS` glob of the crate's
  `src/*.rs` plus its `Cargo.toml`, so editing Rust sources re-invokes cargo on
  the next build (cargo is itself incremental, so a no-op re-invoke is cheap).
  Without this, the `OUTPUT` rule would only fire when the archive was missing and
  could silently link a stale archive.
- On Linux/Android the IMPORTED target's `INTERFACE_LINK_LIBRARIES` pulls in the
  system libraries the Rust `std` archive needs (`Threads::Threads`,
  `${CMAKE_DL_LIBS}`, `m`); macOS resolves these through `libSystem`
  automatically.
