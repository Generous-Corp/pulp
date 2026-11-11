#pragma once

/// @file clap_preset_library_gate.hpp
/// Whether the CLAP adapter is built with the preset library.
///
/// WebCLAP (wasi) compiles a PresetManager stub and has no preset filesystem, so
/// the PresetLibrary sources are not linked there. Program Change selection and
/// the plugin-location preset load are built only where the library is. This
/// header is dependency-free so a test can preprocess it for both targets.

#if defined(__wasi__)
#define PULP_CLAP_PRESET_LIBRARY 0
#else
#define PULP_CLAP_PRESET_LIBRARY 1
#endif
