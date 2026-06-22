// SPDX-License-Identifier: MIT
//
// Bundled Three.js IIFE resource helpers.
//
// Returns the bundled `three.iife.js` source on platforms where it is
// embedded inside the AUv3 `.appex` (currently iOS). The IIFE bundle
// is produced at build time by
// `tools/scripts/bundle_threejs_for_jsc.mjs` and copied into
// `<appex>/threejs/three.iife.js` by the iOS AUv3 CMake rules. (iOS
// bundles are flat — `<appex>/Resources/` is a macOS-only path that
// NSBundle does not search on iOS.)
//
// On non-Apple-mobile platforms this returns `std::nullopt` — the
// macOS / desktop lanes resolve Three.js source files via the
// `examples/threejs-native-demo/main.cpp` ESM resolver pattern
// (`PULP_THREEJS_SOURCE_DIR` + path-on-disk), which has access to V8's
// public ESM API. JSC does not, so iOS uses the IIFE bundle instead.

#pragma once

#include <optional>
#include <string>

namespace pulp::view {

// Returns the embedded `three.iife.js` source if available on this
// platform. On iOS the source lives at
// `threejs/three.iife.js` inside the AUv3 `.appex` (iOS bundles are
// flat — `<appex>/Resources/` is macOS-only). It is located at
// runtime via `[NSBundle bundleForClass:[PulpAUViewController class]]`
// and `pathForResource:ofType:inDirectory:@"threejs"`. On non-iOS
// platforms (and on Apple-mobile builds where the resource is
// missing), returns `std::nullopt`.
//
// The returned string is the full IIFE wrapper that, when run through
// JSC `evaluate()`, registers `THREE` on `globalThis`. Callers should
// then emit the standard Three.js bundle diagnostics:
//
//     PULP_THREEJS: bundle loaded (N bytes)
//     PULP_THREEJS: globalThis.THREE available
//
// to keep the diagnostic surface aligned with the rest of the
// program.
std::optional<std::string> threejs_iife_source();

}  // namespace pulp::view
