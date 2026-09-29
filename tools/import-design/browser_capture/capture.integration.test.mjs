// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
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
  CAPTURE_DEADLINE_MS,
  captureCaseTimeout,
  escapeRegExp,
  execute,
  installedBrowser,
  rgbaPixel,
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
  { timeout: captureCaseTimeout() }, async (context) => {
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
        "--timeout-ms", String(CAPTURE_DEADLINE_MS),
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

test("real browser capture preserves the executable pre-mount document",
  { timeout: captureCaseTimeout() }, async (context) => {
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
        "--timeout-ms", String(CAPTURE_DEADLINE_MS),
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
  { timeout: captureCaseTimeout() }, async (context) => {
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
        "--timeout-ms", String(CAPTURE_DEADLINE_MS),
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

test("real browser capture preserves WebGL through software composition",
  { timeout: captureCaseTimeout() }, async (context) => {
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
        "--timeout-ms", String(CAPTURE_DEADLINE_MS),
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
