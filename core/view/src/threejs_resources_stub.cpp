// SPDX-License-Identifier: MIT
//
// threejs_resources_stub.cpp — non-iOS Three.js resource stub.
//
// Non-iOS implementation of `pulp::view::threejs_iife_source()`.
// On every platform that is NOT iOS / iOS Simulator we return
// `std::nullopt` — the macOS / desktop / Linux / Windows lanes either
// resolve Three.js via V8's public ESM API (macOS V8 lane) or do not
// load Three.js at all (Linux / Windows servers, headless tests).
// This keeps the iOS IIFE bundle out of every desktop and CI image
// where it would just add ~1 MB of dead weight.

#include "pulp/view/threejs_resources.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__APPLE__) || !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)

namespace pulp::view {

std::optional<std::string> threejs_iife_source() {
    return std::nullopt;
}

}  // namespace pulp::view

#endif  // not iOS
