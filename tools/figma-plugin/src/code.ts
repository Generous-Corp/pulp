// Pulp Figma Plugin — sandbox main code (runs in Figma's JS sandbox).

import { extractScene } from "./extract";
import { serializeExport, type LibraryManifestSnapshot } from "./serialize";
import type { FontFamilyAssetSummary, PulpFigmaUIMessage, PulpSandboxMessage } from "./types";
import { UserFontCache } from "./user-fonts";
import { sha256Hex } from "./assets";
import {
  decodeForgeDesignForFigma,
  gradientTransformForFigma,
  type ForgeFigmaPlan,
  type ForgeFigmaPlanNode,
  type ForgePlanColor,
  type ForgePlanGradient,
} from "./forge-roundtrip";

const PLUGIN_VERSION = "0.2.0";

const LIBRARY_MANIFEST: LibraryManifestSnapshot = {
  library_version: "0.1.0",
  required_plugin_version: ">=0.1.0",
  widget_keys: {
    knob: "TBD-paste-after-library-publish",
  },
};

let knownFileKey: string | null = null;
const forgeImportSessionNonce =
  `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
let forgeImportTransactionCounter = 0;

/// Sandbox-side store of user-dropped TTF/OTF bytes, keyed by (family, style).
/// Survives across export attempts within a single plugin session so the user
/// can drop, preview, export, drop more, re-export. Discarded when the plugin
/// closes.
const userFonts = new UserFontCache();

figma.showUI(__html__, { width: 360, height: 540, themeColors: true });

figma.ui.onmessage = async (msg: PulpFigmaUIMessage) => {
  try {
    switch (msg.type) {
      case "ping":
        reply({
          type: "pong",
          figmaVersion: figma.apiVersion,
          editorType: figma.editorType,
          documentName: figma.root.name,
        });
        break;

      case "report-file-key":
        if (msg.fileKey) knownFileKey = msg.fileKey;
        break;

      case "get-selection-summary":
        reply({
          type: "selection-summary",
          count: figma.currentPage.selection.length,
          names: figma.currentPage.selection.map((n) => n.name).slice(0, 8),
          firstTypeIfAny: figma.currentPage.selection[0]?.type ?? null,
        });
        break;

      case "export":
        await handleExport("download");
        break;

      case "copy-selection-for-forge":
        await handleExport("clipboard");
        break;

      case "import-forge-design":
        await handleForgeImport(msg.clipboardText);
        break;

      case "scan-fonts":
        await handleScanFonts();
        break;

      case "user-font":
        await handleUserFont(msg);
        break;

      case "close":
        figma.closePlugin();
        break;

      default:
        reply({ type: "error", message: `Unknown UI message: ${(msg as { type: string }).type}` });
    }
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    reply({ type: "error", message });
    console.error("[pulp-figma-plugin]", err);
  }
};

async function handleExport(delivery: "download" | "clipboard"): Promise<void> {
  const sel = figma.currentPage.selection;
  if (sel.length === 0) {
    reply({ type: "error", message: "Select at least one frame in Figma before exporting." });
    return;
  }

  reply({ type: "progress", stage: "extracting", message: `Walking ${sel.length} root(s)…` });
  const result = await extractScene(sel);

  reply({
    type: "progress",
    stage: "serializing",
    message: `Walked ${result.nodeCount} nodes, ${result.assets.size()} asset(s); serializing…`,
  });

  // Resolve file key: published plugins get figma.fileKey; local dev installs
  // get null. The UI iframe parses window.location and posts back via
  // 'report-file-key'. Fall back to the document name encoded as a URN.
  const fileKey =
    figma.fileKey ??
    knownFileKey ??
    `local-${encodeURIComponent(figma.root.name).slice(0, 32)}`;
  const exportFonts = userFonts;

  const envelope = serializeExport(result.roots, result.diagnostics, {
    fileKey,
    rootNodeId: sel[0].id,
    pluginVersion: PLUGIN_VERSION,
    libraryManifest: LIBRARY_MANIFEST,
    assets: result.assets,
    tokens: result.tokens,
    fontFamilyAssets: result.font_family_assets,
    userFonts: exportFonts,
  });

  const json = JSON.stringify(envelope, null, 2);
  const suggestedName = `${sanitizeFilename(sel[0].name) || "pulp-export"}`;

  // Hand the assets to the UI as { content_hash, mime, bytes } records.
  // Bytes are transferred as plain arrays to keep postMessage compatibility;
  // the UI converts back to Uint8Array for the zip writer. The user-font
  // entries ride in the same bundle list — the UI doesn't need to distinguish
  // them; the zip writer picks the file extension from mime.
  const assetBundles = Array.from(new Map([
    ...result.assets.entries().map((a) => ({
      content_hash: a.content_hash,
      mime: a.mime,
      bytes: Array.from(a.bytes), // postMessage-safe; ~1.5x size overhead vs raw buffer
    })).map((asset) => [asset.content_hash, asset] as const),
    ...exportFonts.entries().map((f) => ({
      content_hash: f.content_hash,
      mime: f.mime,
      bytes: Array.from(f.bytes),
    })).map((asset) => [asset.content_hash, asset] as const),
  ]).values());

  const totalAssetCount = assetBundles.length;

  reply({
    type: "export-result",
    nodeCount: result.nodeCount,
    diagnosticCount: result.diagnostics.length,
    assetCount: totalAssetCount,
    tokenCount:
      Object.keys(result.tokens.colors).length +
      Object.keys(result.tokens.dimensions).length +
      Object.keys(result.tokens.strings).length,
    truncated: result.truncated,
    suggestedName,
    json,
    assets: assetBundles,
    delivery,
  });
}

function solidPaint(color: ForgePlanColor): SolidPaint {
  return {
    type: "SOLID",
    color: { r: color.r, g: color.g, b: color.b },
    opacity: color.a,
  };
}

function gradientPaint(
  gradient: ForgePlanGradient,
  width: number,
  height: number,
): GradientPaint {
  return {
    type: "GRADIENT_LINEAR",
    gradientTransform: gradientTransformForFigma(
      gradient.angleDegrees,
      width,
      height,
    ) as Transform,
    gradientStops: gradient.stops.map((stop) => ({
      position: stop.position,
      color: {
        r: stop.color.r,
        g: stop.color.g,
        b: stop.color.b,
        a: stop.color.a,
      },
    })),
  };
}

function gradientStackFingerprint(paints: readonly Paint[]): string {
  return JSON.stringify(paints.map((paint) =>
    paint.type === "GRADIENT_LINEAR" ? paint : { type: paint.type }));
}

function figmaEffects(plan: ForgeFigmaPlanNode): Effect[] {
  return plan.effects.map((effect): Effect => {
    if (effect.kind === "layer-blur" || effect.kind === "background-blur") {
      return {
        type: effect.kind === "layer-blur" ? "LAYER_BLUR" : "BACKGROUND_BLUR",
        blurType: "NORMAL",
        radius: effect.radius,
        visible: true,
      };
    }
    const color = effect.color ?? { r: 0, g: 0, b: 0, a: 1 };
    const common = {
      color: { r: color.r, g: color.g, b: color.b, a: color.a },
      offset: { x: effect.offsetX ?? 0, y: effect.offsetY ?? 0 },
      radius: effect.radius,
      visible: true,
      blendMode: "NORMAL" as const,
      ...((effect.spread ?? 0) !== 0 ? { spread: effect.spread } : {}),
    };
    if (effect.kind === "inner-shadow") {
      return { type: "INNER_SHADOW", ...common };
    }
    return { type: "DROP_SHADOW", ...common, showShadowBehindNode: true };
  });
}

function planFonts(plan: ForgeFigmaPlan): FontName[] {
  const found = new Map<string, FontName>();
  const visit = (node: ForgeFigmaPlanNode) => {
    if (node.kind === "text") {
      const font = {
        family: node.fontFamily ?? "Inter",
        style: node.fontStyle ?? "Regular",
      };
      found.set(`${font.family}\0${font.style}`, font);
      for (const run of node.textRuns) {
        if (!run.fontStyle) continue;
        const runFont = { family: font.family, style: run.fontStyle };
        found.set(`${runFont.family}\0${runFont.style}`, runFont);
      }
    }
    for (const child of node.children) visit(child);
    for (const alternate of node.alternateFrames) visit(alternate);
  };
  visit(plan.root);
  return [...found.values()];
}

function utf8ByteOffsetToUtf16(text: string, byteOffset: number): number {
  if (byteOffset <= 0) return 0;
  let bytes = 0;
  for (let i = 0; i < text.length;) {
    if (bytes >= byteOffset) return i;
    const cp = text.codePointAt(i)!;
    bytes += cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;
    i += cp > 0xffff ? 2 : 1;
    if (bytes > byteOffset) {
      throw new Error(`Text run byte offset ${byteOffset} splits a UTF-8 code point.`);
    }
  }
  if (bytes === byteOffset) return text.length;
  throw new Error(`Text run byte offset ${byteOffset} exceeds the text length.`);
}

function decodeUtf8(bytes: readonly number[]): string {
  let out = "";
  for (let index = 0; index < bytes.length;) {
    const first = bytes[index++];
    if (first < 0x80) {
      out += String.fromCharCode(first);
      continue;
    }
    const continuation = (count: number): number[] => {
      const found: number[] = [];
      for (let offset = 0; offset < count; offset++) {
        const byte = bytes[index++];
        if (byte === undefined || (byte & 0xc0) !== 0x80) {
          throw new Error("SVG asset is not valid UTF-8.");
        }
        found.push(byte & 0x3f);
      }
      return found;
    };
    let point: number;
    if ((first & 0xe0) === 0xc0) {
      const tail = continuation(1);
      point = ((first & 0x1f) << 6) | tail[0];
      if (point < 0x80) throw new Error("SVG asset contains overlong UTF-8.");
    } else if ((first & 0xf0) === 0xe0) {
      const tail = continuation(2);
      point = ((first & 0x0f) << 12) | (tail[0] << 6) | tail[1];
      if (point < 0x800 || (point >= 0xd800 && point <= 0xdfff)) {
        throw new Error("SVG asset contains invalid UTF-8.");
      }
    } else if ((first & 0xf8) === 0xf0) {
      const tail = continuation(3);
      point = ((first & 0x07) << 18) | (tail[0] << 12) |
        (tail[1] << 6) | tail[2];
      if (point < 0x10000 || point > 0x10ffff) {
        throw new Error("SVG asset contains invalid UTF-8.");
      }
    } else {
      throw new Error("SVG asset is not valid UTF-8.");
    }
    if (point <= 0xffff) out += String.fromCharCode(point);
    else {
      const adjusted = point - 0x10000;
      out += String.fromCharCode(
        0xd800 + (adjusted >> 10),
        0xdc00 + (adjusted & 0x3ff),
      );
    }
  }
  return out;
}

function applyTextRuns(text: TextNode, plan: ForgeFigmaPlanNode): void {
  for (const run of plan.textRuns) {
    const start = utf8ByteOffsetToUtf16(text.characters, run.start);
    const end = utf8ByteOffsetToUtf16(text.characters, run.end);
    if (run.fontSize !== undefined) text.setRangeFontSize(start, end, run.fontSize);
    if (run.fontStyle) {
      text.setRangeFontName(start, end, {
        family: plan.fontFamily ?? "Inter",
        style: run.fontStyle,
      });
    }
    if (run.color) text.setRangeFills(start, end, [solidPaint(run.color)]);
    if (run.letterSpacing !== undefined) {
      text.setRangeLetterSpacing(start, end, { unit: "PIXELS", value: run.letterSpacing });
    }
    if (run.textDecoration) text.setRangeTextDecoration(start, end, run.textDecoration);
  }
}

function applyNodeStyle(
  node: SceneNode,
  plan: ForgeFigmaPlanNode,
  visualStyleBaked = false,
  shapeStyleBaked = false,
): void {
  if (!visualStyleBaked && "opacity" in node && plan.opacity !== undefined) {
    node.opacity = plan.opacity;
  }
  if (!visualStyleBaked && "blendMode" in node && plan.blendMode) {
    (node as SceneNode & { blendMode: BlendMode }).blendMode = plan.blendMode as BlendMode;
  }
  if (!visualStyleBaked && "effects" in node) node.effects = figmaEffects(plan);
  if (!shapeStyleBaked && "strokes" in node) {
    const hasStroke = [
      plan.borderWidth,
      plan.borderTopWidth,
      plan.borderRightWidth,
      plan.borderBottomWidth,
      plan.borderLeftWidth,
    ].some((width) => (width ?? 0) > 0);
    if (plan.borderColor && hasStroke) {
      node.strokes = [solidPaint(plan.borderColor)];
      node.strokeWeight = plan.borderWidth ?? 0;
      if ("dashPattern" in node) node.dashPattern = plan.dashPattern ?? [];
    } else {
      node.strokes = [];
    }
    const individual = node as SceneNode & Partial<IndividualStrokesMixin>;
    if ("strokeTopWeight" in individual) {
      const fallback = plan.borderWidth ?? 0;
      individual.strokeTopWeight = plan.borderTopWidth ?? fallback;
      individual.strokeRightWeight = plan.borderRightWidth ?? fallback;
      individual.strokeBottomWeight = plan.borderBottomWidth ?? fallback;
      individual.strokeLeftWeight = plan.borderLeftWidth ?? fallback;
    }
  }
  const cornerStyleBaked =
    shapeStyleBaked && plan.svgVisualMode !== "inline";
  if (!cornerStyleBaked && "cornerRadius" in node && plan.cornerRadius !== undefined) {
    (node as SceneNode & CornerMixin).cornerRadius = plan.cornerRadius;
  }
  if (!cornerStyleBaked && "topLeftRadius" in node) {
    if (plan.topLeftRadius !== undefined) node.topLeftRadius = plan.topLeftRadius;
    if (plan.topRightRadius !== undefined) node.topRightRadius = plan.topRightRadius;
    if (plan.bottomRightRadius !== undefined) node.bottomRightRadius = plan.bottomRightRadius;
    if (plan.bottomLeftRadius !== undefined) node.bottomLeftRadius = plan.bottomLeftRadius;
  }
  const sized = node as SceneNode & {
    minWidth?: number | null; minHeight?: number | null;
    maxWidth?: number | null; maxHeight?: number | null;
  };
  const hasSizeConstraint = plan.minWidth !== undefined ||
    plan.minHeight !== undefined || plan.maxWidth !== undefined ||
    plan.maxHeight !== undefined;
  const hasAspectRatio = plan.aspectRatio !== undefined;
  const nodeUsesAutoLayout =
    "layoutMode" in node && node.layoutMode !== "NONE";
  const parentUsesAutoLayout = node.parent !== null &&
    "layoutMode" in node.parent && node.parent.layoutMode !== "NONE";
  if (hasSizeConstraint && !nodeUsesAutoLayout && !parentUsesAutoLayout) {
    throw new Error(
      `${plan.name}: min/max sizing requires an auto-layout frame or child; import rolled back.`,
    );
  }
  if ("minWidth" in sized && (nodeUsesAutoLayout || parentUsesAutoLayout)) {
    if (plan.minWidth !== undefined) sized.minWidth = plan.minWidth;
    if (plan.minHeight !== undefined) sized.minHeight = plan.minHeight;
    if (plan.maxWidth !== undefined) sized.maxWidth = plan.maxWidth;
    if (plan.maxHeight !== undefined) sized.maxHeight = plan.maxHeight;
  }
  if (hasAspectRatio && "lockAspectRatio" in node) {
    const currentRatio = node.width / Math.max(node.height, 1e-9);
    if (Math.abs(currentRatio - plan.aspectRatio!) > 1e-4) {
      if (!("resize" in node)) {
        throw new Error(`${plan.name}: aspectRatio cannot resize this Figma node.`);
      }
      node.resize(node.width, node.width / plan.aspectRatio!);
    }
    node.lockAspectRatio();
  }
  if ("constraints" in node && plan.constraints) node.constraints = plan.constraints;
}

async function applyRoundtripMetadata(
  node: SceneNode,
  plan: ForgeFigmaPlanNode,
): Promise<void> {
  if (!("setPluginData" in node)) return;
  node.setPluginData("pulp.design_ir_type", plan.sourceType);
  if (plan.svgVisualMode) {
    node.setPluginData("pulp.svg_visual_mode", plan.svgVisualMode);
  }
  if (Object.keys(plan.attributes).length > 0) {
    node.setPluginData("pulp.design_ir_attributes", JSON.stringify(plan.attributes));
  }
  if (plan.assetRole === "background" &&
      plan.kind !== "image" && plan.kind !== "svg") {
    node.setPluginData("pulp.background_image_asset", "1");
  } else if (plan.assetRole === "node" &&
             plan.kind !== "image" && plan.kind !== "svg") {
    node.setPluginData("pulp.node_image_asset", "1");
  }
  if (plan.audioWidget) node.setPluginData("pulp.audio_widget", plan.audioWidget);
  if (plan.audioLabel) node.setPluginData("pulp.audio_label", plan.audioLabel);
  if (plan.audioMin !== undefined)
    node.setPluginData("pulp.audio_min", String(plan.audioMin));
  if (plan.audioMax !== undefined)
    node.setPluginData("pulp.audio_max", String(plan.audioMax));
  if (plan.audioDefault !== undefined)
    node.setPluginData("pulp.audio_default", String(plan.audioDefault));
  if (plan.audioUnits) node.setPluginData("pulp.audio_units", plan.audioUnits);
  if (plan.binding) node.setPluginData("pulp.binding", plan.binding);
  if (plan.bindingY) node.setPluginData("pulp.binding_y", plan.bindingY);
  if (plan.sourceNodeId) node.setPluginData("pulp.source_node_id", plan.sourceNodeId);
  if (plan.stableAnchorId) node.setPluginData("pulp.stable_anchor_id", plan.stableAnchorId);
  if (plan.backgroundGradient) {
    node.setPluginData("pulp.background_gradient", plan.backgroundGradient.css);
    node.setPluginData(
      "pulp.background_gradient_aspect_ratio",
      String(node.width / Math.max(node.height, 1e-9)),
    );
    if ("fills" in node && Array.isArray(node.fills)) {
      if (node.fills.some((fill) => fill.type === "GRADIENT_LINEAR")) {
        node.setPluginData(
          "pulp.background_gradient_fingerprint",
          gradientStackFingerprint(node.fills),
        );
      }
    }
  }
  if (plan.interactiveElements !== undefined) {
    node.setPluginData("pulp.interactive_elements", JSON.stringify(plan.interactiveElements));
    if (plan.interactiveElements.length > 0 && "exportAsync" in node) {
      const bytes = await (node as ExportMixin).exportAsync({ format: "SVG" });
      node.setPluginData(
        "pulp.interactive_elements_visual_fingerprint",
        await sha256Hex(bytes),
      );
    }
  }
  if (plan.preservedChildren) {
    node.setPluginData(
      "pulp.faithful_semantic_children",
      JSON.stringify(plan.preservedChildren),
    );
  }
}

async function materializeForgeNode(
  plan: ForgeFigmaPlanNode,
  parent: ChildrenMixin,
  transactionId: string,
  insulateFromParentSizing = false,
): Promise<SceneNode> {
  let node: SceneNode;
  if (plan.kind === "text") {
    const text = figma.createText();
    text.setPluginData("pulp.import_transaction", transactionId);
    text.fontName = {
      family: plan.fontFamily ?? "Inter",
      style: plan.fontStyle ?? "Regular",
    };
    text.characters = plan.content ?? plan.name;
    if (plan.fontSize) text.fontSize = plan.fontSize;
    if (plan.textColor) text.fills = [solidPaint(plan.textColor)];
    if (plan.textAlignHorizontal) text.textAlignHorizontal = plan.textAlignHorizontal;
    if (plan.textAlignVertical) text.textAlignVertical = plan.textAlignVertical;
    if (plan.letterSpacing !== undefined) {
      text.letterSpacing = { unit: "PIXELS", value: plan.letterSpacing };
    }
    if (plan.lineHeight !== undefined) {
      text.lineHeight = { unit: "PIXELS", value: plan.lineHeight };
    }
    if (plan.textCase) text.textCase = plan.textCase;
    if (plan.textDecoration) text.textDecoration = plan.textDecoration;
    applyTextRuns(text, plan);
    const autoResize = plan.attributes["figma:text_auto_resize"]?.toUpperCase();
    if (autoResize &&
        !["NONE", "WIDTH_AND_HEIGHT", "HEIGHT", "TRUNCATE"].includes(autoResize)) {
      throw new Error(`${plan.name}: invalid figma:text_auto_resize metadata.`);
    }
    text.textAutoResize = (autoResize ?? "NONE") as
      "NONE" | "WIDTH_AND_HEIGHT" | "HEIGHT" | "TRUNCATE";
    const truncation = plan.attributes["figma:text_truncation"];
    if (truncation) {
      if (truncation !== "ending") {
        throw new Error(`${plan.name}: invalid figma:text_truncation metadata.`);
      }
      text.textTruncation = "ENDING";
    }
    const maxLines = plan.attributes["figma:max_lines"];
    if (maxLines) {
      const parsed = Number(maxLines);
      if (!Number.isInteger(parsed) || parsed <= 0) {
        throw new Error(`${plan.name}: invalid figma:max_lines metadata.`);
      }
      text.maxLines = parsed;
    }
    const hyperlink = plan.attributes["figma:hyperlink"];
    if (hyperlink) {
      text.setRangeHyperlink(0, text.characters.length, {
        type: "URL",
        value: hyperlink,
      });
    }
    node = text;
  } else if (plan.kind === "ellipse") {
    node = figma.createEllipse();
    node.setPluginData("pulp.import_transaction", transactionId);
  } else if (plan.kind === "rectangle") {
    node = figma.createRectangle();
    node.setPluginData("pulp.import_transaction", transactionId);
  } else if (plan.kind === "svg") {
    if (!plan.asset) throw new Error(`${plan.name}: SVG plan is missing bytes.`);
    node = figma.createNodeFromSvg(decodeUtf8(plan.asset.bytes));
    node.setPluginData("pulp.import_transaction", transactionId);
    const hasCornerRadius = (plan.cornerRadius ?? 0) > 0 ||
      (plan.topLeftRadius ?? 0) > 0 || (plan.topRightRadius ?? 0) > 0 ||
      (plan.bottomRightRadius ?? 0) > 0 || (plan.bottomLeftRadius ?? 0) > 0;
    node.clipsContent = plan.clipsContent ||
      (plan.svgVisualMode !== "faithful" && hasCornerRadius);
  } else if (plan.kind === "image") {
    if (!plan.asset) throw new Error(`${plan.name}: image plan is missing bytes.`);
    node = figma.createRectangle();
    node.setPluginData("pulp.import_transaction", transactionId);
  } else {
    const frame = figma.createFrame();
    frame.setPluginData("pulp.import_transaction", transactionId);
    if (plan.layoutMode) {
      frame.layoutMode = plan.layoutMode;
      frame.itemSpacing = plan.itemSpacing ?? 0;
      if (plan.layoutWrap === "WRAP") {
        frame.layoutWrap = "WRAP";
        if (plan.counterAxisSpacing !== undefined) {
          frame.counterAxisSpacing = plan.counterAxisSpacing;
        }
      }
      if (plan.primaryAxisAlignItems) {
        frame.primaryAxisAlignItems = plan.primaryAxisAlignItems;
      }
      if (plan.counterAxisAlignItems) {
        frame.counterAxisAlignItems = plan.counterAxisAlignItems;
      }
      if (plan.layoutWrap === "WRAP" && plan.counterAxisAlignContent) {
        frame.counterAxisAlignContent = plan.counterAxisAlignContent;
      }
      frame.paddingTop = plan.paddingTop ?? 0;
      frame.paddingRight = plan.paddingRight ?? 0;
      frame.paddingBottom = plan.paddingBottom ?? 0;
      frame.paddingLeft = plan.paddingLeft ?? 0;
    }
    frame.clipsContent = plan.clipsContent ?? false;
    node = frame;
  }

  node.name = plan.name;
  parent.appendChild(node);
  if ("resize" in node) node.resize(plan.width, plan.height);
  const parentLaysOutChildren =
    "layoutMode" in parent && parent.layoutMode !== "NONE";
  const sourceParentLaysOutChildren =
    parentLaysOutChildren && !insulateFromParentSizing;
  if ("layoutSizingHorizontal" in node) {
    const nodeIsAutoLayout = "layoutMode" in node && node.layoutMode !== "NONE";
    if (plan.layoutSizingHorizontal &&
        (sourceParentLaysOutChildren ||
         (nodeIsAutoLayout && plan.layoutSizingHorizontal !== "FILL"))) {
      node.layoutSizingHorizontal = plan.layoutSizingHorizontal;
    }
    if (plan.layoutSizingVertical &&
        (sourceParentLaysOutChildren ||
         (nodeIsAutoLayout && plan.layoutSizingVertical !== "FILL"))) {
      node.layoutSizingVertical = plan.layoutSizingVertical;
    }
  }
  if (!insulateFromParentSizing &&
      "layoutGrow" in node && plan.layoutGrow !== undefined) {
    node.layoutGrow = plan.layoutGrow;
  }
  if (!insulateFromParentSizing && "layoutAlign" in node && plan.layoutAlign) {
    node.layoutAlign = plan.layoutAlign;
  }
  if (!insulateFromParentSizing &&
      "layoutPositioning" in node && plan.layoutPositioning) {
    node.layoutPositioning = plan.layoutPositioning;
  }
  if (!parentLaysOutChildren || plan.layoutPositioning === "ABSOLUTE") {
    node.x = plan.x;
    node.y = plan.y;
  }

  if ("fills" in node && plan.kind !== "text" && plan.kind !== "svg") {
    const fills: Paint[] = [];
    if (plan.backgroundColor) fills.push(solidPaint(plan.backgroundColor));
    if (plan.backgroundGradient) {
      fills.push(gradientPaint(plan.backgroundGradient, plan.width, plan.height));
    }
    if (plan.asset) {
      const image = figma.createImage(new Uint8Array(plan.asset.bytes));
      fills.push({
        type: "IMAGE",
        imageHash: image.hash,
        scaleMode: plan.imageScaleMode ?? "FILL",
      });
    }
    node.fills = fills;
  }
  // Faithful whole-node SVG captures already contain every visual style.
  // Generated inline paths bake shape paint/stroke but still need node-level
  // opacity/effects. Ordinary SVG assets retain all wrapper styles.
  applyNodeStyle(
    node,
    plan,
    plan.svgVisualMode === "faithful",
    plan.svgVisualMode === "faithful" || plan.svgVisualMode === "inline",
  );

  if ("appendChild" in node) {
    for (const child of plan.children) {
      const materialized = await materializeForgeNode(
        child,
        node as FrameNode,
        transactionId,
      );
      if (plan.counterAxisStretch &&
          "layoutAlign" in materialized &&
          (!child.layoutAlign || child.layoutAlign === "INHERIT")) {
        materialized.layoutAlign = "STRETCH";
      }
    }
  }
  return node;
}

async function applyRoundtripMetadataTree(
  node: SceneNode,
  plan: ForgeFigmaPlanNode,
): Promise<void> {
  await applyRoundtripMetadata(node, plan);
  if (!("children" in node)) return;
  for (let i = 0; i < plan.children.length; i++) {
    const child = node.children[i];
    if (child) await applyRoundtripMetadataTree(child, plan.children[i]);
  }
}

function clearImportTransaction(node: SceneNode): void {
  node.setPluginData("pulp.import_transaction", "");
  if (!("children" in node)) return;
  for (const child of node.children) clearImportTransaction(child);
}

async function materializeForgePlan(plan: ForgeFigmaPlan): Promise<SceneNode> {
  for (const font of planFonts(plan)) {
    try {
      await figma.loadFontAsync(font);
    } catch {
      throw new Error(
        `Font "${font.family} ${font.style}" must be installed and available ` +
        "to Figma before copy-back; bundled bytes are preserved for export but cannot register a Figma font.",
      );
    }
  }
  const transactionId =
    `forge-import-${forgeImportSessionNonce}-${++forgeImportTransactionCounter}`;
  try {
    if (plan.root.alternateFrames.length === 0) {
      const root = await materializeForgeNode(
        plan.root,
        figma.currentPage,
        transactionId,
      );
      // Later siblings and parent stretch can resize earlier descendants.
      // Fingerprint the complete subtree only after all layout has settled.
      await applyRoundtripMetadataTree(root, plan.root);
      clearImportTransaction(root);
      return root;
    }

    const states = figma.createFrame();
    states.setPluginData("pulp.import_transaction", transactionId);
    states.name = `${plan.root.name} / States`;
    states.x = plan.root.x;
    states.y = plan.root.y;
    states.layoutMode = "HORIZONTAL";
    states.itemSpacing = 48;
    states.fills = [];
    states.clipsContent = false;
    states.setPluginData("pulp.alternate_frames_container", "1");
    const primary = await materializeForgeNode(
      plan.root,
      states,
      transactionId,
      true,
    );
    primary.setPluginData("pulp.alternate_frame_index", "0");
    const alternates: SceneNode[] = [];
    for (let i = 0; i < plan.root.alternateFrames.length; i++) {
      const alternate = await materializeForgeNode(
        plan.root.alternateFrames[i],
        states,
        transactionId,
        true,
      );
      alternate.setPluginData("pulp.alternate_frame_index", String(i + 1));
      alternates.push(alternate);
    }
    states.resizeWithoutConstraints(
      states.children.reduce((sum, child) => sum + child.width, 0) +
        states.itemSpacing * Math.max(0, states.children.length - 1),
      states.children.reduce((height, child) => Math.max(height, child.height), 1),
    );
    await applyRoundtripMetadataTree(primary, plan.root);
    for (let i = 0; i < alternates.length; i++) {
      await applyRoundtripMetadataTree(alternates[i], plan.root.alternateFrames[i]);
    }
    clearImportTransaction(states);
    return states;
  } catch (error) {
    for (const child of [...figma.currentPage.children]) {
      if (child.getPluginData("pulp.import_transaction") === transactionId) {
        child.remove();
      }
    }
    throw error;
  }
}

async function handleForgeImport(clipboardText: string): Promise<void> {
  const plan = decodeForgeDesignForFigma(clipboardText);
  const root = await materializeForgePlan(plan);
  figma.currentPage.selection = [root];
  figma.viewport.scrollAndZoomIntoView([root]);
  reply({
    type: "forge-import-result",
    nodeCount: plan.nodeCount,
    audioWidgetCount: plan.audioWidgetCount,
  });
}

/// Walk the current selection, collect the unique (family, style, weight?,
/// italic?) tuples referenced by text nodes, and reply with a `fonts-detected`
/// message annotated with which entries already have user-supplied bytes in the
/// cache. Invoked early so the UI can render drop zones before the user commits
/// to a full export.
async function handleScanFonts(): Promise<void> {
  const sel = figma.currentPage.selection;
  if (sel.length === 0) {
    reply({ type: "fonts-detected", fonts: [] });
    return;
  }
  const result = await extractScene(sel);
  const fonts: FontFamilyAssetSummary[] = result.font_family_assets.map((f) => ({
    family: f.family,
    style: f.style,
    weight: f.weight,
    italic: f.italic,
    has_user_font: !!userFonts.lookup(f.family, f.style),
  }));
  reply({ type: "fonts-detected", fonts });
}

/// User dropped a TTF/OTF onto the UI. Store it in the cache and ack with the
/// asset_id so the UI can flip the row state. The cache survives until plugin
/// close.
async function handleUserFont(msg: {
  family: string;
  style: string;
  bytes: number[];
  filename: string;
}): Promise<void> {
  const bytes = new Uint8Array(msg.bytes);
  const entry = await userFonts.add(msg.family, msg.style, bytes, msg.filename);
  reply({
    type: "user-font-staged",
    family: entry.family,
    style: entry.style,
    asset_id: entry.asset_id,
  });
}

function sanitizeFilename(s: string): string {
  return s.replace(/[^A-Za-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 64);
}

function reply(msg: PulpSandboxMessage): void {
  figma.ui.postMessage(msg);
}

reply({ type: "ready", pluginVersion: PLUGIN_VERSION });
