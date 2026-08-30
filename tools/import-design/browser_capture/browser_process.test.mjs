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

async function processGroupId(pid) {
  return await new Promise((resolve, reject) => {
    const child = spawn("/bin/ps", ["-p", String(pid), "-o", "pgid="], {
      stdio: ["ignore", "pipe", "ignore"],
    });
    let output = "";
    child.stdout.on("data", (chunk) => { output += String(chunk); });
    child.once("error", reject);
    child.once("exit", (code) => {
      if (code !== 0) reject(new Error(`ps exited ${code}`));
      else resolve(Number(output.trim()));
    });
  });
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
while :; do printf 'browser diagnostic\n' >&2; sleep 0.05; done
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
          await readFile(path.join(profile, "renderer.pid"));
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

test("browser guardian starts without inheriting a caller home",
  { skip: process.platform === "win32" }, async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-process-test-"));
    const profile = path.join(root, "pulp-browser-capture-no-home");
    const fakeBrowser = path.join(root, "fake-browser.sh");
    let launched;
    let browserPid = 0;
    const previous = new Map([
      ["HOME", process.env.HOME],
      ["XDG_CACHE_HOME", process.env.XDG_CACHE_HOME],
      ["XDG_CONFIG_HOME", process.env.XDG_CONFIG_HOME],
      ["XDG_DATA_HOME", process.env.XDG_DATA_HOME],
    ]);
    try {
      await writeFile(fakeBrowser, `#!/bin/sh
profile=""
for arg in "$@"; do
  case "$arg" in
    --user-data-dir=*) profile="\${arg#--user-data-dir=}" ;;
  esac
done
test -n "$profile" || exit 64
test -n "$HOME" || exit 65
case "$HOME" in "$profile"/*) ;; *) exit 66 ;; esac
test -d "$XDG_CACHE_HOME" || exit 67
printf '9222\\n/devtools/browser/test\\n' > "$profile/DevToolsActivePort"
printf '%s\\n' "$$" > "$profile/browser.pid"
while :; do sleep 1; done
`, "utf8");
      await chmod(fakeBrowser, 0o700);
      for (const key of previous.keys()) delete process.env[key];
      await createEmptyProfile(profile);
      launched = await launchBrowser(fakeBrowser, profile, 2000);
      browserPid = Number((await readFile(
        path.join(profile, "browser.pid"), "utf8")).trim());
      assert.equal(processExists(browserPid), true);
    } finally {
      if (launched?.child) await terminateBrowser(launched.child);
      for (const [key, value] of previous) {
        if (value === undefined) delete process.env[key];
        else process.env[key] = value;
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
      assert.notEqual(browserPid, launched.child.pid);
      assert.equal(processExists(launched.child.pid), true);
      assert.equal(await processGroupId(launched.child.pid), launched.child.pid);
      assert.equal(await processGroupId(browserPid), launched.child.pid);
      assert.equal(await processGroupId(rendererPid), launched.child.pid);

      // Reproduce the mutation: the group receives its grace signal, Chromium
      // exits, and a renderer ignores it. The stable custody anchor deliberately
      // survives until verified cleanup escalates for the complete group.
      process.kill(-launched.child.pid, "SIGTERM");
      await waitFor(() => !processExists(browserPid));
      assert.equal(processExists(rendererPid), true);
      assert.equal(processExists(launched.child.pid), true);
      await terminateBrowser(launched.child);
      await waitFor(() => !processExists(launched.child.pid) &&
        !processExists(browserPid) && !processExists(rendererPid));
    } finally {
      if (launched?.child) await terminateBrowser(launched.child);
      if (launched?.child?.pid) {
        try { process.kill(-launched.child.pid, "SIGKILL"); } catch {}
      }
      await rm(root, { recursive: true, force: true });
    }
  });

test("pre-ready browser exit preserves custody of a surviving helper",
  { skip: process.platform === "win32" }, async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "pulp-browser-process-test-"));
    const profile = path.join(root, "pulp-browser-capture-pre-ready-exit");
    const fakeBrowser = path.join(root, "fake-browser.sh");
    let anchorPid = 0;
    let browserPid = 0;
    let helperPid = 0;
    try {
      await writeFile(fakeBrowser, `#!/bin/sh
profile=""
for arg in "$@"; do
  case "$arg" in
    --user-data-dir=*) profile="\${arg#--user-data-dir=}" ;;
  esac
done
test -n "$profile" || exit 64
printf '%s\\n' "$$" > "$profile/browser.pid"
/bin/sh -c 'trap "" TERM; while :; do sleep 1; done' &
printf '%s\\n' "$!" > "$profile/helper.pid"
exit 72
`, "utf8");
      await chmod(fakeBrowser, 0o700);
      await createEmptyProfile(profile);
      await assert.rejects(
        launchBrowser(fakeBrowser, profile, 2000, (child) => {
          anchorPid = child.pid;
        }),
        /browser exited before CDP was ready/);
      browserPid = Number((await readFile(
        path.join(profile, "browser.pid"), "utf8")).trim());
      helperPid = Number((await readFile(
        path.join(profile, "helper.pid"), "utf8")).trim());
      assert.equal(processExists(anchorPid), false);
      assert.equal(processExists(browserPid), false);
      assert.equal(processExists(helperPid), false);
    } finally {
      if (anchorPid) {
        try { process.kill(-anchorPid, "SIGKILL"); } catch {}
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
      let identity = "";
      await waitFor(async () => {
        identity = await browserProcessIdentity(browser.pid);
        return identity.includes("--user-data-dir=");
      });
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
      let identity = "";
      await waitFor(async () => {
        identity = await browserProcessIdentity(browser.pid);
        return identity.includes("--user-data-dir=");
      });
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
