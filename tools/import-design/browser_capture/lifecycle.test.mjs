// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import test from "node:test";

import { armCleanupDeadline } from "./lifecycle.mjs";

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
