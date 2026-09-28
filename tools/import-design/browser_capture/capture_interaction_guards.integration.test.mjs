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
  execute,
  installedBrowser,
  rgbaPixel,
} from "./capture_integration_support.mjs";

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
