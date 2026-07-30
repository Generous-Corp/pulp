// SPDX-License-Identifier: MIT
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import {
  chmod,
  mkdir,
  mkdtemp,
  rm,
  stat,
  symlink,
  writeFile,
} from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import test from "node:test";

import {
  authorizeInput,
  browserEnvironment,
  hostResolverRules,
  installNetworkGuard,
  isPublicAddress,
  normalizeDeclaredHttpsOrigins,
  sanitizeCaptureError,
  sanitizeSnapshot,
  serveAuthorizedRoot,
  serveDenyProxy,
} from "./security.mjs";

const execFileAsync = promisify(execFile);
const captureScript = fileURLToPath(
  new URL("./capture.mjs", import.meta.url));
const securityModuleUrl = new URL("./security.mjs", import.meta.url).href;

async function withTempTree(label, body) {
  const root = await mkdtemp(
    path.join(os.tmpdir(), `pulp-browser-${label}-`));
  try {
    return await body(root);
  } finally {
    await rm(root, { force: true, recursive: true });
  }
}

test("authorized loopback server resolves sibling assets but no escapes",
  async () => withTempTree("server", async (tree) => {
    const staged = path.join(tree, "authorized root");
    const outside = path.join(tree, "outside.txt");
    await mkdir(path.join(staged, "assets"), { recursive: true });
    await writeFile(path.join(staged, "index.html"),
      '<script src="./assets/support.js"></script>');
    await writeFile(path.join(staged, "assets", "support.js"), "support();");
    await writeFile(outside, "secret");
    if (process.platform !== "win32") {
      await symlink(outside, path.join(staged, "assets", "escape.txt"));
    }

    const authorization = await authorizeInput(
      staged, path.join(staged, "index.html"));
    const served = await serveAuthorizedRoot(
      authorization.root, authorization.relativeEntry);
    try {
      const entry = await fetch(served.entryUrl);
      assert.equal(entry.status, 200);
      assert.match(await entry.text(), /support\.js/);

      const sibling = await fetch(`${served.privatePrefix}assets/support.js`);
      assert.equal(sibling.status, 200);
      assert.equal(await sibling.text(), "support();");

      const favicon = await fetch(
        new URL("/favicon.ico", served.entryUrl));
      assert.equal(favicon.status, 204);

      const traversal = await fetch(
        `${served.privatePrefix}%2e%2e%2foutside.txt`);
      assert.equal(traversal.status, 404);

      if (process.platform !== "win32") {
        const symlinkEscape = await fetch(
          `${served.privatePrefix}assets/escape.txt`);
        assert.equal(symlinkEscape.status, 404);
      }
    } finally {
      await served.close();
    }
  }));

test("authorized loopback server serves an extensionless entry as HTML",
  async () => withTempTree("extensionless", async (tree) => {
    const entry = path.join(tree, "export");
    await writeFile(entry, "<!doctype html><main>Design</main>");
    const authorization = await authorizeInput(tree, entry);
    const served = await serveAuthorizedRoot(
      authorization.root, authorization.relativeEntry);
    try {
      const response = await fetch(served.entryUrl);
      assert.equal(
        response.headers.get("content-type"),
        "text/html; charset=utf-8");
    } finally {
      await served.close();
    }
  }));

test("authorized server assigns script MIME to extensionless dependencies",
  async () => withTempTree("extensionless-script", async (tree) => {
    const entry = path.join(tree, "index.html");
    const dependency = path.join(tree, "bootstrap");
    await writeFile(entry, '<script type="module" src="./bootstrap"></script>');
    await writeFile(dependency, "globalThis.ready = true;");
    const authorization = await authorizeInput(tree, entry);
    const served = await serveAuthorizedRoot(
      authorization.root, authorization.relativeEntry);
    try {
      const response = await fetch(`${served.privatePrefix}bootstrap`, {
        headers: { "Sec-Fetch-Dest": "script" },
      });
      assert.equal(
        response.headers.get("content-type"),
        "text/javascript; charset=utf-8");
    } finally {
      await served.close();
    }
  }));

test("default-network deny proxy refuses ordinary requests",
  async () => {
    const proxy = await serveDenyProxy();
    try {
      const response = await fetch(proxy.url);
      assert.equal(response.status, 403);
    } finally {
      await proxy.close();
    }
  });

test("loopback servers survive parallel client socket resets",
  async () => withTempTree("socket-reset", async (tree) => {
    await writeFile(path.join(tree, "index.html"), "<main>Design</main>");
    const script = `
      import { connect } from "node:net";
      import {
        serveAuthorizedRoot,
        serveDenyProxy,
      } from ${JSON.stringify(securityModuleUrl)};

      const root = ${JSON.stringify(tree)};
      const authorized = await serveAuthorizedRoot(root, "index.html");
      const proxy = await serveDenyProxy();
      const reset = (port, request) => new Promise((resolve) => {
        const socket = connect(port, "127.0.0.1", () => {
          socket.write(request);
          socket.resetAndDestroy();
          resolve();
        });
        socket.on("error", resolve);
      });
      try {
        const authorizedPort = Number(new URL(authorized.origin).port);
        const proxyPort = Number(new URL(proxy.url).port);
        await Promise.all(Array.from({ length: 100 }, (_, index) =>
          index % 2 === 0
            ? reset(
                proxyPort,
                "CONNECT example.com:443 HTTP/1.1\\\\r\\\\n" +
                  "Host: example.com\\\\r\\\\n\\\\r\\\\n")
            : reset(
                authorizedPort,
                "GET /truncated HTTP/1.1\\\\r\\\\n" +
                  "Host: 127.0.0.1\\\\r\\\\n")));
        await new Promise((resolve) => setTimeout(resolve, 100));
      } finally {
        await Promise.all([proxy.close(), authorized.close()]);
      }
    `;
    const { stderr } = await execFileAsync(
      process.execPath,
      ["--input-type=module", "--eval", script],
      { timeout: 10000 });
    assert.equal(stderr, "");
  }));

test("input authorization resolves symlinks before containment", {
  skip: process.platform === "win32",
},
  async () => withTempTree("authorize", async (tree) => {
    const staged = path.join(tree, "staged");
    const outside = path.join(tree, "outside.html");
    await mkdir(staged);
    await writeFile(outside, "<main>outside</main>");
    await symlink(outside, path.join(staged, "linked.html"));
    await assert.rejects(
      authorizeInput(staged, path.join(staged, "linked.html")),
      /escapes the authorized staged root/);
  }));

test("capture snapshots redact the private loopback token", () => {
  const prefix = "http://127.0.0.1:43123/private-token/";
  const snapshot = {
    strings: [
      `${prefix}index.html`,
      `url("${prefix}assets/skin.png")`,
    ],
    nested: { source: `${prefix}support.js` },
  };
  assert.deepEqual(sanitizeSnapshot(snapshot, prefix), {
    strings: [
      "pulp-capture:///index.html",
      'url("pulp-capture:///assets/skin.png")',
    ],
    nested: { source: "pulp-capture:///support.js" },
  });
});

test("capture errors redact tokenized URLs from messages and health", () => {
  const prefix = "http://127.0.0.1:43123/private-token/";
  const error = new Error(
    `exception at ${prefix}editor.html:22`);
  error.health = {
    runtime_exceptions: [`at ${prefix}bundle.js:10`],
  };
  assert.deepEqual(sanitizeCaptureError(error, prefix), {
    message: "exception at pulp-capture:///editor.html:22",
    health: {
      runtime_exceptions: ["at pulp-capture:///bundle.js:10"],
    },
  });
});

test("capture errors redact host paths and external URL details", () => {
  const error = new Error(
    "spawn /Applications/Private Browser.app/Contents/MacOS/Private Browser " +
    "ENOENT at " +
    "https://example.com/private?q=secret");
  assert.deepEqual(sanitizeCaptureError(error, ""), {
    message: "spawn <local-path> ENOENT at https://example.com",
  });

  const linuxError = new Error(
    "realpath '/opt/acme secret/private.html' failed; " +
    "workspace=/workspace/acme/private.html");
  assert.deepEqual(sanitizeCaptureError(linuxError, ""), {
    message:
      "realpath '<local-path>' failed; workspace=<local-path>",
  });
});

test("browser environment forwards only the capture allowlist", () => {
  const environment = browserEnvironment({
    HOME: "/private/home",
    HTTPS_PROXY: "https://secret-proxy",
    LANG: "en_US.UTF-8",
    PATH: "/usr/bin",
    PULP_TOKEN: "secret",
    TMPDIR: "/tmp/capture",
  });
  assert.deepEqual(environment, {
    LANG: "en_US.UTF-8",
    PATH: "/usr/bin",
    TMPDIR: "/tmp/capture",
  });
});

test("network guard allows only exact reviewed HTTPS origins",
  async () => {
    const listeners = new Map();
    const calls = [];
    const cdp = {
      on(method, listener) {
        listeners.set(method, listener);
      },
      async call(method, params) {
        calls.push({ method, params });
      },
    };
    const guard = installNetworkGuard(
      cdp,
      "http://127.0.0.1:1234",
      ["https://cdn.example.com"],
      "http://127.0.0.1:1234/token/");
    await guard.enable();
    assert.deepEqual(calls.shift(), {
      method: "Network.setBlockedURLs",
      params: { urls: ["ws://*", "wss://*"] },
    });
    assert.deepEqual(calls.shift(), {
      method: "Fetch.enable",
      params: { patterns: [{ urlPattern: "*" }] },
    });

    await listeners.get("Fetch.requestPaused")({
      requestId: "local",
      request: { url: "http://127.0.0.1:1234/token/index.html" },
    });
    await listeners.get("Fetch.requestPaused")({
      requestId: "root-relative",
      request: { url: "http://127.0.0.1:1234/assets/app.js?v=1" },
    });
    await listeners.get("Fetch.requestPaused")({
      requestId: "allowed",
      request: { url: "https://cdn.example.com/icons.js?v=1" },
    });
    await listeners.get("Fetch.requestPaused")({
      requestId: "external",
      request: { url: "https://example.com/private?q=secret" },
    });
    listeners.get("Network.webSocketCreated")({
      url: "wss://socket.example.com/private?token=secret",
    });
    assert.equal(calls[0].method, "Fetch.continueRequest");
    assert.deepEqual(calls[1], {
      method: "Fetch.continueRequest",
      params: {
        requestId: "root-relative",
        url: "http://127.0.0.1:1234/token/assets/app.js?v=1",
      },
    });
    assert.equal(calls[2].method, "Fetch.continueRequest");
    assert.equal(calls[3].method, "Fetch.failRequest");
    assert.deepEqual(guard.blocked, [
      {
        url: "https://example.com",
        reason: "blockedByClient",
      },
      {
        url: "wss://socket.example.com",
        reason: "blockedByClient",
      },
    ]);
  });

test("declared origin policy rejects private and non-HTTPS endpoints", () => {
  assert.deepEqual(normalizeDeclaredHttpsOrigins([
    "https://cdn.example.com/a.js?token=secret",
    "https://CDN.example.com:443/other.js",
    "http://cdn.example.com/insecure.js",
    "wss://cdn.example.com/socket",
    "https://127.0.0.1/private",
    "https://169.254.169.254/latest/meta-data",
    "https://[::1]/private",
    "https://user:password@cdn.example.com/secret",
  ]), ["https://cdn.example.com"]);

  for (const address of [
    "127.0.0.1",
    "10.0.0.1",
    "100.64.0.1",
    "169.254.169.254",
    "172.16.0.1",
    "192.168.0.1",
    "198.51.100.1",
    "203.0.113.1",
    "::1",
    "::ffff:127.0.0.1",
    "64:ff9b::7f00:1",
    "100::1",
    "2001:db8::1",
    "2002:7f00:1::",
    "fd00::1",
    "fe80::1",
  ]) {
    assert.equal(isPublicAddress(address), false, address);
  }
  assert.equal(isPublicAddress("1.1.1.1"), true);
  assert.equal(isPublicAddress("2606:4700:4700::1111"), true);
});

test("host resolver rules pin reviewed hosts and deny every other lookup", () => {
  const resolved = new Map([
    ["https://cdn.example.com", {
      hostname: "cdn.example.com",
      address: "203.1.2.3",
    }],
  ]);
  assert.equal(
    hostResolverRules(resolved),
    "MAP cdn.example.com 203.1.2.3, MAP * ~NOTFOUND, EXCLUDE 127.0.0.1");
});

test("allowed external responses record content-addressed provenance",
  async () => {
    const listeners = new Map();
    const cdp = {
      on(method, listener) {
        listeners.set(method, listener);
      },
      async call(method) {
        if (method === "Network.getResponseBody") {
          return {
            body: Buffer.from("reviewed dependency").toString("base64"),
            base64Encoded: true,
          };
        }
      },
    };
    const guard = installNetworkGuard(
      cdp,
      "http://127.0.0.1:1234",
      ["https://cdn.example.com"]);
    listeners.get("Network.responseReceived")({
      requestId: "dependency",
      response: {
        url: "https://cdn.example.com/icons.js?cache=ignored",
        status: 200,
        mimeType: "text/javascript",
      },
    });
    listeners.get("Network.loadingFinished")({ requestId: "dependency" });
    await guard.awaitProvenance();
    assert.deepEqual(guard.external, [{
      url: "https://cdn.example.com",
      origin: "https://cdn.example.com",
      status: 200,
      mime_type: "text/javascript",
      sha256:
        "708298514230905ddc2a9bbb833a7950a18b46b5191280effff2206421e90c6b",
      size_bytes: 19,
    }]);
  });

test("capture fails closed when an allowed response cannot be hashed",
  async () => {
    const listeners = new Map();
    const cdp = {
      on(method, listener) {
        listeners.set(method, listener);
      },
      async call(method) {
        if (method === "Network.getResponseBody")
          throw new Error("body evicted");
      },
    };
    const guard = installNetworkGuard(
      cdp,
      "http://127.0.0.1:1234",
      ["https://cdn.example.com"]);
    listeners.get("Network.responseReceived")({
      requestId: "dependency",
      response: {
        url: "https://cdn.example.com/icons.js?secret=redacted",
        status: 200,
        mimeType: "text/javascript",
      },
    });
    listeners.get("Network.loadingFinished")({ requestId: "dependency" });
    await assert.rejects(
      guard.awaitProvenance(),
      (error) =>
        error.code === "capture-network-provenance-incomplete" &&
        !error.message.includes("secret="));
    assert.deepEqual(guard.external, []);
  });

test("failed browser launch still removes the ephemeral profile", {
  skip: Number(process.versions.node.split(".")[0]) < 22,
}, async () => withTempTree("cleanup", async (tree) => {
  const profile = path.join(tree, "profile");
  await mkdir(profile);
  await assert.rejects(
    execFileAsync(process.execPath, [
      captureScript,
      "probe",
      "--browser", path.join(tree, "definitely-not-a-browser"),
      "--profile-dir", profile,
      "--timeout-ms", "500",
    ]),
    /browser-capture-failed|browser exited before CDP was ready/);
  await assert.rejects(stat(profile), /ENOENT/);
}));

test("capture deadline removes the profile of a launching browser", {
  skip: process.platform === "win32" ||
    Number(process.versions.node.split(".")[0]) < 22,
}, async () => withTempTree("deadline-cleanup", async (tree) => {
  const profile = path.join(tree, "profile");
  const browser = path.join(tree, "hanging-browser");
  await mkdir(profile);
  await writeFile(browser, `#!/bin/sh
while :; do sleep 1; done
`);
  await chmod(browser, 0o755);

  await assert.rejects(
    execFileAsync(process.execPath, [
      captureScript,
      "probe",
      "--browser", browser,
      "--profile-dir", profile,
      "--timeout-ms", "300",
    ]),
    (error) => {
      assert.equal(error.code, 124);
      assert.match(error.stderr, /browser-capture-timeout/);
      return true;
    });

  await assert.rejects(stat(profile), /ENOENT/);
}));
