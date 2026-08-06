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
