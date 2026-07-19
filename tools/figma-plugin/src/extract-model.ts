// In-memory model produced by extract.ts and consumed by serialize.ts and the
// library-widget recognizer.
//
// Field names mirror the JSON envelope in schema/figma-plugin-export-v1.json so
// the serializer is mostly a passthrough.

import type { InteractiveElement } from "./faithful-vector";

export type AudioWidgetKind = "knob" | "fader" | "meter" | "xy_pad" | "waveform" | "spectrum";

/// One styled character range inside a text node — the style DELTA against the
/// node's dominant style, in the exact camelCase shape the C++ consumer reads
/// (design_ir_json.cpp::parse_ir_text_runs) and the REST lane already emits.
/// `start`/`end` are [start, end) UTF-8 BYTE offsets into `content` (converted
/// from the Plugin API's UTF-16 code-unit segment indices).
export interface ExtractedTextRun {
  start: number;
  end: number;
  fontSize?: number;
  fontWeight?: number;
  fontStyle?: "italic" | "normal";
  color?: string;
  letterSpacing?: number;
  textDecoration?: string;
}

export interface ExtractedFigmaNode {
  // Identity
  type: string;             // "frame" | "text" | "image" | "vector" | audio widget kinds
  figma_type: string;       // raw SceneNode.type (FRAME, RECTANGLE, INSTANCE, ...)
  name: string;
  figma_node_id: string;
  parent_id: string | null;
  z_order: number;

  // Geometry
  absolute_bounds: { x: number; y: number; w: number; h: number };
  relative_transform: number[][];
  visible: boolean;
  locked: boolean;
  opacity: number;
  blend_mode: string;

  // Style + layout
  style: ExtractedStyle;
  layout: ExtractedLayout;

  // Text
  content?: string;
  // Ordered per-range style overrides for mixed-style text (a bold word, a
  // colored span). Absent for homogeneous text — the flat dominant style in
  // `style` is the whole story then, and the consumer prefers the plain path.
  runs?: ExtractedTextRun[];
  // Free-form string attributes serialized into IRNode.attributes. Used for
  // namespaced preserved-not-lowered text metadata (figma:text_auto_resize,
  // figma:text_truncation, figma:max_lines, figma:hyperlink).
  attributes?: Record<string, string>;

  // Image / vector assets
  exported_asset?: { content_hash: string; mime: string; bytes_size: number };
  asset_ref?: string;       // reference into AssetCache; serialized as node.asset_ref

  // Faithful-vector import. When set, the node renders its own
  // SVG export via DesignFrameView with the interactive overlays below, instead
  // of the widget-recognition rebuild. Mirrors the canonical IR keys the C++
  // parser reads (design_ir_json.cpp::parse_ir_node).
  render_mode?: string;                        // "faithful_svg"
  svg_asset_id?: string;                       // → asset_manifest entry (image/svg+xml)
  interactive_elements?: InteractiveElement[];

  // Component / instance metadata — populated when walking INSTANCE nodes
  component_key?: string;
  component_set_name?: string;
  main_component_id?: string;
  main_component_name?: string;
  remote_library?: boolean;
  component_properties?: Record<string, { type: string; value: string | number | boolean }>;
  variant_properties?: Record<string, string>;

  // Library awareness
  library_widget_kind?: AudioWidgetKind;
  library_version?: string;

  // Explicit widget opt-out for synthetic nodes (the mask-scope wrapper).
  // Serialized as the envelope's `audio_widget: "none"` — the opt-out
  // parse_ir_audio_widget honors, so the C++ name heuristic never guesses a
  // widget out of a wrapper named after its mask.
  audio_widget?: "none";

  // Structured audio-widget properties. Populated when a Pulp library widget is
  // recognized; carried into the JSON envelope at the node root so
  // design_import.cpp::parse_ir_node maps them onto IRNode.audio_label /
  // audio_min / audio_max / audio_default and the attributes map.
  audio_label?: string;
  audio_min?: number;
  audio_max?: number;
  audio_default?: number;
  audio_units?: string;
  audio_binding?: string;
  // XYPad carries a Y-axis binding alongside the primary `binding` (which holds
  // the X-axis route). Lands in IRNode.attributes.binding_y. Populated only for
  // `Pulp / XYPad` library instances.
  audio_binding_y?: string;

  // Variable bindings: which design token (canonical name from the tokens
  // pass) is bound to which property of THIS node, resolved from
  // `node.boundVariables` via tokens.variableIdToName. Serialized into the
  // node's `figma` block as `bound_variables`; design_ir_json.cpp preserves
  // each entry as a `figmaBoundVariable.<property>` attribute. Only resolved
  // bindings appear — an alias whose variable the token pass didn't capture
  // is skipped with a diagnostic, never emitted as a dangling id.
  bound_variables?: Record<string, string>;

  // Resize constraints in Figma's Plugin-API spelling
  // (MIN/MAX/CENTER/STRETCH/SCALE per axis). Serialized at the envelope node
  // root as `constraints`; design_ir_json.cpp normalizes the tokens and
  // codegen lowers them to flex within the parent. Only populated for nodes
  // positioned by their parent's coordinate space (not flowing auto-layout
  // children, whose sizing is governed by the stack instead).
  constraints?: { horizontal?: string; vertical?: string };

  children: ExtractedFigmaNode[];
}

export interface ExtractedStyle {
  background_color?: string;
  background_gradient?: string;
  background_image?: string;
  // CSS background-size keyword — Figma IMAGE-fill scale mode on frame-shaped
  // nodes (FILL → cover, FIT → contain, TILE → auto + background_repeat).
  background_size?: string;
  // CSS object-fit — Figma IMAGE-fill scale mode when the fill becomes a
  // dedicated image node; ImageView::paint honors it.
  object_fit?: string;
  color?: string;
  opacity?: number;
  // CSS mix-blend-mode — the node's layer blend mode when it is in the shared
  // supported-blend table (extract-pure.ts::FIGMA_BLEND_CSS), lowered to the
  // CSS keyword ("multiply", "soft-light"). Unmappable modes emit nothing here
  // and raise `blend-unsupported`; the raw Figma mode always rides in the
  // node's figma.blend_mode for provenance.
  mix_blend_mode?: string;
  border_radius?: number;
  // Per-corner radii — emitted (and border_radius dropped) only when the four
  // corners differ; C++ parse_ir_style reads these four fields directly.
  border_top_left_radius?: number;
  border_top_right_radius?: number;
  border_bottom_right_radius?: number;
  border_bottom_left_radius?: number;
  border?: string;
  border_color?: string;
  border_width?: number;
  border_style?: string;
  // Per-side box strokes (Figma individualStrokeWeights, active when the four
  // sides differ). All four widths are emitted — an explicit 0 side paints
  // nothing — with the single Figma stroke color repeated per painted side;
  // the uniform border/border_width shorthand is omitted in that case. C++
  // parse_ir_style reads all eight fields directly.
  border_top_width?: number;
  border_right_width?: number;
  border_bottom_width?: number;
  border_left_width?: number;
  border_top_color?: string;
  border_right_color?: string;
  border_bottom_color?: string;
  border_left_color?: string;
  box_shadow?: string;
  filter?: string;
  backdrop_filter?: string;
  // CSS clip-path (`path("<d>")`). Emitted on the synthetic mask-scope
  // wrapper a sibling mask lowers to; parse_ir_style reads it (clipPath or
  // clip_path) and codegen lowers to setClipPath → SkPath::FromSVGString.
  clip_path?: string;
  font_family?: string;
  font_size?: number;
  font_weight?: number;
  font_style?: "normal" | "italic";
  text_align?: string;
  /// Figma textAlignVertical normalized to top/middle/bottom. Design
  /// authority: parse_ir_style reads it and codegen honors it over the
  /// tall-slot centering heuristic.
  vertical_align?: "top" | "middle" | "bottom";
  letter_spacing?: number;
  line_height?: number;
  text_transform?: string;
  overflow?: string;
  /// CSS transform, today always `rotate(<deg>deg)` — the spelling the shared
  /// codegen lowers to setRotation (center pivot). Emitted by the walk when a
  /// node's relativeTransform is a pure non-orthogonal rotation; placement is
  /// re-anchored so the center pivot reproduces Figma's rendering.
  transform?: string;
  position?: "absolute" | "relative" | "static";
  top?: number;
  left?: number;
  right?: number;
  bottom?: number;
  width?: number;
  height?: number;
  /// Figma min/max sizing (minWidth …). parse_ir_style resolves the snake
  /// spelling and the flex engines lower them to min/max width/height clamps.
  /// Figma honored them while solving, so they only bind on host resize.
  min_width?: number;
  min_height?: number;
  max_width?: number;
  max_height?: number;
  /// Render bounds (= bounding box + effect bleed). Present only when the
  /// node has effects that extend past the bounding box (drop shadows,
  /// outer strokes). Downstream importers use this to render PNG-captured
  /// assets at their true visual size instead of being clamped to the
  /// smaller layout box. dx/dy are offsets from the bounding box origin.
  render_bounds?: {
    w: number;
    h: number;
    dx: number;
    dy: number;
  };
}

export interface ExtractedLayout {
  display?: "flex" | "grid" | "none";
  direction?: "row" | "column";
  gap?: number;
  padding?: { top: number; right: number; bottom: number; left: number };
  justify?: "flex_start" | "flex_end" | "center" | "stretch" | "space_between" | "space_around";
  align?: "flex_start" | "flex_end" | "center" | "stretch" | "space_between" | "space_around";
  wrap?: boolean;
  width_mode?: "fixed" | "hug" | "fill";
  height_mode?: "fixed" | "hug" | "fill";
  // The keys below use the consumer's camelCase spelling — the exact member
  // names parse_ir_layout (design_ir_json.cpp) reads. Values are CSS
  // spellings for the same reason: align_self/align_content pass through to
  // the flex engines untranslated.
  /// Cross-axis gap between wrapped flex tracks (Figma counterAxisSpacing)
  /// or grid row gap. rowGap for a wrapping row, columnGap for a column.
  rowGap?: number;
  columnGap?: number;
  /// Wrapped-track distribution (Figma counterAxisAlignContent SPACE_BETWEEN).
  alignContent?: "space-between";
  /// Child of a flex auto-layout parent: layoutGrow / layoutAlign.
  flexGrow?: number;
  alignSelf?: "flex-start" | "flex-end" | "center" | "stretch";
  /// targetAspectRatio as width / height. Emitted only when an axis is
  /// flexible — on a fully fixed node it would fight the solved size.
  aspectRatio?: number;
  /// Figma GRID auto-layout: uniform track templates from the row/column
  /// counts, and per-child CSS line placement from the 0-based anchors.
  gridTemplateColumns?: string;
  gridTemplateRows?: string;
  gridColumn?: string;
  gridRow?: string;
}

export interface ExtractedDiagnostic {
  severity: "info" | "warning" | "error";
  code: string;
  kind:
    | "unknown"
    | "unsupported_property"
    | "unsupported_node"
    | "unresolved_asset"
    | "capture_partial"
    | "fallback_used"
    | "recognition_unavailable";
  message: string;
  path: string;
}
