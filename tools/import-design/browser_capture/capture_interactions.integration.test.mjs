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
  execute,
  installedBrowser,
  rgbaPixel,
} from "./capture_integration_support.mjs";

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
