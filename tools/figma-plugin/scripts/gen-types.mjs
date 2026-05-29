#!/usr/bin/env node
// Generates TypeScript types from schema/figma-plugin-export-v1.json.
// This is the SHARED SOURCE OF TRUTH for the export envelope (planning v3 §7.4).
// Run on schema changes; the generated file is committed alongside the schema.

import { compileFromFile } from "json-schema-to-typescript";
import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const schemaPath = path.resolve(here, "..", "schema", "figma-plugin-export-v1.json");
const outPath = path.resolve(here, "..", "src", "types.generated.ts");

const banner = `/* eslint-disable */
// AUTO-GENERATED from schema/figma-plugin-export-v1.json — DO NOT EDIT BY HAND.
// Regenerate via: npm run gen-types
`;

const ts = await compileFromFile(schemaPath, {
  bannerComment: banner,
  style: { semi: true, singleQuote: false, printWidth: 100 },
  additionalProperties: false,
});

await fs.writeFile(outPath, ts, "utf8");
console.log("[pulp figma plugin] wrote", path.relative(process.cwd(), outPath));
