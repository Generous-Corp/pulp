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
import { buildScene, outline, findFrame, materializeFrame, countFramesByName, framesByName, nodesByName, DIAGNOSTIC_SEVERITY, emittedDiagnosticCodes } from './scene.mjs';

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
  // `content`, NOT `text`. This asserted `label.text` and stayed green while the
  // C++ consumer read `content` (design_ir_json.cpp parse_ir_node) and got an
  // empty string — so every string in every .fig import was silently discarded,
  // labels rendered blank, and Yoga measured width=nan. A decoder test that
  // checks the producer's own key against itself proves nothing about the
  // contract; assert the key the consumer actually reads.
  assert.equal(label.content, 'CUTOFF');
  assert.ok(!('text' in label), 'must not emit the unread `text` key');
  assert.equal(label.style.color, '#ffffff');
  assert.ok(!('background_color' in label.style), 'text has no background');

  assert.equal(envelope.asset_manifest.assets.length, 1);
});

test('a rotated box-model layer carries its rotation, not just its origin', () => {
  // A knob's value needle is a thin ROUNDED_RECTANGLE the .fig rotates to the
  // value angle. Dropping the rotation (keeping only m02/m12) rendered it as an
  // axis-aligned stub off-center. The decoder must lower the rotation to
  // `transform: rotate()` and compensate left/top for the renderer's center
  // pivot. A VECTOR (path-baked rotation) must NOT get a second rotation.
  const deg = 45;
  const rad = deg * Math.PI / 180;
  const c = Math.cos(rad), s = Math.sin(rad);
  const rot = (m02, m12) => ({ m00: c, m01: -s, m02, m10: s, m11: c, m12 });
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Knob',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
      size: { x: 40, y: 40 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'ROUNDED_RECTANGLE', name: 'knob indicator',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
      size: { x: 2, y: 10 }, transform: rot(28, 9) },
    { guid: { sessionID: 0, localID: 4 }, type: 'VECTOR', name: 'ring',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'c' },
      size: { x: 20, y: 20 }, transform: rot(4, 4) },
    // A 180deg-rotated solid fill (a slider's progress bar): orthogonal, so it
    // stays axis-aligned and must NOT get a transform — applying one shifted the
    // fill off its track row.
    { guid: { sessionID: 0, localID: 5 }, type: 'ROUNDED_RECTANGLE', name: 'fill',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'd' },
      size: { x: 18, y: 2 }, transform: { m00: -1, m01: 0, m02: 30, m10: 0, m11: -1, m12: 3 } },
  ]});
  const frame = findFrame(scene, 'Knob');
  const { envelope } = materializeFrame(scene, frame, {
    images: new Map(), fileKey: 'K', parserVersion: '0.1.0-test',
    compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z',
  });
  const indicator = envelope.root.children.find((n) => n.name === 'knob indicator');
  assert.equal(indicator.style.transform, 'rotate(45.00deg)',
    'box-model layer lowers its rotation to transform: rotate()');
  // Center-pivot compensation: left/top are shifted off the raw m02/m12 so the
  // renderer's center rotation reproduces Figma's origin rotation.
  assert.notEqual(indicator.style.left, 28, 'left compensated for center pivot');

  const ring = envelope.root.children.find((n) => n.name === 'ring');
  assert.ok(!ring.style.transform,
    'a VECTOR keeps its path-baked rotation and gets no second rotate()');

  // An orthogonal (180deg) box-model rotation stays axis-aligned: plain
  // placement, no transform, so a slider's rotated fill does not float off-row.
  const fill = envelope.root.children.find((n) => n.name === 'fill');
  assert.ok(!fill.style.transform,
    'a 180deg (orthogonal) fill stays axis-aligned and gets no rotate()');
  assert.equal(fill.style.left, 30, 'orthogonal fill keeps plain m02 placement');
  assert.equal(fill.style.top, 3, 'orthogonal fill keeps plain m12 placement');
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

test('absolute children keep their Figma coordinates; auto-layout children do not', () => {
  // The bug this pins: styleFor() read node.size but never node.transform, so
  // every child of a plain (non-auto-layout) frame lost its x/y and collapsed to
  // the parent's content origin. A real 230-node frame rendered as a corner pile
  // while the importer reported "231 elements" and exit 0.
  const plain = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Plain',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    // A footer pinned to the bottom: y=76 in a 100-tall frame.
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Footer',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 100, y: 24 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 76 } },
  ]});
  const pf = materializeFrame(plain, findFrame(plain, 'Plain'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const footer = pf.envelope.root.children[0];
  assert.equal(footer.style.position, 'absolute', 'child of a plain frame is absolute');
  assert.equal(footer.style.left, 0);
  assert.equal(footer.style.top, 76, 'the transform translation IS the y coordinate');

  // Control: inside an auto-layout parent the child FLOWS — its transform is
  // layout output, not input, so emitting it would fight the flex pass.
  const auto = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Auto', stackMode: 'VERTICAL',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Row',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 100, y: 24 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 76 } },
  ]});
  const af = materializeFrame(auto, findFrame(auto, 'Auto'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const row = af.envelope.root.children[0];
  assert.ok(!('position' in row.style), 'auto-layout child stays in flow');
  assert.ok(!('top' in row.style), 'auto-layout child carries no coordinates');
});

test('resize constraints reach the envelope in Figma spelling; flowing auto-layout children drop them', () => {
  // The bug this pins: the decoder read horizontalConstraint/verticalConstraint
  // never, so a footer pinned MAX (bottom) or a panel set SCALE re-anchored to
  // the top-left default on every host resize. The raw kiwi spelling passes
  // through untranslated — design_ir_json.cpp normalizes and codegen lowers to
  // flex, so a translation table here would just be a dialect to keep in sync.
  const plain = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Plain',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Pinned',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 100, y: 24 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 76 },
      horizontalConstraint: 'SCALE', verticalConstraint: 'MAX' },
    // Only one axis set — the other must be absent, not undefined/null.
    { guid: { sessionID: 0, localID: 4 }, type: 'FRAME', name: 'HalfPinned',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 10, y: 10 },
      horizontalConstraint: 'CENTER' },
    { guid: { sessionID: 0, localID: 5 }, type: 'FRAME', name: 'Default',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'c' }, size: { x: 10, y: 10 } },
  ]});
  const pf = materializeFrame(plain, findFrame(plain, 'Plain'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const [pinned, half, dflt] = pf.envelope.root.children;
  assert.deepEqual(pinned.constraints, { horizontal: 'SCALE', vertical: 'MAX' });
  assert.deepEqual(half.constraints, { horizontal: 'CENTER' }, 'single axis stays single');
  assert.ok(!('constraints' in dflt), 'no source field, no key');
  assert.ok(!('constraints' in pf.envelope.root), 'the frame under import anchors the space');

  // A FLOWING auto-layout child is sized by the stack; its (stale) constraints
  // must not ride along to fight the flex pass. A child opted out with
  // stackPositioning ABSOLUTE is back in the parent's coordinate space, so its
  // constraints are meaningful again — same gate as absolute placement.
  const auto = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Auto', stackMode: 'VERTICAL',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Row',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 100, y: 24 },
      horizontalConstraint: 'STRETCH', verticalConstraint: 'MIN' },
    { guid: { sessionID: 0, localID: 4 }, type: 'FRAME', name: 'Badge', stackPositioning: 'ABSOLUTE',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 10, y: 10 },
      transform: { m00: 1, m01: 0, m02: 88, m10: 0, m11: 1, m12: 2 },
      horizontalConstraint: 'MAX', verticalConstraint: 'MIN' },
  ]});
  const af = materializeFrame(auto, findFrame(auto, 'Auto'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const [flowRow, badge] = af.envelope.root.children;
  assert.ok(!('constraints' in flowRow), 'flowing auto-layout child drops constraints');
  assert.deepEqual(badge.constraints, { horizontal: 'MAX', vertical: 'MIN' },
    'ABSOLUTE opt-out keeps constraints');
});

test('a blend mode survives; one CSS lacks is diagnosed, not approximated', () => {
  // A layer that COMPOSITES differently is not cosmetic. This design lays a
  // 912x300 noise texture over its panels at opacity 0.10 blendMode MULTIPLY.
  // The texture's mean luminance is 229/255 — it is LIGHT. Multiplied it darkens
  // the panel (75 -> ~74); composited NORMAL the same light pixels lighten it
  // (75 -> ~90). Reading no blend mode made every panel ~+25/255 too bright,
  // uniformly, and nothing warned — an ignored blend mode still paints.
  const mk = (localID, name, mode, pos) => ({
    guid: { sessionID: 0, localID }, type: 'RECTANGLE', name,
    parentIndex: { guid: { sessionID: 0, localID: 2 }, position: pos },
    size: { x: 10, y: 10 },
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
    blendMode: mode,
    fillPaints: [{ type: 'SOLID', color: { r: 0.3, g: 0.3, b: 0.32, a: 1 } }],
  });
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    mk(3, 'noise', 'MULTIPLY', 'a'),
    mk(4, 'soft', 'SOFT_LIGHT', 'b'),
    mk(5, 'plain', 'NORMAL', 'c'),
    mk(6, 'burn', 'LINEAR_BURN', 'd'),   // real in Figma, absent from CSS
  ]});
  const f = materializeFrame(scene, findFrame(scene, 'Root'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const [noise, soft, plain, burn] = f.envelope.root.children;

  assert.equal(noise.style.mix_blend_mode, 'multiply');
  assert.equal(soft.style.mix_blend_mode, 'soft-light', 'underscores become hyphens');
  assert.ok(!('mix_blend_mode' in plain.style), 'NORMAL is the default — never emitted');
  // CSS has no linear-burn. Approximating it would paint confidently wrong
  // pixels, so it composites normally AND says so.
  assert.ok(!('mix_blend_mode' in burn.style), 'a mode CSS lacks is not approximated');
  const codes = (f.envelope.diagnostics || []).filter((d) => d.code === 'blend-unsupported');
  assert.equal(codes.length, 1, 'and the gap is reported rather than silent');
  assert.match(codes[0].detail, /LINEAR_BURN/);
});

test('drop shadows survive as box_shadow; unsupported effects are skipped', () => {
  // Shadows are what make a control read as an object instead of a decal. The
  // design stacks two drop shadows under every knob; dropping them rendered
  // them flat — a difference no geometry check can catch, because the box is
  // exactly the right size in exactly the right place, it just has no depth.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'RECTANGLE', name: 'knob base',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 28, y: 28 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 0.35, g: 0.36, b: 0.38, a: 1 } }],
      effects: [
        { type: 'DROP_SHADOW', offset: { x: 0, y: 16 }, radius: 6, spread: 0, visible: true,
          color: { r: 0, g: 0, b: 0, a: 0.1026 } },
        { type: 'INNER_SHADOW', offset: { x: 0, y: 1 }, radius: 2, spread: 0, visible: true,
          color: { r: 1, g: 1, b: 1, a: 0.25 } },
        // Hidden: the designer turned it off, so it must not paint.
        { type: 'DROP_SHADOW', offset: { x: 0, y: 99 }, radius: 9, visible: false,
          color: { r: 1, g: 0, b: 0, a: 1 } },
        // No box-shadow equivalent — skipped rather than approximated into
        // something the design never asked for.
        { type: 'LAYER_BLUR', radius: 4, visible: true },
      ] },
  ]});
  const f = materializeFrame(scene, findFrame(scene, 'Root'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const bs = f.envelope.root.children[0].style.box_shadow;
  assert.ok(bs, 'the knob carries a shadow');
  const layers = bs.split(', ');
  assert.equal(layers.length, 2, 'two visible shadows; the hidden and the blur are not layers');
  assert.equal(layers[0], '0px 16px 6px 0px #0000001a');
  assert.ok(layers[1].startsWith('inset '), 'an inner shadow is inset');
  assert.ok(!bs.includes('99px'), 'a hidden effect never paints');
});

test('icon-font text lowers to a glyph outline; real text stays a live label', () => {
  // An icon font's characters are LIGATURE NAMES — "lock" means a padlock — so
  // emitting them as text is always wrong and no font we can ship fixes it.
  // Figma bakes every glyph outline into the file, so the icon renders with no
  // font at all. Real text must NOT be swept up in that: a knob caption has to
  // stay editable and themeable, so it stays a label under the default mode.
  // A glyph outline in EM units, encoded the way Figma does: [u8 tag][f32 args].
  const encode = (...cmds) => Buffer.concat(cmds.map(([tag, ...args]) => {
    const b = Buffer.alloc(1 + args.length * 4);
    b.writeUInt8(tag, 0);
    args.forEach((v, i) => b.writeFloatLE(v, 1 + i * 4));
    return b;
  }));
  const em = encode([1, 0, 0], [2, 1, 0], [2, 1, 1], [0]);  // MOVE LINE LINE CLOSE
  const mk = (name, family, localID, pos) => ({
    guid: { sessionID: 0, localID }, type: 'TEXT', name,
    parentIndex: { guid: { sessionID: 0, localID: 2 }, position: pos },
    size: { x: 12, y: 12 },
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
    fontName: { family },
    textData: { characters: 'lock' },
    fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 } }],
    derivedTextData: { characters: 'lock', glyphs: [{ commandsBlob: 0, position: { x: 0, y: 12 }, fontSize: 12 }] },
  });
  const nodeChanges = [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 40 } },
    mk('icon', 'Font Awesome 6 Pro', 3, 'a'),
    mk('caption', 'Roboto', 4, 'b'),
  ];
  const scene = buildScene({ nodeChanges, blobs: [{ bytes: em }] });
  // Font availability is a property of the MACHINE, so inject it: a test that
  // asks the real font book passes or fails depending on who runs it. Here
  // Roboto is present, which is the case where live text is the right answer.
  const ctx = { images: new Map(), fileKey: 'K', parserVersion: 't', compatSchemaVersion: '1',
    exportedAt: '1970-01-01T00:00:00Z', isFontAvailable: () => true };
  const f = materializeFrame(scene, findFrame(scene, 'Root'), ctx);
  const [icon, caption] = f.envelope.root.children;

  assert.equal(icon.type, 'vector', 'an icon font lowers to its outline');
  assert.ok(icon.path_data, 'and carries path data');
  assert.ok(!('content' in icon), 'the ligature name is not content');
  assert.equal(icon.fill, '#ffffff', 'the glyph paints in the text color');

  assert.equal(caption.type, 'text', 'real text stays live');
  assert.equal(caption.content, 'lock');

  // `always` trades every label for pixel-faithful text — opt-in only.
  const all = materializeFrame(scene, findFrame(scene, 'Root'),
    { ...ctx, textAsOutlines: 'always' });
  assert.equal(all.envelope.root.children[1].type, 'vector', "`always` outlines real text too");

  // The rule that fixes the logo: a font this machine does NOT have cannot lay
  // out live text correctly, because the substitute face's advance widths are
  // not the ones the design was measured with. The logo's text is "TRI  Z" and
  // those two spaces are the gap its A-mark occupies — under a fallback the gap
  // collapses and the Z renders on top of the mark. Outlines are exact.
  const missing = materializeFrame(scene, findFrame(scene, 'Root'),
    { ...ctx, isFontAvailable: (fam) => fam !== 'Roboto' });
  assert.equal(missing.envelope.root.children[1].type, 'vector',
    'text whose font is missing is outlined rather than mis-measured');
});

test('a style-referenced fill resolves to the style, not the master default', () => {
  // Shared styles are the design's tokens, and Figma caches the resolved color
  // on the referencing node only sometimes. In one real design only the FOLEY
  // tab carried a literal fill — and FOLEY was the only tab that imported in its
  // true color. kick/SNARE/TOM/CRASH/RIDE carried a style ref alone, so they
  // fell back to their master's fuchsia and a row of red/yellow/green tabs
  // arrived as a wall of pink. The style ref must be honoured on its own.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 40 } },
    // The style definition. Its `key` is what a referencing node points at.
    { guid: { sessionID: 0, localID: 9 }, type: 'FRAME', name: 'instrument/04 Yellow 85%',
      styleType: 'FILL', key: 'STYLEKEY_YELLOW',
      fillPaints: [{ type: 'SOLID', color: { r: 0.961, g: 0.757, b: 0.318, a: 1 } }] },
    // A tab whose color lives ONLY in the style — no literal to fall back on.
    // Without resolution this node has no paint at all.
    { guid: { sessionID: 0, localID: 3 }, type: 'RECTANGLE', name: 'SNARE tab',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 40, y: 20 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      styleIdForFill: { assetRef: { key: 'STYLEKEY_YELLOW', version: '1:1' } } },
    // An unresolvable ref (style lives in a library the .fig omits): the stale
    // literal is better than nothing, so it must survive.
    { guid: { sessionID: 0, localID: 4 }, type: 'RECTANGLE', name: 'Orphan tab',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 40, y: 20 },
      transform: { m00: 1, m01: 0, m02: 50, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 1, a: 1 } }],
      styleIdForFill: { assetRef: { key: 'MISSING_FROM_FILE', version: '1:1' } } },
  ]});
  const f = materializeFrame(scene, findFrame(scene, 'Root'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const [snare, orphan] = f.envelope.root.children;
  assert.equal(snare.style.background_color, '#f5c151', 'a ref with no literal resolves to the style');
  assert.equal(orphan.style.background_color, '#0000ff', 'an unresolvable ref keeps its literal');
});

test('an instance override does not get repainted by the master style ref', () => {
  // The counterpart bound, and a regression actually shipped: making the style
  // win unconditionally repainted EVERY instrument tab with the master's
  // fuchsia — including FOLEY, the one tab that had been correct.
  //
  // This test used to model FOLEY as a BARE literal over the master's still-
  // attached ref, and assert the literal won. That fixture did not match the
  // file: it was written in the same commit that took styleIdForFill OUT of
  // OVERRIDE_SKIP_KEYS, and the fuchsia regression it recalls was observed
  // WHILE that skip was still in force — i.e. FOLEY's own token ref was being
  // dropped, leaving only the master's default for a blanket rule to paint
  // with. Once an instance's ref survives, the real FOLEY re-points at its own
  // token; a bare literal over an attached ref is Figma's CACHE of that ref,
  // not an instance recolour, and treating it as one painted the switch dot
  // white over the design's #333537 (verified against the file's thumbnail).
  //
  // So both real shapes are modeled here, and the guard is unchanged: the
  // instance's own color must survive, and must never become the master's
  // fuchsia.
  const master = { sessionID: 0, localID: 30 };
  const child  = { sessionID: 0, localID: 31 };
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 40 } },
    { guid: { sessionID: 0, localID: 9 }, type: 'FRAME', name: 'instrument/02 Fuchsia 85%',
      styleType: 'FILL', key: 'STYLE_FUCHSIA',
      fillPaints: [{ type: 'SOLID', color: { r: 0.965, g: 0.443, b: 0.557, a: 1 } }] },
    // Master: swatch points at the fuchsia style by default.
    { guid: master, type: 'SYMBOL', name: 'tab' },
    { guid: child, type: 'RECTANGLE', name: 'swatch',
      parentIndex: { guid: master, position: 'a' }, size: { x: 40, y: 20 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 0.965, g: 0.443, b: 0.557, a: 1 } }],
      styleIdForFill: { assetRef: { key: 'STYLE_FUCHSIA', version: '1:1' } } },
    { guid: { sessionID: 0, localID: 10 }, type: 'FRAME', name: 'instrument/08 Blue 85%',
      styleType: 'FILL', key: 'STYLE_BLUE',
      fillPaints: [{ type: 'SOLID', color: { r: 0.455, g: 0.604, b: 0.984, a: 1 } }] },
    // Shape 1 — the real FOLEY: the instance re-points at its OWN token. The
    // cached literal riding along is Figma's, and is deliberately STALE here
    // (fuchsia) to prove the ref, not the cache, is what renders.
    { guid: { sessionID: 0, localID: 40 }, type: 'INSTANCE', name: 'FOLEY tab',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 40, y: 20 },
      symbolData: { symbolID: master, symbolOverrides: [
        { guidPath: { guids: [child] },
          styleIdForFill: { assetRef: { key: 'STYLE_BLUE', version: '1:1' } },
          fillPaints: [{ type: 'SOLID', color: { r: 0.965, g: 0.443, b: 0.557, a: 1 } }] },
      ] } },
    // Shape 2 — a designer recolouring the instance outright: Figma DETACHES
    // the style (null-guid sentinel) and the literal becomes authoritative.
    { guid: { sessionID: 0, localID: 41 }, type: 'INSTANCE', name: 'CUSTOM tab',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 40, y: 20 },
      symbolData: { symbolID: master, symbolOverrides: [
        { guidPath: { guids: [child] },
          styleIdForFill: { guid: { sessionID: 0xFFFFFFFF, localID: 0xFFFFFFFF } },
          fillPaints: [{ type: 'SOLID', color: { r: 0.455, g: 0.604, b: 0.984, a: 1 } }] },
      ] } },
  ]});
  const f = materializeFrame(scene, findFrame(scene, 'Root'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const [foley, custom] = f.envelope.root.children;
  assert.equal(foley.children[0].name, 'swatch');
  assert.equal(foley.children[0].style.background_color, '#749afb',
    "an instance's own token must not be repainted by the master's style");
  assert.equal(custom.children[0].style.background_color, '#749afb',
    "a detached style leaves the instance's literal authoritative");
});

test('auto-layout lands on the sibling `layout` object the IR actually reads', () => {
  // The counterpart to the test above, and the reason it was not enough: taking
  // a child's coordinates away is only safe if the parent's flex SURVIVES. It
  // did not. parse_ir_layout reads node["layout"] (design_ir_json.cpp:1042) and
  // parse_ir_style has no flex fields, so flex written into `style` matched
  // nothing and every auto-layout frame in every .fig lowered to a bare box —
  // children position-less AND unflowed, i.e. piled on the parent's origin.
  // Asserting on `layout` rather than `style` is the whole point: a test that
  // only checks "we emitted flex somewhere" stays green through this bug.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Row',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 300, y: 80 },
      stackMode: 'HORIZONTAL', stackSpacing: 12,
      stackPrimaryAlignItems: 'SPACE_BETWEEN', stackCounterAlignItems: 'CENTER',
      stackVerticalPadding: 4, stackHorizontalPadding: 8, stackPaddingBottom: 20, stackPaddingRight: 30 },
  ]});
  const f = materializeFrame(scene, findFrame(scene, 'Row'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const l = f.envelope.root.layout;
  assert.ok(l, 'auto-layout frame carries a `layout` object');
  assert.equal(l.direction, 'row');
  assert.equal(l.gap, 12);
  assert.equal(l.justify, 'space-between', 'primary align is justify, not dropped');
  assert.equal(l.align, 'center', 'counter align is align, not dropped');
  // Figma's *Vertical*/*Horizontal* padding fields are top and left; bottom and
  // right ride separately. Mirroring the first pair renders uneven insets wrong.
  assert.deepEqual(l.padding, { top: 4, right: 30, bottom: 20, left: 8 });
  assert.ok(!('flex_direction' in f.envelope.root.style),
    'flex must not be left in `style`, where nothing reads it');
});

test('an auto-layout child never shrinks below the size Figma solved for it', () => {
  // Figma has no flex-shrink: a child is FIXED, HUG, or FILL, and it overflows
  // its parent rather than rendering narrower than its solved size. Yoga
  // defaults flex-shrink to 1, so a parent whose padding exceeds its own width
  // squeezed the child to nothing — and because the glyph inside was absolutely
  // placed, it went on painting from the collapsed box's origin. The icon looked
  // SHOVED RIGHT, so the bug read as positioning and the sizing went unexamined.
  //
  // The numbers are SmallTriaz2's real toolbar button: a 20-wide frame with
  // 11 + 10 padding around a 12-wide icon. Figma solves that negative content
  // box by centring on it (x = 4.5), which is the tell that it never shrinks.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page 1' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Button',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
      size: { x: 20, y: 20 }, stackMode: 'HORIZONTAL',
      stackHorizontalPadding: 11, stackPaddingRight: 10,
      stackPrimaryAlignItems: 'CENTER', stackCounterAlignItems: 'CENTER' },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'fg-icon',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' },
      size: { x: 12, y: 12 },
      transform: { m00: 1, m01: 0, m02: 4.5, m10: 0, m11: 1, m12: 3.5 } },
    // Control: an ABSOLUTE-positioned child has opted out of auto-layout, so it
    // is not flowed and shrink does not apply to it.
    { guid: { sessionID: 0, localID: 4 }, type: 'FRAME', name: 'badge',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
      size: { x: 6, y: 6 }, stackPositioning: 'ABSOLUTE',
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 } },
  ]});
  const f = materializeFrame(scene, findFrame(scene, 'Button'), { images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z' });
  const byName = Object.fromEntries(f.envelope.root.children.map((c) => [c.name, c]));

  // `flexShrink` is the key parse_ir_layout reads (design_ir_json.cpp:426) and
  // it sits on `layout`, not `style` — asserting anywhere else stays green
  // through the bug, exactly as the flex-in-`style` drop above did.
  assert.equal(byName['fg-icon'].layout.flexShrink, 0,
    'a flowed child is pinned against Yoga shrinking it');
  assert.equal(byName['fg-icon'].style.width, 12, 'and keeps the width Figma solved');
  assert.ok(!(byName['badge'].layout && 'flexShrink' in byName['badge'].layout),
    'an ABSOLUTE child is out of the flow entirely — shrink never applied to it');
});

// ── component-instance expansion ─────────────────────────────────────────────
//
// An INSTANCE has no children in the scene graph; its content is its master
// SYMBOL's subtree with per-instance overrides applied. Override guidPaths do
// NOT name subtree guids directly — they resolve through each node's
// `overrideKey` (the guid the overrides were written against before the symbol
// was duplicated/synced). These tests assert the CONSUMER's contract on the
// envelope: expanded `children`, text under `content`, the `audio_widget:
// "none"` opt-out the C++ name-heuristic gate reads, and the `figma`
// component identity the recognition resolver reads.

function instanceScene() {
  const g = (l, s = 0) => ({ sessionID: s, localID: l });
  return buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    // Master A: a label component whose TEXT child carries an overrideKey.
    { guid: g(10), type: 'SYMBOL', name: 'knob label', size: { x: 40, y: 12 } },
    { guid: g(11), type: 'TEXT', name: 'caption', overrideKey: g(109, 241),
      parentIndex: { guid: g(10), position: 'a' }, size: { x: 40, y: 12 },
      textData: { characters: 'Volume' } },
    // Master B: the knob art — a label-slot INSTANCE of A, a body, and a
    // variant part that is hidden in the master.
    { guid: g(14), type: 'SYMBOL', name: 'knob master', componentKey: 'cafef00d',
      size: { x: 40, y: 52 } },
    { guid: g(15), type: 'INSTANCE', name: 'label slot', overrideKey: g(1, 90),
      parentIndex: { guid: g(14), position: 'a' }, size: { x: 40, y: 12 },
      symbolData: { symbolID: g(10) } },
    { guid: g(16), type: 'ROUNDED_RECTANGLE', name: 'knob base',
      parentIndex: { guid: g(14), position: 'b' }, size: { x: 28, y: 28 },
      fillPaints: [{ type: 'SOLID', visible: true, color: { r: 1, g: 0, b: 0, a: 1 } }] },
    { guid: g(17), type: 'ROUNDED_RECTANGLE', name: 'bipolar ring', visible: false,
      parentIndex: { guid: g(14), position: 'c' }, size: { x: 2, y: 2 } },
    // The frame under import, holding two instances of B and one external.
    { guid: g(2), type: 'FRAME', name: 'Root',
      parentIndex: { guid: g(1), position: 'a' }, size: { x: 200, y: 100 } },
    { guid: g(20), type: 'INSTANCE', name: 'sound / knob / big',
      parentIndex: { guid: g(2), position: 'a' }, size: { x: 40, y: 52 },
      transform: { m00: 1, m01: 0, m02: 10, m10: 0, m11: 1, m12: 20 },
      symbolData: {
        symbolID: g(14),
        symbolOverrides: [
          // Two-level path: through the label slot (overrideKey 90:1) into the
          // TEXT (overrideKey 241:109) — the caption THIS instance renders.
          { guidPath: { guids: [g(1, 90), g(109, 241)] },
            textData: { characters: 'Attack' } },
          // Direct-guid paths: recolor the body, unhide the variant part.
          { guidPath: { guids: [g(16)] },
            fillPaints: [{ type: 'SOLID', visible: true, color: { r: 0, g: 0, b: 1, a: 1 } }] },
          { guidPath: { guids: [g(17)] }, visible: true },
        ],
      },
      derivedSymbolData: [
        // Figma-computed layout for the body in THIS instance.
        { guidPath: { guids: [g(16)] }, size: { x: 50, y: 50 },
          transform: { m00: 1, m01: 0, m02: 5, m10: 0, m11: 1, m12: 7 } },
      ] },
    { guid: g(21), type: 'INSTANCE', name: 'sound / knob / big',
      parentIndex: { guid: g(2), position: 'b' }, size: { x: 40, y: 52 },
      symbolData: {
        symbolID: g(14),
        symbolOverrides: [
          { guidPath: { guids: [g(1, 90), g(109, 241)] },
            textData: { characters: 'Release' } },
        ],
      } },
    { guid: g(22), type: 'INSTANCE', name: 'missing / component',
      parentIndex: { guid: g(2), position: 'c' }, size: { x: 10, y: 10 },
      symbolData: { symbolID: g(99, 9) } },
  ]});
}

function materializeRoot(scene) {
  return materializeFrame(scene, findFrame(scene, 'Root'), {
    images: new Map(), fileKey: 'K',
    parserVersion: 't', compatSchemaVersion: '1', exportedAt: '1970-01-01T00:00:00Z',
  });
}

test('an INSTANCE expands to its master SYMBOL subtree with overrides applied', () => {
  const { envelope } = materializeRoot(instanceScene());
  const [inst] = envelope.root.children;
  assert.equal(inst.name, 'sound / knob / big');
  assert.ok(Array.isArray(inst.children) && inst.children.length === 3,
    `instance must materialize master children, got ${(inst.children || []).length}`);

  const [slot, base, ring] = inst.children;

  // Nested instance expanded too, with the outer instance's text override
  // resolved through BOTH overrideKey hops. `content` is the key the C++
  // consumer reads (design_ir_json.cpp parse_ir_node).
  assert.equal(slot.name, 'label slot');
  assert.equal(slot.children.length, 1);
  assert.equal(slot.children[0].type, 'text');
  assert.equal(slot.children[0].content, 'Attack');

  // Direct-guid fill override replaced the master's red with blue.
  assert.equal(base.name, 'knob base');
  assert.equal(base.style.background_color, '#0000ff');
  // Derived per-instance layout: size and transform-translation applied.
  assert.equal(base.style.width, 50);
  assert.equal(base.style.left, 5);
  assert.equal(base.style.top, 7);

  // Master-hidden variant part turned on by the instance override.
  assert.equal(ring.name, 'bipolar ring');
});

test('two instances of one master expand independently', () => {
  const { envelope } = materializeRoot(instanceScene());
  const texts = envelope.root.children
    .filter((c) => c.name === 'sound / knob / big')
    .map((c) => c.children[0].children[0].content);
  assert.deepEqual(texts, ['Attack', 'Release'],
    'each instance renders its own caption, not a shared clone');
});

test('expanded component content is opted out of name-based widget promotion', () => {
  const { envelope } = materializeRoot(instanceScene());
  const [inst] = envelope.root.children;
  // The exact contract the C++ gate reads: audio_widget === "none" suppresses
  // detect_audio_widget, so "knob base" keeps the designer's art instead of
  // being painted over by the built-in silver knob.
  assert.equal(inst.audio_widget, 'none');
  for (const child of inst.children) assert.equal(child.audio_widget, 'none');
  // Component identity for the recognition resolver (never-silent-knob):
  // a matched library component still becomes a real widget through keys.
  assert.equal(inst.figma.component_key, 'cafef00d');
  assert.equal(inst.figma.main_component_name, 'knob master');
});

test('hidden nodes do not render; instance-hidden content is dropped', () => {
  const scene = instanceScene();
  const { envelope } = materializeRoot(scene);
  const [first, second] = envelope.root.children;
  // Master hides "bipolar ring"; only the first instance unhides it.
  assert.ok(first.children.some((c) => c.name === 'bipolar ring'));
  assert.ok(!second.children.some((c) => c.name === 'bipolar ring'),
    'master-hidden node stays hidden without an override');
});

test('an instance whose master is not in the file stays a plain box and is surfaced', () => {
  const { envelope, diagnostics } = materializeRoot(instanceScene());
  const missing = envelope.root.children.find((c) => c.name === 'missing / component');
  assert.ok(missing, 'unexpandable instance still materializes');
  assert.ok(!('children' in missing), 'no fabricated content');
  assert.ok(!('audio_widget' in missing),
    'no opt-out stamp — existing recognition behavior is preserved');
  assert.ok(diagnostics.some((d) => d.code === 'external-component' && d.node_id === '0:22'),
    'missing master is surfaced as a diagnostic');
});

test('a self-recursive symbol terminates instead of expanding forever', () => {
  const g = (l) => ({ sessionID: 0, localID: l });
  const scene = buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    { guid: g(30), type: 'SYMBOL', name: 'ouroboros', size: { x: 10, y: 10 } },
    { guid: g(31), type: 'INSTANCE', name: 'self',
      parentIndex: { guid: g(30), position: 'a' }, size: { x: 10, y: 10 },
      symbolData: { symbolID: g(30) } },
    { guid: g(2), type: 'FRAME', name: 'Root',
      parentIndex: { guid: g(1), position: 'a' }, size: { x: 100, y: 100 } },
    { guid: g(32), type: 'INSTANCE', name: 'top',
      parentIndex: { guid: g(2), position: 'a' }, size: { x: 10, y: 10 },
      symbolData: { symbolID: g(30) } },
  ]});
  const { envelope } = materializeRoot(scene);
  const top = envelope.root.children[0];
  assert.equal(top.name, 'top');
  assert.equal(top.children.length, 1, 'first level expands');
  assert.ok(!('children' in top.children[0]), 'the cycle is cut, not recursed');
});

// ── instance swap (`overriddenSymbolID`) ─────────────────────────────────────
//
// Files that predate component properties swap a nested instance's component
// with an override entry carrying `overriddenSymbolID` — there are no
// componentPropAssignments anywhere. The entry re-points the WHOLE expansion,
// so reading only the authored symbolID expands every sibling from one shared
// master: that is how sixteen mixer channels rendered the same instrument icon
// under sixteen correct labels. Deeper override paths keep resolving after the
// swap because the swap target's children carry the matching overrideKeys.

function swapScene(swapTarget) {
  const g = (l, s = 0) => ({ sessionID: s, localID: l });
  const enc = (...cmds) => Buffer.concat(cmds.map(([tag, ...args]) => {
    const b = Buffer.alloc(1 + args.length * 4);
    b.writeUInt8(tag, 0);
    args.forEach((v, i) => b.writeFloatLE(v, 1 + i * 4));
    return b;
  }));
  const I = { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 };
  const white = [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }];
  return buildScene({
    nodeChanges: [
      { guid: g(1), type: 'CANVAS', name: 'Page' },
      // Authored icon master: one vector (the kick glyph, blob 0).
      { guid: g(10), type: 'SYMBOL', name: 'Kick', size: { x: 8, y: 8 } },
      { guid: g(11), type: 'VECTOR', name: 'Vector', overrideKey: g(700),
        parentIndex: { guid: g(10), position: 'a' }, size: { x: 8, y: 8 },
        transform: I, fillGeometry: [{ commandsBlob: 0 }], fillPaints: white },
      // Swap target: its own vector with its own authored geometry (blob 1).
      { guid: g(12), type: 'SYMBOL', name: 'Layer', size: { x: 8, y: 8 } },
      { guid: g(13), type: 'VECTOR', name: 'Layer_Vector', overrideKey: g(701),
        parentIndex: { guid: g(12), position: 'a' }, size: { x: 8, y: 8 },
        transform: I, fillGeometry: [{ commandsBlob: 1 }], fillPaints: white },
      // Channel master: an icon instance authored to point at Kick.
      { guid: g(20), type: 'SYMBOL', name: 'Channel', size: { x: 60, y: 100 } },
      { guid: g(21), type: 'INSTANCE', name: 'Kick', overrideKey: g(710),
        parentIndex: { guid: g(20), position: 'a' }, size: { x: 8, y: 8 },
        transform: I, symbolData: { symbolID: g(10) } },
      // The frame under import: channel 1 keeps the authored icon, channel 2
      // swaps it. The derived entry resolves through the swap target's child
      // overrideKey — it can only land if the expansion actually swapped.
      { guid: g(2), type: 'FRAME', name: 'Root',
        parentIndex: { guid: g(1), position: 'a' }, size: { x: 200, y: 100 } },
      { guid: g(30), type: 'INSTANCE', name: 'channel 1',
        parentIndex: { guid: g(2), position: 'a' }, size: { x: 60, y: 100 },
        transform: I, symbolData: { symbolID: g(20) } },
      { guid: g(31), type: 'INSTANCE', name: 'channel 2',
        parentIndex: { guid: g(2), position: 'b' }, size: { x: 60, y: 100 },
        transform: { ...I, m02: 70 },
        symbolData: { symbolID: g(20), symbolOverrides: [
          { guidPath: { guids: [g(710)] }, overriddenSymbolID: swapTarget },
        ] },
        derivedSymbolData: [
          { guidPath: { guids: [g(710), g(701)] },
            fillGeometry: [{ commandsBlob: 2 }] },
        ] },
    ],
    blobs: [
      { bytes: enc([1, 0, 0], [2, 8, 0], [2, 8, 8], [0]) },       // 0: kick
      { bytes: enc([1, 0, 0], [2, 6, 0], [2, 6, 6], [2, 0, 6], [0]) }, // 1: layer, authored
      { bytes: enc([1, 0, 0], [2, 5, 0], [2, 5, 5], [0]) },       // 2: layer, instance-solved
    ],
  });
}

test('an override carrying overriddenSymbolID swaps the master an instance expands', () => {
  const scene = swapScene({ sessionID: 0, localID: 12 });
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'),
    { images: new Map(), fileKey: 'K', parserVersion: 't', compatSchemaVersion: '1',
      exportedAt: '1970-01-01T00:00:00Z' });
  const [ch1, ch2] = envelope.root.children;
  const icon1 = ch1.children[0];
  const icon2 = ch2.children[0];

  // The unswapped channel renders the authored master's subtree…
  assert.equal(icon1.children[0].name, 'Vector');
  assert.equal(icon1.children[0].path_data, 'M0 0 L8 0 L8 8 Z');
  // …and the swapped channel renders the swap TARGET's subtree, with the
  // deeper derived-geometry entry resolved through the target's overrideKey.
  assert.equal(icon2.children[0].name, 'Layer_Vector');
  assert.equal(icon2.children[0].path_data, 'M0 0 L5 0 L5 5 Z');
  // Component identity follows the swap — that is what Figma renders, and it
  // is what the recognition resolver must key on.
  assert.equal(icon2.figma.main_component_name, 'Layer');
});

test('a swapped master missing from the file falls back to the authored one', () => {
  const scene = swapScene({ sessionID: 9, localID: 999 });
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Root'),
    { images: new Map(), fileKey: 'K', parserVersion: 't', compatSchemaVersion: '1',
      exportedAt: '1970-01-01T00:00:00Z' });
  const [, ch2] = envelope.root.children;
  const icon2 = ch2.children[0];
  assert.equal(icon2.children[0].name, 'Vector',
    'the authored master still expands rather than dropping the instance');
  assert.ok(diagnostics.some((d) => d.code === 'external-component'),
    'the unreachable swap target is surfaced, not silently ignored');
});

// ── clip content → overflow ──────────────────────────────────────────────────
//
// Figma clips a container's content to its bounds unless "Clip content" is
// unchecked (`frameMaskDisabled: true`); a GROUP is a frame with `resizeToFit`
// and never clips regardless of the flag. The envelope key is `style.overflow`
// — the same key parse_ir_style reads and figma_rest_export.py emits — so an
// expanded master whose decoration overhangs its symbol bounds is clipped the
// way Figma clips it instead of painting over whatever sits below.

test('a clipping container emits overflow:clip; a group or opted-out frame does not', () => {
  const g = (l) => ({ sessionID: 0, localID: l });
  const scene = buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    { guid: g(2), type: 'FRAME', name: 'Root', frameMaskDisabled: false,
      parentIndex: { guid: g(1), position: 'a' }, size: { x: 100, y: 100 } },
    { guid: g(3), type: 'FRAME', name: 'clips', frameMaskDisabled: false,
      parentIndex: { guid: g(2), position: 'a' }, size: { x: 40, y: 40 } },
    { guid: g(4), type: 'FRAME', name: 'group', frameMaskDisabled: false, resizeToFit: true,
      parentIndex: { guid: g(2), position: 'b' }, size: { x: 40, y: 40 } },
    { guid: g(5), type: 'FRAME', name: 'open', frameMaskDisabled: true,
      parentIndex: { guid: g(2), position: 'c' }, size: { x: 40, y: 40 } },
  ]});
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'),
    { images: new Map(), fileKey: 'K', parserVersion: 't', compatSchemaVersion: '1',
      exportedAt: '1970-01-01T00:00:00Z' });
  const byName = Object.fromEntries(envelope.root.children.map((c) => [c.name, c]));
  assert.equal(envelope.root.style.overflow, 'clip');
  assert.equal(byName['clips'].style.overflow, 'clip');
  assert.ok(!('overflow' in byName['group'].style),
    'a group never clips, whatever its mask flag says');
  assert.ok(!('overflow' in byName['open'].style),
    'an unchecked Clip content stays unclipped — Figma draws that overflow');
});

// ── geometry sidecar (layout-parity ground truth) ────────────────────────────
//
// The envelope deliberately DROPS an auto-layout child's coordinates — they
// flow, and emitting them would fight the flex pass. That is right for the
// importer and useless for validation, which is exactly how a dropped flex hid:
// the one place that knew where the children belonged had thrown it away. The
// `.fig` still carries Figma's solved rect for those children, so the sidecar
// recovers it INDEPENDENTLY of `style` — that independence is the whole point,
// and a test that read the answer back out of `style` would prove nothing.

test('the geometry sidecar recovers solved rects for auto-layout children', () => {
  const g = (l) => ({ sessionID: 0, localID: l });
  const T = (x, y) => ({ m00: 1, m01: 0, m02: x, m10: 0, m11: 1, m12: y });
  const scene = buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    // The frame under import sits somewhere on the Figma canvas; that offset is
    // not part of the UI, so the sidecar must anchor the frame at the origin —
    // the same space `style.left`/`top` are expressed in.
    { guid: g(2), type: 'FRAME', name: 'Root', parentIndex: { guid: g(1), position: 'a' },
      size: { x: 400, y: 200 }, transform: T(1234, 5678) },
    { guid: g(3), type: 'FRAME', name: 'Transport', parentIndex: { guid: g(2), position: 'a' },
      size: { x: 300, y: 40 }, transform: T(50, 100), stackMode: 'HORIZONTAL' },
    // Figma's post-layout transforms for the flowed children (m02 = 0, 38, 76).
    { guid: g(4), type: 'RECTANGLE', name: 'Play', parentIndex: { guid: g(3), position: 'a' },
      size: { x: 24, y: 24 }, transform: T(0, 8) },
    { guid: g(5), type: 'RECTANGLE', name: 'Stop', parentIndex: { guid: g(3), position: 'b' },
      size: { x: 24, y: 24 }, transform: T(38, 8) },
    { guid: g(6), type: 'RECTANGLE', name: 'Rec', parentIndex: { guid: g(3), position: 'c' },
      size: { x: 24, y: 24 }, transform: T(76, 8) },
  ]});
  const { envelope, geometry } = materializeFrame(scene, findFrame(scene, 'Root'), {
    images: new Map(), fileKey: 'K', parserVersion: 't', compatSchemaVersion: '1',
    exportedAt: '1970-01-01T00:00:00Z' });

  const by = Object.fromEntries(geometry.nodes.map((n) => [n.node_id, n]));
  assert.equal(geometry.nodes.length, 5, 'every emitted node carries a rect');
  assert.deepEqual([by['0:2'].x, by['0:2'].y], [0, 0],
    "the imported frame anchors the space; its canvas position is not the UI's");
  assert.deepEqual([by['0:3'].x, by['0:3'].y], [50, 100]);
  // Absolute = the child's solved transform composed with its ancestors'.
  assert.deepEqual([by['0:4'].x, by['0:4'].y], [50, 108]);
  assert.deepEqual([by['0:5'].x, by['0:5'].y], [88, 108]);
  assert.deepEqual([by['0:6'].x, by['0:6'].y], [126, 108]);
  assert.deepEqual([by['0:6'].width, by['0:6'].height], [24, 24]);
  assert.equal(by['0:4'].parent_id, '0:3', 'parentage is what lets findings cluster');
  assert.equal(by['0:3'].parent_id, '0:2');
  assert.equal(by['0:2'].parent_id, null);

  // The control that makes the above meaningful: `style` genuinely has no
  // coordinates for these children, so the sidecar is not echoing it back.
  const transport = envelope.root.children[0];
  for (const child of transport.children)
    assert.ok(!('left' in child.style), 'auto-layout children stay position-less');
});

test('every emitted node carries a node_id, unique across instances of one master', () => {
  // Without an id there is nothing to join on and every node-keyed check —
  // fidelity_diff, layout parity — silently skips, reporting success by finding
  // nothing. The raw guid is not enough: an expanded instance's children reuse
  // their MASTER's guids, so two instances of one component would collide and a
  // parity run would compare one instance's rect against the other's.
  const { envelope, geometry } = materializeRoot(instanceScene());
  const ids = [];
  (function collect(node) {
    ids.push(node.node_id);
    for (const c of node.children || []) collect(c);
  })(envelope.root);

  assert.ok(ids.every((id) => typeof id === 'string' && id.length),
    'every node in the envelope has a node_id');
  assert.equal(new Set(ids).size, ids.length, 'node_ids are unique');
  assert.deepEqual(ids.slice().sort(), geometry.nodes.map((n) => n.node_id).sort(),
    'the sidecar covers exactly the nodes the envelope emits — no id joins to nothing');

  // The collision this guards: both instances expand the same master child.
  const captions = ids.filter((id) => id.endsWith('/0:11'));
  assert.equal(captions.length, 2, 'both instances expand the master caption');
  assert.notEqual(captions[0], captions[1],
    "two instances of one master must not share their children's ids");
});

test('a nested rotated transform composes rather than accumulating translations', () => {
  // m02/m12 are only the translation COLUMN. Adding them down the tree is right
  // for the common unrotated case and wrong the moment an ancestor rotates or
  // scales, because a child's offset is expressed in the PARENT's rotated basis.
  const g = (l) => ({ sessionID: 0, localID: l });
  const scene = buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    { guid: g(2), type: 'FRAME', name: 'Root', parentIndex: { guid: g(1), position: 'a' },
      size: { x: 200, y: 200 } },
    // Rotated 90° CCW about the frame origin, then moved to (100, 20).
    { guid: g(3), type: 'FRAME', name: 'Rotated', parentIndex: { guid: g(2), position: 'a' },
      size: { x: 80, y: 40 },
      transform: { m00: 0, m01: -1, m02: 100, m10: 1, m11: 0, m12: 20 } },
    { guid: g(4), type: 'RECTANGLE', name: 'Pip', parentIndex: { guid: g(3), position: 'a' },
      size: { x: 10, y: 10 },
      transform: { m00: 1, m01: 0, m02: 30, m10: 0, m11: 1, m12: 0 } },
  ]});
  const { geometry } = materializeFrame(scene, findFrame(scene, 'Root'), {
    images: new Map(), fileKey: 'K', parserVersion: 't', compatSchemaVersion: '1',
    exportedAt: '1970-01-01T00:00:00Z' });
  const pip = geometry.nodes.find((n) => n.node_id === '0:4');
  // Naive addition would say (130, 20). The parent's basis turns the child's
  // +30 on ITS x into +30 on the frame's y.
  assert.deepEqual([pip.x, pip.y], [100, 50]);
});

// ---------------------------------------------------------------------------
// Gradient paints → CSS. Asserted through materializeFrame, so what is checked
// is what design_ir_json actually reads off the envelope (`backgroundGradient`
// on the style, `fillGradient` on a vector), not a producer-side convenience.
// ---------------------------------------------------------------------------

// Figma's paint transform for a plain top→bottom ramp: it maps the node's box
// INTO gradient space, so this is the INVERSE of the axis it produces.
const TOP_TO_BOTTOM = { m00: 0, m01: 1, m02: 0, m10: -1, m11: 0, m12: 1 };

function gradientScene(paint, { size = { x: 100, y: 100 }, type = 'ELLIPSE' } = {}) {
  return buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
      size: { x: 200, y: 200 } },
    { guid: { sessionID: 0, localID: 3 }, type, name: 'Shape',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' },
      size, fillPaints: [paint] },
  ]});
}

const CTX_MIN = { images: new Map(), fileKey: 'K', fileName: 'F', frameName: 'Root' };

function findByName(node, name) {
  if (node.name === name) return node;
  for (const c of node.children || []) {
    const hit = findByName(c, name);
    if (hit) return hit;
  }
  return null;
}

function linearPaint(stops, extra = {}) {
  return { type: 'GRADIENT_LINEAR', visible: true, opacity: 1,
           transform: TOP_TO_BOTTOM, stops, ...extra };
}

const WHITE_TO_BLACK = [
  { color: { r: 1, g: 1, b: 1, a: 1 }, position: 0 },
  { color: { r: 0, g: 0, b: 0, a: 1 }, position: 1 },
];

test('a linear gradient lowers to a real CSS gradient, not its mean color', () => {
  const scene = gradientScene(linearPaint(WHITE_TO_BLACK));
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const shape = findByName(envelope.root, 'Shape');
  const css = shape.style.background_gradient;
  assert.ok(css, 'a linear gradient must survive as a gradient');
  assert.match(css, /^linear-gradient\(/);
  // The flattened mean is what this used to produce; it must no longer be the
  // paint, and no stale solid may sit under the gradient to show through it.
  assert.equal(shape.style.background_color, undefined);
  // Nothing was approximated, so nothing may claim it was.
  assert.equal(diagnostics.filter((d) => d.code === 'gradient-approximated').length, 0);
});

test('the paint transform is inverted, so a top→bottom ramp is not flipped', () => {
  // The regression this guards is silent and total: using Figma's matrix
  // forward renders EVERY gradient 180° off — the design's knob rim highlight
  // lights from below. Verified against Figma's own export of the source file.
  const scene = gradientScene(linearPaint(WHITE_TO_BLACK));
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const css = findByName(envelope.root, 'Shape').style.background_gradient;
  assert.match(css, /^linear-gradient\(180deg,/, 'top→bottom is 180deg (`to bottom`)');
  // White leads, black trails: light at the top, dark at the bottom.
  const first = css.indexOf('#ffffff');
  const last = css.indexOf('#000000');
  assert.ok(first > 0 && last > 0, `both stops present: ${css}`);
  assert.ok(first < last, `white must lead black, got: ${css}`);
});

test('a left→right ramp reads as 90deg', () => {
  // Inverse of a (1,0)→(0,1)-style axis: dx=1,dy=0 must be `to right`.
  const scene = gradientScene(linearPaint(WHITE_TO_BLACK, {
    // Identity ⇒ its inverse is identity ⇒ axis (0,0)→(1,0).
    transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
  }));
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const css = findByName(envelope.root, 'Shape').style.background_gradient;
  assert.match(css, /^linear-gradient\(90deg,/, `dx=1,dy=0 is 90deg, got: ${css}`);
});

test('stop alpha and paint opacity both fold into the emitted color', () => {
  // The rim highlight is white at alpha 0.24 fading to transparent. Dropping
  // either factor turns it into a hard white ring.
  const scene = gradientScene(linearPaint([
    { color: { r: 1, g: 1, b: 1, a: 0.5 }, position: 0 },
    { color: { r: 1, g: 1, b: 1, a: 0 }, position: 1 },
  ], { opacity: 0.5 }));
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const css = findByName(envelope.root, 'Shape').style.background_gradient;
  // 0.5 stop alpha * 0.5 paint opacity = 0.25 ⇒ 0x40.
  assert.match(css, /#ffffff40/, `stop alpha x paint opacity must fold, got: ${css}`);
  assert.match(css, /#ffffff00/, `the transparent end must stay transparent, got: ${css}`);
});

test('a radial gradient paints on the BOX branch, and still flattens on the vector one', () => {
  // Which gradient kinds are expressible depends on the CONSUMER, and this test
  // used to assert "radial must not claim a gradient" for both. That was true of
  // parse_svg_linear_gradient (svg_path_widget.cpp:157, literal `linear-gradient(`
  // only) and FALSE of setBackgroundGradient, which parses radial end to end
  // (css_gradient.cpp:192 → View::set_background_gradient_radial → all three
  // canvases). The test encoded a deferral as a requirement, so it passed
  // happily while the design's 167x119 xy-pad vignette flattened to a uniform
  // wash — a visible loss, on a capability we already had.
  const paint = {
    type: 'GRADIENT_RADIAL', visible: true, opacity: 1,
    transform: TOP_TO_BOTTOM, stops: WHITE_TO_BLACK,
  };

  // Box branch: an ELLIPSE/rect lowers to a background, which CAN paint radial.
  const box = gradientScene(paint);
  const boxOut = materializeFrame(box, findFrame(box, 'Root'), CTX_MIN);
  const shape = findByName(boxOut.envelope.root, 'Shape');
  assert.match(shape.style.background_gradient ?? '', /^radial-gradient\(circle at /,
               'a box paints the radial rather than flattening it');
  assert.equal(shape.style.background_color, undefined,
               'and must not ALSO carry a solid, or the box paints under the ramp');
  // Still a loss: the consumer's radial is a circle, Figma's may be an ellipse.
  // A quieter approximation is still an approximation.
  assert.ok(boxOut.diagnostics.some((d) => d.code === 'gradient-approximated'
                                        && /circle/.test(d.detail || '')),
            'the circle approximation must still be diagnosed');

  // Vector branch: the SvgPath fill consumer parses linear ONLY, so a radial
  // there must keep flattening — emitting one would paint nothing at all.
  const vec = gradientScene(paint, { type: 'VECTOR' });
  const vecOut = materializeFrame(vec, findFrame(vec, 'Root'), CTX_MIN);
  const v = findByName(vecOut.envelope.root, 'Shape');
  assert.equal(v.style?.background_gradient, undefined,
               'a vector must not claim a radial its consumer cannot parse');
  assert.ok(vecOut.diagnostics.some((d) => d.code === 'gradient-approximated'),
            'and must say it flattened');
});

test('a single-stop gradient falls back rather than emitting a broken ramp', () => {
  // parse_svg_linear_gradient requires >= 2 colors and returns nullopt below
  // that, which would drop the widget back to its solid fill anyway.
  const scene = gradientScene(linearPaint([{ color: { r: 1, g: 0, b: 0, a: 1 }, position: 0 }]));
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const shape = findByName(envelope.root, 'Shape');
  assert.equal(shape.style.background_gradient, undefined);
  assert.equal(shape.style.background_color, '#ff0000');
});

// ── override paint provenance: style ref vs the literal beside it ────────────
//
// An instance override that sets fillPaints and says nothing about the style
// leaves the style ATTACHED, and Figma renders the attached style — the literal
// is only Figma's (lossy) cache of it. Only an explicit DETACH (the null-guid
// sentinel) makes the instance's literal authoritative.
//
// The shape is taken from SmallTriaz2.fig, where getting it backwards painted
// `sound / button / switch`'s dot white over the design's #333537 and dropped
// the Sync radio's dot to 20% opacity (reported as a MISSING dot). Both cases
// live here because a rule that fixes one by fiat breaks the other: preferring
// the style unconditionally repaints the detached OFF tabs with the master's
// fuchsia.
function provenanceScene() {
  const g = (l, s = 0) => ({ sessionID: s, localID: l });
  const NULL_REF = { guid: { sessionID: 0xFFFFFFFF, localID: 0xFFFFFFFF } };
  const solid = (r, gr, b, opacity = 1) => [
    { type: 'SOLID', visible: true, color: { r, g: gr, b, a: 1 }, opacity },
  ];
  return buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    // Style table: the dark token the dot's ref names, and the fuchsia one the
    // tab's ref names.
    { guid: g(5), type: 'STYLE', styleType: 'FILL', key: 'darkkey',
      name: 'button / icon on', fillPaints: solid(0.2, 0.208, 0.216) },
    { guid: g(6), type: 'STYLE', styleType: 'FILL', key: 'fuchsiakey',
      name: 'instrument / 02 Fuchsia', fillPaints: solid(0.96, 0.44, 0.56) },

    // Master: a dot that REFERENCES the dark style, and a tab label that
    // references the fuchsia one. Both cache their style's color as a literal.
    { guid: g(10), type: 'SYMBOL', name: 'switch off', size: { x: 18, y: 10 } },
    { guid: g(11), type: 'ELLIPSE', name: 'dot', overrideKey: g(500),
      parentIndex: { guid: g(10), position: 'a' }, size: { x: 6, y: 6 },
      styleIdForFill: { assetRef: { key: 'darkkey', version: '1' } },
      fillPaints: solid(0.2, 0.208, 0.216) },
    { guid: g(12), type: 'TEXT', name: 'tab', overrideKey: g(501),
      parentIndex: { guid: g(10), position: 'b' }, size: { x: 6, y: 6 },
      styleIdForFill: { assetRef: { key: 'fuchsiakey', version: '1' } },
      fillPaints: solid(0.96, 0.44, 0.56), textData: { characters: 'OFF' } },

    // Wrapper master: an INSTANCE of the above that (a) recolours the dot with
    // a literal but leaves its ref ALONE, and (b) DETACHES the tab's ref and
    // paints it white. This is the `switch / on` + `inst selection / off left`
    // shape.
    { guid: g(20), type: 'SYMBOL', name: 'switch on', size: { x: 18, y: 10 } },
    { guid: g(21), type: 'INSTANCE', name: 'inner', overrideKey: g(600),
      parentIndex: { guid: g(20), position: 'a' }, size: { x: 18, y: 10 },
      symbolData: { symbolID: g(10), symbolOverrides: [
        { guidPath: { guids: [g(500)] }, fillPaints: solid(1, 1, 1) },
        { guidPath: { guids: [g(501)] },
          styleIdForFill: NULL_REF, fillPaints: solid(1, 1, 1) },
      ] } },

    { guid: g(2), type: 'FRAME', name: 'Root',
      parentIndex: { guid: g(1), position: 'a' }, size: { x: 200, y: 100 } },
    // The design's instance. Its forwarded entry re-states a literal on the
    // ALREADY-DETACHED tab at a new opacity — the outer-instance case that must
    // still keep its literal rather than snapping back to fuchsia.
    { guid: g(30), type: 'INSTANCE', name: 'switch on',
      parentIndex: { guid: g(2), position: 'a' }, size: { x: 18, y: 10 },
      symbolData: { symbolID: g(20), symbolOverrides: [
        { guidPath: { guids: [g(600), g(501)] }, fillPaints: solid(1, 1, 1, 0.35) },
      ] } },
  ]});
}

test('an override literal does not beat the style ref it left attached', () => {
  const { envelope } = materializeFrame(
    provenanceScene(), findFrame(provenanceScene(), 'Root'), CTX_MIN);

  // The override set the dot white but said nothing about its style, so the
  // style is still attached and IS the color. Rendering the white literal is
  // the FILTER-switch bug: a pure-white dot where the design has #333537.
  const dot = findByName(envelope.root, 'dot');
  assert.ok(dot, 'dot must survive expansion');
  assert.equal(dot.style.background_color, '#333537');
});

test('an override that DETACHES a style keeps its own literal, at every level', () => {
  const { envelope } = materializeFrame(
    provenanceScene(), findFrame(provenanceScene(), 'Root'), CTX_MIN);

  // The inner instance cut the fuchsia style loose, so the tab is white — and
  // the outer instance's forwarded literal (white at 35%) still applies on top
  // rather than the ref reasserting itself. Snapping back to #f6718e here is
  // the "wall of pink" regression.
  const tab = findByName(envelope.root, 'tab');
  assert.ok(tab, 'tab must survive expansion');
  assert.equal(tab.style.color ?? tab.style.background_color, '#ffffff59');
});

// ── material fidelity: what the design declares must survive or be said aloud ──
//
// Each of these three properties was dropped SILENTLY: the design declared it,
// the import discarded it, no diagnostic fired, and every checker we own stayed
// green — because none of them counts what the file declares. material_audit.mjs
// is the counterpart that does; these are the fixes it exists to prove.

test('a stroke paint carries its own opacity, not just its color alpha', () => {
  // Figma multiplies paint.opacity by color.a. Reading only color.a rendered a
  // stroke the designer set to 20% at FULL strength — and that does not read as
  // a dropped property, it reads as a design with harder edges than expected, so
  // it never got reported as a bug. The fill and text paths already take the
  // product; only the stroke path forgot.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Faint',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 50, y: 50 },
      strokeWeight: 1,
      strokePaints: [{ type: 'SOLID', visible: true, opacity: 0.2,
                       color: { r: 1, g: 1, b: 1, a: 1 } }] },
  ]});
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const faint = findByName(envelope.root, 'Faint');
  // 0.2 * 1.0 -> alpha 51 -> 0x33. Asserting the ALPHA specifically: a test that
  // only checked "a border was emitted" stays green through the whole bug.
  assert.equal(faint.style.border, '1px solid #ffffff33');
});

test('an effect with no lowering is diagnosed, never dropped in silence', () => {
  // effectsToBoxShadow `continue`s past anything that is not a shadow, so a
  // LAYER_BLUR produced no shadow, no style, and no word about either.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Blurred',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 50, y: 50 },
      effects: [{ type: 'LAYER_BLUR', visible: true, radius: 8 }] },
    // Control: a shadow DOES lower, so it must not be diagnosed as unsupported.
    { guid: { sessionID: 0, localID: 4 }, type: 'FRAME', name: 'Shadowed',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 50, y: 50 },
      effects: [{ type: 'DROP_SHADOW', visible: true, radius: 4,
                  offset: { x: 0, y: 2 }, color: { r: 0, g: 0, b: 0, a: 0.25 } }] },
    // Control: an invisible effect is not declared at all — diagnosing it would
    // be noise, and noise is how a checker gets ignored.
    { guid: { sessionID: 0, localID: 5 }, type: 'FRAME', name: 'Off',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'c' }, size: { x: 50, y: 50 },
      effects: [{ type: 'LAYER_BLUR', visible: false, radius: 8 }] },
  ]});
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const unsupported = diagnostics.filter((d) => d.code === 'effect-unsupported');
  assert.equal(unsupported.length, 1, 'exactly the blur is diagnosed');
  assert.equal(unsupported[0].node_id, '0:3');
  assert.match(unsupported[0].detail, /LAYER_BLUR/);
  assert.ok(findByName(envelope.root, 'Shadowed').style.box_shadow, 'a shadow still lowers');
});

test('non-uniform corner radii are diagnosed rather than silently squared', () => {
  // cornerRadius() returns null unless all four corners agree, so an asymmetric
  // card used to import with SQUARE corners. IRStyle carries all four radii and
  // the bridge takes setCornerRadius(id, 'TopLeft', r), so the corners lower
  // exactly — the earlier diagnostic that explained the loss now has nothing to
  // report, and this asserts the values rather than the apology.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'ROUNDED_RECTANGLE', name: 'Card',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 50, y: 50 },
      rectangleTopLeftCornerRadius: 8, rectangleTopRightCornerRadius: 8,
      rectangleBottomRightCornerRadius: 0, rectangleBottomLeftCornerRadius: 0 },
    // Control: four corners that AGREE lower fine and must stay undiagnosed —
    // otherwise the check fires on every rounded box in every design.
    { guid: { sessionID: 0, localID: 4 }, type: 'ROUNDED_RECTANGLE', name: 'Even',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 50, y: 50 },
      rectangleTopLeftCornerRadius: 4, rectangleTopRightCornerRadius: 4,
      rectangleBottomRightCornerRadius: 4, rectangleBottomLeftCornerRadius: 4 },
    // Control: a genuinely square node declares no radius, so "could not express
    // it" must not be confused with "did not ask for it".
    { guid: { sessionID: 0, localID: 5 }, type: 'FRAME', name: 'Square',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'c' }, size: { x: 50, y: 50 } },
  ]});
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);

  // The asymmetric card lowers per-corner, and carries NO uniform radius —
  // emitting both would let codegen's single setCornerRadius(id, 'All', r)
  // square off the two corners the design rounds.
  const card = findByName(envelope.root, 'Card').style;
  assert.equal(card.border_radius, undefined, 'no uniform radius beside per-corner ones');
  assert.equal(card.border_top_left_radius, 8);
  assert.equal(card.border_top_right_radius, 8);
  assert.equal(card.border_bottom_right_radius, 0);
  assert.equal(card.border_bottom_left_radius, 0);

  // Corners that agree stay on the uniform path: one call, not four.
  const even = findByName(envelope.root, 'Even').style;
  assert.equal(even.border_radius, 4);
  assert.equal(even.border_top_left_radius, undefined);

  // A square node asked for nothing and must not acquire a radius of 0.
  const square = findByName(envelope.root, 'Square').style;
  assert.equal(square.border_radius, undefined);
  assert.equal(square.border_top_left_radius, undefined);
});

test('the materials sidecar reports what the design declares, not what we read', () => {
  // The sidecar's independence is the whole point of the audit: it is built from
  // the resolved node's own fields, never from what styleFor chose to look at.
  // If it were sourced from the emitter it would agree with the emitter by
  // construction and could never catch a drop — which is precisely the blind
  // spot every other checker has.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'FRAME', name: 'Rich',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 50, y: 50 },
      strokeWeight: 2, blendMode: 'MULTIPLY',
      strokePaints: [{ type: 'SOLID', visible: true, opacity: 0.5, color: { r: 1, g: 1, b: 1, a: 1 } }],
      effects: [{ type: 'LAYER_BLUR', visible: true, radius: 4 }],
      rectangleTopLeftCornerRadius: 6, rectangleTopRightCornerRadius: 2,
      rectangleBottomRightCornerRadius: 6, rectangleBottomLeftCornerRadius: 2 },
    // A node with no material at all stays OUT of the sidecar, so it tracks the
    // design's material rather than its node count.
    { guid: { sessionID: 0, localID: 4 }, type: 'FRAME', name: 'Bare',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 10, y: 10 } },
  ]});
  const { materials } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const rich = materials.nodes.find((n) => n.node_id === '0:3');
  assert.ok(rich, 'a node declaring material is inventoried');
  assert.deepEqual(rich.declared.stroke, [{ type: 'SOLID', opacity: 0.5, color_alpha: 1 }]);
  assert.deepEqual(rich.declared.effects, ['LAYER_BLUR']);
  assert.deepEqual(rich.declared.corner_radius, [6, 2, 6, 2]);
  assert.equal(rich.declared.blend_mode, 'MULTIPLY');
  assert.ok(!materials.nodes.some((n) => n.node_id === '0:4'),
    'a node declaring no material is not inventoried');
});

test('a stacked fill list composites; taking only the first paints the wrong color', () => {
  // Figma's fillPaints is a STACK painted in array order, index 0 at the bottom.
  // Reading `.find(SOLID)` took the bottom paint and threw the rest away, so the
  // slider thumb — a #4b4d51 base with white at 55% OVER it — rendered as the
  // bare dark base instead of the light thumb the design shows.
  //
  // The failure mode is what makes this worth a test: a dropped paint does not
  // look dropped, it looks like the WRONG COLOR, which sends you hunting through
  // style refs and override precedence for a bug that is in neither.
  //
  // These are the thumb's real declared paints, and #aeafb1 is not a guess: it is
  // what Figma's own embedded thumbnail samples at that node.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'ELLIPSE', name: 'Thumb',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 7, y: 7 },
      fillPaints: [
        { type: 'SOLID', visible: true, opacity: 1,
          color: { r: 0.29411765933036804, g: 0.3019607961177826, b: 0.3176470696926117, a: 1 } },
        { type: 'SOLID', visible: true, opacity: 0.550000011920929,
          color: { r: 1, g: 1, b: 1, a: 1 } },
      ] },
    // Control: an invisible paint in the stack must not composite — it is not
    // painted in Figma either.
    { guid: { sessionID: 0, localID: 4 }, type: 'ELLIPSE', name: 'HiddenTop',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 7, y: 7 },
      fillPaints: [
        { type: 'SOLID', visible: true, opacity: 1, color: { r: 0, g: 0, b: 0, a: 1 } },
        { type: 'SOLID', visible: false, opacity: 1, color: { r: 1, g: 1, b: 1, a: 1 } },
      ] },
    // Control: an opaque paint ON TOP hides what is under it. This is the
    // assertion that pins the ORDER — if the stack composited top-to-bottom this
    // would come out white, and the Thumb case alone would not catch it.
    { guid: { sessionID: 0, localID: 5 }, type: 'ELLIPSE', name: 'OpaqueTop',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'c' }, size: { x: 7, y: 7 },
      fillPaints: [
        { type: 'SOLID', visible: true, opacity: 1, color: { r: 1, g: 1, b: 1, a: 1 } },
        { type: 'SOLID', visible: true, opacity: 1, color: { r: 0, g: 0, b: 0, a: 1 } },
      ] },
    // Control: a single paint is unchanged — the common case must not move.
    { guid: { sessionID: 0, localID: 6 }, type: 'ELLIPSE', name: 'Single',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'd' }, size: { x: 7, y: 7 },
      fillPaints: [{ type: 'SOLID', visible: true, opacity: 0.35, color: { r: 0, g: 0, b: 0, a: 1 } }] },
  ]});
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const bg = (n) => findByName(envelope.root, n).style.background_color;
  assert.equal(bg('Thumb'), '#aeafb1', 'white@0.55 over #4b4d51 — what Figma rasterizes');
  assert.equal(bg('HiddenTop'), '#000000', 'an invisible paint contributes nothing');
  assert.equal(bg('OpaqueTop'), '#000000', 'the LAST paint is on top and hides the first');
  assert.equal(bg('Single'), '#00000059', 'a lone paint still folds its opacity, unchanged');
});

test('a box-model mask clips via its synthesized outline', () => {
  // A rectangle / ellipse used as a mask has no geometry blobs — its outline
  // is derived from the box. The lowering must still produce a parent-space
  // clip: corner radii as arc cubics, the transform baked into every point.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
      size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'ROUNDED_RECTANGLE', name: 'window', mask: true,
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' },
      size: { x: 40, y: 20 }, cornerRadius: 4,
      transform: { m00: 1, m01: 0, m02: 2, m10: 0, m11: 1, m12: 1 },
      fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] },
    { guid: { sessionID: 0, localID: 4 }, type: 'ROUNDED_RECTANGLE', name: 'content',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
      size: { x: 80, y: 80 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 0, a: 1 }, visible: true }] },
  ]});
  const ctx = { images: new Map(), parserVersion: 't', compatSchemaVersion: '1', exportedAt: 'x' };
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Panel'), ctx);
  const scope = envelope.root.children.find((n) => n.name === 'window (mask scope)');
  assert.ok(scope, 'synthetic clip scope missing');
  // Top-left corner starts at radius offset (2+4, 1), the top edge runs to the
  // radius before the top-right corner, then an arc cubic takes over.
  assert.ok(scope.style.clip_path.startsWith('path("M6 1 L38 1 C'),
    `unexpected outline start: ${scope.style.clip_path}`);
  assert.ok(scope.style.clip_path.endsWith('Z")'));
  assert.deepEqual(scope.children.map((n) => n.name), ['content']);
});

test('inexact mask lowerings say so — and the mask still never paints', () => {
  const ctx = { images: new Map(), parserVersion: 't', compatSchemaVersion: '1', exportedAt: 'x' };
  // A translucent alpha mask clips harder than the design intended: the soft
  // alpha flattens to the hard outline. That must be confessed, not silent —
  // a mask that over-clips looks like a cropping bug, not a dropped property.
  const soft = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Panel',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
      size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'ROUNDED_RECTANGLE', name: 'fade', mask: true,
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' },
      size: { x: 40, y: 20 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, opacity: 0.5, visible: true }] },
    { guid: { sessionID: 0, localID: 4 }, type: 'ROUNDED_RECTANGLE', name: 'content',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
      size: { x: 80, y: 80 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 0, a: 1 }, visible: true }] },
  ]});
  const softOut = materializeFrame(soft, findFrame(soft, 'Panel'), ctx);
  assert.ok(softOut.diagnostics.some((d) => d.code === 'mask-approximated' && /alpha mask/.test(d.detail)));
  assert.equal(DIAGNOSTIC_SEVERITY['mask-approximated'], 'warning');
  assert.ok(softOut.envelope.root.children.some((n) => n.name === 'fade (mask scope)'),
    'the outline clip still applies');

  // Inside auto-layout the wrapper would fight the flex pass: no lowering,
  // but the one invariant that has no exception holds — the mask never paints.
  const flow = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Row', stackMode: 'HORIZONTAL',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' },
      size: { x: 100, y: 20 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'ROUNDED_RECTANGLE', name: 'm', mask: true,
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' },
      size: { x: 20, y: 20 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 1, g: 1, b: 1, a: 1 }, visible: true }] },
    { guid: { sessionID: 0, localID: 4 }, type: 'ROUNDED_RECTANGLE', name: 'flowed',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' },
      size: { x: 20, y: 20 },
      transform: { m00: 1, m01: 0, m02: 0, m10: 0, m11: 1, m12: 0 },
      fillPaints: [{ type: 'SOLID', color: { r: 0, g: 0, b: 0, a: 1 }, visible: true }] },
  ]});
  const flowOut = materializeFrame(flow, findFrame(flow, 'Row'), ctx);
  assert.ok(flowOut.diagnostics.some((d) => d.code === 'mask-approximated' && /auto-layout/.test(d.detail)));
  assert.deepEqual(flowOut.envelope.root.children.map((n) => n.name), ['flowed']);
});

test('every diagnostic code this module emits is registered with a severity', () => {
  // An unregistered code falls through `DIAGNOSTIC_SEVERITY[code] || 'info'` to
  // info, and BOTH consumers drop info: fig_decode's "N warning(s)" count
  // excludes it, and print_import_diagnostics skips it. So a diagnostic written
  // precisely to make a fidelity loss visible becomes invisible — the loss AND
  // its report both vanish because a table entry was missing. Four codes were in
  // that state at once, found by a review rather than by any test.
  //
  // This reads the SOURCE instead of listing codes by hand, because a hand-kept
  // list is the same failure one level up: it would drift the moment someone
  // adds a fifth.
  const src = readFileSync(join(here, 'scene.mjs'), 'utf8');
  const emitted = emittedDiagnosticCodes(src);
  const registered = Object.keys(DIAGNOSTIC_SEVERITY).sort();

  assert.ok(emitted.length >= 8, `expected the decoder to emit several codes, saw ${emitted.length}`);

  const unregistered = emitted.filter((c) => !registered.includes(c));
  assert.deepEqual(unregistered, [],
    'emitted but unregistered — these silently downgrade to info and are shown nowhere');

  // The other direction: a registered code nobody emits is a promise not kept.
  // 'unresolved-token' sat here for exactly that reason — the table implied a
  // design with an unresolvable token would be told, and it never was.
  const unemitted = registered.filter((c) => !emitted.includes(c));
  assert.deepEqual(unemitted, [],
    'registered but never emitted — the table promises a warning that cannot happen');
});

test('a paint-level blend mode is recorded even though nothing lowers it', () => {
  // Figma composites an INDIVIDUAL paint with its own mode. We have no per-paint
  // primitive, and the node-level mix_blend_mode is not a substitute — lifting a
  // paint's mode there is only sound when the node has exactly one visible fill
  // and nothing else.
  //
  // The point of this test is the SIDECAR, not the lowering. The property was
  // triple-silent: unread by the decoder, unnamed by any diagnostic, unrecorded
  // by the inventory. The last is the one that mattered — a property the audit
  // cannot see is a hole in the audit's own thesis, and it would have reported
  // "everything survived" over a design that dropped this.
  const scene = buildScene({ nodeChanges: [
    { guid: { sessionID: 0, localID: 1 }, type: 'CANVAS', name: 'Page' },
    { guid: { sessionID: 0, localID: 2 }, type: 'FRAME', name: 'Root',
      parentIndex: { guid: { sessionID: 0, localID: 1 }, position: 'a' }, size: { x: 100, y: 100 } },
    { guid: { sessionID: 0, localID: 3 }, type: 'RECTANGLE', name: 'Blended',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'a' }, size: { x: 50, y: 50 },
      fillPaints: [{ type: 'SOLID', visible: true, opacity: 1, blendMode: 'LIGHTEN',
                     color: { r: 1, g: 0, b: 0, a: 1 } }] },
    // Control: NORMAL is "just composite it" and must not be recorded, or every
    // paint in every design shows up as a declaration and the count is noise.
    { guid: { sessionID: 0, localID: 4 }, type: 'RECTANGLE', name: 'Plain',
      parentIndex: { guid: { sessionID: 0, localID: 2 }, position: 'b' }, size: { x: 50, y: 50 },
      fillPaints: [{ type: 'SOLID', visible: true, opacity: 1, blendMode: 'NORMAL',
                     color: { r: 0, g: 1, b: 0, a: 1 } }] },
  ]});
  const { materials } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);
  const blended = materials.nodes.find((n) => n.node_id === '0:3');
  assert.equal(blended.declared.fill[0].blend_mode, 'LIGHTEN',
    'a paint-level blend is inventoried even with no lowering for it');
  const plain = materials.nodes.find((n) => n.node_id === '0:4');
  assert.equal(plain.declared.fill[0].blend_mode, null, 'NORMAL is not a declaration');
});

// A variant built by nesting the OTHER variant and overriding it. This is the
// real shape of the design's switch: the "on" SYMBOL's only child is an
// INSTANCE of the "off" symbol, and three overrides turn one into the other —
// hide the master's visible dot, show the master's hidden one, repaint the
// track. Every one of those overrides reaches its target through TWO hops, so
// nothing here is exercised by a direct-guid override test.
//
// What this pins, established by breaking each candidate and watching: it is
// the ONLY test that fails when `cloneSymbolChildren` decides visibility from
// the master instead of from the post-override state. Sabotaging the
// deeper-override FORWARDING leaves it green — the nested instance authors
// these overrides itself, so they resolve at depth 1 inside its own expansion,
// and the first draft of this comment claiming otherwise was wrong.
//
// Nothing else covered the ordering, so the toggle could have gone back to
// rendering the wrong dot in silence — a plausible switch in the wrong state
// reads as a design decision, not a bug, which is how it survived.
function nestedVariantScene() {
  const g = (l, s = 0) => ({ sessionID: s, localID: l });
  return buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    // Master OFF: a track plus two dots. The master shows "on position" and
    // hides "off position" — backwards-looking, and copied from the real file,
    // because a fixture that tidies this up stops testing it.
    { guid: g(10), type: 'SYMBOL', name: 'switch / off', size: { x: 18, y: 10 } },
    { guid: g(11), type: 'ROUNDED_RECTANGLE', name: 'track', overrideKey: g(21, 1554),
      parentIndex: { guid: g(10), position: 'a' }, size: { x: 18, y: 10 },
      fillPaints: [{ type: 'SOLID', visible: true, opacity: 0.35,
                     color: { r: 0, g: 0, b: 0, a: 1 } }] },
    { guid: g(12), type: 'ELLIPSE', name: 'on position', overrideKey: g(22, 1554),
      visible: true, parentIndex: { guid: g(10), position: 'b' }, size: { x: 6, y: 6 },
      fillPaints: [{ type: 'SOLID', visible: true, color: { r: 0.47, g: 0.48, b: 0.48, a: 1 } }] },
    { guid: g(13), type: 'ELLIPSE', name: 'off position', overrideKey: g(50262, 1956),
      visible: false, parentIndex: { guid: g(10), position: 'c' }, size: { x: 6, y: 6 },
      fillPaints: [{ type: 'SOLID', visible: true, color: { r: 0.2, g: 0.21, b: 0.22, a: 1 } }] },
    // Master ON: nothing of its own — just an INSTANCE of OFF, flipped by
    // overrides that this instance authors.
    { guid: g(14), type: 'SYMBOL', name: 'switch / on', size: { x: 18, y: 10 } },
    { guid: g(15), type: 'INSTANCE', name: 'switch / off', overrideKey: g(98, 0),
      parentIndex: { guid: g(14), position: 'a' }, size: { x: 18, y: 10 },
      symbolData: { symbolID: g(10), symbolOverrides: [
        { guidPath: { guids: [g(22, 1554)] }, visible: false },
        { guidPath: { guids: [g(50262, 1956)] }, visible: true },
      ] } },
    { guid: g(2), type: 'FRAME', name: 'Root',
      parentIndex: { guid: g(1), position: 'a' }, size: { x: 100, y: 40 } },
    { guid: g(20), type: 'INSTANCE', name: 'sound / button / switch / on',
      parentIndex: { guid: g(2), position: 'a' }, size: { x: 18, y: 10 },
      symbolData: { symbolID: g(14) } },
  ]});
}

test('a variant nested inside a variant flips visibility through both hops', () => {
  const scene = nestedVariantScene();
  const { envelope } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);

  // The master's visible dot is OFF in this variant, and its hidden one is ON.
  // Getting this backwards renders a plausible switch in the wrong state, which
  // is why it survived: it looks like a design decision, not a bug.
  assert.ok(!findByName(envelope.root, 'on position'),
    'the master-visible dot must be hidden by the nested override');
  const dot = findByName(envelope.root, 'off position');
  assert.ok(dot, 'the master-hidden dot must be shown by the nested override');

  // The track comes through untouched, so a test that passes by dropping the
  // whole subtree cannot look like a pass.
  assert.ok(findByName(envelope.root, 'track'), 'the track must survive expansion');
});

// ── Node-type dispatch fidelity ──────────────────────────────────────────────
// Exhaustive dispatch: no node family reaches the envelope through a silent
// default-to-frame. Skipped families (SLICE, editor/FigJam/Slides) emit no
// node + a diagnostic; diagnosed families (SLOT, TEXT_PATH, unknown types)
// emit a node + a diagnostic; TRANSFORM (the kiwi spelling of the plugin
// API's TRANSFORM_GROUP) is an explicit, silent container. Kept in lockstep
// with the plugin lane (extract-pure.ts::dispatchNodeType) and the REST lane
// (figma_rest_export.py::dispatch_node_type).
function nodeDispatchScene() {
  const g = (l) => ({ sessionID: 0, localID: l });
  const child = (l, type, name, extra = {}) => ({
    guid: g(l), type, name,
    parentIndex: { guid: g(2), position: String.fromCharCode(97 + l) },
    size: { x: 40, y: 20 }, ...extra,
  });
  return buildScene({ nodeChanges: [
    { guid: g(1), type: 'CANVAS', name: 'Page' },
    { guid: g(2), type: 'FRAME', name: 'Root',
      parentIndex: { guid: g(1), position: 'a' }, size: { x: 400, y: 300 } },
    child(3, 'TEXT_PATH', 'Curved Label',
      { textData: { characters: 'WOW FACTOR' },
        fillPaints: [{ type: 'SOLID', visible: true,
                       color: { r: 1, g: 1, b: 1, a: 1 } }] }),
    child(4, 'TRANSFORM', 'Spun Group'),
    child(5, 'SLOT', 'Content Slot'),
    child(6, 'SLICE', 'Export @2x'),
    child(7, 'STICKY', 'Reviewer Note'),
    child(8, 'HOLOGRAM_2027', 'Future Thing'),
  ]});
}

test('node dispatch: SLICE and STICKY are skipped (no node) with diagnostics', () => {
  const scene = nodeDispatchScene();
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);

  // Before this contract both became empty generic frames that looked
  // successfully imported — a slice paints NOTHING in Figma, so skipping it
  // is the correct rendering, and a sticky is FigJam furniture.
  assert.ok(!findByName(envelope.root, 'Export @2x'), 'SLICE emits no node');
  assert.ok(!findByName(envelope.root, 'Reviewer Note'), 'STICKY emits no node');

  const slice = diagnostics.filter((d) => d.code === 'slice-skipped');
  assert.equal(slice.length, 1);
  assert.equal(slice[0].severity, 'warning');
  assert.equal(slice[0].node_name, 'Export @2x');

  const sticky = diagnostics.filter((d) => d.code === 'unsupported-node');
  assert.equal(sticky.length, 1);
  assert.match(sticky[0].detail, /STICKY/);
});

test('node dispatch: TEXT_PATH lands as text with its characters and a flatten diagnostic', () => {
  const scene = nodeDispatchScene();
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);

  const curved = findByName(envelope.root, 'Curved Label');
  assert.ok(curved, 'TEXT_PATH emits a node');
  assert.equal(curved.type, 'text');
  // The content is the point: TEXT_PATH carries real copy, and re-typing it
  // must not drop it.
  assert.equal(curved.content, 'WOW FACTOR');
  // The fill is the glyph color, not a background wash behind the text.
  assert.equal(curved.style.color, '#ffffff');
  assert.ok(!curved.style.background_color, 'fill must not double as background');

  const flat = diagnostics.filter((d) => d.code === 'text-path-flattened');
  assert.equal(flat.length, 1);
  assert.equal(flat[0].severity, 'warning');
});

test('node dispatch: TRANSFORM is a silent explicit frame; SLOT is a diagnosed frame', () => {
  const scene = nodeDispatchScene();
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);

  const spun = findByName(envelope.root, 'Spun Group');
  assert.ok(spun, 'TRANSFORM emits a node');
  assert.equal(spun.type, 'frame');
  // A transform group renders fine as a container — explicit dispatch, no noise.
  assert.equal(diagnostics.filter((d) => d.node_name === 'Spun Group').length, 0);

  const slot = findByName(envelope.root, 'Content Slot');
  assert.ok(slot, 'SLOT emits a (placeholder) node');
  assert.equal(slot.type, 'frame');
  const slotDiag = diagnostics.filter((d) => d.code === 'slot-placeholder');
  assert.equal(slotDiag.length, 1);
});

test('node dispatch: an unknown type falls back to frame WITH a diagnostic', () => {
  const scene = nodeDispatchScene();
  const { envelope, diagnostics } = materializeFrame(scene, findFrame(scene, 'Root'), CTX_MIN);

  // Never crash on new families — the node survives as a frame...
  const future = findByName(envelope.root, 'Future Thing');
  assert.ok(future, 'unknown type still emits a node');
  assert.equal(future.type, 'frame');
  // ...but the fallback is stated, not silent.
  const unk = diagnostics.filter((d) => d.code === 'unknown-node-type');
  assert.equal(unk.length, 1);
  assert.match(unk[0].detail, /HOLOGRAM_2027/);
});
