import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const packed = JSON.parse(execFileSync(
  "npm",
  ["pack", "--dry-run", "--json"],
  { cwd: packageRoot, encoding: "utf8" },
));
const paths = new Set(packed[0].files.map(({ path }) => path));

for (const required of [
  "LICENSE",
  "THIRD_PARTY_NOTICES.md",
  "src/theme/Inter-Regular.ttf",
  "src/theme/inter.woff2",
]) {
  assert(paths.has(required), `packed npm artifact is missing ${required}`);
}

const license = readFileSync(resolve(packageRoot, "LICENSE"), "utf8");
assert.match(license, /^MIT License/m);
assert.match(license, /Copyright \(c\) 2026 Daniel Raffel/);

const notices = readFileSync(resolve(packageRoot, "THIRD_PARTY_NOTICES.md"), "utf8");
assert.match(notices, /Copyright \(c\) 2016 The Inter Project Authors/);
assert.match(notices, /SIL OPEN FONT LICENSE Version 1\.1/);
assert.match(notices, /^5\) The Font Software/m);
assert.match(notices, /^TERMINATION$/m);

console.log("package license payload: ok");
