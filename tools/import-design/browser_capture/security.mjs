// SPDX-License-Identifier: MIT
import { createReadStream } from "node:fs";
import { realpath, stat } from "node:fs/promises";
import { createServer } from "node:http";
import { randomBytes } from "node:crypto";
import path from "node:path";

const MIME_TYPES = new Map([
  [".css", "text/css; charset=utf-8"],
  [".gif", "image/gif"],
  [".htm", "text/html; charset=utf-8"],
  [".html", "text/html; charset=utf-8"],
  [".ico", "image/x-icon"],
  [".jpeg", "image/jpeg"],
  [".jpg", "image/jpeg"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".mjs", "text/javascript; charset=utf-8"],
  [".otf", "font/otf"],
  [".png", "image/png"],
  [".svg", "image/svg+xml"],
  [".ttf", "font/ttf"],
  [".wasm", "application/wasm"],
  [".webp", "image/webp"],
  [".woff", "font/woff"],
  [".woff2", "font/woff2"],
]);

export function isPathInside(root, candidate) {
  const relative = path.relative(root, candidate);
  return relative === "" ||
    (!relative.startsWith(`..${path.sep}`) && relative !== ".." &&
      !path.isAbsolute(relative));
}

export async function authorizeInput(rootPath, inputPath) {
  const root = await realpath(rootPath);
  const input = await realpath(inputPath);
  const [rootStat, inputStat] = await Promise.all([stat(root), stat(input)]);
  if (!rootStat.isDirectory()) throw new Error("staged root is not a directory");
  if (!inputStat.isFile()) throw new Error("capture input is not a regular file");
  if (!isPathInside(root, input)) {
    throw new Error("capture input escapes the authorized staged root");
  }
  return { root, input, relativeEntry: path.relative(root, input) };
}

function encodeRelativePath(relativePath) {
  return relativePath.split(path.sep).map(encodeURIComponent).join("/");
}

async function resolveServedFile(root, requestPath) {
  let decoded;
  try {
    decoded = decodeURIComponent(requestPath);
  } catch {
    return null;
  }
  if (decoded.includes("\0") || decoded.includes("\\")) return null;
  const unresolved = path.resolve(root, decoded.replace(/^\/+/, ""));
  if (!isPathInside(root, unresolved)) return null;

  let resolved;
  try {
    resolved = await realpath(unresolved);
  } catch {
    return null;
  }
  if (!isPathInside(root, resolved)) return null;
  const fileStat = await stat(resolved);
  return fileStat.isFile() ? { resolved, size: fileStat.size } : null;
}

function servedContentType(request, servedPath, authorizedEntry) {
  if (servedPath === authorizedEntry)
    return "text/html; charset=utf-8";
  const extension = path.extname(servedPath).toLowerCase();
  const known = MIME_TYPES.get(extension);
  if (known) return known;
  if (!extension) {
    const destination =
      String(request.headers["sec-fetch-dest"] || "").toLowerCase();
    if (destination === "script" || destination === "worker" ||
        destination === "serviceworker") {
      return "text/javascript; charset=utf-8";
    }
    if (destination === "style")
      return "text/css; charset=utf-8";
  }
  return "application/octet-stream";
}

function tolerateClientSocketErrors(server) {
  // Browser shutdown and parallel capture cleanup can reset a loopback
  // connection while a response is still being written. Raw CONNECT sockets
  // are handed directly to serveDenyProxy's listener, so Node's HTTP machinery
  // does not retain an error handler for them after the event is dispatched.
  server.on("connection", (socket) => {
    socket.on("error", () => {});
  });
  server.on("clientError", (_error, socket) => {
    socket.destroy();
  });
}

export async function serveAuthorizedRoot(root, relativeEntry) {
  const token = randomBytes(24).toString("hex");
  const prefix = `/${token}/`;
  const authorizedEntry =
    await realpath(path.resolve(root, relativeEntry));
  const server = createServer(async (request, response) => {
    try {
      if (request.method !== "GET" && request.method !== "HEAD") {
        response.writeHead(405, { Allow: "GET, HEAD" });
        response.end();
        return;
      }
      const requestUrl = new URL(request.url ?? "/", "http://127.0.0.1");
      // Chromium requests this automatically when a document does not declare
      // an icon. Keep that browser-generated request out of capture health
      // without treating arbitrary missing authored resources as successful.
      if (requestUrl.pathname === "/favicon.ico") {
        response.writeHead(204, { "Cache-Control": "no-store" });
        response.end();
        return;
      }
      if (!requestUrl.pathname.startsWith(prefix)) {
        response.writeHead(404);
        response.end();
        return;
      }
      const served = await resolveServedFile(
        root, requestUrl.pathname.slice(prefix.length));
      if (!served) {
        response.writeHead(404);
        response.end();
        return;
      }
      response.writeHead(200, {
        "Cache-Control": "no-store",
        "Content-Length": String(served.size),
        "Content-Type": servedContentType(
          request, served.resolved, authorizedEntry),
        "X-Content-Type-Options": "nosniff",
      });
      if (request.method === "HEAD") {
        response.end();
        return;
      }
      createReadStream(served.resolved).pipe(response);
    } catch (error) {
      response.writeHead(500);
      response.end(String(error));
    }
  });
  tolerateClientSocketErrors(server);

  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (!address || typeof address === "string") {
    server.close();
    throw new Error("loopback server did not expose a TCP address");
  }
  const origin = `http://127.0.0.1:${address.port}`;
  const entryUrl = `${origin}${prefix}${encodeRelativePath(relativeEntry)}`;
  return {
    origin,
    entryUrl,
    privatePrefix: `${origin}${prefix}`,
    publicUrl: `pulp-capture:///${encodeRelativePath(relativeEntry)}`,
    close: () =>
      new Promise((resolve) => server.close(() => resolve())),
  };
}

export async function serveDenyProxy() {
  const server = createServer((request, response) => {
    response.writeHead(403, {
      "Cache-Control": "no-store",
      "Connection": "close",
    });
    response.end("browser capture network access denied");
  });
  tolerateClientSocketErrors(server);
  server.on("connect", (_request, socket) => {
    socket.end(
      "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
  });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (!address || typeof address === "string") {
    server.close();
    throw new Error("deny proxy did not expose a TCP address");
  }
  return {
    url: `http://127.0.0.1:${address.port}`,
    close: () =>
      new Promise((resolve) => server.close(() => resolve())),
  };
}

export function installNetworkGuard(
    cdp, allowedOrigin, allowNetwork, privatePrefix = "") {
  const blocked = [];
  cdp.on("Fetch.requestPaused", async ({ requestId, request }) => {
    try {
      const url = new URL(request.url);
      const local = url.origin === allowedOrigin;
      const embedded = url.protocol === "data:" || url.protocol === "blob:" ||
        url.protocol === "about:";
      const externalAllowed = allowNetwork &&
        (url.protocol === "http:" || url.protocol === "https:" ||
         url.protocol === "ws:" || url.protocol === "wss:");
      if (local || embedded || externalAllowed) {
        const localRootRelative =
          local && privatePrefix &&
          !request.url.startsWith(privatePrefix) &&
          url.pathname !== "/favicon.ico";
        await cdp.call("Fetch.continueRequest", {
          requestId,
          ...(localRootRelative
            ? {
                url: `${privatePrefix}${url.pathname.replace(/^\/+/, "")}` +
                  url.search,
              }
            : {}),
        });
      } else {
        blocked.push({ url: sanitizedUrl(request.url), reason: "blockedByClient" });
        await cdp.call("Fetch.failRequest", {
          requestId,
          errorReason: "BlockedByClient",
        });
      }
    } catch {
      blocked.push({ url: "(invalid URL)", reason: "blockedByClient" });
      await cdp.call("Fetch.failRequest", {
        requestId,
        errorReason: "BlockedByClient",
      });
    }
  });
  if (!allowNetwork) {
    cdp.on("Network.webSocketCreated", ({ url }) => {
      blocked.push({
        url: sanitizedUrl(url),
        reason: "blockedByClient",
      });
    });
  }
  return {
    blocked,
    enable: async () => {
      await cdp.call("Network.setBlockedURLs", {
        urls: allowNetwork ? [] : ["ws://*", "wss://*"],
      });
      await cdp.call("Fetch.enable", {
        patterns: [{ urlPattern: "*" }],
      });
    },
  };
}

export function sanitizeSnapshot(value, privatePrefix) {
  if (typeof value === "string") {
    let sanitized = privatePrefix
      ? value.split(privatePrefix).join("pulp-capture:///")
      : value;
    sanitized = sanitized.replace(
      /\bspawn\s+.+?\s+(ENOENT|EACCES|EPERM)\b/giu,
      "spawn <local-path> $1");
    sanitized = sanitized.replace(
      /\b(?:https?|wss?):\/\/[^\s"'<>]+/giu,
      (url) => sanitizedUrl(url));
    const captureUrls = [];
    sanitized = sanitized.replace(
      /pulp-capture:\/\/\/[^\s"'<>)]*/giu,
      (url) => {
        const marker = `PULP_CAPTURE_URL_${captureUrls.length}_`;
        captureUrls.push(url);
        return marker;
      });
    sanitized = sanitized.replace(
      /file:\/\/\/[^\s"'<>]+/giu, "file:///<local-path>");
    sanitized = sanitized.replace(
      /(["'])(\/[^"'\r\n]+)\1/gu, "$1<local-path>$1");
    sanitized = sanitized.replace(
      /(?<![\w:/.-])\/(?!\/)[^\s"'<>;,)]*/gu, "<local-path>");
    sanitized = sanitized.replace(
      /\b[A-Za-z]:\\[^\s"'<>]*/gu, "<local-path>");
    captureUrls.forEach((url, index) => {
      sanitized = sanitized.replace(`PULP_CAPTURE_URL_${index}_`, url);
    });
    return sanitized;
  }
  if (Array.isArray(value)) {
    return value.map((item) => sanitizeSnapshot(item, privatePrefix));
  }
  if (value && typeof value === "object") {
    const result = {};
    for (const [key, item] of Object.entries(value)) {
      result[key] = sanitizeSnapshot(item, privatePrefix);
    }
    return result;
  }
  return value;
}

export function sanitizeCaptureError(error, privatePrefix) {
  const sanitized = {
    message: sanitizeSnapshot(
      String(error?.message ?? error), privatePrefix),
  };
  if (error?.health) {
    sanitized.health = sanitizeSnapshot(error.health, privatePrefix);
  }
  return sanitized;
}

export function sanitizedUrl(value) {
  try {
    const url = new URL(value);
    if (url.hostname === "127.0.0.1" || url.hostname === "localhost") {
      return "pulp-capture:///";
    }
    return `${url.protocol}//${url.host}`;
  } catch {
    return "(invalid URL)";
  }
}

export function browserEnvironment(source = process.env) {
  const result = {};
  for (const key of [
    "LANG",
    "LC_ALL",
    "PATH",
    "SystemRoot",
    "TEMP",
    "TMP",
    "TMPDIR",
    "WINDIR",
  ]) {
    if (source[key]) result[key] = source[key];
  }
  return result;
}
