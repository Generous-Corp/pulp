// iOS-D.3c scene.js — rotating Three.js cube via THREE.WebGPURenderer.
//
// Runs AFTER the build-time-bundled `three.iife.js` (which registers
// `globalThis.THREE`) and `web-compat-three-shim.js` have been
// evaluated by the C++ side. The Pulp WidgetBridge has already wired
// `navigator.gpu`, `canvas.getContext('webgpu')`, and the
// `requestAnimationFrame` pump that drives the iOS PluginViewHost's
// frame clock.
//
// Log markers (search syslog/Xcode console — `console.info` routes
// through Pulp's `runtime::log_info` via `ScriptedUiSession::make_engine`):
//   - PULP_THREE_DEMO: scene start (THREE=<typeof>)
//   - PULP_THREE_DEMO: renderer.init ok
//   - PULP_THREE_RENDER: first frame submitted
//   - PULP_THREE_RENDER: 60-frame avg <ms>/<FPS>
//
// On the iOS path the JS engine is JSC interpreter-only (no JIT in
// .appex extensions). iOS-D.3a measured Three.js scene-graph traversal
// at ~230 FPS @ 2000 cubes on the iPad Pro 11" 3rd-gen, so a single
// cube has effectively zero CPU cost — anything below 60 FPS points
// at the GPU bridge (presentable swapchain, queue.submit), not at JS.

(function () {
    "use strict";

    // Tiny PulpCanvas wrapper mirroring the macOS threejs-native-demo.
    // Three.js calls `.addEventListener`, `.setPointerCapture`, etc.
    // on the canvas object; the Pulp HTMLCanvasElement shim supports
    // them but Three.js inspects `.style` early which the native
    // element does support — keep the wrapper minimal.
    function PulpCanvas(canvas) {
        this._canvas = canvas;
        this.style = canvas.style || {};
    }
    Object.defineProperty(PulpCanvas.prototype, "width", {
        get: function () { return this._canvas.width; },
        set: function (v) { this._canvas.width = v; }
    });
    Object.defineProperty(PulpCanvas.prototype, "height", {
        get: function () { return this._canvas.height; },
        set: function (v) { this._canvas.height = v; }
    });
    Object.defineProperty(PulpCanvas.prototype, "clientWidth", {
        get: function () { return this._canvas.width; }
    });
    Object.defineProperty(PulpCanvas.prototype, "clientHeight", {
        get: function () { return this._canvas.height; }
    });
    PulpCanvas.prototype.addEventListener = function (t, fn, o) {
        return this._canvas.addEventListener(t, fn, o);
    };
    PulpCanvas.prototype.removeEventListener = function (t, fn, o) {
        return this._canvas.removeEventListener(t, fn, o);
    };
    PulpCanvas.prototype.dispatchEvent = function (e) {
        return this._canvas.dispatchEvent(e);
    };
    PulpCanvas.prototype.setPointerCapture = function (id) {
        if (typeof this._canvas.setPointerCapture === "function") {
            return this._canvas.setPointerCapture(id);
        }
    };
    PulpCanvas.prototype.releasePointerCapture = function (id) {
        if (typeof this._canvas.releasePointerCapture === "function") {
            return this._canvas.releasePointerCapture(id);
        }
    };

    if (typeof globalThis.THREE === "undefined") {
        console.error("PULP_THREE_DEMO: globalThis.THREE missing — IIFE bundle did not register namespace; rendering placeholder.");
        var fallback = document.createElement("div");
        fallback.style.padding = "24px";
        fallback.style.color = "#ff4444";
        fallback.style.fontSize = "20px";
        fallback.style.fontWeight = "700";
        fallback.style.textAlign = "center";
        fallback.textContent = "Three.js not loaded (IIFE missing)";
        document.body.style.backgroundColor = "#1a0606";
        document.body.appendChild(fallback);
        return;
    }
    console.info("PULP_THREE_DEMO: scene start (THREE=" + (typeof globalThis.THREE) + ")");

    var THREE = globalThis.THREE;
    var rootWidth = 540;
    var rootHeight = 720;

    // ── Backdrop card so the editor pane has a visible chrome around
    //    the canvas. Plain flex layout — Pulp's CSS-in-JS shim follows
    //    the same Yoga-backed flex+grid contract documented in
    //    docs/reference/layout-model.md.
    document.body.style.backgroundColor = "#040912";
    document.body.style.padding = "18px";

    // iOS-D.3c polish: flex-fill the body so the shell uses whatever size the
    // host hands the editor. The AUv3 GPU host pins the root to the 540×720
    // design viewport and scales the composited output up to the editor pane
    // (set_design_viewport in plugin_view_host_ios.mm), so filling the body
    // here means the shell — and the cube card inside it — fill that pane
    // edge-to-edge instead of sitting at a fixed size in one corner.
    document.body.style.flexDirection = "column";
    document.body.style.width = "100%";
    document.body.style.height = "100%";

    var shell = document.createElement("div");
    shell.id = "pulp-three-demo-shell";
    shell.style.flexDirection = "column";
    shell.style.gap = "12px";
    shell.style.padding = "16px";
    shell.style.backgroundColor = "#0c1424";
    shell.style.borderRadius = "20px";
    // Fill the available body box (flex column). minWidth/minHeight keep the
    // layout sane if the host ever reports a degenerate size before the first
    // real bounds land.
    shell.style.flexGrow = "1";
    shell.style.width = "100%";
    shell.style.minWidth = Math.max(320, rootWidth - 36) + "px";
    shell.style.minHeight = Math.max(420, rootHeight - 36) + "px";
    document.body.appendChild(shell);

    var eyebrow = document.createElement("span");
    eyebrow.textContent = "Pulp · iOS-D.3c · JSC + three.webgpu.js";
    eyebrow.style.color = "#7dd3fc";
    eyebrow.style.fontSize = "12px";
    eyebrow.style.fontWeight = "600";
    // iOS-D.3c (#3217 follow-up): clamp the eyebrow to the shell width so the
    // long "Pulp · iOS-D.3c · JSC + three.webgpu.js" label wraps instead of
    // overflowing the rounded shell on narrow editor sizes (AUM, split-view).
    eyebrow.style.maxWidth = "100%";
    eyebrow.style.overflowWrap = "break-word";
    eyebrow.style.wordBreak = "break-word";
    shell.appendChild(eyebrow);

    var title = document.createElement("h2");
    title.textContent = "Rotating Cube";
    title.style.color = "#f8fafc";
    title.style.fontSize = "26px";
    title.style.fontWeight = "700";
    title.style.margin = "0";
    // Same containment for the title — keeps it inside the shell if a future
    // edit lengthens it or the editor pane gets very narrow.
    title.style.maxWidth = "100%";
    title.style.overflowWrap = "break-word";
    title.style.wordBreak = "break-word";
    shell.appendChild(title);

    var subtitle = document.createElement("p");
    subtitle.textContent =
        "THREE.WebGPURenderer painting through Pulp's Dawn/Metal surface inside an AUv3 .appex.";
    subtitle.style.color = "#cbd5e1";
    subtitle.style.fontSize = "13px";
    // iOS-D.3c (#3217): clamp subtitle to fit the shell width on narrow
    // editor sizes (AUM, iPad split-view, Logic AUv3 small-window mode).
    subtitle.style.margin = "4px 0 8px 0";
    subtitle.style.maxWidth = "100%";
    subtitle.style.overflowWrap = "break-word";
    subtitle.style.wordBreak = "break-word";
    shell.appendChild(subtitle);

    var canvasCard = document.createElement("div");
    canvasCard.style.flexGrow = "1";
    canvasCard.style.padding = "12px";
    canvasCard.style.backgroundColor = "#06101c";
    canvasCard.style.borderRadius = "16px";
    shell.appendChild(canvasCard);

    // ── Canvas: sized in CSS pixels; Three.js sets the drawing buffer
    //    via `renderer.setSize` below. The exact pixel size is less
    //    important than the canvas existing before
    //    `canvas.getContext('webgpu')` is queried.
    var canvasWidth = Math.max(260, rootWidth - 84);
    var canvasHeight = Math.max(320, rootHeight - 220);
    var canvasEl = document.createElement("canvas");
    canvasEl.id = "pulp-three-demo-canvas";
    canvasEl.width = canvasWidth;
    canvasEl.height = canvasHeight;
    canvasEl.style.width = canvasWidth + "px";
    canvasEl.style.height = canvasHeight + "px";
    canvasEl.style.borderRadius = "12px";
    canvasCard.appendChild(canvasEl);

    var hud = document.createElement("div");
    hud.style.flexDirection = "row";
    hud.style.gap = "12px";
    hud.style.padding = "12px";
    hud.style.backgroundColor = "#0a1322";
    hud.style.borderRadius = "12px";
    shell.appendChild(hud);

    function metric(label) {
        var col = document.createElement("div");
        col.style.flexGrow = "1";
        col.style.gap = "4px";
        col.style.padding = "6px";
        var l = document.createElement("span");
        l.textContent = label;
        l.style.color = "#94a3b8";
        l.style.fontSize = "11px";
        col.appendChild(l);
        var v = document.createElement("span");
        v.textContent = "—";
        v.style.color = "#f8fafc";
        v.style.fontSize = "16px";
        v.style.fontWeight = "700";
        col.appendChild(v);
        hud.appendChild(col);
        return v;
    }
    var fpsMetric = metric("FPS");
    var frameMetric = metric("Frame");
    var backendMetric = metric("Backend");

    // ── Three.js scene ───────────────────────────────────────────────
    // Same minimum shape as the macOS lane: WebGPURenderer wraps the
    // canvas context, a perspective camera frames a unit cube with a
    // MeshBasicMaterial. `MeshBasicMaterial` deliberately avoids any
    // light pass so the WGSL the renderer compiles stays small —
    // useful for first bring-up on a new platform where compilation
    // overhead matters more than visual quality.
    var context = canvasEl.getContext("webgpu");
    var wrappedCanvas = new PulpCanvas(canvasEl);

    var renderer;
    try {
        renderer = new THREE.WebGPURenderer({
            canvas: wrappedCanvas,
            context: context,
            antialias: false
        });
    } catch (e) {
        console.error(
            "PULP_THREE_DEMO: WebGPURenderer constructor failed: " + (e && e.message ? e.message : e));
        return;
    }

    var scene = new THREE.Scene();
    scene.background = new THREE.Color(0x05080f);

    var camera = new THREE.PerspectiveCamera(
        60, canvasWidth / canvasHeight, 0.1, 100);
    // iOS-D.3c polish (task 1 — fill the canvas): pull the camera in from 2.4
    // to 1.7 and grow the cube from 0.9 to 1.0 so the rotating cube fills most
    // of the canvas card. At z=1.7 with a 60° vertical FOV the visible
    // half-height at the cube plane is 1.7·tan(30°) ≈ 0.98, comfortably larger
    // than the cube's worst-case half-extent at a 45° spin (≈0.87 across the
    // face diagonal), so it never clips at the rotation extremes.
    camera.position.z = 1.7;

    var cube = new THREE.Mesh(
        new THREE.BoxGeometry(1.0, 1.0, 1.0),
        new THREE.MeshBasicMaterial({ color: 0x76ff7a })
    );
    scene.add(cube);
    globalThis.__pulpThreeDemoMesh = cube;

    // iOS-D.3c polish (task 4 — touch orbit). OrbitControls is bundled into
    // three.iife.js alongside three.webgpu.js (see
    // tools/scripts/bundle_threejs_for_jsc.mjs), so it is exposed as
    // THREE.OrbitControls. Drag-rotate and pinch-zoom reach JS via the iOS
    // AUv3 touch→pointer-event bridge added in
    // core/view/platform/ios/plugin_view_host_ios.mm (PulpMetalPluginView).
    // Pass the RAW native canvas element (not the PulpCanvas wrapper): the
    // wrapper only forwards width/height/addEventListener, but OrbitControls
    // also needs `ownerDocument` (to add document-level pointermove/pointerup
    // listeners), `getRootNode`, `getBoundingClientRect`, and
    // `setPointerCapture` — all provided by Pulp's native HTMLCanvasElement
    // shim (web-compat-element.js), which the renderer's wrapper deliberately
    // does not re-expose. This matches the macOS threejs-native-demo, which
    // also hands OrbitControls the raw canvas while the renderer gets the
    // wrapper.
    var controls = null;
    if (typeof THREE.OrbitControls === "function") {
        controls = new THREE.OrbitControls(camera, canvasEl);
        controls.enableDamping = true;   // inertial feel; needs update() each frame
        controls.dampingFactor = 0.08;
        controls.enablePan = false;       // pan is off per the demo spec
        controls.enableRotate = true;     // one-finger drag → orbit
        controls.enableZoom = true;       // two-finger pinch → dolly
        controls.rotateSpeed = 0.9;
        controls.zoomSpeed = 0.9;
        // Keep the camera from diving inside the cube or drifting so far the
        // cube shrinks to nothing.
        controls.minDistance = 1.2;
        controls.maxDistance = 6.0;
        controls.target.set(0, 0, 0);
        controls.update();
        console.info(
            "PULP_THREE_DEMO: OrbitControls wired (rotate+pinch-zoom, pan off, damping on)");
    } else {
        console.error(
            "PULP_THREE_DEMO: THREE.OrbitControls missing — touch orbit disabled "
            + "(three.iife.js bundle did not include the OrbitControls addon).");
    }

    var firstFrameSubmitted = false;
    var frameCount = 0;
    var lastSampleTime = 0;
    var lastSampleFrame = 0;

    function onceFirstFrame() {
        if (firstFrameSubmitted) return;
        firstFrameSubmitted = true;
        // The iOS-D.3b acceptance criteria call this marker out by
        // name — emit it through console.info so it lands in
        // `runtime::log_info`, the same logging path the rest of the
        // PULP_* markers use. Searching syslog for the exact string
        // is how iPad device walk-throughs confirm Three.js made it
        // all the way to a real submitted command buffer.
        console.info("PULP_THREE_RENDER: first frame submitted");
        backendMetric.textContent =
            (renderer.backend && renderer.backend.constructor)
                ? renderer.backend.constructor.name
                : "active";
    }

    // Frame loop — driven by Pulp's `requestAnimationFrame`, which
    // is itself driven by the iOS PluginViewHost's `idle_callback_`
    // running before the per-vsync repaint (pulp #1402 contract).
    function tick() {
        cube.rotation.x += 0.01;
        cube.rotation.y += 0.01;
        // OrbitControls orbits the CAMERA around the target; the cube's own
        // spin above is independent, so the demo stays alive while still
        // responding to drag/pinch. update() applies damping inertia and any
        // pending touch input — it must run every frame when enableDamping is
        // on, even when the user isn't actively touching.
        if (controls) controls.update();
        try {
            renderer.render(scene, camera);
        } catch (e) {
            console.error(
                "PULP_THREE_DEMO: renderer.render threw: " + (e && e.message ? e.message : e));
            return;  // stop scheduling further frames
        }
        onceFirstFrame();
        frameCount += 1;
        var now = (typeof performance !== "undefined" && performance.now)
            ? performance.now()
            : Date.now();
        if (lastSampleTime === 0) {
            lastSampleTime = now;
            lastSampleFrame = frameCount;
        } else if (frameCount - lastSampleFrame >= 60) {
            var dt = now - lastSampleTime;
            var fps = (1000.0 * (frameCount - lastSampleFrame)) / Math.max(1.0, dt);
            var ms = dt / Math.max(1, frameCount - lastSampleFrame);
            fpsMetric.textContent = fps.toFixed(1);
            frameMetric.textContent = String(frameCount);
            console.info("PULP_THREE_RENDER: 60-frame avg "
                + ms.toFixed(2) + " ms/frame ("
                + fps.toFixed(1) + " FPS, total=" + frameCount + ")");
            lastSampleTime = now;
            lastSampleFrame = frameCount;
        }
        requestAnimationFrame(tick);
    }

    // Three.js's WebGPURenderer.init() is async in newer releases —
    // call it explicitly and only start the frame loop when it
    // resolves. The macOS lane does the same; mirroring it avoids the
    // race where the first `.render()` runs before the backend is
    // ready and the queue.submit silently no-ops.
    if (renderer.init && typeof renderer.init === "function") {
        var maybePromise = renderer.init();
        if (maybePromise && typeof maybePromise.then === "function") {
            maybePromise.then(function () {
                console.info("PULP_THREE_DEMO: renderer.init ok");
                requestAnimationFrame(tick);
            }, function (err) {
                console.error(
                    "PULP_THREE_DEMO: renderer.init failed: "
                    + (err && err.message ? err.message : err));
            });
        } else {
            console.info("PULP_THREE_DEMO: renderer.init returned non-promise; starting loop");
            requestAnimationFrame(tick);
        }
    } else {
        console.info("PULP_THREE_DEMO: renderer has no init(); starting loop");
        requestAnimationFrame(tick);
    }
})();
