// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import {
  armCleanupDeadline,
  createCaptureProgress,
} from "./lifecycle.mjs";

test("capture progress names the stalled step and the last completed one",
  () => {
    const progress = createCaptureProgress("loopback-server");
    assert.match(progress.describe(), /phase=loopback-server/);
    assert.match(progress.describe(), /last-completed=none/);

    progress.enterPhase("same-frame-capture");
    progress.begin("DOMSnapshot.captureSnapshot")();
    const stalled = progress.begin("Page.captureScreenshot");

    const described = progress.describe();
    assert.match(described, /phase=same-frame-capture/);
    assert.match(
      described, /last-completed=DOMSnapshot\.captureSnapshot \(\d+ms\) ago/);
    assert.match(described, /stalled=Page\.captureScreenshot \(\d+ms\)/);

    stalled();
    const settled = progress.describe();
    assert.match(settled, /last-completed=Page\.captureScreenshot/);
    assert.doesNotMatch(settled, /stalled=/);
  });

test("an abandoned capture step is not reported as the last completed one",
  () => {
    const progress = createCaptureProgress();
    progress.begin("Runtime.evaluate")();
    progress.begin("Page.captureScreenshot")(false);

    const described = progress.describe();
    assert.match(described, /last-completed=Runtime\.evaluate/);
    assert.doesNotMatch(described, /stalled=/);
  });

test("capture progress reports concurrent stalled steps oldest first", () => {
  const progress = createCaptureProgress();
  for (const method of ["Page.enable", "Runtime.enable", "Log.enable",
    "Network.enable"]) {
    progress.begin(method);
  }
  const described = progress.describe();
  assert.match(described, /stalled=Page\.enable .*Runtime\.enable .*Log\.enable/);
  assert.doesNotMatch(described, /Network\.enable/);
});

test("capture phase totals attribute every millisecond to one phase", () => {
  let clock = 1000;
  const progress = createCaptureProgress("loopback-server", () => clock);
  clock += 5;
  progress.enterPhase("browser-launch");
  clock += 300;
  progress.enterPhase("page-settle");
  clock += 1000;
  progress.enterPhase("authored-viewport-reload");
  clock += 200;
  // A phase entered a second time accumulates rather than restarting.
  progress.enterPhase("page-settle");
  clock += 1500;
  progress.enterPhase("artifact-write");
  clock += 40;

  const timings = progress.timings();
  assert.equal(timings.schema, "pulp-browser-capture-timings-v1");
  assert.deepEqual(timings.phases, {
    "loopback-server": 5,
    "browser-launch": 300,
    "page-settle": 2500,
    "authored-viewport-reload": 200,
    "artifact-write": 40,
  });
  assert.equal(timings.total_ms, 3045);
  const attributed = Object.values(timings.phases)
    .reduce((sum, ms) => sum + ms, 0);
  assert.equal(attributed, timings.total_ms);

  // The running phase keeps running after a report.
  clock += 10;
  assert.equal(progress.timings().phases["artifact-write"], 50);
});

test("capture steps are counted per name, including abandoned ones",
  async () => {
    let clock = 0;
    const progress = createCaptureProgress("same-frame-capture", () => clock);
    const first = progress.begin("Runtime.evaluate");
    clock += 30;
    first();
    const second = progress.begin("Runtime.evaluate");
    clock += 12;
    second(false);
    await progress.time("semantics", async () => {
      clock += 250;
    });
    await assert.rejects(progress.time("canvas-assets", async () => {
      clock += 7;
      throw new Error("canvas went away");
    }), /canvas went away/);

    const { steps } = progress.timings();
    assert.deepEqual(steps["Runtime.evaluate"], { count: 2, ms: 42 });
    assert.deepEqual(steps.semantics, { count: 1, ms: 250 });
    assert.deepEqual(steps["canvas-assets"], { count: 1, ms: 7 });
    // Ordered by time spent so the dominant step reads first.
    assert.deepEqual(Object.keys(steps),
      ["semantics", "Runtime.evaluate", "canvas-assets"]);
    // A failed timed step is not the last thing that completed.
    assert.match(progress.describe(), /last-completed=semantics/);
  });

test("capture deadline finishes cleanup before expiring the runtime", async () => {
  const calls = [];
  await new Promise((resolve) => {
    armCleanupDeadline({
      timeoutMs: 1,
      cleanup: async () => {
        calls.push("cleanup-start");
        await new Promise((done) => setTimeout(done, 5));
        calls.push("cleanup-finished");
      },
      onExpired: (cleanupError) => {
        assert.equal(cleanupError, undefined);
        calls.push("expired");
        resolve();
      },
    });
  });
  assert.deepEqual(
    calls, ["cleanup-start", "cleanup-finished", "expired"]);
});

test("capture deadline reads the stalled state before cleanup erases it",
  async () => {
    const progress = createCaptureProgress("same-frame-capture");
    const stalled = progress.begin("Page.captureScreenshot");
    let summary = "";
    await new Promise((resolve) => {
      armCleanupDeadline({
        timeoutMs: 1,
        onExpiring: () => {
          summary = progress.describe();
        },
        // Cleanup closes the connection, which settles every in-flight call.
        cleanup: async () => stalled(false),
        onExpired: resolve,
      });
    });
    assert.match(summary, /stalled=Page\.captureScreenshot/);
    assert.doesNotMatch(progress.describe(), /stalled=/);
  });

test("completed capture cancels its cleanup deadline", async () => {
  let called = false;
  const cancel = armCleanupDeadline({
    timeoutMs: 5,
    cleanup: async () => {
      called = true;
    },
    onExpired: () => {
      called = true;
    },
  });
  assert.equal(cancel(), true);
  assert.equal(cancel(), false);
  await new Promise((resolve) => setTimeout(resolve, 15));
  assert.equal(called, false);
});
