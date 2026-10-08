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
  CAPTURE_DEADLINE_MS,
  captureCaseTimeout,
  execute,
  installedBrowser,
} from "./capture_integration_support.mjs";

// Fitting captures the page, reloads it at the authored frame and captures
// again inside one deadline.
const FIT_RELOAD_DEADLINE_MS = CAPTURE_DEADLINE_MS * 3 / 2;

test("authored-frame fitting reloads once at a contained fixed point",
  { timeout: captureCaseTimeout(1, FIT_RELOAD_DEADLINE_MS) }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-authored-fit-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
  html,body{margin:0;background:#111}
  body{display:flex;justify-content:center}
  #panel{flex:0 0 920px;height:558px;background:#246}
</style><div id="panel"></div>
`);

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
        "--timeout-ms", String(FIT_RELOAD_DEADLINE_MS),
        "--fit-authored-frame",
      ], { maxBuffer: 1024 * 1024 });

      const envelope = JSON.parse(
        await readFile(path.join(output, "capture.json"), "utf8"));
      const viewport = envelope.provenance.viewport;
      assert.deepEqual(viewport.initial, { width: 1280, height: 800 });
      assert.equal(viewport.width_pinned, false);
      assert.deepEqual(viewport.resolved, { width: 920, height: 558 });
      assert.equal(viewport.resolution.mode, "authored-frame-fixed-point");
      assert.equal(viewport.resolution.source, "first-occupying-body-child");
      assert.deepEqual(viewport.resolution.target, { width: 920, height: 558 });
      assert.equal(viewport.resolution.fixed_point, true);
      assert.equal(viewport.resolution.contained, true);
      assert.equal(viewport.resolution.reload_count, 1);
      assert.deepEqual(envelope.reference.authored_frame,
        { x: 0, y: 0, width: 920, height: 558 });
      assert.equal(envelope.reference.logical_width, 920);
      assert.equal(envelope.reference.logical_height, 558);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("oversized authored canvas settles before default and fit capture",
  { timeout: captureCaseTimeout(2, FIT_RELOAD_DEADLINE_MS) }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-authored-fit-canvas-"));
    const input = path.join(root, "panel.html");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
  html,body{margin:0;width:1320px;height:860px;overflow:hidden;background:#111}
  #panel{position:absolute;left:0;top:0;width:1320px;height:860px;background:#246}
  canvas{position:absolute;inset:0;width:1320px;height:860px}
</style><div id="panel"><canvas id="surface"></canvas></div><script>
  const canvas = document.getElementById('surface');
  const paint = () => {
    const context = canvas.getContext('2d');
    context.fillStyle = 'rgb(210,40,70)';
    context.fillRect(0, 0, canvas.width, canvas.height);
  };
  const resize = () => {
    canvas.width = innerWidth * devicePixelRatio;
    canvas.height = innerHeight * devicePixelRatio;
    requestAnimationFrame(paint);
  };
  resize();
  addEventListener('resize', resize);
</script>
`);

      for (const fit of [false, true]) {
        const output = path.join(root, fit ? "fit" : "default");
        const args = [
          script,
          "capture",
          "--browser", browser,
          "--input", input,
          "--root", root,
          "--output", output,
          "--initial-width", "1280",
          "--initial-height", "800",
          "--dpr", "2",
          "--timeout-ms", String(FIT_RELOAD_DEADLINE_MS),
        ];
        if (fit) args.push("--fit-authored-frame");
        await execute(process.execPath, args, { maxBuffer: 1024 * 1024 });
        const envelope = JSON.parse(
          await readFile(path.join(output, "capture.json"), "utf8"));
        assert.deepEqual(envelope.provenance.viewport.resolved,
          { width: 1320, height: 860 });
        assert.deepEqual(envelope.reference.authored_frame,
          { x: 0, y: 0, width: 1320, height: 860 });
        assert.equal(envelope.reference.logical_width, 1320);
        assert.equal(envelope.reference.logical_height, 860);
      }
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("authored-frame fitting rejects a viewport-relative non-fixed point",
  { timeout: captureCaseTimeout() }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-authored-nonconvergent-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
  html,body{margin:0;background:#111}
  #panel{width:calc(100vw - 1px);height:200px;background:#246}
</style><div id="panel"></div>
`);

      await assert.rejects(execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "1280",
        "--initial-height", "300",
        "--dpr", "2",
        "--timeout-ms", String(CAPTURE_DEADLINE_MS),
        "--fit-authored-frame",
      ], { maxBuffer: 1024 * 1024 }), (error) =>
        error.stderr.includes("capture-authored-viewport-nonconvergent"));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("authored-frame fitting rejects a root lost before sidecar collection",
  { timeout: captureCaseTimeout() }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-authored-presidecar-missing-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
  html,body{margin:0;background:#111}
  #panel{width:920px;height:200px;background:#246}
</style><div id="panel"></div><script>
  const queryAll = document.querySelectorAll.bind(document);
  document.querySelectorAll = selector => {
    if (selector === "body *") document.getElementById("panel")?.remove();
    return queryAll(selector);
  };
</script>
`);

      await assert.rejects(execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "1280",
        "--initial-height", "300",
        "--dpr", "2",
        "--timeout-ms", String(CAPTURE_DEADLINE_MS),
        "--fit-authored-frame",
      ], { maxBuffer: 1024 * 1024 }), (error) =>
        error.stderr.includes("capture-authored-frame-unavailable"));
      const diagnostic = JSON.parse(
        await readFile(path.join(output, "capture-error.json"), "utf8"));
      assert.equal(diagnostic.code, "capture-authored-frame-unavailable");
      assert.equal(diagnostic.phase, "same-frame-capture");
      await assert.rejects(access(path.join(output, "capture.json")));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("authored-frame fitting rejects a root lost before accepted pixels",
  { timeout: captureCaseTimeout() }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-authored-late-missing-"));
    const input = path.join(root, "panel.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
  html,body{margin:0;background:#111}
  #panel{width:920px;height:200px;background:#246}
</style><div id="panel"></div><script>
  const queryAll = document.querySelectorAll.bind(document);
  document.querySelectorAll = selector => {
    const result = queryAll(selector);
    // Health collection is after the existing pre-sidecar authored-frame
    // check. Queue the removal so that health sees its synchronous result but
    // browser.png captures the changed DOM, specifically exercising the pixel
    // boundary rather than either earlier guard.
    if (selector ===
        "body *:not(script):not(style):not(link):not(meta):not(template)") {
      queueMicrotask(() => document.getElementById("panel")?.remove());
    }
    return result;
  };
</script>
`);

      await assert.rejects(execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--initial-width", "1280",
        "--initial-height", "300",
        "--dpr", "2",
        "--timeout-ms", String(CAPTURE_DEADLINE_MS),
        "--fit-authored-frame",
      ], { maxBuffer: 1024 * 1024 }), (error) =>
        error.stderr.includes("capture-authored-frame-unavailable"));
      const diagnostic = JSON.parse(
        await readFile(path.join(output, "capture-error.json"), "utf8"));
      assert.equal(diagnostic.code, "capture-authored-frame-unavailable");
      assert.equal(diagnostic.phase, "same-frame-capture");
      await assert.rejects(access(path.join(output, "capture.json")));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("authored-frame fitting rejects a missing or uncontained root",
  { timeout: captureCaseTimeout(2) }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    const cases = [
      {
        name: "missing",
        html: "<!doctype html><style>html,body{margin:0;background:#111}</style>",
        code: "capture-authored-frame-unavailable",
      },
      {
        name: "uncontained",
        html: `<!doctype html><style>
  html,body{margin:0;background:#111}
  #panel{margin-left:10px;width:920px;height:200px;background:#246}
</style><div id="panel"></div>`,
        code: "capture-authored-frame-not-contained",
      },
    ];
    for (const fixture of cases) {
      const root = await mkdtemp(
        path.join(os.tmpdir(), `pulp-browser-authored-${fixture.name}-`));
      const input = path.join(root, "panel.html");
      const output = path.join(root, "capture");
      try {
        await writeFile(input, fixture.html);
        await assert.rejects(execute(process.execPath, [
          script,
          "capture",
          "--browser", browser,
          "--input", input,
          "--root", root,
          "--output", output,
          "--initial-width", "1280",
          "--initial-height", "300",
          "--dpr", "2",
          "--timeout-ms", String(CAPTURE_DEADLINE_MS),
          "--fit-authored-frame",
        ], { maxBuffer: 1024 * 1024 }), (error) =>
          error.stderr.includes(fixture.code));
      } finally {
        await rm(root, { recursive: true, force: true });
      }
    }
  });
