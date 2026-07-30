// SPDX-License-Identifier: MIT
import { spawn } from "node:child_process";
import {
  mkdir,
  mkdtemp,
  readFile,
  readdir,
} from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { browserEnvironment } from "./security.mjs";

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
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
  if (!child || child.exitCode !== null) return;
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
    return;
  }
  try {
    process.kill(-child.pid, "SIGTERM");
  } catch {
    try {
      child.kill("SIGTERM");
    } catch {
      return;
    }
  }
  await Promise.race([exited, delay(1500)]);
  if (child.exitCode !== null) return;
  try {
    process.kill(-child.pid, "SIGKILL");
  } catch {
    try {
      child.kill("SIGKILL");
    } catch {
      // The process may have exited between the state check and signal.
    }
  }
  await Promise.race([exited, delay(1000)]);
}

export async function createEmptyProfile(profileArg) {
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
    const endpoint = await waitForDevToolsPort(profileDir, child, timeoutMs);
    return { child, endpoint, stderr: () => stderr };
  } catch (error) {
    await terminateBrowser(child);
    error.message += stderr ? `: ${stderr.trim().slice(0, 1000)}` : "";
    throw error;
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
