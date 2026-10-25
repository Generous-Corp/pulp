// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { readdir, rm } from "node:fs/promises";
import test from "node:test";

import {
  createEmptyProfile,
  launchBrowser,
  pageTarget,
  terminateBrowser,
} from "./browser_process.mjs";
import {
  CAPTURE_DEADLINE_MS,
  captureCaseTimeout,
  installedBrowser,
} from "./capture_integration_support.mjs";

function cdpCall(pipe) {
  let nextId = 1000;
  const pending = new Map();
  pipe.onMessage((data) => {
    const message = JSON.parse(data);
    const settle = pending.get(message.id);
    if (!settle) return;
    pending.delete(message.id);
    settle(message);
  });
  return (method, params = {}, sessionId = undefined) =>
    new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, (message) => message.error
        ? reject(new Error(`${method}: ${message.error.message}`))
        : resolve(message.result));
      pipe.send(JSON.stringify(
        { id, method, params, ...(sessionId ? { sessionId } : {}) }));
    });
}

// lsof exits 1 when no process in the group matches, and 0 with one line per
// listening socket when any does. Null means lsof is not installed.
function listeningTcpSockets(processGroupId) {
  return new Promise((resolve, reject) => {
    execFile("lsof",
      ["-nP", "-a", "-g", String(processGroupId), "-iTCP", "-sTCP:LISTEN"],
      (error, stdout) => {
        if (!error) resolve(stdout.trim());
        else if (error.code === 1 && !stdout.trim()) resolve("");
        else if (error.code === "ENOENT") resolve(null);
        else reject(error);
      });
  });
}

test("a real browser answers CDP over its pipe and listens on no TCP port",
  { timeout: captureCaseTimeout(), skip: process.platform === "win32" },
  async (context) => {
    const browser = await installedBrowser();
    if (!browser) {
      context.skip("no compatible system browser is installed");
      return;
    }
    const profile = await createEmptyProfile();
    let launched;
    try {
      launched = await launchBrowser(browser, profile, CAPTURE_DEADLINE_MS);
      const call = cdpCall(launched.pipe);
      const page = await pageTarget(call, CAPTURE_DEADLINE_MS);
      const { sessionId } = await call(
        "Target.attachToTarget", { targetId: page.targetId, flatten: true });
      const evaluated = await call(
        "Runtime.evaluate", { expression: "6 * 7" }, sessionId);
      assert.equal(evaluated.result.value, 42);

      // A debugging port announces itself in the profile; the pipe does not.
      assert.equal((await readdir(profile)).includes("DevToolsActivePort"),
        false);
      // The launch leads its own process group, which holds Chromium and
      // every helper it forked. macOS always ships lsof; a Linux image may not.
      const listening = await listeningTcpSockets(launched.child.pid);
      if (listening !== null || process.platform === "darwin") {
        assert.equal(listening, "");
      }
    } finally {
      if (launched?.child) await terminateBrowser(launched.child);
      await rm(profile, {
        force: true, recursive: true, maxRetries: 3, retryDelay: 50 });
    }
  });
