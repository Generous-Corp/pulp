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
} from "./capture_integration_support.mjs";

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

// The automatic width correction grows the viewport by both clipped margins
// once. A layout that changes width at a breakpoint moves the edge that growth
// was chasing, so the single bounded step lands mid-breakpoint and stays
// negative, while the authored desktop width resolves the same page exactly.
// The uncorrected run is the control: without it a passing pinned run proves
// only that the page captures, not that pinning decided anything.
test("an explicit --width resolves a layout the bounded correction cannot",
  { timeout: 60000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-pinned-width-"));
    const input = path.join(root, "shell.html");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
  html,body{margin:0;background:#111}
  body{display:flex;justify-content:center}
  #canvas{flex:0 0 1600px;height:200px;background:#246}
  @media (min-width:1500px){ #canvas{flex-basis:2600px} }
</style><div id="canvas"></div>
`);

      const uncorrected = path.join(root, "uncorrected");
      await assert.rejects(execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", uncorrected,
        "--initial-width", "1280",
        "--initial-height", "300",
        "--dpr", "2",
        "--timeout-ms", "30000",
      ], { maxBuffer: 1024 * 1024 }), (error) =>
        error.stderr.includes("capture-negative-overflow"));

      const pinned = path.join(root, "pinned");
      await execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", pinned,
        "--width", "2600",
        "--initial-height", "300",
        "--dpr", "2",
        "--timeout-ms", "30000",
      ], { maxBuffer: 1024 * 1024 });

      const envelope = JSON.parse(
        await readFile(path.join(pinned, "capture.json"), "utf8"));
      assert.equal(envelope.provenance.viewport.width_pinned, true);
      assert.equal(envelope.provenance.viewport.initial.width, 2600);
      assert.equal(envelope.provenance.viewport.resolved.width, 2600);
      assert.equal(envelope.reference.logical_width, 2600);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("authored-frame fitting reloads once at a contained fixed point",
  { timeout: 60000 }, async (context) => {
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
        "--timeout-ms", "45000",
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

test("authored-frame fitting rejects a viewport-relative non-fixed point",
  { timeout: 30000 }, async (context) => {
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
        "--timeout-ms", "20000",
        "--fit-authored-frame",
      ], { maxBuffer: 1024 * 1024 }), (error) =>
        error.stderr.includes("capture-authored-viewport-nonconvergent"));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

test("authored-frame fitting rejects a root lost before sidecar collection",
  { timeout: 30000 }, async (context) => {
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
        "--timeout-ms", "20000",
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
  { timeout: 45000 }, async (context) => {
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
        "--timeout-ms", "30000",
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
  { timeout: 60000 }, async (context) => {
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
          "--timeout-ms", "20000",
          "--fit-authored-frame",
        ], { maxBuffer: 1024 * 1024 }), (error) =>
          error.stderr.includes(fixture.code));
      } finally {
        await rm(root, { recursive: true, force: true });
      }
    }
  });

// Content anchored left of the document origin is unreachable at every
// viewport width, so a refusal that names a width as its remedy sends the
// caller round a loop that cannot terminate. The refusal must describe what
// was measured and both layouts that produce it.
test("an unreachable left overflow names its cause, not a phantom flag",
  { timeout: 30000 }, async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }

    const root = await mkdtemp(
      path.join(os.tmpdir(), "pulp-browser-anchored-overflow-"));
    const input = path.join(root, "anchored.html");
    const output = path.join(root, "capture");
    const script = fileURLToPath(new URL("./capture.mjs", import.meta.url));
    try {
      await writeFile(input, `<!doctype html><style>
  html,body{margin:0;background:#111}
  #panel{width:600px;height:200px;background:#246}
  #glow{position:absolute;left:-40px;top:20px;width:120px;height:120px;
    background:#8a4}
</style><div id="panel"></div><div id="glow"></div>
`);

      await assert.rejects(execute(process.execPath, [
        script,
        "capture",
        "--browser", browser,
        "--input", input,
        "--root", root,
        "--output", output,
        "--width", "900",
        "--initial-height", "300",
        "--dpr", "2",
        "--timeout-ms", "20000",
      ], { maxBuffer: 1024 * 1024 }), (error) => {
        assert.ok(error.stderr.includes("capture-negative-overflow"));
        assert.ok(error.stderr.includes("x=-40px"));
        assert.ok(error.stderr.includes(
          "overflow-hidden ancestor clips those pixels"));
        assert.ok(error.stderr.includes(
          "anchors content left of the document origin"));
        assert.ok(error.stderr.includes("position: fixed"));
        assert.equal(error.stderr.includes("pass an explicit --width"), false);
        assert.ok(error.stderr.includes("does not resolve at 900px"));
        return true;
      });
      const diagnostic = JSON.parse(
        await readFile(path.join(output, "capture-error.json"), "utf8"));
      assert.equal(diagnostic.code, "capture-negative-overflow");
      await assert.rejects(access(path.join(output, "capture.json")));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
