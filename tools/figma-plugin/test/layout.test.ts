// Auto-layout extraction for the Figma plugin lane: the container's own flex
// or GRID contract plus the properties a node carries as a CHILD of an
// auto-layout parent (layoutGrow / layoutAlign / grid cell placement). The
// child fields are gated on parent context because Figma leaves them stale on
// nodes outside an auto-layout parent. Kept in lockstep with the REST lane
// (tools/import-design/figma_rest_export.py + test/test_figma_rest_export.py)
// and the .fig lane (tools/import-design/fig/scene.mjs + fig.test.mjs).

import { test } from "node:test";
import assert from "node:assert/strict";

import { extractLayout, parentLayoutMode } from "../src/extract-pure";

// extractLayout reads type + layout members only; the cast keeps fixtures to
// the fields under test rather than standing up a full SceneNode.
const sceneNode = (over: Record<string, unknown>) => over as unknown as SceneNode;

const flexRow = (over: Record<string, unknown> = {}) =>
  sceneNode({ type: "FRAME", layoutMode: "HORIZONTAL", ...over });
const flexColumn = (over: Record<string, unknown> = {}) =>
  sceneNode({ type: "FRAME", layoutMode: "VERTICAL", ...over });
const plainFrame = (over: Record<string, unknown> = {}) =>
  sceneNode({ type: "FRAME", layoutMode: "NONE", ...over });

test("wrap emits counterAxisSpacing as the cross-axis track gap", () => {
  // A row's wrapped tracks stack vertically → rowGap.
  const row = extractLayout(
    flexRow({ layoutWrap: "WRAP", counterAxisSpacing: 12 }),
    null,
  );
  assert.equal(row.wrap, true);
  assert.equal(row.rowGap, 12);
  assert.equal(row.columnGap, undefined);
  // A column's wrapped tracks stack horizontally → columnGap.
  const col = extractLayout(
    flexColumn({ layoutWrap: "WRAP", counterAxisSpacing: 8 }),
    null,
  );
  assert.equal(col.columnGap, 8);
  assert.equal(col.rowGap, undefined);
});

test("counterAxisSpacing / alignContent stay off non-wrapping stacks", () => {
  const l = extractLayout(
    flexRow({ layoutWrap: "NO_WRAP", counterAxisSpacing: 12, counterAxisAlignContent: "SPACE_BETWEEN" }),
    null,
  );
  assert.equal(l.rowGap, undefined);
  assert.equal(l.alignContent, undefined);
});

test("counterAxisAlignContent SPACE_BETWEEN maps to alignContent; AUTO emits nothing", () => {
  const spread = extractLayout(
    flexRow({ layoutWrap: "WRAP", counterAxisAlignContent: "SPACE_BETWEEN" }),
    null,
  );
  assert.equal(spread.alignContent, "space-between");
  const auto = extractLayout(
    flexRow({ layoutWrap: "WRAP", counterAxisAlignContent: "AUTO" }),
    null,
  );
  assert.equal(auto.alignContent, undefined);
});

test("child of a flex auto-layout parent emits layoutGrow and layoutAlign", () => {
  const l = extractLayout(
    sceneNode({ type: "RECTANGLE", layoutGrow: 2, layoutAlign: "STRETCH" }),
    flexRow(),
  );
  assert.equal(l.flexGrow, 2);
  assert.equal(l.alignSelf, "stretch");
});

test("layoutAlign maps MIN/MAX/CENTER; INHERIT emits nothing", () => {
  const parent = flexColumn();
  assert.equal(extractLayout(sceneNode({ type: "TEXT", layoutAlign: "MIN" }), parent).alignSelf, "flex-start");
  assert.equal(extractLayout(sceneNode({ type: "TEXT", layoutAlign: "MAX" }), parent).alignSelf, "flex-end");
  assert.equal(extractLayout(sceneNode({ type: "TEXT", layoutAlign: "CENTER" }), parent).alignSelf, "center");
  // INHERIT (follow the parent's counterAxisAlignItems) is the flex default —
  // omitting align-self IS inherit, so nothing is emitted.
  assert.equal(extractLayout(sceneNode({ type: "TEXT", layoutAlign: "INHERIT" }), parent).alignSelf, undefined);
});

test("child of a NON-auto-layout parent never gets grow/align", () => {
  // Figma keeps layoutGrow/layoutAlign populated on nodes whose parent has no
  // auto-layout; emitting them would inject flex into an absolute layout.
  const child = sceneNode({ type: "RECTANGLE", layoutGrow: 1, layoutAlign: "STRETCH" });
  const none = extractLayout(child, plainFrame());
  assert.equal(none.flexGrow, undefined);
  assert.equal(none.alignSelf, undefined);
  // A GROUP parent has no layoutMode member at all.
  const group = extractLayout(child, sceneNode({ type: "GROUP" }));
  assert.equal(group.flexGrow, undefined);
  assert.equal(group.alignSelf, undefined);
});

test("an ABSOLUTE stack child is out of flow: no grow/align", () => {
  // layoutPositioning ABSOLUTE opts the child out of the stack; it takes the
  // absolute-position + constraints path instead, and grow/align would fight
  // that coordinate placement.
  const l = extractLayout(
    sceneNode({ type: "RECTANGLE", layoutPositioning: "ABSOLUTE", layoutGrow: 1, layoutAlign: "CENTER" }),
    flexRow(),
  );
  assert.equal(l.flexGrow, undefined);
  assert.equal(l.alignSelf, undefined);
});

test("GRID container lowers to repeat templates and grid gaps", () => {
  const l = extractLayout(
    sceneNode({
      type: "FRAME", layoutMode: "GRID",
      gridColumnCount: 4, gridRowCount: 3, gridColumnGap: 6, gridRowGap: 4,
    }),
    null,
  );
  assert.equal(l.display, "grid");
  assert.equal(l.gridTemplateColumns, "repeat(4, 1fr)");
  assert.equal(l.gridTemplateRows, "repeat(3, 1fr)");
  assert.equal(l.columnGap, 6);
  assert.equal(l.rowGap, 4);
});

test("GRID child placement: 0-based anchors become CSS 1-based lines with spans", () => {
  const grid = sceneNode({ type: "FRAME", layoutMode: "GRID", gridColumnCount: 4, gridRowCount: 4 });
  const cell = extractLayout(
    sceneNode({ type: "RECTANGLE", gridColumnAnchorIndex: 1, gridRowAnchorIndex: 0 }),
    grid,
  );
  assert.equal(cell.gridColumn, "2");
  assert.equal(cell.gridRow, "1");
  const spanned = extractLayout(
    sceneNode({
      type: "RECTANGLE",
      gridColumnAnchorIndex: 0, gridColumnSpan: 2,
      gridRowAnchorIndex: 2, gridRowSpan: 2,
    }),
    grid,
  );
  assert.equal(spanned.gridColumn, "1 / span 2");
  assert.equal(spanned.gridRow, "3 / span 2");
  // Cell anchors are grid-only: a flex parent must not read them.
  const flexChild = extractLayout(
    sceneNode({ type: "RECTANGLE", gridColumnAnchorIndex: 1, gridRowAnchorIndex: 1 }),
    flexRow(),
  );
  assert.equal(flexChild.gridColumn, undefined);
  assert.equal(flexChild.gridRow, undefined);
});

test("targetAspectRatio emits only on a flexible axis", () => {
  // Flexible (grow) → emitted as width / height.
  const flexible = extractLayout(
    sceneNode({ type: "RECTANGLE", targetAspectRatio: { x: 2, y: 1 }, layoutGrow: 1 }),
    flexRow(),
  );
  assert.equal(flexible.aspectRatio, 2);
  // Fully fixed → the solved w/h already encode the ratio; emitting it would
  // let Yoga re-derive the cross axis and fight the solved size over rounding.
  const fixed = extractLayout(
    sceneNode({ type: "RECTANGLE", targetAspectRatio: { x: 2, y: 1 } }),
    flexRow(),
  );
  assert.equal(fixed.aspectRatio, undefined);
});

test("parentLayoutMode reports flex, grid, and non-layout parents", () => {
  assert.equal(parentLayoutMode(flexRow()), "HORIZONTAL");
  assert.equal(parentLayoutMode(sceneNode({ type: "FRAME", layoutMode: "GRID" })), "GRID");
  assert.equal(parentLayoutMode(plainFrame()), null);
  assert.equal(parentLayoutMode(sceneNode({ type: "GROUP" })), null);
  assert.equal(parentLayoutMode(null), null);
});
