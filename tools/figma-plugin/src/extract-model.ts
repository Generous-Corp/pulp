// In-memory model produced by extract.ts (Phase 2a) and consumed by
// serialize.ts (Phase 2b) + the future Phase 3 recognizer.
//
// Field names mirror the JSON envelope in schema/figma-plugin-export-v1.json so
// the Phase 2b serializer is mostly a passthrough.

export type AudioWidgetKind = "knob" | "fader" | "meter" | "xy_pad" | "waveform" | "spectrum";

export interface ExtractedFigmaNode {
  // Identity
  type: string;             // "frame" | "text" | "image" | "vector" | (after Phase 3) audio widget kinds
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

  // Image / vector — populated in slice 2
  exported_asset?: { content_hash: string; mime: string; bytes_size: number };

  // Component / instance metadata — populated when walking INSTANCE nodes (slice 2)
  component_key?: string;
  component_set_name?: string;
  main_component_id?: string;
  main_component_name?: string;
  remote_library?: boolean;
  component_properties?: Record<string, { type: string; value: string | number | boolean }>;
  variant_properties?: Record<string, string>;

  // Library awareness (Phase 3)
  library_widget_kind?: AudioWidgetKind;
  library_version?: string;

  children: ExtractedFigmaNode[];
}

export interface ExtractedStyle {
  background_color?: string;
  background_gradient?: string;
  background_image?: string;
  color?: string;
  opacity?: number;
  border_radius?: number;
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
