#!/usr/bin/env node
// Offline decoder for local Figma `.fig` files.
//
// Two modes:
//   outline <file.fig> [--json]
//       Read-only inventory of pages and top-level frames (name, guid, size,
//       subtree weight). Use it to pick a frame before a full decode — large
//       community files carry hundreds of frames across many pages.
//   emit <file.fig> --frame <guid|name> --out <dir> [--page <name>]
//       Decode one frame into <dir>/scene.pulp.json plus <dir>/assets/, a Pulp
//       figma-plugin export envelope consumable by
//       `pulp import-design --from figma-plugin --file <dir>/scene.pulp.json`.
//
// No Figma account, MCP, or network access is used; everything comes from the
// local file. Requires Node >= 22 (native zstd).

import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { unpackFig } from './fig/container.mjs';
import { ByteReader, readSchema, makeDecoder } from './fig/kiwi.mjs';
import { buildScene, outline, findFrame, materializeFrame, countFramesByName } from './fig/scene.mjs';

const PARSER_VERSION = '0.1.0-fig';
const COMPAT_SCHEMA_VERSION = '1';

function fail(msg, code = 1) {
  process.stderr.write(`fig_decode: ${msg}\n`);
  process.exit(code);
}

function parseArgs(argv) {
  const opts = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--json') opts.json = true;
    else if (a === '--frame') opts.frame = argv[++i];
    else if (a === '--page') opts.page = argv[++i];
    else if (a === '--out') opts.out = argv[++i];
    else if (a === '--file-key') opts.fileKey = argv[++i];
    else if (a === '--max-frames') opts.maxFrames = Number(argv[++i]);
    else opts._.push(a);
  }
  return opts;
}

function loadScene(path) {
  const bytes = readFileSync(path);
  const container = unpackFig(bytes);
  const schema = readSchema(new ByteReader(container.schemaBytes));
  const decoder = makeDecoder(schema);
  if (decoder.rootIndex === undefined) fail('schema has no root Message type', 3);
  const message = decoder.decodeMessage(new ByteReader(container.messageBytes), decoder.rootIndex);
  return { scene: buildScene(message), container, schemaDefs: schema.length };
}

function renderOutlineText(o) {
  const lines = [];
  lines.push(`file: ${o.title || '(untitled)'}   pages: ${o.pageCount}`);
  for (const page of o.pages) {
    lines.push('');
    lines.push(`  ${page.name}   [${page.frameCount} frame${page.frameCount === 1 ? '' : 's'}]`);
    for (const f of page.frames) {
      const size = f.width && f.height ? `${f.width}×${f.height}` : '—';
      lines.push(`    ${(f.name || '(unnamed)').padEnd(40)} ${size.padStart(11)}  ${f.type.toLowerCase()}  (${f.descendants} nodes)  ${f.guid}`);
    }
  }
  return lines.join('\n');
}

function cmdOutline(path, opts) {
  const { scene, container } = loadScene(path);
  const o = outline(scene, container.meta);
  if (opts.json) process.stdout.write(JSON.stringify(o, bigintReplacer, 2) + '\n');
  else process.stdout.write(renderOutlineText(o) + '\n');
}

function cmdEmit(path, opts) {
  if (!opts.frame) fail('emit requires --frame <guid|name>', 1);
  if (!opts.out) fail('emit requires --out <dir>', 1);
  const { scene, container } = loadScene(path);
  const frame = findFrame(scene, opts.frame);
  if (!frame) fail(`frame not found: ${opts.frame}`, 2);
  // A name (not a guid) can match several frames; warn so an ambiguous pick is
  // visible rather than silently resolving to the first match.
  if (!opts.frame.includes(':') && countFramesByName(scene, opts.frame) > 1) {
    process.stderr.write(
      `fig_decode: warning: multiple frames named '${opts.frame}'; using the first. ` +
      `Pass its guid (see --outline) to disambiguate.\n`,
    );
  }

  const ctx = {
    images: container.images,
    fileKey: opts.fileKey || null,
    parserVersion: PARSER_VERSION,
    compatSchemaVersion: COMPAT_SCHEMA_VERSION,
    // A fixed timestamp keeps envelope output deterministic; callers that need a
    // real time can override provenance downstream.
    exportedAt: '1970-01-01T00:00:00Z',
  };
  const { envelope, assetHashes, diagnostics } = materializeFrame(scene, frame, ctx);

  mkdirSync(opts.out, { recursive: true });
  if (assetHashes.size) mkdirSync(join(opts.out, 'assets'), { recursive: true });
  for (const asset of envelope.asset_manifest.assets) {
    const bytes = container.images.get(asset.hash);
    if (bytes) writeFileSync(join(opts.out, asset.local_path), bytes);
  }
  writeFileSync(join(opts.out, 'scene.pulp.json'), JSON.stringify(envelope, bigintReplacer, 2));

  const warn = diagnostics.filter((d) => d.severity === 'warning').length;
  process.stderr.write(
    `fig_decode: wrote ${join(opts.out, 'scene.pulp.json')} (${envelope.asset_manifest.assets.length} asset(s), ${warn} warning(s))\n`,
  );
}

function bigintReplacer(_k, v) {
  return typeof v === 'bigint' ? Number(v) : v;
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const [cmd, file] = opts._;
  if (!cmd || !file) {
    fail('usage: fig_decode <outline|emit> <file.fig> [options]', 1);
  }
  try {
    if (cmd === 'outline') cmdOutline(file, opts);
    else if (cmd === 'emit') cmdEmit(file, opts);
    else fail(`unknown command: ${cmd}`, 1);
  } catch (err) {
    fail(err && err.message ? err.message : String(err), 3);
  }
}

main();
