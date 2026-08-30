// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

import {
  browserProcessIdentity,
  createEmptyProfile,
  launchBrowser,
  recoverStaleBrowserProfiles,
  terminateBrowser,
} from "./browser_process.mjs";

function processExists(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error?.code === "EPERM";
  }
}

async function waitFor(predicate, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  throw new Error("timed out waiting for browser lifecycle condition");
}

test("browser guardian closes an exact detached tree after abrupt owner death",
  { skip: process.platform === "win32" }, async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-process-test-"));
    const profile = path.join(root, "pulp-browser-capture-owned");
    const nextProfile = path.join(root, "pulp-browser-capture-next");
    const fakeBrowser = path.join(root, "fake-browser.sh");
    const ownerScript = path.join(root, "owner.mjs");
    let owner;
    let browserPid = 0;
    let rendererPid = 0;
    try {
      await writeFile(fakeBrowser, `#!/bin/sh
profile=""
for arg in "$@"; do
  case "$arg" in
    --user-data-dir=*) profile="\${arg#--user-data-dir=}" ;;
  esac
done
test -n "$profile" || exit 64
printf '9222\\n/devtools/browser/test\\n' > "$profile/DevToolsActivePort"
printf '%s\\n' "$$" > "$profile/browser.pid"
/bin/sh -c 'trap "" TERM; while :; do sleep 1; done' &
printf '%s\\n' "$!" > "$profile/renderer.pid"
while :; do sleep 1; done
`, "utf8");
      await chmod(fakeBrowser, 0o700);

      const moduleUrl = pathToFileURL(path.join(
        path.dirname(fileURLToPath(import.meta.url)), "browser_process.mjs"));
      await writeFile(ownerScript, `
import { createEmptyProfile, launchBrowser } from ${JSON.stringify(moduleUrl.href)};
const profile = process.argv[2];
const browser = process.argv[3];
await createEmptyProfile(profile);
await launchBrowser(browser, profile, 2000);
await import("node:fs/promises").then(({writeFile}) =>
  writeFile(process.argv[4], "ready\\n"));
setInterval(() => {}, 1000);
`, "utf8");

      const ready = path.join(root, "ready");
      owner = spawn(process.execPath, [ownerScript, profile, fakeBrowser, ready], {
        stdio: "ignore",
      });
      await waitFor(async () => {
        try {
          await readFile(ready);
          return true;
        } catch {
          return false;
        }
      });
      browserPid = Number((await readFile(
        path.join(profile, "browser.pid"), "utf8")).trim());
      rendererPid = Number((await readFile(
        path.join(profile, "renderer.pid"), "utf8")).trim());
      assert.equal(processExists(browserPid), true);
      assert.equal(processExists(rendererPid), true);

      owner.kill("SIGKILL");
      await waitFor(() => !processExists(browserPid) && !processExists(rendererPid));

      // The killed owner could not remove its profile. A later public capture
      // removes only the profile whose marker proves both stale ownership and
      // the exact browser identity; it never scans or kills generic Chrome.
      await createEmptyProfile(nextProfile);
      await assert.rejects(readFile(profile), { code: "ENOENT" });
    } finally {
      owner?.kill("SIGKILL");
      if (browserPid) {
        try { process.kill(-browserPid, "SIGKILL"); } catch {}
      }
      await rm(root, { recursive: true, force: true });
    }
  });

test("normal termination kills a SIGTERM-resistant browser helper",
  { skip: process.platform === "win32" }, async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-process-test-"));
    const profile = path.join(root, "pulp-browser-capture-owned");
    const fakeBrowser = path.join(root, "fake-browser.sh");
    let launched;
    let browserPid = 0;
    let rendererPid = 0;
    try {
      await writeFile(fakeBrowser, `#!/bin/sh
profile=""
for arg in "$@"; do
  case "$arg" in
    --user-data-dir=*) profile="\${arg#--user-data-dir=}" ;;
  esac
done
test -n "$profile" || exit 64
printf '9222\\n/devtools/browser/test\\n' > "$profile/DevToolsActivePort"
printf '%s\\n' "$$" > "$profile/browser.pid"
/bin/sh -c 'trap "" TERM; while :; do sleep 1; done' &
printf '%s\\n' "$!" > "$profile/renderer.pid"
while :; do sleep 1; done
`, "utf8");
      await chmod(fakeBrowser, 0o700);
      await createEmptyProfile(profile);
      launched = await launchBrowser(fakeBrowser, profile, 2000);
      browserPid = Number((await readFile(
        path.join(profile, "browser.pid"), "utf8")).trim());
      rendererPid = Number((await readFile(
        path.join(profile, "renderer.pid"), "utf8")).trim());
      assert.equal(browserPid, launched.child.pid);

      // Reproduce the mutation: the group leader exits promptly while a
      // renderer ignores SIGTERM. Cleanup must inspect the group, not return
      // merely because the ChildProcess leader has an exit code.
      process.kill(browserPid, "SIGKILL");
      await waitFor(() => !processExists(browserPid));
      assert.equal(processExists(rendererPid), true);
      await terminateBrowser(launched.child);
      await waitFor(() => !processExists(browserPid) && !processExists(rendererPid));
    } finally {
      if (launched?.child) await terminateBrowser(launched.child);
      if (browserPid) {
        try { process.kill(-browserPid, "SIGKILL"); } catch {}
      }
      await rm(root, { recursive: true, force: true });
    }
  });

test("stale recovery refuses a browser whose identity does not match",
  { skip: process.platform === "win32" }, async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-process-test-"));
    const profile = path.join(root, "pulp-browser-capture-unrelated");
    let browser;
    try {
      await createEmptyProfile(profile);
      browser = spawn(
        "/bin/sh", ["-c", "while :; do sleep 1; done", "--",
          `--user-data-dir=${profile}`],
        { detached: true, stdio: "ignore" });
      await waitFor(() => processExists(browser.pid));
      await writeFile(path.join(profile, ".pulp-browser-owner-v1.json"),
        `${JSON.stringify({
          schema: "pulp-browser-owner-v1",
          owner_pid: 2147483647,
          owner_identity_sha256: "0".repeat(64),
          browser_pid: browser.pid,
          browser_identity_sha256: "0".repeat(64),
        })}\n`, "utf8");

      assert.equal(await recoverStaleBrowserProfiles(root), 0);
      assert.equal(processExists(browser.pid), true);
      assert.equal(await readFile(
        path.join(profile, ".pulp-browser-owner-v1.json"), "utf8") !== "", true);
    } finally {
      if (browser?.pid && processExists(browser.pid)) {
        try { process.kill(-browser.pid, "SIGKILL"); } catch {}
      }
      await rm(root, { recursive: true, force: true });
    }
  });

test("stale recovery requires an exact browser profile argument",
  { skip: process.platform === "win32" }, async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-process-test-"));
    const profile = path.join(root, "pulp-browser-capture-owned");
    const otherProfile = `${profile}-extra`;
    let browser;
    try {
      await createEmptyProfile(profile);
      browser = spawn(
        "/bin/sh", ["-c", "while :; do sleep 1; done", "--",
          `--user-data-dir=${otherProfile}`],
        { detached: true, stdio: "ignore" });
      await waitFor(() => processExists(browser.pid));
      const identity = await browserProcessIdentity(browser.pid);
      await writeFile(path.join(profile, ".pulp-browser-owner-v1.json"),
        `${JSON.stringify({
          schema: "pulp-browser-owner-v1",
          owner_pid: 2147483647,
          owner_identity_sha256: "0".repeat(64),
          browser_pid: browser.pid,
          browser_identity_sha256:
            createHash("sha256").update(identity).digest("hex"),
        })}\n`, "utf8");

      assert.equal(await recoverStaleBrowserProfiles(root), 0);
      assert.equal(processExists(browser.pid), true);
    } finally {
      if (browser?.pid && processExists(browser.pid)) {
        try { process.kill(-browser.pid, "SIGKILL"); } catch {}
      }
      await rm(root, { recursive: true, force: true });
    }
  });

test("stale recovery reaches an exact owner beyond 64 unrecoverable profiles",
  { skip: process.platform === "win32" }, async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-process-test-"));
    const profile = path.join(root, "pulp-browser-capture-stale");
    let browser;
    try {
      for (let index = 0; index < 70; index++) {
        await mkdir(path.join(
          root, `pulp-browser-capture-junk-${String(index).padStart(3, "0")}`));
      }
      await createEmptyProfile(profile);
      browser = spawn(
        "/bin/sh", ["-c", "while :; do sleep 1; done", "--",
          `--user-data-dir=${profile}`],
        { detached: true, stdio: "ignore" });
      await waitFor(() => processExists(browser.pid));
      const identity = await browserProcessIdentity(browser.pid);
      assert.match(identity, /--user-data-dir=/);
      await writeFile(path.join(profile, ".pulp-browser-owner-v1.json"),
        `${JSON.stringify({
          schema: "pulp-browser-owner-v1",
          owner_pid: 2147483647,
          owner_identity_sha256: "0".repeat(64),
          browser_pid: browser.pid,
          browser_identity_sha256:
            createHash("sha256").update(identity).digest("hex"),
        })}\n`, "utf8");

      assert.equal(await recoverStaleBrowserProfiles(root), 1);
      await waitFor(() => !processExists(browser.pid));
      await assert.rejects(readFile(profile), { code: "ENOENT" });
    } finally {
      if (browser?.pid && processExists(browser.pid)) {
        try { process.kill(-browser.pid, "SIGKILL"); } catch {}
      }
      await rm(root, { recursive: true, force: true });
    }
  });
