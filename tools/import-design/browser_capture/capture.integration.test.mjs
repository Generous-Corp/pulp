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
  }, 100)">Open working screen</button>
</section>
<section id="working">WORKING SCREEN STRING EXISTS WHILE HIDDEN</section>
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
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("real browser wait-for visible rejects invisible ancestors and overlays",
  { timeout: 30000 }, async (context) => {
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
        style: "#overlay { position:absolute;inset:40px 0 0;z-index:2;background:#000 }",
        target: `<div id="target" style="background:rgb(20,210,40)"></div>
          <div id="overlay"></div>`,
        mutation: "document.getElementById('overlay').remove()",
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
