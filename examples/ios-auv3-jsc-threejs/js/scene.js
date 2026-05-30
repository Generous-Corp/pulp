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

    var shell = document.createElement("div");
    shell.id = "pulp-three-demo-shell";
    shell.style.flexDirection = "column";
    shell.style.gap = "12px";
    shell.style.padding = "16px";
    shell.style.backgroundColor = "#0c1424";
    shell.style.borderRadius = "20px";
    shell.style.width = Math.max(320, rootWidth - 36) + "px";
    shell.style.height = Math.max(420, rootHeight - 36) + "px";
    document.body.appendChild(shell);

    var eyebrow = document.createElement("span");
    eyebrow.textContent = "Pulp · iOS-D.3c · JSC + three.webgpu.js";
    eyebrow.style.color = "#7dd3fc";
    eyebrow.style.fontSize = "12px";
    eyebrow.style.fontWeight = "600";
    shell.appendChild(eyebrow);

    var title = document.createElement("h2");
    title.textContent = "Rotating Cube";
    title.style.color = "#f8fafc";
    title.style.fontSize = "26px";
    title.style.fontWeight = "700";
    shell.appendChild(title);

    var subtitle = document.createElement("p");
    subtitle.textContent =
        "THREE.WebGPURenderer painting through Pulp's Dawn/Metal surface inside an AUv3 .appex.";
    subtitle.style.color = "#cbd5e1";
    subtitle.style.fontSize = "13px";
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
    camera.position.z = 2.4;

    var cube = new THREE.Mesh(
        new THREE.BoxGeometry(0.9, 0.9, 0.9),
        new THREE.MeshBasicMaterial({ color: 0x76ff7a })
    );
    scene.add(cube);
    globalThis.__pulpThreeDemoMesh = cube;

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
