// web-compat-three-shim.js — Three.js bundle diagnostics.
//
// Diagnostic shim that runs AFTER `three.iife.js` has been evaluated
// in the JSC context. The IIFE bundle registers `THREE` as a
// `globalThis.THREE` namespace; this shim:
//
//   1. Verifies `globalThis.THREE` is actually present (catches the
//      "bundle silently failed to load" failure mode that the
//      `PULP_THREEJS:` print markers from the bundle script would
//      otherwise hide if the script aborted mid-evaluate).
//   2. Emits `PULP_THREE_SHIM: ready` so smoke tests can verify the
//      diagnostic shim is wired.
//   3. Surfaces a `PULP_THREE_SHIM: webgpu-renderer-present` marker
//      if `THREE.WebGPURenderer` is available — this is the same
//      symbol the JSC test in `test/test_widget_bridge.cpp`
//      asserts, and the marker lets the iPad device walk-through
//      gate on the symbol without having to introspect the bundle
//      manually.
//
// Preludes that use ESM `import * as THREE from "three"` should be
// processed at build time by stripping the import (replace with
// `var THREE = globalThis.THREE;`) so they evaluate cleanly under JSC.

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
