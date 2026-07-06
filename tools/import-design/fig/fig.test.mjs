// Unit tests for the offline .fig decoder. Run with: node --test
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

import { ByteReader, readSchema, makeDecoder } from './kiwi.mjs';
import { unpackFig, isZip } from './container.mjs';
import { buildScene, outline, findFrame, materializeFrame, countFramesByName, framesByName, nodesByName } from './scene.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const FIXTURE = join(here, '../../../test/fixtures/imports/fig/synthetic.fig');
const GENERATOR = join(here, 'testdata/make_synthetic_fig.mjs');

function loadFixtureScene() {
  const container = unpackFig(readFileSync(FIXTURE));
  const schema = readSchema(new ByteReader(container.schemaBytes));
  const decoder = makeDecoder(schema);
  const message = decoder.decodeMessage(new ByteReader(container.messageBytes), decoder.rootIndex);
  return { scene: buildScene(message), container, schema, decoder };
}

test('ByteReader round-trips varint, float, and string encodings', () => {
  // Encode a few values with the generator's writer path by exercising the
  // reader against known kiwi byte sequences.
  const r = new ByteReader(Buffer.from([0x00])); // float zero optimization
  assert.equal(r.float(), 0);

  const s = new ByteReader(Buffer.from([0x96, 0x01])); // varuint 150
  assert.equal(s.varUint(), 150);

  const str = new ByteReader(Buffer.from([0x68, 0x69, 0x00, 0xff])); // "hi\0"
  assert.equal(str.string(), 'hi');
  assert.equal(str.byte(), 0xff);
});

test('fixture is a ZIP container with an inner canvas', () => {
  const bytes = readFileSync(FIXTURE);
  assert.ok(isZip(bytes), 'fixture should be a ZIP');
  const container = unpackFig(bytes);
  assert.equal(container.magic, 'fig-kiwi');
  assert.equal(container.version, 106);
  assert.ok(container.images.size >= 1, 'should bundle at least one image');
  assert.equal(container.meta && container.meta.file_name, 'Synthetic Fixture');
});

test('schema decodes and exposes a root Message type', () => {
  const { schema, decoder } = loadFixtureScene();
  assert.ok(schema.length >= 5, 'schema should carry several definitions');
  assert.notEqual(decoder.rootIndex, undefined, 'root Message must resolve');
});

test('outline lists pages and their top-level frames', () => {
  const { scene, container } = loadFixtureScene();
  const o = outline(scene, container.meta);
  assert.equal(o.pageCount, 2);
  const pageOne = o.pages.find((p) => p.name === 'Page One');
  assert.ok(pageOne, 'Page One present');
  assert.equal(pageOne.frameCount, 1);
  assert.equal(pageOne.frames[0].name, 'Plugin UI');
  assert.equal(pageOne.frames[0].width, 320);
  assert.equal(pageOne.frames[0].height, 200);
  const empty = o.pages.find((p) => p.name === 'Empty Page');
  assert.equal(empty.frameCount, 0);
});

test('findFrame resolves by name and by guid', () => {
  const { scene } = loadFixtureScene();
  assert.equal(findFrame(scene, 'Plugin UI').name, 'Plugin UI');
  assert.equal(findFrame(scene, '0:2').name, 'Plugin UI');
  assert.equal(findFrame(scene, 'plugin ui').name, 'Plugin UI', 'case-insensitive');
  assert.equal(findFrame(scene, 'nope'), null);
});

// A scene with the same top-level frame name on two pages — the exact shape that
// makes a bare --frame <name> ambiguous in a large community file.
function twoPageDuplicateScene() {
  const frame = (local, name, parentLocal) => ({
    guid: { sessionID: 0, localID: local }, type: 'FRAME', name,
    parentIndex: { guid: { sessionID: 0, localID: parentLocal }, position: 'a' },
    size: { x: 10, y: 10 },
  });
  return buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page One' },
    { guid: { sessionID: 0, localID: 2 }, type: 'CANVAS', name: 'Page Two' },
    frame(3, 'Panel', 1),   // Page One / Panel
    frame(4, 'Panel', 2),   // Page Two / Panel  (same name, different page)
    frame(5, 'Solo', 1),    // Page One / Solo   (unique)
  ]});
}

test('framesByName reports every match with its page and guid', () => {
  const scene = twoPageDuplicateScene();
  const matches = framesByName(scene, 'Panel');
  assert.equal(matches.length, 2);
  assert.deepEqual(matches.map((m) => m.page).sort(), ['Page One', 'Page Two']);
  assert.equal(countFramesByName(scene, 'Panel'), 2);        // delegates to framesByName
  assert.equal(countFramesByName(scene, 'Solo'), 1);
  // A page restriction narrows the match set.
  const onPageTwo = framesByName(scene, 'Panel', 'Page Two');
  assert.equal(onPageTwo.length, 1);
  assert.equal(onPageTwo[0].page, 'Page Two');
});

test('findFrame restricts a name lookup to the requested page', () => {
  const scene = twoPageDuplicateScene();
  const onPageOne = findFrame(scene, 'Panel', 'Page One');
  const onPageTwo = findFrame(scene, 'Panel', 'Page Two');
  assert.ok(onPageOne && onPageTwo);
  // Distinct frames on distinct pages — page scoping actually selects.
  assert.equal(onPageOne.guid.localID, 3);
  assert.equal(onPageTwo.guid.localID, 4);
  // A name absent from the requested page is not found, even if it exists elsewhere.
  assert.equal(findFrame(scene, 'Solo', 'Page Two'), null);
  // A guid is global; a page hint does not suppress it.
  assert.equal(findFrame(scene, '0:4', 'Page One').guid.localID, 4);
});

test('a colon in a frame name is not mistaken for a guid', () => {
  // Two top-level frames literally named "16:9" — a name a substring `:` test
  // would wrongly treat as a guid, skipping the ambiguity guard. No node has the
  // guid "16:9", so guid-by-membership correctly classifies it as a name.
  const frame = (local, name, parentLocal) => ({
    guid: { sessionID: 0, localID: local }, type: 'FRAME', name,
    parentIndex: { guid: { sessionID: 0, localID: parentLocal }, position: 'a' },
    size: { x: 10, y: 10 },
  });
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page One' },
    { guid: { sessionID: 0, localID: 2 }, type: 'CANVAS', name: 'Page Two' },
    frame(3, '16:9', 1),
    frame(4, '16:9', 2),
  ]});
  assert.equal(scene.byGuid.has('16:9'), false, 'the name is not a guid key');
  assert.equal(framesByName(scene, '16:9').length, 2, 'both same-named frames are seen');
});

test('nodesByName finds same-named nested nodes for the fallback guard', () => {
  // Two nodes named "Button" nested under different top-level frames, with no
  // top-level "Button" — the case where findFrame falls back to a nested match.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Card A',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 10, y: 10 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Card B',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'b' }, size: { x: 10, y: 10 } },
    { guid: { sessionID: 0, localID: 4 }, type: 'FRAME', name: 'Button',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 5, y: 5 } },
    { guid: { sessionID: 0, localID: 5 }, type: 'FRAME', name: 'Button',
      parentIndex: { guid: { sessionID: 0, localID: 3 }, position: 'a' }, size: { x: 5, y: 5 } },
  ]});
  assert.equal(framesByName(scene, 'Button').length, 0, 'no top-level Button');
  assert.equal(nodesByName(scene, 'Button').length, 2, 'two nested Buttons caught');
});

test('materializeFrame builds a valid figma-plugin envelope', () => {
  const { scene, container } = loadFixtureScene();
  const frame = findFrame(scene, 'Plugin UI');
  const { envelope } = materializeFrame(scene, frame, {
    images: container.images,
    fileKey: 'TESTKEY',
    parserVersion: '0.1.0-test',
    compatSchemaVersion: '1',
    exportedAt: '1970-01-01T00:00:00Z',
  });
  assert.equal(envelope.format_version, '2026.05-figma-plugin-v1');
  assert.equal(envelope.provenance.adapter, 'figma-plugin');
  assert.equal(envelope.root.name, 'Plugin UI');
  assert.equal(envelope.root.style.width, 320);
  assert.equal(envelope.root.style.background_color, '#14171c');
  assert.equal(envelope.root.style.border_radius, 8);

  // Recognition is the importer's job; the decoder only carries structure.
  const knob = envelope.root.children.find((c) => c.name.startsWith('Knob'));
  assert.ok(!('audio_widget' in knob), 'decoder does not classify widgets');
  assert.ok(knob.asset_ref, 'knob carries its image asset ref');
  assert.equal(knob.style.border_radius, 32);

  const label = envelope.root.children.find((c) => c.name === 'Label');
  assert.equal(label.type, 'text');
  assert.equal(label.text, 'CUTOFF');
  assert.equal(label.style.color, '#ffffff');
  assert.ok(!('background_color' in label.style), 'text has no background');

  assert.equal(envelope.asset_manifest.assets.length, 1);
});

test('unterminated string throws instead of hanging', () => {
  const r = new ByteReader(Buffer.from([0x68, 0x69])); // "hi" with no NUL, then EOF
  assert.throws(() => r.string(), /unterminated string/);
});

test('a cyclic parent graph does not recurse without bound', () => {
  // Two frames that are each other's parent — a malformed graph.
  const message = { nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'P' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'A',
      parentIndex: { guid: { sessionID: 0, localID: 3 }, position: 'a' }, size: { x: 10, y: 10 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'B',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 10, y: 10 } },
  ]};
  const scene = buildScene(message);
  // outline (countDescendants) and materialize (walk) must both terminate.
  const o = outline(scene, null);
  assert.ok(Array.isArray(o.pages));
  const frameA = findFrame(scene, 'A');
  const ctx = { images: new Map(), parserVersion: 't', compatSchemaVersion: '1', exportedAt: 'x' };
  const { envelope } = materializeFrame(scene, frameA, ctx);
  assert.equal(envelope.root.name, 'A');
});

test('fill alpha composites color.a * paint.opacity', () => {
  const message = { nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'FRAME', name: 'Box', size: { x: 10, y: 10 },
      fillPaints: [{ type: 'SOLID', visible: true, opacity: 0.5, color: { r: 0, g: 0, b: 0, a: 0.5 } }] },
  ]};
  const scene = buildScene(message);
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Box'), {
    images: new Map(), parserVersion: 't', compatSchemaVersion: '1', exportedAt: 'x',
  });
  // 0.5 * 0.5 = 0.25 → alpha channel round(0.25*255)=64=0x40.
  assert.ok(envelope.root.style.background_color.toLowerCase().endsWith('40'),
    `expected 25% alpha suffix, got ${envelope.root.style.background_color}`);
});

test('countFramesByName detects duplicate frame names', () => {
  const message = { nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'P' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Main',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Main',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'b' } },
  ]};
  const scene = buildScene(message);
  assert.equal(countFramesByName(scene, 'Main'), 2);
  assert.equal(countFramesByName(scene, 'Other'), 0);
});

test('generator output is deterministic (committed fixture is fresh)', () => {
  const dir = mkdtempSync(join(tmpdir(), 'fig-gen-'));
  const regen = join(dir, 'synthetic.fig');
  execFileSync('node', [GENERATOR, regen]);
  // Compare DECODED content, not raw file bytes. The inner fig-kiwi chunks are
  // DEFLATE-compressed, and zlib's exact byte output varies by version across
  // hosts (a runner on a newer Node emits a few more/fewer bytes for the same
  // input), so a raw byte-compare tests zlib rather than the generator and goes
  // red on any host whose zlib differs from the one that committed the fixture.
  // The decompressed schema + message + rasters are what the generator actually
  // authors; they are byte-identical across environments, so comparing them
  // keeps the "fixture reflects the current generator" invariant while staying
  // portable across CI hosts.
  const a = unpackFig(readFileSync(regen));
  const b = unpackFig(readFileSync(FIXTURE));
  const stale = 'committed fixture is out of date — re-run make_synthetic_fig.mjs';
  assert.deepEqual(a.schemaBytes, b.schemaBytes, stale);
  assert.deepEqual(a.messageBytes, b.messageBytes, stale);
  assert.deepEqual([...a.images.keys()].sort(), [...b.images.keys()].sort(), stale);
  for (const key of a.images.keys())
    assert.deepEqual(a.images.get(key), b.images.get(key), `${stale} (image ${key})`);
});
