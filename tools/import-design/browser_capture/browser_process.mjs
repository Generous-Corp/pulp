// SPDX-License-Identifier: MIT
import { createHash } from "node:crypto";
import { spawn } from "node:child_process";
import {
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { browserEnvironment } from "./security.mjs";

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

const OWNER_MARKER = ".pulp-browser-owner-v1.json";
const GUARDIAN_READY = ".pulp-browser-guardian-ready";
const guardians = new WeakMap();

function processExists(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error?.code === "EPERM";
  }
}

function processGroupExists(processGroupId) {
  if (process.platform === "win32") return processExists(processGroupId);
  try {
    process.kill(-processGroupId, 0);
    return true;
  } catch (error) {
    return error?.code === "EPERM";
  }
}

async function commandOutput(command, args) {
  return await new Promise((resolve) => {
    let settled = false;
    let timer;
    const finish = (output) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve(output);
    };
    const child = spawn(command, args, {
      env: browserEnvironment(),
      stdio: ["ignore", "pipe", "ignore"],
      windowsHide: true,
    });
    timer = setTimeout(() => {
      try { child.kill("SIGKILL"); } catch {}
      finish("");
    }, 3000);
    let output = "";
    child.stdout?.on("data", (chunk) => {
      if (output.length < 128 * 1024) output += String(chunk);
    });
    child.once("error", () => finish(""));
    child.once("exit", (code) => finish(code === 0 ? output.trim() : ""));
  });
}

export async function browserProcessIdentity(pid) {
  if (!Number.isInteger(pid) || pid <= 0 || !processExists(pid)) return "";
  if (process.platform === "win32") {
    const script =
      `$p=Get-CimInstance Win32_Process -Filter \"ProcessId=${pid}\";` +
      "if($p){$p.CreationDate.ToString('o')+' '+$p.CommandLine}";
    return await commandOutput(
      "powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", script]);
  }
  return await commandOutput(
    "/bin/ps", ["-ww", "-p", String(pid), "-o", "lstart=", "-o", "command="]);
}

function identityHash(identity) {
  return createHash("sha256").update(identity).digest("hex");
}

function identityOwnsProfile(identity, profileDir) {
  const argument = `--user-data-dir=${profileDir}`;
  const nextArgument = "--disable-background-networking";
  return identity.endsWith(argument) ||
    identity.endsWith(`"${argument}"`) ||
    identity.includes(`${argument} ${nextArgument}`) ||
    identity.includes(`"${argument}" ${nextArgument}`);
}

async function terminateOwnedBrowserPid(pid) {
  if (process.platform === "win32") {
    if (!processExists(pid)) return;
    const treeKill = spawn(
      "taskkill.exe", ["/pid", String(pid), "/T", "/F"],
      { stdio: "ignore", windowsHide: true });
    await Promise.race([
      new Promise((resolve) => {
        treeKill.once("error", resolve);
        treeKill.once("exit", resolve);
      }),
      delay(1500),
    ]);
    return;
  }
  if (!processGroupExists(pid)) return;
  try {
    process.kill(-pid, "SIGTERM");
  } catch {
    return;
  }
  const deadline = Date.now() + 1500;
  while (Date.now() < deadline && processGroupExists(pid)) await delay(25);
  if (!processGroupExists(pid)) return;
  try {
    process.kill(-pid, "SIGKILL");
  } catch {
    // The exact process group may have exited between the check and signal.
  }
  const killDeadline = Date.now() + 1000;
  while (Date.now() < killDeadline && processGroupExists(pid)) await delay(25);
}

async function writeOwnershipMarker(profileDir, browserPid, browserIdentity) {
  const ownerIdentity = await browserProcessIdentity(process.pid);
  if (!ownerIdentity || !browserIdentity) {
    throw new Error("could not establish browser lifecycle process identity");
  }
  const marker = {
    schema: "pulp-browser-owner-v1",
    owner_pid: process.pid,
    owner_identity_sha256: identityHash(ownerIdentity),
    browser_pid: browserPid,
    browser_identity_sha256: identityHash(browserIdentity),
  };
  await writeFile(
    path.join(profileDir, OWNER_MARKER), `${JSON.stringify(marker)}\n`,
    { encoding: "utf8", mode: 0o600 });
}

async function recoverStaleBrowserProfile(profileDir) {
  const markerPath = path.join(profileDir, OWNER_MARKER);
  let marker;
  try {
    marker = JSON.parse(await readFile(markerPath, "utf8"));
  } catch {
    return false;
  }
  if (marker?.schema !== "pulp-browser-owner-v1" ||
      !Number.isInteger(marker.owner_pid) ||
      !Number.isInteger(marker.browser_pid) ||
      typeof marker.owner_identity_sha256 !== "string" ||
      typeof marker.browser_identity_sha256 !== "string") {
    return false;
  }

  const ownerIdentity = await browserProcessIdentity(marker.owner_pid);
  if (ownerIdentity &&
      identityHash(ownerIdentity) === marker.owner_identity_sha256) {
    return false;
  }

  const browserIdentity = await browserProcessIdentity(marker.browser_pid);
  if (browserIdentity) {
    if (identityHash(browserIdentity) !== marker.browser_identity_sha256 ||
        !identityOwnsProfile(browserIdentity, profileDir)) {
      return false;
    }
    await terminateOwnedBrowserPid(marker.browser_pid);
    if (processGroupExists(marker.browser_pid)) return false;
  } else if (processGroupExists(marker.browser_pid)) {
    // The recorded leader is gone, so its process-start identity can no longer
    // be rechecked. Preserve a surviving group fail-closed: after a reboot or
    // PID reuse, its numeric PGID alone is not enough proof to signal it.
    return false;
  }
  await rm(profileDir, { force: true, recursive: true, maxRetries: 3 });
  return true;
}

export async function recoverStaleBrowserProfiles(parentDir) {
  let entries;
  try {
    entries = await readdir(parentDir, { withFileTypes: true });
  } catch {
    return 0;
  }
  let recovered = 0;
  for (const entry of entries
    .filter((candidate) => candidate.isDirectory() &&
      candidate.name.startsWith("pulp-browser-capture-"))) {
    if (await recoverStaleBrowserProfile(path.join(parentDir, entry.name))) {
      recovered++;
    }
  }
  return recovered;
}

async function startBrowserGuardian(child, profileDir) {
  if (!child?.pid) throw new Error("browser launched without a process id");
  const browserIdentity = await browserProcessIdentity(child.pid);
  if (!browserIdentity || !identityOwnsProfile(browserIdentity, profileDir)) {
    throw new Error("browser launch identity did not match its owned profile");
  }
  await writeOwnershipMarker(profileDir, child.pid, browserIdentity);

  const guardian = spawn(
    process.execPath,
    [fileURLToPath(import.meta.url), "guard", String(child.pid), profileDir,
      identityHash(browserIdentity)],
    {
      detached: true,
      env: browserEnvironment(),
      stdio: ["pipe", "ignore", "ignore"],
      windowsHide: true,
    });
  await new Promise((resolve, reject) => {
    guardian.once("spawn", resolve);
    guardian.once("error", reject);
  });
  guardians.set(child, guardian);
  guardian.unref();
  guardian.stdin?.unref?.();
  const readyPath = path.join(profileDir, GUARDIAN_READY);
  const deadline = Date.now() + (process.platform === "win32" ? 5000 : 1500);
  while (Date.now() < deadline) {
    if (guardian.exitCode !== null) {
      throw new Error("browser lifecycle guardian exited before custody");
    }
    try {
      if ((await readFile(readyPath, "utf8")).trim() ===
          identityHash(browserIdentity)) return;
    } catch {
      // The guardian publishes readiness after its own identity check.
    }
    await delay(25);
  }
  throw new Error("browser lifecycle guardian did not establish custody");
}

async function stopBrowserGuardian(child) {
  const guardian = guardians.get(child);
  guardians.delete(child);
  if (!guardian) return;
  const exited = new Promise((resolve) => guardian.once("exit", resolve));
  guardian.stdin?.end();
  await Promise.race([exited, delay(6000)]);
}

async function runGuardian(browserPid, profileDir, expectedIdentityHash) {
  const initialIdentity = await browserProcessIdentity(browserPid);
  if (!initialIdentity || identityHash(initialIdentity) !== expectedIdentityHash ||
      !identityOwnsProfile(initialIdentity, profileDir)) {
    process.exitCode = 2;
    return;
  }
  await writeFile(
    path.join(profileDir, GUARDIAN_READY), `${expectedIdentityHash}\n`,
    { encoding: "utf8", mode: 0o600 });
  await new Promise((resolve) => {
    process.stdin.once("end", resolve);
    process.stdin.once("error", resolve);
    process.stdin.resume();
  });
  const currentIdentity = await browserProcessIdentity(browserPid);
  if (!currentIdentity) {
    // The browser leader may exit before a SIGTERM-resistant renderer. This
    // guardian established custody while the leader identity was live, so the
    // still-existing process group remains the exact group it owns.
    await terminateOwnedBrowserPid(browserPid);
    return;
  }
  if (identityHash(currentIdentity) !== expectedIdentityHash ||
      !identityOwnsProfile(currentIdentity, profileDir)) {
    process.exitCode = 3;
    return;
  }
  await terminateOwnedBrowserPid(browserPid);
}

async function waitForDevToolsPort(profileDir, child, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  const activePortFile = path.join(profileDir, "DevToolsActivePort");
  let lastError = "";
  while (Date.now() < deadline) {
    if (child.exitCode !== null) {
      throw new Error(`browser exited before CDP was ready (${child.exitCode})`);
    }
    try {
      const content = await readFile(activePortFile, "utf8");
      const [port, endpoint] = content.trim().split(/\r?\n/);
      if (/^[0-9]+$/.test(port) && endpoint) {
        return { port: Number(port), endpoint };
      }
    } catch (error) {
      lastError = String(error);
    }
    await delay(50);
  }
  throw new Error(`timed out waiting for browser CDP endpoint: ${lastError}`);
}

export async function terminateBrowser(child) {
  if (!child) return;
  if (child.exitCode !== null) {
    // Once the leader has exited its numeric PID is no longer an identity.
    // The guardian established custody while that identity was live, so only
    // it may terminate a surviving renderer process group.
    await stopBrowserGuardian(child);
    return;
  }
  const exited = new Promise((resolve) => child.once("exit", resolve));
  if (process.platform === "win32") {
    // Node's child.kill() maps to TerminateProcess on Windows and reaches only
    // the browser parent. taskkill /T closes Chromium's renderer/GPU process
    // tree as well, so the profile can be removed before the launcher exits.
    const treeKill = spawn(
      "taskkill.exe",
      ["/pid", String(child.pid), "/T", "/F"],
      { stdio: "ignore", windowsHide: true });
    const treeKillFinished = new Promise((resolve) => {
      treeKill.once("error", resolve);
      treeKill.once("exit", resolve);
    });
    await Promise.race([treeKillFinished, delay(1500)]);
    if (treeKill.exitCode === null) {
      try {
        treeKill.kill("SIGKILL");
      } catch {
        // The taskkill helper may have exited between the check and signal.
      }
    }
    if (child.exitCode === null) {
      try {
        child.kill("SIGKILL");
      } catch {
        // taskkill may have reaped the browser between the state check and kill.
      }
    }
    await Promise.race([exited, delay(1000)]);
    await stopBrowserGuardian(child);
    return;
  }
  await terminateOwnedBrowserPid(child.pid);
  await Promise.race([exited, delay(1000)]);
  await stopBrowserGuardian(child);
}

export async function createEmptyProfile(profileArg) {
  await recoverStaleBrowserProfiles(
    profileArg ? path.dirname(profileArg) : os.tmpdir());
  const profileDir = profileArg ||
    await mkdtemp(path.join(os.tmpdir(), "pulp-browser-capture-"));
  await mkdir(profileDir, { recursive: true });
  const entries = await readdir(profileDir);
  if (entries.length !== 0) {
    throw new Error("refusing to use a non-empty browser profile directory");
  }
  return profileDir;
}

export async function launchBrowser(
  browserPath, profileDir, timeoutMs, onSpawn = () => {},
  extraArgs = []) {
  const args = [
    "--headless=new",
    "--remote-debugging-address=127.0.0.1",
    "--remote-debugging-port=0",
    `--user-data-dir=${profileDir}`,
    "--disable-background-networking",
    "--disable-background-timer-throttling",
    "--disable-breakpad",
    "--disable-component-extensions-with-background-pages",
    "--disable-component-update",
    "--disable-default-apps",
    "--disable-domain-reliability",
    "--disable-extensions",
    "--disable-features=AutofillServerCommunication,MediaRouter,OptimizationHints,Translate",
    // Software composition avoids partially assembled full-page headless
    // tiles without forcing imported WebGL/Three.js into SwiftShader.
    "--disable-gpu-compositing",
    "--disable-renderer-backgrounding",
    "--disable-sync",
    "--enable-automation",
    "--force-color-profile=srgb",
    "--hide-scrollbars",
    "--lang=en-US",
    "--metrics-recording-only",
    "--no-default-browser-check",
    "--no-first-run",
    "--password-store=basic",
    "--use-mock-keychain",
    ...extraArgs,
    "about:blank",
  ];
  const child = spawn(browserPath, args, {
    detached: process.platform !== "win32",
    env: browserEnvironment(),
    stdio: ["ignore", "ignore", "pipe"],
    windowsHide: true,
  });
  onSpawn(child);
  let stderr = "";
  child.stderr.on("data", (chunk) => {
    if (stderr.length < 256 * 1024) stderr += String(chunk);
  });
  child.once("error", (error) => {
    // Node's spawn error string embeds the complete executable path. Preserve
    // the actionable error code without persisting host-private paths.
    stderr += `\nbrowser launch failed (${error?.code ?? "spawn-error"})`;
  });
  try {
    await startBrowserGuardian(child, profileDir);
    const endpoint = await waitForDevToolsPort(profileDir, child, timeoutMs);
    return { child, endpoint, stderr: () => stderr };
  } catch (error) {
    await terminateBrowser(child);
    error.message += stderr ? `: ${stderr.trim().slice(0, 1000)}` : "";
    throw error;
  }
}

if (process.argv[1] &&
    import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href &&
    process.argv[2] === "guard") {
  const browserPid = Number(process.argv[3]);
  const profileDir = process.argv[4] ?? "";
  const expectedIdentityHash = process.argv[5] ?? "";
  if (!Number.isInteger(browserPid) || browserPid <= 0 || !profileDir ||
      !/^[0-9a-f]{64}$/.test(expectedIdentityHash)) {
    process.exitCode = 64;
  } else {
    await runGuardian(browserPid, profileDir, expectedIdentityHash);
  }
}

export async function pageTarget(port, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastError = "";
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/list`);
      if (response.ok) {
        const targets = await response.json();
        const page = targets.find((target) => target.type === "page");
        if (page?.webSocketDebuggerUrl) return page;
      }
    } catch (error) {
      lastError = String(error);
    }
    await delay(50);
  }
  throw new Error(`browser exposed no page target: ${lastError}`);
}
