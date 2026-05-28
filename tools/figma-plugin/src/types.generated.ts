/* eslint-disable */
// AUTO-GENERATED from schema/figma-plugin-export-v1.json — DO NOT EDIT BY HAND.
// Regenerate via: npm run gen-types

/**
 * Envelope emitted by the 'Design for Pulp' Figma plugin. Consumed by `pulp import-design --from figma-plugin --file <path>`. See planning/2026-05-28-pulp-figma-plugin-strategy.md §7.2 for the parser mapping to Pulp's DesignIR.
 */
export interface PulpFigmaPluginExport {
  $schema?: string;
  /**
   * Export format identity. Bumped on incompatible JSON-shape changes.
   */
  format_version: "2026.05-figma-plugin-v1";
  /**
   * Plugin emitter version. Bumped independently as the plugin gains coverage.
   */
  parser_version: string;
  /**
   * Pulp compat.json schema version this export was generated against.
   */
  compat_schema_version: string;
  provenance: {
    adapter: "figma-plugin";
    /**
     * Plugin version that produced this export.
     */
    version: string;
    /**
     * URN of the Figma file + node, e.g. figma://<fileKey>/<nodeId>
     */
    source_uri: string;
    exported_at?: string;
  };
  /**
   * Pulp Figma Library version metadata at time of export. Plugin reads tools/figma-plugin/library-manifest.json and copies the active section here.
   */
  library_manifest?: {
    library_version?: string;
    required_plugin_version?: string;
    widget_keys?: {
      /**
       * Figma ComponentSetNode.key for each Pulp library widget
       */
      [k: string]: string;
    };
  };
  /**
   * Design tokens extracted from Figma variables. Maps to IRTokens in Pulp.
   */
  tokens?: {
    colors?: {
      [k: string]: string;
    };
    dimensions?: {
      [k: string]: number;
    };
    strings?: {
      [k: string]: string;
    };
  };
  /**
   * Maps to IRAssetManifest after Phase 4 adds asset_manifest to DesignIR.
   */
  asset_manifest?: {
    version: 1;
    assets: Asset[];
  };
  /**
   * Maps to ImportDiagnostic[]. Plugin records unsupported features here so the importer can surface them without re-running detection.
   */
  diagnostics?: Diagnostic[];
  root: Node;
}
/**
 * Maps to IRAssetRef.
 */
export interface Asset {
  /**
   * Stable handle used by nodes via asset_ref.
   */
  asset_id: string;
  original_uri: string;
  original_uri_aliases?: string[];
  /**
   * Path relative to the JSON file.
   */
  local_path?: string;
  /**
   * sha256 of resolved bytes.
   */
  content_hash: string;
  mime: string;
  width?: number;
  height?: number;
  /**
   * For font references (no bytes).
   */
  font_family?: string;
  license?: string;
  source_url?: string;
}
/**
 * Maps to ImportDiagnostic.
 */
export interface Diagnostic {
  severity: "info" | "warning" | "error";
  code: string;
  /**
   * Pointer into the JSON tree, e.g. /root/children/4
   */
  path?: string;
  message?: string;
  kind:
    | "unknown"
    | "unsupported_property"
    | "unresolved_asset"
    | "snapshot_semantics_warning"
    | "legacy_field_shortcut"
    | "capture_partial"
    | "fallback_used"
    | "recognition_unavailable";
  anchor_id?: string;
  property?: string;
}
export interface Node {
  /**
   * Node kind. Pulp library widgets get specific kinds (knob, fader, meter, ...). Everything else is one of the generic types.
   */
  type:
    | "frame"
    | "text"
    | "image"
    | "vector"
    | "button"
    | "input"
    | "knob"
    | "fader"
    | "meter"
    | "xy_pad"
    | "waveform"
    | "spectrum"
    | "label"
    | "panel"
    | "col"
    | "row";
  name: string;
  /**
   * Native Figma node id. Populates IRNode.source_node_id after parsing.
   */
  figma_node_id: string;
  /**
   * Text content (when type=text). Maps to IRNode.text_content.
   */
  content?: string;
  style?: Style;
  layout?: Layout;
  audio?: Audio;
  binding?: Binding;
  figma?: FigmaMetadata;
  /**
   * When type=image, references asset_manifest.assets[*].asset_id
   */
  asset_ref?: string;
  children?: Node[];
}
/**
 * Snake_case keys. Parser maps to IRStyle (which uses the same snake_case in C++).
 */
export interface Style {
  background_color?: string;
  background_gradient?: string;
  background_image?: string;
  background_repeat?: string;
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
  text_decoration?: string;
  white_space?: string;
  overflow?: string;
  cursor?: string;
  position?: "absolute" | "relative" | "static";
  top?: number;
  right?: number;
  bottom?: number;
  left?: number;
  z_index?: number;
  transform?: string;
  width?: number;
  height?: number;
  min_width?: number;
  min_height?: number;
  max_width?: number;
  max_height?: number;
}
/**
 * Maps to IRLayout. Padding is nested as an object here; parser flattens to padding_top/right/bottom/left.
 */
export interface Layout {
  display?: "flex" | "grid" | "none";
  direction?: "row" | "column";
  gap?: number;
  row_gap?: number;
  column_gap?: number;
  padding?: {
    top?: number;
    right?: number;
    bottom?: number;
    left?: number;
  };
  margin?: {
    top?: number;
    right?: number;
    bottom?: number;
    left?: number;
  };
  justify?: "flex_start" | "flex_end" | "center" | "stretch" | "space_between" | "space_around";
  align?: "flex_start" | "flex_end" | "center" | "stretch" | "space_between" | "space_around";
  wrap?: boolean;
  flex_grow?: number;
  flex_shrink?: number;
  aspect_ratio?: number;
  width_mode?: "fixed" | "hug" | "fill";
  height_mode?: "fixed" | "hug" | "fill";
}
/**
 * Present when node.type is one of the audio-widget kinds (knob/fader/meter/xy_pad/waveform/spectrum). Parser maps to IRNode.audio_label/audio_min/audio_max/audio_default and audio_widget enum (derived from node.type).
 */
export interface Audio {
  label?: string;
  min?: number;
  max?: number;
  default?: number;
  /**
   * normalized = 0..1 (param-range mapping applied at runtime); raw = values are in real units (passed through to setParam).
   */
  value_domain?: "normalized" | "raw";
  step?: number;
}
/**
 * Audio param / event / theme binding metadata. Carries forward into IRNode.attributes['binding'] as a JSON-encoded string and is consumed by the binding resolver (bindParam JS helper + `pulp bindings check`).
 */
export interface Binding {
  kind: "param" | "event" | "theme" | "none";
  /**
   * Designer-supplied identifier. Matches against ParamInfo.name for kind=param. Blank means TODO_BIND.
   */
  key: string;
  source: "figma-component-property" | "figma-name-suffix" | "none";
  /**
   * False initially; flipped to true by `pulp bindings` after StateStore registration check.
   */
  resolved: boolean;
}
/**
 * Figma-specific metadata captured at extraction. Parser flattens to IRNode.attributes['figma:*'] keys as JSON-encoded strings.
 */
export interface FigmaMetadata {
  component_key?: string;
  component_set_name?: string;
  main_component_id?: string;
  main_component_name?: string;
  library_widget_kind?: string;
  library_version?: string;
  component_properties?: {
    [k: string]: unknown;
  };
  variant_properties?: {
    [k: string]: string;
  };
  parent_id?: string | null;
  z_order?: number;
  absolute_transform?: number[][];
  visible?: boolean;
  locked?: boolean;
  blend_mode?: string;
  /**
   * True if this node is a mask layer. Pulp does not support masks; importer emits opaque + diagnostic.
   */
  mask?: boolean;
  [k: string]: unknown;
}
