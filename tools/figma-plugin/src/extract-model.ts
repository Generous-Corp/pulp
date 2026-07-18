// In-memory model produced by extract.ts and consumed by serialize.ts and the
// library-widget recognizer.
//
// Field names mirror the JSON envelope in schema/figma-plugin-export-v1.json so
// the serializer is mostly a passthrough.

import type { InteractiveElement } from "./faithful-vector";

export type AudioWidgetKind = "knob" | "fader" | "meter" | "xy_pad" | "waveform" | "spectrum";

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
  color?: string;
  opacity?: number;
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
  box_shadow?: string;
  filter?: string;
  backdrop_filter?: string;
  font_family?: string;
  font_size?: number;
  font_weight?: number;
  font_style?: "normal" | "italic";
  text_align?: string;
  letter_spacing?: number;
  line_height?: number;
  text_transform?: string;
  overflow?: string;
  position?: "absolute" | "relative" | "static";
  top?: number;
  left?: number;
  right?: number;
  bottom?: number;
  width?: number;
  height?: number;
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
}

export interface ExtractedDiagnostic {
  severity: "info" | "warning" | "error";
  code: string;
  kind:
    | "unknown"
    | "unsupported_property"
    | "unresolved_asset"
    | "capture_partial"
    | "fallback_used"
    | "recognition_unavailable";
  message: string;
  path: string;
}
