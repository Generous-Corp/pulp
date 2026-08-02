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
