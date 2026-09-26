// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  access,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  browserCandidates,
  escapeRegExp,
  execute,
  installedBrowser,
  laidOutNodes,
  rgbaPixel,
  runCapture,
} from "./capture_integration_support.mjs";

test("escapeRegExp preserves arbitrary text as a literal pattern", () => {
  const literal = "version\\with.*+?^${}()|[]characters";
  const pattern = new RegExp(`^${escapeRegExp(literal)}$`);
  assert.match(literal, pattern);
  assert.doesNotMatch(`${literal}suffix`, pattern);
});

test("browser integration fails closed for an inaccessible pinned browser", async () => {
  const environment = {
    PULP_DESIGN_BROWSER: "/inaccessible/pinned/chrome",
    PULP_BROWSER: "/legacy/chrome",
  };
  assert.deepEqual(
    browserCandidates(environment),
    ["/inaccessible/pinned/chrome"],
  );
  assert.equal(await installedBrowser(environment), "");
});

test("a CI lane without the pinned browser never falls back to a system browser",
  async () => {
    assert.deepEqual(
      browserCandidates({
        GITHUB_ACTIONS: "true",
        PULP_BROWSER: "/legacy/chrome",
      }),
      []);
    assert.equal(await installedBrowser({ GITHUB_ACTIONS: "true" }), "");
    // Control: the same environment outside CI still resolves the legacy and
    // conventional candidates, so the empty list above is the CI rule at work.
    assert.equal(
      browserCandidates({ PULP_BROWSER: "/legacy/chrome" })[0],
      "/legacy/chrome");
  });

test("installedBrowser prefers a provisioned browser over system installations",
  async () => {
    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-preference-"));
    const provisioned = path.join(root, "Google Chrome for Testing");
    const absent = path.join(root, "not-installed");
    try {
      await writeFile(provisioned, "#!/bin/sh\nexit 0\n", { mode: 0o755 });

      const conventional = await installedBrowser({});
      assert.notEqual(
        provisioned, conventional,
        "the provisioned path must not be one this host already resolves");

      assert.equal(
        await installedBrowser({ PULP_DESIGN_BROWSER: provisioned }),
        provisioned);
      assert.equal(
        await installedBrowser({ PULP_BROWSER: provisioned }), provisioned);

      // Control: an unreadable pinned override fails closed rather than
      // silently measuring a mutable conventional installation.
      assert.equal(
        await installedBrowser({ PULP_DESIGN_BROWSER: absent }), "");
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser capture waits through a delayed DOM commit",
  { timeout: 20000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-delayed-commit-"));
    const input = path.join(root, "delayed.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  body { margin: 0; background: #111; color: white; }
  main { width: 320px; height: 240px; background: #243; }
</style>
<main id="status"><span>EARLY</span><span>A</span><span>B</span></main>
<script>
  globalThis.__pulpCaptureReady = new Promise((resolve) => {
    setTimeout(() => {
      document.getElementById('status').replaceChildren(
        Object.assign(document.createElement('span'), {
          textContent: 'DELAYED READY'
        }),
        Object.assign(document.createElement('span'), { textContent: 'C' }),
        Object.assign(document.createElement('span'), { textContent: 'D' })
      );
      resolve();
    }, 1400);
});

</script>
`);
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "320",
        "--initial-height", "240",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 });

      const snapshot = JSON.parse(
        await readFile(path.join(output, "dom-snapshot.json"), "utf8"));
      assert.ok(snapshot.strings.includes("DELAYED READY"));
      assert.equal(snapshot.strings.includes("EARLY"), false);
      const envelope = JSON.parse(
        await readFile(path.join(output, "capture.json"), "utf8"));
      assert.deepEqual(envelope.provenance.readiness, {
        contract: "__pulpCaptureReady",
        awaited: true,
      });
      await assert.rejects(
        access(path.join(output, "interaction-report.json")));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("indicator-free capture removes only declared moving art",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) return context.skip("no compatible system browser is installed");
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-static-art-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
        html,body{margin:0;width:80px;height:60px;background:#246}
        .body{position:absolute;left:10px;top:10px;width:50px;height:40px;background:#ca8}
        .pointer{position:absolute;left:23px;top:4px;width:4px;height:28px;background:#f00}
      </style><div class="body" data-pulp-kind="knob">
        <div class="pointer" data-pulp-indicator></div></div>`);
      await runCapture(script, browser, input, root, output, 80, 60);
      const original = await readFile(path.join(output, "browser.png"));
      const clean = await readFile(path.join(output, "browser-static.png"));
      assert.deepEqual(rgbaPixel(original, 100, 90), rgbaPixel(clean, 100, 90),
        "pixels outside the declared indicator stay exact");
      assert.deepEqual(rgbaPixel(original, 100, 90), [204, 170, 136, 255]);
      assert.deepEqual(rgbaPixel(original, 70, 40), [255, 0, 0, 255]);
      assert.deepEqual(rgbaPixel(clean, 70, 40), [204, 170, 136, 255],
        "the clean frame reveals the exact body beneath moving art");
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("a panel without declared indicators reuses its exact screenshot",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) return context.skip("no compatible system browser is installed");
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-static-zero-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input,
        `<!doctype html><style>html,body{margin:0}main{width:40px;height:30px;background:#357}</style><main></main>`);
      await runCapture(script, browser, input, root, output, 40, 30);
      assert.deepEqual(
        await readFile(path.join(output, "browser-static.png")),
        await readFile(path.join(output, "browser.png")),
        "zero-indicator capture publishes the original stable bytes");
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser capture preserves the executable pre-mount document",
  { timeout: 20000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-materialized-document-"));
    const input = path.join(root, "loader.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      const fontBase64 = (await readFile(fileURLToPath(new URL(
        "../../../packages/pulp-web-player/src/theme/inter.woff2",
        import.meta.url)))).toString("base64");
      await writeFile(input, `<!doctype html><html><body>
<script>
  (async () => {
    const source = 'window.__materializedAssetRan = true;';
    const url = URL.createObjectURL(new Blob([source], {
      type: 'text/javascript'
    }));
    const fontBytes = Uint8Array.from(atob(${JSON.stringify(fontBase64)}),
      (character) => character.charCodeAt(0));
    const fontUrl = URL.createObjectURL(new Blob([fontBytes], {
      type: 'font/woff2'
    }));
    const html = '<!doctype html><html><body style="margin:0;background:#123">' +
      '<style>@font-face{font-family:"Captured Inter";src:url("' + fontUrl +
      '") format("woff2");font-weight:400;font-style:normal;' +
      'unicode-range:U+0000-00FF,U+20AC}</style>' +
      '<main style="width:320px;height:240px">' +
      '<button style="width:120px;height:40px;font-family:&quot;Captured Inter&quot;"' +
      ' onclick="void 0">READY</button>' +
      '<svg width="16" height="16" style="color:#42b6f0">' +
      '<rect width="16" height="16" fill="currentColor" /></svg>' +
      '</main>' +
      '<script src="' + url + '"><\\/script></body></html>';
    const doc = new DOMParser().parseFromString(html, 'text/html');
    document.documentElement.replaceWith(doc.documentElement);
    // Parsing a later helper document must not replace the executable source
    // authority selected by the live-root installation above.
    new DOMParser().parseFromString(
      '<!doctype html><html><body>STALE HELPER</body></html>', 'text/html');
    const replacement = document.createElement('script');
    replacement.src = url;
    document.body.appendChild(replacement);
    await new Promise((resolve) => { replacement.onload = resolve; });
    await document.fonts.load('16px "Captured Inter"');
    await document.fonts.ready;
    globalThis.__pulpCaptureReady = Promise.resolve();
  })();
</script></body></html>`);
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "320",
        "--initial-height", "240",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 });

      const materialized = JSON.parse(await readFile(
        path.join(output, "materialized-document.json"), "utf8"));
      assert.match(materialized.html, /<button[^>]*>READY<\/button>/);
      assert.doesNotMatch(materialized.html, /STALE HELPER/);
      assert.doesNotMatch(materialized.html, /blob:/);
      assert.equal(materialized.assets.length, 2);
      const scriptAsset = materialized.assets.find(
        (asset) => asset.mime_type === "text/javascript");
      const fontAsset = materialized.assets.find(
        (asset) => asset.mime_type === "font/woff2");
      assert.ok(scriptAsset);
      assert.ok(fontAsset);
      assert.match(scriptAsset.id,
        /^pulp-materialized-asset-[0-9a-f]{64}$/);
      assert.equal(materialized.html.includes(scriptAsset.id), true);
      assert.equal(materialized.html.includes(fontAsset.id), true);
      assert.equal("url" in scriptAsset, false);
      assert.equal(
        Buffer.from(scriptAsset.data_base64, "base64").toString(),
        "window.__materializedAssetRan = true;");
      assert.deepEqual(materialized.font_bindings, [{
        family: "Captured Inter",
        asset_id: fontAsset.id,
        weight: "400",
        style: "normal",
        unicode_range: "U+0000-00FF,U+20AC",
        runtime_family: `Captured Inter [${fontAsset.id}]`,
      }]);
      assert.equal(materialized.semantic_bindings.length, 1);
      for (const binding of materialized.semantic_bindings) {
        assert.equal(binding.anchor,
          `chromium:backend-node:${binding.backend_node_id}`);
        assert.ok(binding.backend_node_id > 0);
        assert.ok(binding.bounds.width > 0);
        assert.ok(binding.bounds.height > 0);
      }
      assert.equal(materialized.semantic_bindings[0].tag, "button");
      assert.equal(materialized.semantic_bindings[0].name, "READY");
      assert.equal(materialized.semantic_bindings[0].bounds.width, 120);
      assert.equal(materialized.semantic_bindings[0].bounds.height, 40);
      assert.ok(materialized.text_bindings.length >= 1);
      assert.ok(Array.isArray(materialized.paint_bindings));
      assert.ok(materialized.paint_bindings.length > 0,
        'same-frame computed SVG paint must survive capture serialization');
      const readyText = materialized.text_bindings.find(
        (binding) => binding.text === "READY");
      assert.ok(readyText);
      assert.equal(readyText.anchor, "body");
      assert.equal(readyText.path.at(-1).tag, "button");
      assert.ok(readyText.basis.width > 0);
      assert.ok(readyText.basis.resolved_face.length > 0);
      assert.ok(readyText.boxes.length >= 1);
      assert.ok(readyText.boxes.every((box) =>
        Number.isFinite(box.left) && Number.isFinite(box.top) &&
        box.width >= 0 && box.height > 0 && box.length > 0));
      const envelope = JSON.parse(await readFile(
        path.join(output, "capture.json"), "utf8"));
      assert.equal(
        envelope.provenance.source.materialized_document,
        "materialized-document.json");
      assert.equal(envelope.provenance.source.materialized_asset_count, 2);
      assert.match(
        envelope.provenance.source.materialized_document_sha256,
        /^[0-9a-f]{64}$/);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser capture separates authored geometry from its affine transform",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-authored-coordinate-space-"));
    const input = path.join(root, "transformed.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><html><body><script>
  const html = ${JSON.stringify(`<!doctype html><html><head><style>
    html, body { width: 100%; height: 100%; margin: 0; overflow: hidden; }
    #root {
      position: absolute; left: 50%; top: 50%; width: 1320px; height: 860px;
      transform: translate(-50%, -50%) scale(0.9302325581395349);
      transform-origin: center center; background: #05070a;
    }
    button { position: absolute; left: 688px; top: 11px; width: 48px;
      height: 22px; font: 12px sans-serif; }
  </style></head><body><main id="root"><button>A</button></main>
  </body></html>`)};
  const doc = new DOMParser().parseFromString(html, 'text/html');
  document.documentElement.replaceWith(doc.documentElement);
  globalThis.__pulpCaptureReady = Promise.resolve();
</script></body></html>`);
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "1280",
        "--initial-height", "800",
        "--dpr", "2",
        "--timeout-ms", "20000",
      ], { maxBuffer: 1024 * 1024 });

      const materialized = JSON.parse(await readFile(
        path.join(output, "materialized-document.json"), "utf8"));
      assert.equal(materialized.coordinate_space.authored_box.width, 1320);
      assert.equal(materialized.coordinate_space.authored_box.height, 860);
      const transform = materialized.coordinate_space.captured_transform;
      assert.ok(Math.abs(transform.a - 800 / 860) < 1e-6);
      assert.ok(Math.abs(transform.d - 800 / 860) < 1e-6);
      assert.ok(Math.abs(transform.b) < 1e-9);
      assert.ok(Math.abs(transform.c) < 1e-9);
      assert.ok(Math.abs(transform.e - 26.0465116279) < 0.05);
      assert.ok(Math.abs(transform.f) < 0.05);

      const button = materialized.layout_bindings.find(
        (binding) => binding.path.at(-1)?.tag === "button");
      assert.ok(button);
      assert.ok(Math.abs(button.box.left - 688) < 0.05);
      assert.ok(Math.abs(button.box.top - 11) < 0.05);
      assert.ok(Math.abs(button.box.width - 48) < 0.05);
      assert.ok(Math.abs(button.box.height - 22) < 0.05);

      const label = materialized.text_bindings.find(
        (binding) => binding.text === "A");
      assert.ok(label);
      assert.ok(Math.abs(label.basis.width - 48) < 0.05);
      assert.ok(label.boxes.every((box) => box.left >= -0.05 &&
        box.top >= -0.05 && box.left + box.width <= 48.05 &&
        box.top + box.height <= 22.05));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

// Pixels are read with virtual time running, so the JS freeze is the only
// thing holding a canvas animation still. A page that repaints on every frame
// without touching the DOM settles the document sample immediately and then
// keeps the compositor busy forever, so this fails loudly — as a timeout or as
// capture-frame-not-deterministic — if the freeze ever stops taking effect.
test("real browser capture freezes a canvas animation and names its browser",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-canvas-freeze-"));
    const input = path.join(root, "animated.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  body { margin: 0; background: #111; }
  canvas { display: block; }
</style>
<canvas id="surface" width="320" height="240"></canvas>
<script>
  const context = document.getElementById('surface').getContext('2d');
  let frame = 0;
  const paint = () => {
    frame += 1;
    context.fillStyle = 'hsl(' + (frame * 37 % 360) + ' 80% 50%)';
    context.fillRect(0, 0, 320, 240);
    requestAnimationFrame(paint);
  };
  requestAnimationFrame(paint);
</script>
`);
      const run = await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "320",
        "--initial-height", "240",
        "--dpr", "2",
        "--timeout-ms", "20000",
      ], { maxBuffer: 1024 * 1024 });

      const envelope = JSON.parse(
        await readFile(path.join(output, "capture.json"), "utf8"));
      assert.match(envelope.provenance.browser.version, /^\d+\./);
      assert.match(
        run.stderr,
        new RegExp(`\\[browser-capture\\] browser=[^/]+/${
          escapeRegExp(envelope.provenance.browser.version)} `));
      const snapshot = JSON.parse(
        await readFile(path.join(output, "dom-snapshot.json"), "utf8"));
      const document = snapshot.documents[0];
      const canvasIndex = document.nodes.nodeName.findIndex(
        (name) => String(snapshot.strings[name]).toLowerCase() === "canvas");
      assert.notEqual(canvasIndex, -1);
      const backendNodeId = document.nodes.backendNodeId[canvasIndex];
      const canvasAsset = envelope.assets.find(
        (asset) => asset.kind === "canvas-snapshot");
      assert.deepEqual(canvasAsset, {
        id: `canvas:${backendNodeId}`,
        kind: "canvas-snapshot",
        mime_type: "image/png",
        path: `canvas-${backendNodeId}.png`,
        sha256: canvasAsset.sha256,
        // Canvas evidence is a browser-composited viewport plane at capture
        // DPR, not the element's untransformed backing-store bitmap.
        width_px: 640,
        height_px: 480,
        backend_node_id: backendNodeId,
        bounds: canvasAsset.bounds,
      });
      assert.match(canvasAsset.sha256, /^[0-9a-f]{64}$/);
      const canvasPng = await readFile(path.join(output, canvasAsset.path));
      assert.equal(
        createHash("sha256").update(canvasPng).digest("hex"),
        canvasAsset.sha256);
      const [red, green, blue, alpha] = rgbaPixel(canvasPng, 40, 80);
      assert.ok(red + green + blue > 80 && alpha > 240);

      const compositeAsset = envelope.assets.find(
        (asset) => asset.kind === "canvas-composite-evidence");
      assert.deepEqual(compositeAsset, {
        id: "evidence:browser-canvas-composite",
        kind: "canvas-composite-evidence",
        mime_type: "image/png",
        path: "browser-canvas-composite.png",
        sha256: compositeAsset.sha256,
        width_px: 640,
        height_px: 480,
        changed_pixels: compositeAsset.changed_pixels,
      });
      assert.match(compositeAsset.sha256, /^[0-9a-f]{64}$/);
      assert.ok(compositeAsset.changed_pixels > 0);
      const compositePng = await readFile(
        path.join(output, compositeAsset.path));
      assert.equal(
        createHash("sha256").update(compositePng).digest("hex"),
        compositeAsset.sha256);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

// The capture is what decides what can ever be drawn. Every assertion here is
// a property whose absence renders as a plausible wrong picture: a tiled grid
// collapsed to one hairline, a dashed left edge silently gone, a layered panel
// stacked by a z-index guess instead of by Chromium's answer.
test("real browser capture round-trips whole-panel paint properties",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-paint-properties-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  html, body { margin: 0; width: 320px; height: 240px; overflow: hidden; }
  body { background: #111; color: #eee; font: 12px sans-serif; }
  /* The standard CSS grid idiom: one hard-stop gradient, tiled. The column
     count exists only in background-size. */
  #grid-x {
    position: absolute; inset: 0;
    background-image: linear-gradient(to right,
      rgba(255,255,255,.25) 0 1px, transparent 1px 100%);
    background-size: 12.5% 100%;
    /* Every value here is deliberately NOT the CSS initial value, so an
       assertion that passes cannot be passing on a default. */
    background-position: 4px 6px;
    background-repeat: repeat-x;
    background-origin: border-box;
    background-clip: content-box;
  }
  #edges {
    position: absolute; left: 20px; top: 20px; width: 120px; height: 40px;
    border-width: 2px 3px 4px 5px;
    border-color: #f00 #0f0 #00f #ff0;
    border-top-style: none;
    border-right-style: solid;
    border-bottom-style: dotted;
    border-left-style: dashed;
    outline: 2px dotted rgb(10, 200, 240);
    outline-offset: 3px;
  }
  #typo {
    position: absolute; left: 20px; top: 90px;
    word-spacing: 7px;
    text-decoration-line: underline;
    text-decoration-color: rgb(240, 90, 10);
    text-decoration-style: wavy;
    text-decoration-thickness: 3px;
    text-underline-offset: 4px;
  }
  #generated::before { content: "GENERATED"; }
  /* opacity < 1 makes #context a stacking context, so #deep cannot escape it
     however large its z-index. A z-index sort disagrees with Chromium here. */
  #context { position: absolute; left: 20px; top: 130px; z-index: 1; opacity: .99; }
  #deep { position: relative; z-index: 999; }
  #sibling { position: absolute; left: 20px; top: 170px; z-index: 2; }
</style>
<div id="grid-x"></div>
<div id="edges"></div>
<div id="typo">spaced out words</div>
<div id="generated"></div>
<div id="context"><button id="deep">DEEP</button></div>
<button id="sibling">SIBLING</button>
`);
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "320",
        "--initial-height", "240",
        "--dpr", "2",
        "--timeout-ms", "20000",
      ], { maxBuffer: 1024 * 1024 });

      const snapshot = JSON.parse(
        await readFile(path.join(output, "dom-snapshot.json"), "utf8"));
      const laidOut = laidOutNodes(snapshot);
      const byId = new Map(
        laidOut.filter((node) => node.attributes.id)
          .map((node) => [node.attributes.id, node]));

      const grid = byId.get("grid-x");
      assert.ok(grid, "the tiled-gradient node must reach the snapshot");
      assert.equal(grid.style["background-size"], "12.5% 100%",
        "without this exact value the grid lowers to a single 1px line");
      assert.equal(grid.style["background-repeat"], "repeat-x");
      assert.equal(grid.style["background-position"], "4px 6px");
      assert.equal(grid.style["background-origin"], "border-box");
      assert.equal(grid.style["background-clip"], "content-box");

      const edges = byId.get("edges");
      assert.ok(edges, "the mixed-edge border node must reach the snapshot");
      assert.equal(edges.style["border-left-style"], "dashed",
        "a dashed left border must survive a capture that used to read " +
        "only the top edge");
      assert.equal(edges.style["border-top-style"], "none");
      assert.equal(edges.style["border-right-style"], "solid");
      assert.equal(edges.style["border-bottom-style"], "dotted");
      assert.equal(edges.style["outline-style"], "dotted");
      assert.equal(edges.style["outline-width"], "2px");
      assert.equal(edges.style["outline-offset"], "3px");
      assert.equal(edges.style["outline-color"], "rgb(10, 200, 240)");

      const typo = byId.get("typo");
      assert.equal(typo.style["word-spacing"], "7px");
      assert.equal(typo.style["text-decoration-color"], "rgb(240, 90, 10)");
      assert.equal(typo.style["text-decoration-style"], "wavy");
      assert.equal(typo.style["text-decoration-thickness"], "3px");
      assert.equal(typo.style["text-underline-offset"], "4px");

      // The ::before box is its own laid out node with no DOM text child, so
      // `content` is the only place its text exists.
      const generated = laidOut.filter(
        (node) => node.style.content.includes("GENERATED"));
      assert.equal(generated.length, 1,
        "generated content must be recoverable from the capture");
      assert.equal(generated[0].tag, "::before");

      // Paint order in the report is Chromium's, verified against the array
      // Chromium returned rather than against a rule we reimplemented.
      //
      // The anchor is the node's own id in the page source, NOT the candidate's
      // backend id: the candidate's backend id comes from the same element walk
      // as its paint order, so checking one against the other would agree even
      // when both point at the wrong node. This page puts a ::before ahead of
      // both buttons for exactly that reason -- counting pseudo boxes as
      // elements hands #deep the data of #context.
      const report = JSON.parse(
        await readFile(path.join(output, "semantic-report.json"), "utf8"));
      const deep = byId.get("deep");
      const sibling = byId.get("sibling");
      const byName = new Map(
        report.candidates.map((candidate) => [candidate.name, candidate]));
      for (const [name, node] of [["DEEP", deep], ["SIBLING", sibling]]) {
        const candidate = byName.get(name);
        assert.ok(candidate, `${name} must be recognised as a candidate`);
        assert.equal(candidate.backend_node_id, node.backend_node_id,
          `${name} must resolve to its own snapshot node`);
        assert.equal(candidate.paint_order, node.paint_order,
          `${name} must carry the paint order Chromium reported for it`);
      }
      assert.equal(report.summary.paint_ordered, report.candidates.length);

      // A z-index sort would put #deep (999) above #sibling (2). Chromium does
      // not, because #deep is trapped in the stacking context #context created
      // with opacity < 1. Consuming the reported order is what gets this right.
      assert.ok(deep.paint_order < sibling.paint_order,
        "Chromium paints the stacking-context-trapped node first; a z-index " +
        "sort would invert this pair");
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser capture preserves WebGL through software composition",
  { timeout: 20000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-webgl-"));
    const input = path.join(root, "webgl.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  * { box-sizing: border-box; }
  html, body { margin: 0; width: 160px; height: 120px; overflow: hidden; }
  canvas { display: block; width: 160px; height: 120px; }
</style>
<canvas id="surface" width="320" height="240"></canvas>
<script>
  const canvas = document.getElementById("surface");
  const gl = canvas.getContext("webgl2") || canvas.getContext("webgl");
  globalThis.__pulpCaptureReady = gl
    ? Promise.resolve().then(() => {
        gl.clearColor(1, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.finish();
        document.body.dataset.webgl = "ready";
      })
    : Promise.reject(new Error("WebGL unavailable"));
</script>
`);
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "160",
        "--initial-height", "120",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 });

      const snapshot = JSON.parse(
        await readFile(path.join(output, "dom-snapshot.json"), "utf8"));
      assert.ok(snapshot.strings.includes("ready"));
      const screenshot = await readFile(path.join(output, "browser.png"));
      const [red, green, blue, alpha] = rgbaPixel(screenshot, 40, 40);
      assert.ok(red > 240 && green < 16 && blue < 16 && alpha > 240);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
