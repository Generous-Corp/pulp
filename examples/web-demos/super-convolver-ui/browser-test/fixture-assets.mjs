import { existsSync } from "node:fs";
import { extname, resolve } from "node:path";

export function resolveFixtureAsset(requestUrl, { sourceDir, buildDir }) {
  let path;
  try {
    path = decodeURIComponent(requestUrl.split("?")[0]);
  } catch {
    return null;
  }

  const name = path.replace(/^\//, "");
  if (!name || name === "." || name === ".." || /[/\\\0]/.test(name)) {
    return null;
  }

  const sourceFile = resolve(sourceDir, name);
  if (extname(name) === ".js" && existsSync(sourceFile)) {
    return sourceFile;
  }
  return resolve(buildDir, name);
}
