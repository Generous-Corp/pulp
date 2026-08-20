import { extname } from "node:path";
import { canonicalRoot, decodeLocalRequestPath, resolveCanonicalAsset } from "../../tools/local-http-security.mjs";

export function resolveFixtureAsset(requestUrl, { sourceDir, buildDir }) {
  const requestPath = decodeLocalRequestPath(requestUrl);
  if (!requestPath || requestPath.segments.length !== 1) return null;
  const [name] = requestPath.segments;
  if (extname(name) === ".js") {
    const sourceFile = resolveCanonicalAsset(canonicalRoot(sourceDir), [name]);
    if (sourceFile) return sourceFile;
  }
  return resolveCanonicalAsset(canonicalRoot(buildDir), [name]);
}
