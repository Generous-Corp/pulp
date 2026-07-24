import { test } from "node:test";
import assert from "node:assert/strict";

import {
  decodeForgeDesignForFigma,
  encodeFigmaSelectionForForge,
  FIGMA_PLUGIN_DATA_VALUE_MAX_BYTES,
  FORGE_CLIPBOARD_FORMAT,
  gradientTransformForFigma,
} from "../src/forge-roundtrip";
import { serializeExport } from "../src/serialize";

function faithfulSvgEnvelope(root: Record<string, unknown>): string {
  return JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: JSON.stringify({
      version: 1,
      assetManifest: {
        version: 1,
        assets: [{
          asset_id: "faithful-svg",
          content_hash: "915087052b0dd3ee",
          mime: "image/svg+xml",
        }],
      },
      root,
    }),
    assets: [{
      content_hash: "915087052b0dd3ee",
      mime: "image/svg+xml",
      base64: "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciPjwvc3ZnPg==",
    }],
  });
}

test("CSS cardinal gradient directions use Figma's inverse transform convention", () => {
  const rounded = (matrix: ReturnType<typeof gradientTransformForFigma>) =>
    matrix.map((row) => row.map((value) => {
      const roundedValue = Math.round(value * 1e9) / 1e9;
      return Object.is(roundedValue, -0) ? 0 : roundedValue;
    }));
  assert.deepEqual(rounded(gradientTransformForFigma(90, 100, 100)), [
    [1, 0, 0],
    [0, 1, 0],
  ]);
  assert.deepEqual(rounded(gradientTransformForFigma(180, 100, 100)), [
    [0, 1, 0],
    [-1, 0, 1],
  ]);
  assert.deepEqual(rounded(gradientTransformForFigma(270, 100, 100)), [
    [-1, 0, 1],
    [0, -1, 1],
  ]);
  assert.deepEqual(rounded(gradientTransformForFigma(0, 100, 100)), [
    [0, -1, 1],
    [1, 0, 0],
  ]);

  const transform = gradientTransformForFigma(45, 200, 100);
  const apply = (x: number, y: number) => ({
    x: transform[0][0] * x + transform[0][1] * y + transform[0][2],
    y: transform[1][0] * x + transform[1][1] * y + transform[1][2],
  });
  const radians = (45 - 90) * Math.PI / 180;
  const dx = Math.cos(radians);
  const dy = Math.sin(radians);
  const extent = (Math.abs(dx) * 200 + Math.abs(dy) * 100) / 2;
  const start = apply(0.5 - dx * extent / 200, 0.5 - dy * extent / 100);
  const end = apply(0.5 + dx * extent / 200, 0.5 + dy * extent / 100);
  assert.ok(Math.abs(start.x) < 1e-9);
  assert.ok(Math.abs(start.y - 0.5) < 1e-9);
  assert.ok(Math.abs(end.x - 1) < 1e-9);
  assert.ok(Math.abs(end.y - 0.5) < 1e-9);
});

test("Figma export clipboard embeds the scene and content-addressed assets", () => {
  const text = encodeFigmaSelectionForForge(
    "{\"format_version\":\"2026.05-figma-plugin-v1\"}",
    [{
      content_hash: "abc123",
      mime: "image/png",
      bytes: [0, 1, 2, 253, 254, 255],
    }],
  );
  const parsed = JSON.parse(text);
  assert.equal(parsed.format, FORGE_CLIPBOARD_FORMAT);
  assert.equal(parsed.kind, "figma-plugin-export");
  assert.equal(
    parsed.scene_json,
    "{\"format_version\":\"2026.05-figma-plugin-v1\"}",
  );
  assert.deepEqual(parsed.assets, [{
    content_hash: "abc123",
    mime: "image/png",
    base64: "AAEC/f7/",
  }]);
});

test("faithful semantic children remain canonical DesignIR envelope nodes on export", () => {
  const preservedLeaf = {
    type: "text",
    name: "Semantic label",
    content: "Cutoff",
    style: { color: "#ffffff" },
    layout: {},
  };
  const exported = serializeExport([{
    type: "vector",
    name: "Faithful control",
    figma_node_id: "1:2",
    parent_id: null,
    z_order: 0,
    absolute_bounds: { x: 0, y: 0, w: 100, h: 100 },
    relative_transform: [[1, 0, 0], [0, 1, 0]],
    visible: true,
    locked: false,
    opacity: 1,
    blend_mode: "PASS_THROUGH",
    style: {},
    layout: {},
    render_mode: "faithful_svg",
    svg_asset_id: "faithful-svg",
    preserved_semantic_children: [preservedLeaf],
    children: [],
  }], [], {
    fileKey: "test",
    rootNodeId: "1:2",
    pluginVersion: "test",
    assets: { entries: () => [] } as never,
    tokens: { colors: {}, dimensions: {}, strings: {}, variableIdToName: new Map() },
  });

  assert.deepEqual(
    (exported as { root: { children: unknown[] } }).root.children,
    [{
      ...preservedLeaf,
      figma_node_id: "1:2",
    }],
  );
});

test("oversized faithful semantic metadata is rejected during pure decode", () => {
  const root = {
    type: "vector",
    name: "Faithful control",
    render_mode: "faithful_svg",
    svg_asset_id: "faithful-svg",
    style: { width: 100, height: 100 },
    layout: {},
    children: [{
      type: "text",
      name: "x".repeat(FIGMA_PLUGIN_DATA_VALUE_MAX_BYTES),
      content: "label",
      style: {},
      layout: {},
      children: [],
    }],
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify({
        version: 1,
        assetManifest: {
          version: 1,
          assets: [{
            asset_id: "faithful-svg",
            content_hash: "915087052b0dd3ee",
            mime: "image/svg+xml",
          }],
        },
        root,
      }),
      assets: [{
        content_hash: "915087052b0dd3ee",
        mime: "image/svg+xml",
        base64: "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciPjwvc3ZnPg==",
      }],
    })),
    /pulp\.faithful_semantic_children.*100000-byte total entry limit.*before mutation/,
  );
});

test("all variable-size plugin metadata is preflighted before mutation", () => {
  const huge = "x".repeat(FIGMA_PLUGIN_DATA_VALUE_MAX_BYTES);
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "frame",
      style: {},
      layout: {},
      attributes: { note: huge },
      children: [],
    })),
    /pulp\.design_ir_attributes.*100000-byte total entry limit.*before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "frame",
      style: {},
      layout: {},
      interactive_elements: [{
        kind: "custom",
        x: 0,
        y: 0,
        w: 10,
        h: 10,
        factory_id: huge,
      }],
      children: [],
    })),
    /pulp\.interactive_elements.*100000-byte total entry limit.*before mutation/,
  );
});

test("faithful semantic children with manifest dependencies fail before mutation", () => {
  const root = {
    type: "vector",
    name: "Faithful control",
    render_mode: "faithful_svg",
    svg_asset_id: "faithful-svg",
    style: { width: 100, height: 100 },
    layout: {},
    children: [{
      type: "image",
      name: "Nested asset",
      attributes: { srcAssetId: "faithful-svg" },
      style: { width: 20, height: 20 },
      layout: {},
      children: [],
    }],
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify({
        version: 1,
        assetManifest: {
          version: 1,
          assets: [{
            asset_id: "faithful-svg",
            content_hash: "915087052b0dd3ee",
            mime: "image/svg+xml",
          }],
        },
        root,
      }),
      assets: [{
        content_hash: "915087052b0dd3ee",
        mime: "image/svg+xml",
        base64: "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciPjwvc3ZnPg==",
      }],
    })),
    /faithful semantic subtrees with asset references.*import refused before mutation/,
  );
});

test("faithful semantic children bypass editable-Figma style lowering", () => {
  const plan = decodeForgeDesignForFigma(faithfulSvgEnvelope({
    type: "vector",
    name: "Faithful control",
    render_mode: "faithful_svg",
    svg_asset_id: "faithful-svg",
    style: { width: 100, height: 100 },
    layout: {},
    children: [{
      type: "frame",
      name: "Rotated semantic child",
      style: { transform: "rotate(45deg)" },
      layout: {},
      children: [],
    }],
  }));
  assert.deepEqual(plan.root.children, []);
  assert.equal(
    (plan.root.preservedChildren?.[0] as { style: { transform: string } })
      .style.transform,
    "rotate(45deg)",
  );
});

test("Forge DesignIR becomes an editable Figma plan with hierarchy and bindings intact", () => {
  const designIr = JSON.stringify({
    version: 1,
    root: {
      type: "frame",
      name: "Synth",
      style: { width: 640, height: 360, backgroundColor: "#101820" },
      layout: { display: "flex", direction: "row", gap: 16, paddingLeft: 20 },
      source_node_id: "1:1",
      children: [{
        type: "knob",
        name: "Cutoff",
        audioWidget: "knob",
        label: "Cutoff",
        min: 20,
        max: 20000,
        default: 880,
        style: { width: 72, height: 72, left: 24, top: 40 },
        layout: {},
        attributes: {
          binding: "filter.cutoff",
          pulpParamKey: "filter.cutoff",
          units: "Hz",
        },
        stable_anchor_id: "figma-plugin:2:4",
      }, {
        type: "text",
        name: "Heading",
        content: "FILTER",
        style: { width: 120, height: 24, color: "#ffffff", fontSize: 14 },
        layout: {},
      }],
    },
  });
  const plan = decodeForgeDesignForFigma(JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: designIr,
  }));

  assert.equal(plan.nodeCount, 3);
  assert.equal(plan.audioWidgetCount, 1);
  assert.equal(plan.root.name, "Synth");
  assert.equal(plan.root.sourceType, "frame");
  assert.equal(plan.root.layoutMode, "HORIZONTAL");
  assert.equal(plan.root.children[0].audioWidget, "knob");
  assert.equal(plan.root.children[0].binding, "filter.cutoff");
  assert.equal(plan.root.children[0].audioLabel, "Cutoff");
  assert.equal(plan.root.children[0].audioMin, 20);
  assert.equal(plan.root.children[0].audioMax, 20000);
  assert.equal(plan.root.children[0].audioDefault, 880);
  assert.equal(plan.root.children[0].audioUnits, "Hz");
  assert.equal(plan.root.children[0].stableAnchorId, "figma-plugin:2:4");
  assert.deepEqual(plan.root.children[0].attributes, {
    binding: "filter.cutoff",
    pulpParamKey: "filter.cutoff",
    units: "Hz",
  });
  assert.equal(plan.root.children[1].content, "FILTER");
});

test("Forge decode does not depend on the Figma-missing TextEncoder global", () => {
  const globals = globalThis as unknown as { TextEncoder?: typeof TextEncoder };
  const original = globals.TextEncoder;
  try {
    globals.TextEncoder = undefined;
    const plan = decodeForgeDesignForFigma(forgeEnvelope({
      type: "frame",
      name: "Unicode \u{1f3b9}",
      style: { width: 100, height: 100 },
      layout: {},
      children: [],
    }));
    assert.equal(plan.root.name, "Unicode \u{1f3b9}");
  } finally {
    globals.TextEncoder = original;
  }
});

test("Forge clipboard rejects ordinary JSON and unsupported widget claims", () => {
  assert.throws(
    () => decodeForgeDesignForFigma("{\"root\":{}}"),
    /does not contain/,
  );
  const designIr = JSON.stringify({
    root: {
      type: "frame",
      style: {},
      layout: {},
      audioWidget: "secret_widget",
    },
  });
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: designIr,
    })),
    /Unsupported audio widget/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "frame",
      style: {},
      layout: {},
      children: {},
    })),
    /children: expected an array/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "frame",
      style: {},
      layout: {},
      children: [null],
    })),
    /children\[0\]: expected a DesignIR node object/,
  );
  const optOut = decodeForgeDesignForFigma(forgeEnvelope({
    type: "frame",
    name: "Window (mask scope)",
    audio_widget: "none",
    style: { width: 100, height: 100 },
    layout: {},
    children: [],
  }));
  assert.equal(optOut.root.audioWidget, "none");
  assert.equal(optOut.audioWidgetCount, 0);
  const omittedWidget = decodeForgeDesignForFigma(forgeEnvelope({
    type: "frame",
    name: "Knob base",
    style: { width: 100, height: 100 },
    layout: {},
    children: [],
  }));
  assert.equal(omittedWidget.root.audioWidget, "none");
  assert.equal(omittedWidget.audioWidgetCount, 0);
});

function forgeEnvelope(
  root: Record<string, unknown>,
  extra: Record<string, unknown> = {},
): string {
  return JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: JSON.stringify({ version: 1, root, ...extra }),
  });
}

test("Forge plan preserves Inspector-editable effects, text, blend, layout, and constraints", () => {
  const plan = decodeForgeDesignForFigma(forgeEnvelope({
    type: "frame",
    name: "Rich panel",
    style: {
      width: 480,
      height: 240,
      backgroundColor: "rgba(16, 24, 32, 0.9)",
      backgroundGradient: "linear-gradient(90deg, #ff0000 40%, #00ff00, rgba(0, 0, 255, 0.5) 100%)",
      opacity: 0.75,
      mixBlendMode: "multiply",
      borderColor: "#abcdef",
      borderWidth: 2,
      borderStyle: "dashed",
      border: "2px dashed #abcdef",
      borderTopWidth: 3,
      borderTopLeftRadius: 12,
      boxShadow: "0 4px 12px 1px rgba(0,0,0,0.4), inset 0 1px 2px #ffffff80",
      filter: "blur(2px) blur(4px)",
      backdropFilter: "blur(6px)",
      overflow: "clip",
    },
    layout: {
      display: "flex",
      direction: "row",
      gap: 12,
      rowGap: 8,
      padding: { top: 10, right: 11, bottom: 12, left: 13 },
      justify: "space-between",
      align: "center",
      alignContent: "space-between",
      wrap: true,
      widthMode: "fixed",
      heightMode: "hug",
      constraints: { horizontal: "center", vertical: "bottom" },
    },
    children: [{
      type: "text",
      name: "Title",
      content: "LOUD title",
      style: {
        width: 160,
        height: 28,
        color: "#fefefe",
        fontFamily: "Inter",
        fontSize: 16,
        fontWeight: 700,
        fontStyle: "italic",
        textAlign: "center",
        verticalAlign: "middle",
        letterSpacing: 1.25,
        lineHeight: 20,
        textTransform: "uppercase",
        textDecoration: "underline",
      },
      layout: { constraints: { horizontal: "scale", vertical: "top" } },
      runs: [{
        start: 0,
        end: 4,
        fontSize: 18,
        fontWeight: 600,
        color: "#00ff00",
        letterSpacing: 2,
        textDecoration: "line-through",
      }, {
        start: 5,
        end: 10,
        color: "#ff00ff",
      }],
      children: [],
    }],
    interactive_elements: [{
      kind: "swap",
      x: 10,
      y: 20,
      w: 100,
      h: 40,
      target_frame: 1,
      source_node_id: "77:12",
      label: "Next state",
    }],
    attributes: { "figma:dash_pattern": "6,3" },
  }));

  assert.deepEqual(plan.root.backgroundColor, {
    r: 16 / 255, g: 24 / 255, b: 32 / 255, a: 0.9,
  });
  assert.equal(plan.root.backgroundGradient?.kind, "linear");
  assert.equal(plan.root.backgroundGradient?.angleDegrees, 90);
  assert.equal(
    plan.root.backgroundGradient?.css,
    "linear-gradient(90deg, #ff0000 40%, #00ff00, rgba(0, 0, 255, 0.5) 100%)",
  );
  assert.equal(plan.root.backgroundGradient?.stops[1].position, 0.7);
  assert.equal(plan.root.backgroundGradient?.stops[2].color.a, 0.5);
  assert.equal(plan.root.opacity, 0.75);
  assert.equal(plan.root.clipsContent, true);
  assert.equal(plan.root.blendMode, "MULTIPLY");
  const normalBlend = decodeForgeDesignForFigma(forgeEnvelope({
    type: "frame",
    style: { mixBlendMode: "normal" },
    layout: {},
    children: [],
  }));
  assert.equal(normalBlend.root.blendMode, undefined);
  assert.deepEqual(plan.root.effects.map((effect) => effect.kind), [
    "drop-shadow", "inner-shadow", "layer-blur", "layer-blur", "background-blur",
  ]);
  assert.equal(plan.root.effects[0].radius, 12);
  assert.deepEqual(plan.root.dashPattern, [6, 3]);
  assert.equal(plan.root.layoutMode, "HORIZONTAL");
  assert.equal(plan.root.layoutWrap, "WRAP");
  assert.equal(plan.root.itemSpacing, 12);
  assert.equal(plan.root.counterAxisSpacing, 8);
  assert.equal(plan.root.primaryAxisAlignItems, "SPACE_BETWEEN");
  assert.equal(plan.root.counterAxisAlignItems, "CENTER");
  assert.equal(plan.root.counterAxisStretch, false);
  assert.equal(plan.root.counterAxisAlignContent, "SPACE_BETWEEN");
  assert.equal(plan.root.paddingTop, 10);
  assert.equal(plan.root.paddingRight, 11);
  assert.equal(plan.root.paddingBottom, 12);
  assert.equal(plan.root.paddingLeft, 13);
  assert.deepEqual(plan.root.constraints, { horizontal: "CENTER", vertical: "MAX" });

  const text = plan.root.children[0];
  assert.equal(text.fontFamily, "Inter");
  assert.equal(text.fontStyle, "Bold Italic");
  assert.equal(text.textAlignHorizontal, "CENTER");
  assert.equal(text.textAlignVertical, "CENTER");
  assert.equal(text.textCase, "UPPER");
  assert.equal(text.textDecoration, "UNDERLINE");
  assert.equal(text.textRuns[0].fontStyle, "Semi Bold Italic");
  assert.equal(text.textRuns[0].textDecoration, "STRIKETHROUGH");
  assert.equal(text.textRuns[1].fontStyle, undefined);
  assert.deepEqual(text.constraints, { horizontal: "SCALE", vertical: "MIN" });
  assert.deepEqual(plan.root.interactiveElements, [{
    kind: "swap",
    x: 10,
    y: 20,
    w: 100,
    h: 40,
    target_frame: 1,
    source_node_id: "77:12",
    label: "Next state",
  }]);

  const originOverlay = decodeForgeDesignForFigma(forgeEnvelope({
    type: "frame",
    name: "Origin overlay",
    style: {},
    layout: {},
    interactive_elements: [{ kind: "action", w: 20, h: 10, action: "reset" }],
    children: [],
  }));
  assert.deepEqual(originOverlay.root.interactiveElements, [{
    kind: "action", x: 0, y: 0, w: 20, h: 10, action: "reset",
  }]);
});

test("Forge plan hydrates content-addressed raster and SVG assets", () => {
  const imageBytes = [104, 101, 108, 108, 111];
  const imageHash = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
  const svg = '<svg xmlns="http://www.w3.org/2000/svg"></svg>';
  const svgBytes = Array.from(new TextEncoder().encode(svg));
  const svgHash = "8d9b4b794affc5daf4eafa12e0c6294ab31aaeed330886145676bd6b832e8b98";
  const design = {
    version: 1,
    assetManifest: {
      version: 1,
      assets: [{
        asset_id: "img-a",
        content_hash: imageHash,
        mime: "image/png",
      }, {
        asset_id: "svg-b",
        content_hash: svgHash,
        mime: "image/svg+xml",
      }],
    },
    root: {
      type: "frame",
      name: "Primary",
      style: { width: 300, height: 200 },
      layout: {},
      children: [{
        type: "image",
        name: "Photo",
        asset_ref: "img-a",
        style: { width: 80, height: 60, objectFit: "contain" },
        layout: {},
        children: [],
      }],
    },
  };
  const plan = decodeForgeDesignForFigma(JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: JSON.stringify(design),
    assets: [{
      content_hash: imageHash,
      mime: "image/png",
      base64: "aGVsbG8=",
    }, {
      content_hash: svgHash,
      mime: "image/svg+xml",
      base64: btoa(svg),
    }],
  }));

  assert.equal(plan.assetCount, 2);
  assert.equal(plan.nodeCount, 2);
  assert.equal(plan.root.children[0].kind, "image");
  assert.equal(plan.root.children[0].imageScaleMode, "FIT");
  assert.deepEqual(plan.root.children[0].asset?.bytes, imageBytes);
  assert.equal(plan.root.alternateFrames.length, 0);

  const faithfulStates = {
    ...design,
    root: {
      type: "frame",
      name: "Primary state",
      render_mode: "faithful_svg",
      svg_asset_id: "svg-b",
      style: { width: 300, height: 200, overflow: "clip" },
      layout: {},
      children: [],
      alternate_frames: [{
        type: "frame",
        name: "Alternate state",
        render_mode: "faithful_svg",
        svg_asset_id: "svg-b",
        style: { width: 300, height: 200, overflow: "clip" },
        layout: {},
        children: [],
      }],
    },
  };
  const statesPlan = decodeForgeDesignForFigma(JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: JSON.stringify(faithfulStates),
    assets: [{
      content_hash: imageHash,
      mime: "image/png",
      base64: "aGVsbG8=",
    }, {
      content_hash: svgHash,
      mime: "image/svg+xml",
      base64: btoa(svg),
    }],
  }));
  assert.equal(statesPlan.root.alternateFrames.length, 1);
  assert.equal(statesPlan.root.alternateFrames[0].svgVisualMode, "faithful");
  assert.deepEqual(statesPlan.root.interactiveElements, []);
  assert.deepEqual(statesPlan.root.alternateFrames[0].interactiveElements, []);
  assert.deepEqual(statesPlan.root.alternateFrames[0].asset?.bytes, svgBytes);

  const srcAssetDesign = {
    ...design,
    root: {
      ...design.root,
      children: design.root.children.map((child) => {
        const { asset_ref: _assetRef, ...withoutRootRef } = child;
        return {
          ...withoutRootRef,
          attributes: { srcAssetId: "img-a" },
        };
      }),
    },
  };
  const srcAssetPlan = decodeForgeDesignForFigma(JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: JSON.stringify(srcAssetDesign),
    assets: [{
      content_hash: imageHash,
      mime: "image/png",
      base64: "aGVsbG8=",
    }, {
      content_hash: svgHash,
      mime: "image/svg+xml",
      base64: btoa(svg),
    }],
  }));
  assert.equal(srcAssetPlan.root.children[0].asset?.assetId, "img-a");
  assert.equal(srcAssetPlan.root.children[0].attributes.srcAssetId, undefined);

  const foregroundAndBackgroundDesign = {
    ...design,
    root: {
      ...design.root,
      asset_ref: "svg-b",
      style: {
        ...design.root.style,
        backgroundImage: "img-a",
      },
    },
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify(foregroundAndBackgroundDesign),
      assets: [{
        content_hash: imageHash,
        mime: "image/png",
        base64: "aGVsbG8=",
      }, {
        content_hash: svgHash,
        mime: "image/svg+xml",
        base64: btoa(svg),
      }],
    })),
    /independent node and background image assets cannot be round-tripped.*import refused before mutation/,
  );
  const fallbackAndBackgroundDesign = {
    ...design,
    root: {
      type: "image",
      name: "Ambiguous fallback image",
      attributes: { hoverAssetId: "svg-b" },
      style: {
        width: 80,
        height: 60,
        backgroundImage: "img-a",
        backgroundSize: "contain",
      },
      layout: {},
      children: [],
    },
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify(fallbackAndBackgroundDesign),
      assets: [{
        content_hash: imageHash,
        mime: "image/png",
        base64: "aGVsbG8=",
      }, {
        content_hash: svgHash,
        mime: "image/svg+xml",
        base64: btoa(svg),
      }],
    })),
    /independent node and background image assets cannot be round-tripped.*import refused before mutation/,
  );

  const duplicateIdDesign = {
    ...design,
    assetManifest: {
      ...design.assetManifest,
      assets: design.assetManifest.assets.map((entry, index) =>
        index === 1 ? { ...entry, asset_id: "img-a" } : entry),
    },
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify(duplicateIdDesign),
      assets: [{
        content_hash: imageHash,
        mime: "image/png",
        base64: "aGVsbG8=",
      }, {
        content_hash: svgHash,
        mime: "image/svg+xml",
        base64: btoa(svg),
      }],
    })),
    /duplicate asset ID "img-a"/,
  );

  const webpDesign = {
    ...design,
    assetManifest: {
      ...design.assetManifest,
      assets: design.assetManifest.assets.map((entry, index) =>
        index === 0 ? { ...entry, mime: "image/webp" } : entry),
    },
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify(webpDesign),
      assets: [{
        content_hash: imageHash,
        mime: "image/webp",
        base64: "aGVsbG8=",
      }, {
        content_hash: svgHash,
        mime: "image/svg+xml",
        base64: btoa(svg),
      }],
    })),
    /createImage supports PNG, JPEG, and GIF; got "image\/webp"/,
  );

  const svgBackgroundDesign = {
    ...design,
    root: {
      ...design.root,
      asset_ref: "svg-b",
    },
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify(svgBackgroundDesign),
      assets: [{
        content_hash: imageHash,
        mime: "image/png",
        base64: "aGVsbG8=",
      }, {
        content_hash: svgHash,
        mime: "image/svg+xml",
        base64: btoa(svg),
      }],
    })),
    /SVG backgrounds require a faithful wrapper.*import refused before mutation/,
  );

  const svgImageBackground = {
    ...design,
    root: {
      type: "image",
      name: "SVG with backing color",
      asset_ref: "svg-b",
      style: { width: 100, height: 100, backgroundColor: "#ff0000" },
      layout: {},
      children: [],
    },
  };
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify(svgImageBackground),
      assets: [{
        content_hash: imageHash,
        mime: "image/png",
        base64: "aGVsbG8=",
      }, {
        content_hash: svgHash,
        mime: "image/svg+xml",
        base64: btoa(svg),
      }],
    })),
    /backgrounds behind SVG assets require a wrapper frame.*import refused before mutation/,
  );
});

test("Forge copy-back rejects bundled font bytes it cannot persist durably", () => {
  const hash = "c0e3780188f9cddc4399161492b7d066d995aba8f07722eb56fdbb2985536711";
  assert.throws(() => decodeForgeDesignForFigma(JSON.stringify({
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "design-ir",
    design_ir_json: JSON.stringify({
      version: 1,
      assetManifest: {
        version: 1,
        assets: [{
          asset_id: "font-a",
          content_hash: hash,
          mime: "font/ttf",
        }],
      },
      fontFamilyAssets: [{
        family: "Bundled Face",
        style: "normal",
        weight: 400,
        asset_id: "font-a",
      }],
      root: {
        type: "text",
        name: "Bundled text",
        content: "hello",
        style: { fontFamily: "Bundled Face", fontWeight: 400 },
        layout: {},
        children: [],
      },
    }),
    assets: [{
      content_hash: hash,
      mime: "font/ttf",
      base64: "AAEAAAABAAAAAAAA",
    }],
  })), /bundled font bytes cannot be persisted durably.*refused before mutation/);
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "text",
      content: "bad font",
      style: { fontFamily: "Bundled Face", fontWeight: 400 },
      layout: {},
      children: [],
    }, {
      assetManifest: {
        version: 1,
        assets: [{
          asset_id: "bad-font",
          content_hash: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
          mime: "font/ttf",
        }],
      },
      fontFamilyAssets: [{
        family: "Bundled Face",
        style: "Regular",
        weight: 400,
        asset_id: "bad-font",
      }],
    }).replace(
      /}$/,
      ',"assets":[{"content_hash":"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824","mime":"font/ttf","base64":"aGVsbG8="}]}',
    )),
    /does not match a supported SFNT\/WOFF font signature/,
  );
});

test("Forge copy-back fails closed before mutation for unrepresentable or unverifiable state", () => {
  const base = {
    type: "frame",
    name: "Root",
    style: { width: 100, height: 100 },
    layout: {},
    children: [],
  };
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: { ...base.style, transform: "skewX(20deg)" },
    })),
    /style\.transform.*cannot be represented/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: { ...base.style, filter: "grayscale(1)" },
    })),
    /only blur\(Npx\)/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: {
        ...base.style,
        backgroundGradient: "radial-gradient(circle, #000000, #ffffff)",
      },
    })),
    /radial\/conic CSS geometry.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: {
        ...base.style,
        backgroundGradient: "linear-gradient(90deg, red -50%, blue 100%)",
      },
    })),
    /positions outside 0%-100%.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      interactive_elements: [{ kind: "unknown" }],
    })),
    /unsupported interactive control/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      interactive_elements: [{ kind: "swap" }],
    })),
    /interactive_elements\[0\]\.w: required for swap/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      interactive_elements: [{
        kind: "knob", cx: "bad", cy: 10, hit_radius: 5,
      }],
    })),
    /interactive_elements\[0\]\.cx: expected a finite number/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      interactive_elements: [{
        kind: "action", x: 0, y: 0, w: 10, h: 10, surprise: true,
      }],
    })),
    /interactive_elements\[0\]\.surprise: unsupported interactive field/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: { ...base.style, borderWidth: 1 },
    })),
    /visible stroke width requires a color/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "text",
      name: "Leaf",
      content: "text",
      style: {},
      layout: {},
      children: [base],
    })),
    /Figma leaf.*refusing to flatten/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "text",
      name: "Badge",
      content: "NEW",
      style: { backgroundColor: "#ff0000" },
      layout: {},
      children: [],
    })),
    /text backgrounds require a wrapper frame.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "text",
      name: "Outlined label",
      content: "LOUD",
      style: { borderColor: "#ffffff", borderWidth: 1 },
      layout: {},
      children: [],
    })),
    /text box borders require a wrapper frame.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: { ...base.style, backgroundSize: "auto" },
    })),
    /backgroundSize: "auto".*no exact Figma scaleMode/,
  );
  const emptyBackground = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    style: { ...base.style, backgroundImage: "none" },
  }));
  assert.equal(emptyBackground.root.asset, undefined);
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      layout: { flexGrow: 2 },
    })),
    /flexGrow: Figma supports only 0 or 1.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "text",
      name: "Spread label",
      content: "shadow",
      style: { boxShadow: "0 2px 4px 1px #00000080" },
      layout: {},
      children: [],
    })),
    /nonzero spread is unsupported.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      attributes: { role: 42 },
    })),
    /attributes\.role: expected a string/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify({
        assetManifest: {
          assets: [{
            asset_id: "bad",
            content_hash: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
            mime: "image/png",
          }],
        },
        root: {
          type: "image", name: "Bad", asset_ref: "bad",
          style: {}, layout: {}, children: [],
        },
      }),
      assets: [{
        content_hash: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
        mime: "image/png",
        base64: "d3Jvbmc=",
      }],
    })),
    /content_hash does not match/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      children: [{
        ...base,
        alternate_frames: [{ ...base, name: "Nested alternate" }],
      }],
    })),
    /nested alternate-frame sets/,
  );

  const sideStroke = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    style: {
      ...base.style,
      borderColor: "#112233",
      borderTopWidth: 3,
    },
  }));
  assert.equal(sideStroke.root.borderWidth, undefined);
  assert.equal(sideStroke.root.borderTopWidth, 3);
  assert.deepEqual(sideStroke.root.borderColor, {
    r: 0x11 / 255,
    g: 0x22 / 255,
    b: 0x33 / 255,
    a: 1,
  });
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: {
        ...base.style,
        borderWidth: 2,
        borderColor: "#000000",
        borderTopColor: "#ff0000",
      },
    })),
    /cannot represent different colors per side.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      type: "ellipse",
      style: {
        ...base.style,
        borderColor: "#000000",
        borderTopWidth: 2,
      },
    })),
    /per-side strokes cannot be represented on ellipse nodes/,
  );

  const derivedBounds = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    style: {
      ...base.style,
      box_shadow: "0 4px 8px #00000080",
      render_bounds: { w: 116, h: 120, dx: -8, dy: -4 },
    },
  }));
  assert.equal(derivedBounds.root.effects[0].kind, "drop-shadow");
  const colorFirstShadow = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    style: {
      ...base.style,
      box_shadow: "#00000080 0 4px 8px",
    },
  }));
  assert.equal(colorFirstShadow.root.effects[0].kind, "drop-shadow");

  const stretched = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    layout: { display: "flex", direction: "row", align: "stretch" },
  }));
  assert.equal(stretched.root.counterAxisStretch, true);
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      layout: { display: "flex", justify: "stretch" },
    })),
    /alignment "stretch" cannot be represented on Figma's primary axis/,
  );

  const defaultFlexDirection = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    layout: { display: "flex" },
  }));
  assert.equal(defaultFlexDirection.root.layoutMode, "HORIZONTAL");

  const scalarPadding = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    layout: { display: "flex", padding: 16 },
  }));
  assert.equal(scalarPadding.root.paddingTop, 16);
  assert.equal(scalarPadding.root.paddingRight, 16);
  assert.equal(scalarPadding.root.paddingBottom, 16);
  assert.equal(scalarPadding.root.paddingLeft, 16);

  const inertAlignContent = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    layout: {
      display: "flex",
      direction: "row",
      alignContent: "space-between",
    },
  }));
  assert.equal(inertAlignContent.root.counterAxisAlignContent, undefined);

  for (const [type, expectedLayoutAlign] of [
    ["panel", "MIN"],
    ["row", "CENTER"],
    ["col", "MAX"],
  ] as const) {
    const alias = decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      type,
      layout: {
        display: "flex",
        direction: type === "row" ? "row" : "column",
        alignSelf: type === "panel" ? "flex-start"
          : type === "row" ? "center" : "flex-end",
      },
    }));
    assert.equal(alias.root.kind, "frame");
    assert.equal(alias.root.layoutAlign, expectedLayoutAlign);
  }

  const ordinary = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    layout: {
      direction: "column",
      widthMode: "fixed",
      heightMode: "fixed",
      constraints: { horizontal: "MIN", vertical: "MAX" },
    },
    attributes: {
      "pulp:roundtrip_source_node_id": "42:7",
      "pulp:roundtrip_stable_anchor_id": "figma-plugin:42:7",
    },
  }));
  assert.equal(ordinary.root.layoutMode, undefined);
  assert.equal(ordinary.root.counterAxisStretch, false);
  assert.deepEqual(ordinary.root.constraints, { horizontal: "MIN", vertical: "MAX" });
  assert.equal(ordinary.root.sourceNodeId, "42:7");
  assert.equal(ordinary.root.stableAnchorId, "figma-plugin:42:7");

  const weights = decodeForgeDesignForFigma(forgeEnvelope({
    type: "text",
    name: "Weights",
    content: "thin black",
    style: { fontFamily: "Inter", fontWeight: 900 },
    layout: {},
    runs: [{ start: 0, end: 4, fontWeight: 100 }],
    children: [],
  }, {
    fontFamilyAssets: [
      { family: "Inter", style: "normal", weight: 900 },
      { family: "Inter", style: "normal", weight: 100 },
    ],
  }));
  assert.equal(weights.root.fontStyle, "Black");
  assert.equal(weights.root.textRuns[0].fontStyle, "Thin");

  const defaultFace = decodeForgeDesignForFigma(forgeEnvelope({
    type: "text",
    name: "Default face",
    content: "regular",
    style: { fontFamily: "Inter" },
    layout: {},
    children: [],
  }, {
    fontFamilyAssets: [
      { family: "Inter", style: "normal", weight: 700 },
      { family: "Inter", style: "normal", weight: 400 },
    ],
  }));
  assert.equal(defaultFace.root.fontStyle, "Regular");

  const normalBeforeItalic = decodeForgeDesignForFigma(forgeEnvelope({
    type: "text",
    name: "Normal face",
    content: "plain",
    style: { fontFamily: "Inter", fontWeight: 400 },
    layout: {},
    children: [],
  }, {
    fontFamilyAssets: [
      { family: "Inter", style: "Italic", weight: 400, italic: true },
      { family: "Inter", style: "Regular", weight: 400 },
    ],
  }));
  assert.equal(normalBeforeItalic.root.fontStyle, "Regular");

  const omittedSemanticWeight = decodeForgeDesignForFigma(forgeEnvelope({
    type: "text",
    name: "Semantic normal",
    content: "plain",
    style: { fontFamily: "Inter", fontStyle: "normal" },
    layout: {},
    children: [],
  }, {
    fontFamilyAssets: [
      { family: "Inter", style: "Bold", weight: 700 },
      { family: "Inter", style: "Regular", weight: 400 },
    ],
  }));
  assert.equal(omittedSemanticWeight.root.fontStyle, "Regular");

  const catalogNamedDefaultFace = decodeForgeDesignForFigma(forgeEnvelope({
    type: "text",
    name: "Named default face",
    content: "plain",
    style: { fontFamily: "Editorial" },
    layout: {},
    children: [],
  }, {
    fontFamilyAssets: [
      { family: "Editorial", style: "Book", weight: 400 },
      { family: "Editorial", style: "Bold", weight: 700 },
    ],
  }));
  assert.equal(catalogNamedDefaultFace.root.fontStyle, "Book");

  const omittedCatalogStyle = decodeForgeDesignForFigma(forgeEnvelope({
    type: "text",
    name: "Catalog default",
    content: "bold",
    style: { fontFamily: "Inter", fontWeight: 700 },
    layout: {},
    children: [],
  }, {
    fontFamilyAssets: [{ family: "Inter", weight: 700 }],
  }));
  assert.equal(omittedCatalogStyle.root.fontStyle, "Bold");

  const namedCatalogStyle = decodeForgeDesignForFigma(forgeEnvelope({
    type: "text",
    name: "Named face",
    content: "specific",
    style: {
      fontFamily: "Inter",
      fontWeight: 600,
      fontStyle: "Semi Bold",
    },
    layout: {},
    children: [],
  }, {
    fontFamilyAssets: [
      { family: "Inter", style: "Regular", weight: 600 },
      { family: "Inter", style: "Semi Bold", weight: 600 },
    ],
  }));
  assert.equal(namedCatalogStyle.root.fontStyle, "Semi Bold");
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "frame",
      style: {},
      layout: {},
      children: [{
        type: "text",
        content: "ambiguous",
        style: { fontFamily: "Inter", fontWeight: 400, fontStyle: "normal" },
        layout: {},
        children: [],
      }],
    }, {
      fontFamilyAssets: [
        { family: "Inter", style: "Regular", weight: 400 },
        { family: "Inter", style: "Condensed", weight: 400 },
      ],
    })),
    /font catalog faces "Regular" and "Condensed".*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      type: "label",
      content: "ambiguous label",
      style: { fontFamily: "Inter", fontWeight: 400, fontStyle: "normal" },
      layout: {},
      children: [],
    }, {
      fontFamilyAssets: [
        { family: "Inter", style: "Regular", weight: 400 },
        { family: "Inter", style: "Condensed", weight: 400 },
      ],
    })),
    /font catalog faces "Regular" and "Condensed".*import refused before mutation/,
  );

  const explicitGaps = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    layout: {
      display: "flex",
      direction: "row",
      columnGap: 18,
      rowGap: 7,
      wrap: true,
    },
  }));
  assert.equal(explicitGaps.root.layoutMode, "HORIZONTAL");
  assert.equal(explicitGaps.root.itemSpacing, 18);
  assert.equal(explicitGaps.root.counterAxisSpacing, 7);

  const semanticButton = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    type: "button",
    attributes: { role: "button", onclick: "toggleBypass()" },
  }));
  assert.equal(semanticButton.root.sourceType, "button");
  assert.deepEqual(semanticButton.root.attributes, {
    role: "button",
    onclick: "toggleBypass()",
  });

  const unrelatedAssetMetadata = decodeForgeDesignForFigma(forgeEnvelope({
    ...base,
    attributes: { hoverAssetId: "decorative-hover-state" },
  }));
  assert.equal(unrelatedAssetMetadata.root.asset, undefined);

  const inlineVector = decodeForgeDesignForFigma(forgeEnvelope({
    type: "vector",
    name: "Triangle",
    style: {
      width: 20,
      height: 20,
      backgroundColor: "#ff0000",
      borderColor: "#0000ff",
      borderWidth: 2,
      borderStyle: "solid",
    },
    layout: {},
    attributes: { path_data: "M0 0L20 0L10 20Z" },
    children: [],
  }));
  const inlineSvg = new TextDecoder().decode(
    new Uint8Array(inlineVector.root.asset?.bytes ?? []),
  );
  assert.match(inlineSvg, /fill="rgba\(255,0,0,1\)"/);
  assert.match(inlineSvg, /stroke="rgba\(0,0,255,1\)"/);
  assert.equal(inlineVector.root.svgVisualMode, "inline");
  assert.equal(inlineVector.root.borderWidth, undefined);

  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: { ...base.style, objectFit: "scale-down" },
    })),
    /objectFit.*no exact Figma image scaleMode equivalent/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: { ...base.style, position: "absolute", right: 0 },
    })),
    /style\.right: this canonical DesignIR property cannot be represented/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify({
        version: 1,
        assetManifest: {
          version: 1,
          assets: [{
            asset_id: "img-a",
            content_hash: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
            mime: "image/png",
          }],
        },
        root: {
          type: "image",
          name: "Default-stretch image",
          asset_ref: "img-a",
          style: { width: 80, height: 60 },
          layout: {},
          children: [],
        },
      }),
      assets: [{
        content_hash: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
        mime: "image/png",
        base64: "aGVsbG8=",
      }],
    })),
    /omitted image\/background sizing has no exact Figma image scaleMode.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(JSON.stringify({
      format: FORGE_CLIPBOARD_FORMAT,
      kind: "design-ir",
      design_ir_json: JSON.stringify({
        version: 1,
        assetManifest: {
          version: 1,
          assets: [{
            asset_id: "img-a",
            content_hash: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
            mime: "image/png",
          }],
        },
        root: {
          type: "frame",
          name: "Unsized non-repeating background",
          style: {
            width: 80,
            height: 60,
            backgroundImage: "img-a",
            backgroundRepeat: "no-repeat",
          },
          layout: {},
          children: [],
        },
      }),
      assets: [{
        content_hash: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
        mime: "image/png",
        base64: "aGVsbG8=",
      }],
    })),
    /omitted image\/background sizing has no exact Figma image scaleMode.*import refused before mutation/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: {
        ...base.style,
        objectFit: "contain",
        backgroundRepeat: "repeat",
      },
    })),
    /tiled images cannot also request cover\/contain/,
  );
  assert.throws(
    () => decodeForgeDesignForFigma(forgeEnvelope({
      ...base,
      style: {
        ...base.style,
        position: "relative",
        left: 4,
      },
    })),
    /relative offsets have no truthful Figma auto-layout equivalent/,
  );
});
