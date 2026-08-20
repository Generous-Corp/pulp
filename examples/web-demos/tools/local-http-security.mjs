import { realpathSync, statSync } from "node:fs";
import { isAbsolute, join, relative, resolve, sep } from "node:path";

export const LOOPBACK_HOST = "127.0.0.1";

const SAFE_SEGMENT = /^[A-Za-z0-9._-]+$/;

export function decodeLocalRequestPath(requestUrl) {
  const target = requestUrl || "/";
  if (!target.startsWith("/") || target.startsWith("//")) return null;
  const encodedPath = target.split(/[?#]/, 1)[0];
  if (/%(?:2f|5c)/i.test(encodedPath)) return null;

  let pathname;
  try {
    pathname = decodeURIComponent(encodedPath);
  } catch {
    return null;
  }

  if (!pathname.startsWith("/") || /[\\\0]/.test(pathname)) return null;
  const segments = pathname.split("/").filter(Boolean);
  if (segments.some((segment) =>
    segment === "." || segment === ".." || !SAFE_SEGMENT.test(segment))) {
    return null;
  }
  return { pathname, segments };
}

function isContained(root, candidate) {
  const rel = relative(root, candidate);
  return rel === "" || (!rel.startsWith(`..${sep}`) && rel !== ".." && !isAbsolute(rel));
}

export function canonicalRoot(root) {
  return realpathSync(resolve(root));
}

export function resolveCanonicalAsset(root, segments, { indexFile = null } = {}) {
  const canonicalBase = canonicalRoot(root);
  const lexicalCandidate = resolve(canonicalBase, ...segments);
  if (!isContained(canonicalBase, lexicalCandidate)) return null;

  let candidate;
  try {
    candidate = realpathSync(lexicalCandidate);
  } catch {
    return null;
  }
  if (!isContained(canonicalBase, candidate)) return null;

  if (indexFile && statSync(candidate).isDirectory()) {
    const indexCandidate = join(candidate, indexFile);
    try {
      candidate = realpathSync(indexCandidate);
    } catch {
      return null;
    }
    if (!isContained(canonicalBase, candidate)) return null;
  }
  return candidate;
}

export function sendFixedText(res, statusCode, body) {
  res.writeHead(statusCode, {
    "content-type": "text/plain; charset=utf-8",
    "cache-control": "no-store",
  });
  res.end(body);
}
