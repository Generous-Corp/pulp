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
)
```

### Parameters

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `FORMATS` | Yes | -- | Space-separated list: `VST3`, `AU`, `CLAP`, `AAX`, `Standalone` |
| `PLUGIN_NAME` | No | `<target>` | Display name for the plugin |
| `BUNDLE_ID` | No | -- | Reverse-DNS identifier (e.g., `com.company.plugin`) |
| `MANUFACTURER` | No | `"Unknown"` | Manufacturer name |
| `VERSION` | No | `"1.0.0"` | Plugin version string |
| `CATEGORY` | No | `"Effect"` | Plugin category: `Effect`, `Instrument`, or `MidiEffect` |
| `PLUGIN_CODE` | AU only | -- | 4-character AU plugin code |
| `MANUFACTURER_CODE` | AU/AAX only | -- | 4-character manufacturer code used by AU and AAX |
| `AAX_PRODUCT_CODE` | AAX only | -- | 4-character stable AAX product identifier |
| `AAX_NATIVE_CODE` | AAX only | -- | 4-character stable AAX Native identifier |
| `SOURCES` | No | -- | Source files for the core object library |
| `PROCESSOR_FACTORY` | No | -- | Factory function name (for generated entry points) |

### Created Targets

| Target | Type | Description |
|--------|------|-------------|
| `${target}_Core` | OBJECT or INTERFACE | Shared processor code |
| `${target}_VST3` | MODULE | VST3 bundle (`.vst3`) |
| `${target}_AU` | MODULE | AU v2 component (`.component`), macOS only |
| `${target}_CLAP` | MODULE | CLAP bundle (`.clap`) |
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
- CLAP: requires `PULP_HAS_CLAP` (fetched via FetchContent)
- AAX: requires `APPLE` or `WIN32`, `PULP_ENABLE_AAX=ON`, and `PULP_AAX_SDK_DIR` pointing to a developer-supplied out-of-tree AAX SDK
- Standalone: always available

On Linux and Ubuntu, requesting `AAX` in `FORMATS` is a configure-time error.

### Install Target

`pulp-install-${target}` copies built bundles to platform-standard locations:

| Platform | VST3 | CLAP | AU | AAX |
|----------|------|------|----|-----|
| macOS | `~/Library/Audio/Plug-Ins/VST3/` | `~/Library/Audio/Plug-Ins/CLAP/` | `~/Library/Audio/Plug-Ins/Components/` | `/Library/Application Support/Avid/Audio/Plug-Ins/` |
| Windows | `%COMMONPROGRAMFILES%/VST3/` | `%COMMONPROGRAMFILES%/CLAP/` | -- | `%COMMONPROGRAMFILES%/Avid/Audio/Plug-Ins/` |
| Linux | `~/.vst3/` | `~/.clap/` | -- | -- |

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
    extern const size_t logo_png_size;
}
```

### Parameters

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `SOURCES` | Yes | -- | Files to embed |
| `NAMESPACE` | No | `<target>` | C++ namespace for the generated symbols |
