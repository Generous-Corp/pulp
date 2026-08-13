// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
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
import { promisify } from "node:util";
import test from "node:test";
import { inflateSync } from "node:zlib";

const execute = promisify(execFile);

async function runCapture(script, browser, input, root, output, width, height) {
  await execute(process.execPath, [
    script, "capture", "--browser", browser, "--input", input,
    "--root", root, "--output", output,
    "--initial-width", String(width), "--initial-height", String(height),
    "--dpr", "2", "--timeout-ms", "20000",
  ], { maxBuffer: 1024 * 1024 });
}

function paeth(left, above, upperLeft) {
  const estimate = left + above - upperLeft;
  const leftDistance = Math.abs(estimate - left);
  const aboveDistance = Math.abs(estimate - above);
  const upperLeftDistance = Math.abs(estimate - upperLeft);
  if (leftDistance <= aboveDistance && leftDistance <= upperLeftDistance)
    return left;
  return aboveDistance <= upperLeftDistance ? above : upperLeft;
}

function rgbaPixel(png, x, y) {
  let offset = 8;
  let width = 0;
  let height = 0;
  let colorType = 0;
  const compressed = [];
  while (offset + 12 <= png.length) {
    const length = png.readUInt32BE(offset);
    const type = png.subarray(offset + 4, offset + 8).toString("ascii");
    const data = png.subarray(offset + 8, offset + 8 + length);
    offset += length + 12;
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      assert.equal(data[8], 8, "capture PNG must use 8-bit channels");
      colorType = data[9];
      assert.ok(
        colorType === 2 || colorType === 6,
        "capture PNG must use RGB or RGBA pixels");
    } else if (type === "IDAT") {
      compressed.push(data);
    } else if (type === "IEND") {
      break;
    }
  }
  assert.ok(x >= 0 && x < width && y >= 0 && y < height);
  const encoded = inflateSync(Buffer.concat(compressed));
  const bytesPerPixel = colorType === 6 ? 4 : 3;
  const stride = width * bytesPerPixel;
  const decoded = Buffer.alloc(stride * height);
  let source = 0;
  for (let row = 0; row < height; row += 1) {
    const filter = encoded[source++];
    for (let column = 0; column < stride; column += 1) {
      const raw = encoded[source++];
      const destination = row * stride + column;
      const left = column >= bytesPerPixel
        ? decoded[destination - bytesPerPixel]
        : 0;
      const above = row > 0 ? decoded[destination - stride] : 0;
      const upperLeft = row > 0 && column >= bytesPerPixel
        ? decoded[destination - stride - bytesPerPixel]
        : 0;
      const prediction = [
        0,
        left,
        above,
        Math.floor((left + above) / 2),
        paeth(left, above, upperLeft),
      ][filter];
      assert.notEqual(prediction, undefined, `unsupported PNG filter ${filter}`);
      decoded[destination] = (raw + prediction) & 0xff;
    }
  }
  const pixel = y * stride + x * bytesPerPixel;
  const channels = [...decoded.subarray(pixel, pixel + bytesPerPixel)];
  if (bytesPerPixel === 3) channels.push(255);
  return channels;
}

async function installedBrowser() {
  const candidates = [
    process.env.PULP_BROWSER,
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
  ].filter(Boolean);
  for (const candidate of candidates) {
    try {
      await access(candidate);
      return candidate;
    } catch {
      // Try the next conventional installation.
    }
  }
  return "";
}

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
          envelope.provenance.browser.version.replace(/\./g, "\\.")} `));
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
        width_px: 320,
        height_px: 240,
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
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser interactions capture a same-document secondary screen",
  { timeout: 20000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-interactions-"));
    const input = path.join(root, "prototype.html");
    const interactions = path.join(root, "interactions.json");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  html, body { margin: 0; width: 160px; height: 120px; overflow: hidden; }
  section { position: absolute; inset: 0; }
  #landing { background: rgb(220, 20, 20); }
  #working { display: none; background: rgb(20, 210, 40); }
  body.working #landing { display: none; }
  body.working #working { display: block; }
</style>
<section id="landing">
  <button id="open" onclick="setTimeout(() => {
    history.pushState({}, '', '#working');
    document.body.className = 'working';
    const icon = document.createElement('i');
    icon.dataset.lucide = 'check';
    document.getElementById('working').append(icon);
  }, 100)">Open working screen</button>
</section>
<section id="working">WORKING SCREEN STRING EXISTS WHILE HIDDEN</section>
<script>
  globalThis.lucide = {
    createIcons() {
      for (const icon of document.querySelectorAll('i[data-lucide]')) {
        icon.replaceWith(document.createElementNS(
          'http://www.w3.org/2000/svg', 'svg'));
      }
    }
  };
</script>
`);
      await writeFile(interactions, JSON.stringify({
        schema: "pulp-browser-interactions-v1",
        version: 1,
        actions: [
          { action: "click", selector: "#open" },
          {
            action: "wait-for",
            selector: "#working",
            state: "visible",
            timeout_ms: 3000,
          },
        ],
      }));
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--interactions", interactions,
        "--initial-width", "160",
        "--initial-height", "120",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 });

      const screenshot = await readFile(path.join(output, "browser.png"));
      const [red, green, blue, alpha] = rgbaPixel(screenshot, 40, 80);
      assert.ok(red < 40 && green > 190 && blue < 70 && alpha > 240);
      const report = JSON.parse(
        await readFile(path.join(output, "interaction-report.json"), "utf8"));
      assert.equal(report.action_count, 2);
      assert.deepEqual(report.actions.map(({ action }) => action),
        ["click", "wait-for"]);
      const envelope = JSON.parse(
        await readFile(path.join(output, "capture.json"), "utf8"));
      assert.equal(envelope.provenance.interactions.action_count, 2);
      assert.equal(
        envelope.provenance.interactions.report,
        "interaction-report.json");
      assert.equal(
        envelope.provenance.interactions.report_sha256,
        createHash("sha256")
          .update(await readFile(
            path.join(output, "interaction-report.json")))
          .digest("hex"));
      assert.deepEqual(envelope.provenance.renderer_hooks, [{
        name: "lucide",
        applied: true,
        placeholders: 1,
        remaining: 0,
      }]);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser clicks pass decorative overlays and use exposed target points",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-exposed-click-"));
    const input = path.join(root, "prototype.html");
    const interactions = path.join(root, "interactions.json");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  html,body { margin:0;width:160px;height:120px;overflow:hidden }
  button { position:absolute;left:20px;top:20px;width:120px;height:80px }
  #texture { position:absolute;inset:0;z-index:3;pointer-events:none;
    background:rgba(255,255,255,.02) }
  #blocker { position:absolute;left:50px;top:40px;width:60px;height:40px;
    z-index:2;background:#000 }
</style>
<button id="target" onclick="document.body.style.background='rgb(20,210,40)'">
  Target
</button>
<div id="blocker"></div><div id="texture"></div>
`);
      await writeFile(interactions, JSON.stringify({
        schema: "pulp-browser-interactions-v1",
        version: 1,
        actions: [{ action: "click", selector: "#target" }],
      }));
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--interactions", interactions,
        "--initial-width", "160",
        "--initial-height", "120",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 });

      const screenshot = await readFile(path.join(output, "browser.png"));
      const [red, green, blue] = rgbaPixel(screenshot, 10, 10);
      assert.ok(red < 40 && green > 190 && blue < 60);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser context-click captures the rendered context menu",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-context-click-"));
    const input = path.join(root, "prototype.html");
    const interactions = path.join(root, "interactions.json");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  html,body { margin:0;width:160px;height:120px;overflow:hidden;background:#101820 }
  #band { position:absolute;left:20px;top:20px;width:40px;height:80px;background:#2be1ff }
  #menu { display:none;position:absolute;left:70px;top:30px;width:70px;height:50px;background:rgb(210,40,80) }
</style>
<div id="band"></div><div id="menu">MENU</div>
<script>
  document.getElementById('band').addEventListener('contextmenu', (event) => {
    event.preventDefault();
    document.getElementById('menu').style.display = 'block';
  });
</script>
`);
      await writeFile(interactions, JSON.stringify({
        schema: "pulp-browser-interactions-v1",
        version: 1,
        actions: [
          { action: "context-click", selector: "#band" },
          { action: "wait-for", selector: "#menu", state: "visible" },
        ],
      }));
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--interactions", interactions,
        "--initial-width", "160",
        "--initial-height", "120",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 });

      const screenshot = await readFile(path.join(output, "browser.png"));
      const [red, green, blue, alpha] = rgbaPixel(screenshot, 200, 100);
      assert.ok(red > 190 && green < 60 && blue > 60 && alpha > 240);
      const report = JSON.parse(
        await readFile(path.join(output, "interaction-report.json"), "utf8"));
      assert.deepEqual(report.actions.map(({ action }) => action),
        ["context-click", "wait-for"]);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser wait-for visible rejects invisible ancestors and overlays",
  // Three independent browser captures run sequentially here. Materialized
  // font/layout evidence makes each capture more expensive than the original
  // screenshot-only probe, so bound the individual interaction at 4 seconds
  // while allowing the three cold Chrome processes to finish.
  { timeout: 60000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const cases = [
      {
        name: "ancestor-opacity",
        style: "#target-parent { opacity: 0; }",
        target: `<div id="target-parent">
          <div id="target" style="background:rgb(230,20,20)"></div>
        </div>`,
        mutation: "document.getElementById('target-parent').style.opacity = '1'",
        expected: ([red, green, blue]) =>
          red > 210 && green < 40 && blue < 40,
      },
      {
        name: "covering-overlay",
        style: "#overlay { position:absolute;inset:40px 0 0;z-index:2;background:#000;pointer-events:none }",
        target: `<div id="target" style="background:rgb(20,210,40)"></div>
          <div id="overlay"></div>`,
        mutation: "document.getElementById('overlay').remove()",
        expected: ([red, green, blue]) =>
          red < 40 && green > 190 && blue < 60,
      },
      {
        name: "ancestor-pseudo-element",
        style: `
          #target-parent { position:relative }
          #target-parent::after {
            content:"";position:absolute;inset:0;background:#000;z-index:2
          }
          #target-parent.uncovered::after { display:none }
        `,
        target: `<div id="target-parent">
          <div id="target" style="background:rgb(20,210,40)"></div>
        </div>`,
        mutation:
          "document.getElementById('target-parent').classList.add('uncovered')",
        expected: ([red, green, blue]) =>
          red < 40 && green > 190 && blue < 60,
      },
    ];
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    for (const scenario of cases) {
      const root = await mkdtemp(
        path.join(os.tmpdir(), `pulp-browser-${scenario.name}-`));
      const input = path.join(root, "prototype.html");
      const interactions = path.join(root, "interactions.json");
      const output = path.join(root, "capture");
      try {
        await writeFile(input, `<!doctype html>
<style>
  html,body { margin:0;width:160px;height:120px;overflow:hidden }
  #begin { width:160px;height:40px }
  #target,#target-parent { width:160px;height:80px }
  ${scenario.style}
</style>
<button id="begin" onclick="setTimeout(() => {
  ${scenario.mutation};
}, 1500)">Begin</button>
${scenario.target}
`);
        await writeFile(interactions, JSON.stringify({
          schema: "pulp-browser-interactions-v1",
          version: 1,
          actions: [
            { action: "click", selector: "#begin" },
            {
              action: "wait-for",
              selector: "#target",
              state: "visible",
              timeout_ms: 4000,
            },
          ],
        }));
        await execute(process.execPath, [
          script,
          "capture",
          "--browser", browser,
          "--input", input,
          "--root", root,
          "--output", output,
          "--interactions", interactions,
          "--initial-width", "160",
          "--initial-height", "120",
          "--dpr", "2",
          "--timeout-ms", "15000",
        ], { maxBuffer: 1024 * 1024 });

        const screenshot = await readFile(path.join(output, "browser.png"));
        assert.equal(
          scenario.expected(rgbaPixel(screenshot, 80, 160)),
          true,
          `${scenario.name} must wait for the target to become visible`);
      } finally {
        await rm(root, { recursive: true, force: true });
      }
    }
  });

test("real browser interactions reject main-frame navigation",
  { timeout: 20000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-navigation-rejection-"));
    const input = path.join(root, "prototype.html");
    const interactions = path.join(root, "interactions.json");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<button id="navigate" onclick="location.href='other.html'">Navigate</button>
`);
      await writeFile(path.join(root, "other.html"), "<main>OTHER PAGE</main>");
      await writeFile(interactions, JSON.stringify({
        schema: "pulp-browser-interactions-v1",
        version: 1,
        actions: [{ action: "click", selector: "#navigate" }],
      }));

      await assert.rejects(execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--interactions", interactions,
        "--initial-width", "160",
        "--initial-height", "120",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 }), (error) =>
        error.stderr.includes("browser-interaction-navigation-rejected"));
      const diagnostic = JSON.parse(
        await readFile(path.join(output, "capture-error.json"), "utf8"));
      assert.equal(
        diagnostic.code,
        "browser-interaction-navigation-rejected");
      await assert.rejects(access(path.join(output, "capture.json")));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser interactions reject and close popup pages",
  { timeout: 20000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-popup-rejection-"));
    const input = path.join(root, "prototype.html");
    const interactions = path.join(root, "interactions.json");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<button id="popup" onclick="window.open('other.html', '_blank')">Popup</button>
`);
      await writeFile(path.join(root, "other.html"), "<main>POPUP PAGE</main>");
      await writeFile(interactions, JSON.stringify({
        schema: "pulp-browser-interactions-v1",
        version: 1,
        actions: [{ action: "click", selector: "#popup" }],
      }));

      await assert.rejects(execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--interactions", interactions,
        "--initial-width", "160",
        "--initial-height", "120",
        "--dpr", "2",
        "--timeout-ms", "15000",
      ], { maxBuffer: 1024 * 1024 }), (error) =>
        error.stderr.includes("browser-interaction-navigation-rejected"));
      const diagnostic = JSON.parse(
        await readFile(path.join(output, "capture-error.json"), "utf8"));
      assert.equal(
        diagnostic.code,
        "browser-interaction-navigation-rejected");
      await assert.rejects(access(path.join(output, "capture.json")));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

// `layout.styles` rows are positional against the request order the snapshot
// records as `computedStyleNames`, and every value is a string-table index.
// Decoding through the recorded names is the only way to read a row without
// hardcoding a parallel property list that drifts the first time the capture
// collects one more property.
function laidOutNodes(snapshot) {
  const strings = snapshot.strings;
  const names = snapshot.computedStyleNames;
  assert.ok(Array.isArray(names) && names.length > 0,
    "the snapshot must record the property request order");
  const document = snapshot.documents[0];
  const nodes = document.nodes;
  const layout = document.layout;
  const decode = (index) =>
    typeof index === "number" && index >= 0 && index < strings.length
      ? strings[index]
      : "";
  const attributesFor = (nodeIndex) => {
    const pairs = nodes.attributes?.[nodeIndex] ?? [];
    const result = {};
    for (let offset = 0; offset + 1 < pairs.length; offset += 2) {
      result[decode(pairs[offset])] = decode(pairs[offset + 1]);
    }
    return result;
  };
  const result = [];
  // A node can own more than one layout entry -- a box that also lays out an
  // inline text box contributes two, and a ::before with generated content is
  // the common case. The first entry is the node's own box; the capture keys
  // paint order the same way, so this reader must not diverge from it.
  const seen = new Set();
  for (let entry = 0; entry < layout.nodeIndex.length; entry++) {
    const nodeIndex = layout.nodeIndex[entry];
    if (seen.has(nodeIndex)) continue;
    seen.add(nodeIndex);
    const row = layout.styles?.[entry] ?? [];
    const style = {};
    names.forEach((name, position) => {
      style[name] = decode(row[position]);
    });
    result.push({
      node_index: nodeIndex,
      backend_node_id: nodes.backendNodeId?.[nodeIndex] ?? null,
      tag: decode(nodes.nodeName?.[nodeIndex]).toLowerCase(),
      attributes: attributesFor(nodeIndex),
      paint_order: layout.paintOrders?.[entry] ?? null,
      style,
    });
  }
  return result;
}

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

test("a pointer drawn straight up survives the capture",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    // getBoundingClientRect() does not include stroke, so an SVG <line> drawn
    // at 12, 3, 6 or 9 o'clock reports ZERO extent across its own axis however
    // thick it is. Twelve o'clock is the resting position of every centred
    // bipolar parameter, so a guard that refuses a zero axis drops the
    // commonest pointer there is -- silently, falling back to the derived tick,
    // which renders as a plausible knob and is why no picture caught it.
    //
    // The rotated knob is the positive control. Every probe in the corpus
    // carries a rotation, and the rotation is exactly what hid this: a rotated
    // line has extent on both axes. Without an unrotated case in the same file
    // the guard passes on the one orientation that cannot fail.
    //
    // The scaled knob is the second trap: stroke-width is in USER UNITS, so a
    // 2-unit stroke in a 24-unit viewBox drawn at 96px paints 8 CSS px. Reading
    // the stroke without the viewBox scale recovers the pointer at a quarter of
    // its width, which looks like a hairline rather than a miss.
    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-axis-aligned-pointer-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  html, body { margin: 0; width: 320px; height: 160px; background: #111; }
  .dial { position: absolute; top: 20px; width: 96px; height: 96px; }
</style>
<div id="up" class="dial" style="left:8px" data-pulp-kind="knob">
  <svg width="96" height="96" viewBox="0 0 96 96">
    <circle cx="48" cy="48" r="46" fill="#333"/>
    <line data-pulp-indicator x1="48" y1="48" x2="48" y2="10"
          stroke="#fff" stroke-width="4"/>
  </svg>
</div>
<div id="turned" class="dial" style="left:112px" data-pulp-kind="knob">
  <svg width="96" height="96" viewBox="0 0 96 96">
    <circle cx="48" cy="48" r="46" fill="#333"/>
    <line data-pulp-indicator x1="48" y1="48" x2="48" y2="10"
          stroke="#fff" stroke-width="4" transform="rotate(38 48 48)"/>
  </svg>
</div>
<div id="scaled" class="dial" style="left:216px" data-pulp-kind="knob">
  <svg width="96" height="96" viewBox="0 0 24 24">
    <circle cx="12" cy="12" r="11.5" fill="#333"/>
    <line data-pulp-indicator x1="12" y1="12" x2="12" y2="3"
          stroke="#fff" stroke-width="2"/>
  </svg>
</div>
`);
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "320",
        "--initial-height", "160",
        "--dpr", "2",
        "--timeout-ms", "20000",
      ], { maxBuffer: 1024 * 1024 });

      const report = JSON.parse(
        await readFile(path.join(output, "semantic-report.json"), "utf8"));
      // Keyed on the dial's own left edge, which the page fixes at 8 / 112 /
      // 216. The marked <line> also surfaces as its own candidate (it carries a
      // data-pulp- attribute), so the kind filter is load-bearing.
      const boxFor = (left) => {
        const candidate = report.candidates.find(
          (c) => c.kind === "knob" && c.bounds && Math.abs(c.bounds.left - left) < 1);
        assert.ok(candidate, `a knob at left=${left} must be a semantic candidate`);
        return candidate.indicator_bounds;
      };

      const up = boxFor(8);
      assert.ok(up, "an unrotated 12 o'clock pointer must survive the capture; " +
        "dropping it falls back to the derived tick and looks correct");
      assert.ok(Math.abs(up.width - 4) < 0.5,
        `a 4px stroke must arrive 4px wide, got ${up && up.width}`);
      assert.ok(up.height > 30, "the pointer keeps its length");

      const turned = boxFor(112);
      assert.ok(turned, "the rotated control must keep working");
      assert.ok(turned.width > 20 && turned.height > 20,
        "a rotated pointer reports a fat axis-aligned box on both axes; this " +
        "is the case that passed while 12 o'clock silently failed");

      const scaled = boxFor(216);
      assert.ok(scaled, "a scaled-viewBox pointer must survive too");
      assert.ok(Math.abs(scaled.width - 8) < 0.5,
        "stroke-width is in user units: 2 units in a 24-unit viewBox drawn at " +
        `96px paints 8 CSS px, got ${scaled && scaled.width}`);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("a rotated pointer is described in its own space, from either source",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    // The same 4x38 needle three ways, turned 38 degrees in every one.
    //
    // getBoundingClientRect() reports the box a rotated shape SWEEPS rather than
    // the shape, so all three arrive as a 26.5x32.4 footprint and a width read
    // off that is ten times the truth -- a white slab over a third of the dial.
    // The element's own box is the only description a rotation does not distort,
    // so the candidate carries that and the matrix that places it.
    //
    // Three sources because the recovery differs and the answer must not. An SVG
    // shape has getBBox() in USER UNITS plus a screen CTM; the scaled one proves
    // the units are handled, since 1 user unit in a 24-unit viewBox at 96px is
    // 4 CSS px and reading it as 1 gives a quarter-width hairline. An HTML box
    // has no getBBox at all -- the SVG path returns nothing for a div -- and
    // answers offsetWidth/offsetHeight against its computed matrix instead.
    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-rotated-pointer-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html>
<style>
  html, body { margin: 0; width: 648px; height: 160px; background: #111; }
  .dial { position: absolute; top: 20px; width: 96px; height: 96px; }
  .needle {
    position: absolute; left: 46px; top: 10px; width: 4px; height: 38px;
    background: #fff; rotate: z 38deg;
  }
  .fractional { width: 3.5px; }
  .zoomed { width: 2px; height: 19px; zoom: 2; }
</style>
<div id="svg" class="dial" style="left:8px" data-pulp-kind="knob">
  <svg width="96" height="96" viewBox="0 0 96 96">
    <circle cx="48" cy="48" r="46" fill="#333"/>
    <line data-pulp-indicator x1="48" y1="48" x2="48" y2="10"
          stroke="#fff" stroke-width="4" transform="rotate(38 48 48)"/>
  </svg>
</div>
<div id="scaled" class="dial" style="left:112px" data-pulp-kind="knob">
  <svg width="96" height="96" viewBox="0 0 24 24">
    <circle cx="12" cy="12" r="11.5" fill="#333"/>
    <line data-pulp-indicator x1="12" y1="12" x2="12" y2="2.5"
          stroke="#fff" stroke-width="1" transform="rotate(38 12 12)"/>
  </svg>
</div>
<div id="html" class="dial" style="left:216px" data-pulp-kind="knob">
  <div data-pulp-indicator class="needle zoomed"></div>
</div>
<div id="fractional" class="dial" style="left:320px" data-pulp-kind="knob">
  <div data-pulp-indicator class="needle fractional"></div>
</div>
<div id="asymmetric" class="dial" style="left:424px" data-pulp-kind="knob">
  <svg width="96" height="96" viewBox="0 0 96 96">
    <path data-pulp-indicator d="M48 8 L54 48 L44 48 Z"
          fill="#fff" transform="rotate(38 48 48)"/>
  </svg>
</div>
<div id="non-scaling" class="dial" style="left:528px" data-pulp-kind="knob">
  <svg width="96" height="96" viewBox="0 0 24 24">
    <circle cx="12" cy="12" r="11.5" fill="#333"/>
    <line data-pulp-indicator x1="12" y1="12" x2="12" y2="2.5"
          stroke="#fff" stroke-width="4" vector-effect="non-scaling-stroke"
          transform="rotate(38 12 12)"/>
  </svg>
</div>
`);
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "648",
        "--initial-height", "160",
        "--dpr", "2",
        "--timeout-ms", "20000",
      ], { maxBuffer: 1024 * 1024 });

      const report = JSON.parse(
        await readFile(path.join(output, "semantic-report.json"), "utf8"));
      const boxFor = (left) => {
        const candidate = report.candidates.find(
          (c) => c.kind === "knob" && c.bounds && Math.abs(c.bounds.left - left) < 1);
        assert.ok(candidate, `a knob at left=${left} must be a semantic candidate`);
        return candidate.indicator_bounds;
      };

      for (const [name, left] of [["svg", 8], ["scaled", 112], ["html", 216],
        ["non-scaling", 528]]) {
        const box = boxFor(left);
        assert.ok(box, `the ${name} pointer must survive the capture`);

        // The footprint keeps its old meaning. It is the pointer's painted
        // pixels, which is what the sprite pass crops and erases, so for a
        // rotated needle the fat box is the right answer here and the wrong one
        // for geometry. Recording the element space ADDS a field; it must not
        // quietly redefine this one.
        assert.ok(box.width > 20 && box.height > 20,
          `${name}: the footprint stays the box the needle sweeps, got ` +
          `${box.width}x${box.height}`);

        assert.ok(box.intrinsic, `${name}: the element's own size must be recorded`);
        assert.ok(Array.isArray(box.transform) && box.transform.length === 6,
          `${name}: the matrix must be recorded as six numbers`);
        const [a, b, c, d] = box.transform;

        // The payload, expressed identically for all three: the page-space
        // length of each of the element's own axes is its intrinsic extent times
        // the length of that axis's image. The needle is 4 CSS px across and 38
        // long however it was authored and however it is turned.
        const across = box.intrinsic.width * Math.hypot(a, b);
        const along = box.intrinsic.height * Math.hypot(c, d);
        assert.ok(Math.abs(across - 4) < 0.6,
          `${name}: the needle is 4 CSS px across, got ${across}`);
        assert.ok(Math.abs(along - 38) < 0.6,
          `${name}: the needle is 38 CSS px long, got ${along}`);

        // The rotation really is in the matrix, so a consumer that needs the
        // orientation has it. A pure scale would leave b and c at zero and the
        // extents above would still pass, which is why this is asserted apart
        // from them.
        assert.ok(Math.abs(b) > 0.1 && Math.abs(c) > 0.1,
          `${name}: a 38-degree rotation must survive into the matrix, got ` +
          `[${a}, ${b}, ${c}, ${d}]`);
      }

      // The scaled case is the one that fails silently if user units are read as
      // CSS px, so pin the two numbers that distinguish it: 1x9.5 in its own
      // space, carried by a matrix whose axes are 4 CSS px per user unit.
      const scaled = boxFor(112);
      assert.ok(Math.abs(scaled.intrinsic.width - 1) < 0.01,
        `the scaled needle is 1 USER UNIT across, got ${scaled.intrinsic.width}`);
      assert.ok(Math.abs(Math.hypot(scaled.transform[0], scaled.transform[1]) - 4)
        < 0.01, "a 24-unit viewBox drawn at 96px scales by 4");

      const svg = boxFor(8);
      assert.ok(Math.abs(svg.width - 26.547) < 0.2 &&
        Math.abs(svg.height - 32.407) < 0.2,
        "the rotated SVG footprint includes the 4px stroke without erasing " +
        `past its butt-capped endpoints, got ${svg.width}x${svg.height}`);

      const fractional = boxFor(320);
      const fractionalAcross = fractional.intrinsic.width *
        Math.hypot(fractional.transform[0], fractional.transform[1]);
      assert.ok(Math.abs(fractionalAcross - 3.5) < 0.05,
        `fractional HTML width must not round through offsetWidth, got ${fractionalAcross}`);

      const asymmetric = boxFor(424);
      const [aa, ab, ac, ad, ae, af] = asymmetric.transform;
      const intrinsicCenter = {
        x: aa * (asymmetric.intrinsic.x + asymmetric.intrinsic.width / 2) +
          ac * (asymmetric.intrinsic.y + asymmetric.intrinsic.height / 2) + ae,
        y: ab * (asymmetric.intrinsic.x + asymmetric.intrinsic.width / 2) +
          ad * (asymmetric.intrinsic.y + asymmetric.intrinsic.height / 2) + af,
      };
      assert.ok(Math.abs(asymmetric.intrinsic.x - 44) < 0.01 &&
        Math.abs(asymmetric.intrinsic.y - 8) < 0.01,
        "SVG intrinsic geometry keeps its element-space origin, not only size");
      assert.ok(Number.isFinite(intrinsicCenter.x) &&
        Number.isFinite(intrinsicCenter.y),
        "the full matrix places the intrinsic centre in page space");

      const nonScaling = boxFor(528);
      assert.ok(nonScaling.intrinsic && nonScaling.transform,
        "a non-scaling line must keep oriented page-space geometry");
      assert.ok(Math.abs(nonScaling.width - 26.55) < 0.3 &&
        Math.abs(nonScaling.height - 32.41) < 0.3,
        "non-scaling line erasure must use its 4px page-space stroke, got " +
        `${nonScaling.width}x${nonScaling.height}`);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
