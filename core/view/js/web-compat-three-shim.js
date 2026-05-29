// web-compat-three-shim.js — iOS-D.3b Slice 2.
//
// Diagnostic shim that runs AFTER `three.iife.js` has been evaluated
// in the JSC context. The IIFE bundle registers `THREE` as a
// `globalThis.THREE` namespace; this shim:
//
//   1. Verifies `globalThis.THREE` is actually present (catches the
//      "bundle silently failed to load" failure mode that the
//      `PULP_THREEJS:` print markers from the bundle script would
//      otherwise hide if the script aborted mid-evaluate).
//   2. Emits `PULP_THREE_SHIM: ready` so the iOS-D.3b log-marker
//      grep matches `PULP_THREE_SHIM: ready` for the "shim wired"
//      verification step.
//   3. Surfaces a `PULP_THREE_SHIM: webgpu-renderer-present` marker
//      if `THREE.WebGPURenderer` is available — this is the same
//      symbol the JSC test in `test/test_widget_bridge.cpp`
//      asserts, and the marker lets the iPad device walk-through
//      gate on the symbol without having to introspect the bundle
//      manually.
//
// Future preludes that use ESM `import * as THREE from "three"`
// should be processed at build time by stripping the import (replace
// with `var THREE = globalThis.THREE;`) so they evaluate cleanly
// under JSC. Slice 3 documents that processing step; slice 2 ships
// only the IIFE bundle + this diagnostic shim.

(function () {
    "use strict";
    var hasThree = typeof globalThis.THREE !== "undefined" && globalThis.THREE !== null;
    if (!hasThree) {
        if (typeof globalThis.print === "function") {
            globalThis.print("PULP_THREE_SHIM: globalThis.THREE missing — three.iife.js did not register namespace");
        }
        return;
    }
    if (typeof globalThis.print === "function") {
        globalThis.print("PULP_THREE_SHIM: ready");
        if (typeof globalThis.THREE.WebGPURenderer === "function") {
            globalThis.print("PULP_THREE_SHIM: webgpu-renderer-present");
        } else {
            globalThis.print("PULP_THREE_SHIM: webgpu-renderer-missing");
        }
    }
})();
