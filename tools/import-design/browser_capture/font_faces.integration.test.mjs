// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { cp, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  captureCaseTimeout,
  installedBrowser,
  runCapture,
} from "./capture_integration_support.mjs";

const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
const fixture = fileURLToPath(new URL(
  "../../../test/fixtures/browser-capture-font-face-url/", import.meta.url));
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

test("a plain page's url() @font-face files are carried with their descriptors",
  { timeout: captureCaseTimeout(1), skip: process.platform === "win32" },
  async (context) => {
    const browser = await installedBrowser();
    if (!browser) return context.skip("no compatible system browser is installed");
    const output = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-font-faces-"));
    try {
      await runCapture(script, browser, path.join(fixture, "panel.html"), fixture,
        output, 480, 200);
      const record = JSON.parse(await readFile(path.join(output, "font-faces.json"), "utf8"));
      const regular = sha256(await readFile(path.join(fixture, "fonts/Jost-Regular.ttf")));
      const bold = sha256(await readFile(path.join(fixture, "fonts/Jost-Bold.ttf")));
      // The unused family was never fetched, so it is not here.
      assert.deepEqual(record.faces.map((face) =>
        [face.family, face.weight, face.style, face.format, face.source, face.sha256]), [
        ["Pulp Page Face", "400", "normal", "truetype",
          "pulp-capture:///fonts/Jost-Regular.ttf", regular],
        ["Pulp Page Face", "700", "normal", "truetype",
          "pulp-capture:///fonts/Jost-Bold.ttf", bold],
      ]);
      for (const asset of record.assets) {
        assert.equal(sha256(Buffer.from(asset.data_base64, "base64")), asset.sha256);
        assert.equal(asset.mime_type, "font/ttf");
      }

      // The oracle: Chromium itself reports shaping each paragraph with the
      // page's own face, at the weight the record binds to that file.
      const fonts = JSON.parse(await readFile(path.join(output, "platform-fonts.json"), "utf8"));
      const shaped = new Map();
      for (const run of fonts.runs) {
        const face = [...run.resolved].sort((a, b) => b.glyph_count - a.glyph_count)[0];
        shaped.set(run.requested.font_weight, face);
      }
      for (const [weight, postScript] of [["400", "Jost-Regular"], ["700", "Jost-Bold"]]) {
        assert.equal(shaped.get(weight)?.is_custom_font, true, weight);
        assert.equal(shaped.get(weight)?.post_script_name, postScript, weight);
      }
    } finally {
      await rm(output, { recursive: true, force: true });
    }
  });

test("a page that names a font file it does not have fails the capture by name",
  { timeout: captureCaseTimeout(1), skip: process.platform === "win32" },
  async (context) => {
    const browser = await installedBrowser();
    if (!browser) return context.skip("no compatible system browser is installed");
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-font-missing-"));
    try {
      await cp(path.join(fixture, "fonts"), path.join(root, "fonts"), { recursive: true });
      await writeFile(path.join(root, "panel.html"), `<!doctype html><style>
        @font-face { font-family: 'Absent Face'; src: url('fonts/absent.ttf'); }
        body { margin: 0; width: 200px; height: 80px; font: 20px 'Absent Face', serif; }
      </style><p>Missing font</p>`);
      await assert.rejects(
        runCapture(script, browser, path.join(root, "panel.html"), root,
          path.join(root, "out"), 200, 80),
        (error) => /capture-font-missing/.test(error.stderr) &&
          /"fonts\/absent\.ttf"/.test(error.stderr) &&
          /pulp-capture:\/\/\/fonts\/absent\.ttf/.test(error.stderr));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
