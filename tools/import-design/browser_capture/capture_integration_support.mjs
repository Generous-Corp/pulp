// SPDX-License-Identifier: MIT
// Shared helpers for the real-browser capture integration suites. The cases
// are split across several *.integration.test.mjs files so the Node runner can
// execute them concurrently: each case spends most of its time waiting on a
// cold Chrome launch or a bounded settle, not on CPU.
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { access } from "node:fs/promises";
import { promisify } from "node:util";
import { inflateSync } from "node:zlib";

export const execute = promisify(execFile);

export function escapeRegExp(value) {
  return String(value).replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

export async function runCapture(script, browser, input, root, output, width, height) {
  await execute(process.execPath, [
    script, "capture", "--browser", browser, "--input", input,
    "--root", root, "--output", output,
    "--initial-width", String(width), "--initial-height", String(height),
    "--dpr", "2", "--timeout-ms", "20000",
  ], { maxBuffer: 1024 * 1024 });
}

export function paeth(left, above, upperLeft) {
  const estimate = left + above - upperLeft;
  const leftDistance = Math.abs(estimate - left);
  const aboveDistance = Math.abs(estimate - above);
  const upperLeftDistance = Math.abs(estimate - upperLeft);
  if (leftDistance <= aboveDistance && leftDistance <= upperLeftDistance)
    return left;
  return aboveDistance <= upperLeftDistance ? above : upperLeft;
}

export function rgbaPixel(png, x, y) {
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

export function browserCandidates(environment = process.env) {
  // Match the product and documented CI selection contract. The required
  // macOS gate installs a pinned Chrome-for-Testing build here. An explicit
  // but inaccessible override must fail closed instead of silently exercising
  // whichever mutable system Chrome happens to be installed.
  if (environment.PULP_DESIGN_BROWSER)
    return [environment.PULP_DESIGN_BROWSER];
  // A CI lane that did not provision the pinned build must not fall through to
  // the runner image's mutable Chrome: that measures an unversioned browser,
  // and a hosted image's Chrome has hung the suite. Such a lane skips visibly.
  if (environment.GITHUB_ACTIONS === "true")
    return [];
  return [
    // Retain the test-only legacy override for local callers that already use
    // it when the canonical product setting is absent.
    environment.PULP_BROWSER,
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
  ].filter(Boolean);
}

export async function installedBrowser(environment = process.env) {
  const candidates = browserCandidates(environment);
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

// `layout.styles` rows are positional against the request order the snapshot
// records as `computedStyleNames`, and every value is a string-table index.
// Decoding through the recorded names is the only way to read a row without
// hardcoding a parallel property list that drifts the first time the capture
// collects one more property.
export function laidOutNodes(snapshot) {
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
