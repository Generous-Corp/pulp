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
