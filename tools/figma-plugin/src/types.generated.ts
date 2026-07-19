/* eslint-disable */
// AUTO-GENERATED from schema/figma-plugin-export-v1.json — DO NOT EDIT BY HAND.
// Regenerate via: npm run gen-types

/**
 * One interactive overlay on a faithful_svg node. The SVG underneath always renders; the overlay only adds interaction on top. `kind` selects the control: knob/fader/xy_pad translate or rotate `svg_patch_d`; the overlay kinds (dropdown/text_field/tab_group/stepper) and toggle position a control over [x,y,w,h]. Coordinates are in the SVG's own space.
 */
export type InteractiveElement = {
  [k: string]: unknown;
} & {
  /**
   * Control type. The C++ materializer (to_frame_elements in design_import_native_common.cpp) maps each to a DesignFrameElement::Kind the runtime already backs.
   */
  kind:
    | "knob"
    | "fader"
    | "toggle"
    | "dropdown"
    | "text_field"
    | "tab_group"
    | "stepper"
    | "swap"
    | "action"
    | "xy_pad"
    | "value_label"
    | "custom";
  /**
   * knob/fader/switch: pivot or baked-center X, SVG coords.
   */
  cx?: number;
  /**
   * knob/fader/switch: pivot or baked-center Y, SVG coords.
   */
  cy?: number;
  /**
   * knob: click-target radius, SVG coords.
   */
  hit_radius?: number;
  /**
   * Generic patch target. knob rotates this `d` around (cx,cy); fader/xy_pad translate it by value; a toggle with this set is a switch whose dot is this path. Empty if none was identified.
   */
  svg_patch_d?: string;
  /**
   * Initial normalized value (0..1).
   */
  default_value?: number;
  /**
   * toggle only: a press-flash command button (lights on press, clears on release) instead of the default sticky on/off flip. Maps to DesignFrameElement::flash.
   */
  flash?: boolean;
  /**
   * swap only: the frame index this swap-link button activates on click. Maps to DesignFrameElement::target_frame.
   */
  target_frame?: number;
  /**
   * action only: the command id fired on click (e.g. "octave_up"). Maps to DesignFrameElement::action.
   */
  action?: string;
  /**
   * value_label only: the initial readout string painted over the rect. Maps to DesignFrameElement::text.
   */
  text?: string;
  /**
   * value_label only: left-align the readout (for a "LABEL <value>" overlay whose value grows rightward). Maps to DesignFrameElement::value_left_align.
   */
  value_left_align?: boolean;
  /**
   * xy_pad only: initial normalized Y (0=top, 1=bottom); the X axis reuses default_value. Maps to DesignFrameElement::value_y.
   */
  default_value_y?: number;
  /**
   * P7 import report: which ladder rung resolved this control. 0=unset; 1=explicit identity; 2=Tier-1 affordance primitive; 3=name/token; 4=registered custom factory; 5=inert render + diagnostic.
   */
  resolution_rung?: number;
  /**
   * P7 import report: confidence (0..1) the resolved kind is correct. 1.0 = unset/legacy.
   */
  confidence_score?: number;
  /**
   * P7 import report: cross-signal conflicts detected (e.g. name says knob but geometry is a wide track+thumb). Empty = none; non-empty flags the control for review.
   */
  conflict_signals?: string[];
  /**
   * P7 import report: whether render-level verification (overlay covers its node, type doesn't contradict the skin) passed. Defaults true.
   */
  verification_pass?: boolean;
  /**
   * kind=custom (P7 Tier-3): the id the native overlay factory is registered under (register_design_control_factory). Maps to DesignFrameElement::factory_id.
   */
  factory_id?: string;
  /**
   * kind=custom: opaque props handed to the factory (typically JSON); Pulp does not parse them. Maps to DesignFrameElement::custom_props.
   */
  custom_props?: string;
  /**
   * Overlay box X (dropdown/text_field/tab_group/stepper/toggle, and fader/xy_pad track), SVG coords.
   */
  x?: number;
  /**
   * Overlay box Y, SVG coords.
   */
  y?: number;
  /**
   * Overlay box width, SVG coords.
   */
  w?: number;
  /**
   * Overlay box height, SVG coords.
   */
  h?: number;
  /**
   * dropdown/stepper: the shown value(s); tab_group: the tab labels.
   */
  options?: string[];
  /**
   * dropdown/tab_group/stepper: initially selected option index.
   */
  selected_index?: number;
  /**
   * text_field: placeholder shown until typed.
   */
  placeholder?: string;
  /**
   * text_field: the design's own field background ("#RRGGBB") so the overlay edge is seamless.
   */
  bg_color?: string;
  /**
   * Human-readable control name from the design's caption (e.g. "DEPTH"); the name a host surfaces for the generated parameter.
   */
  label?: string;
  /**
   * Figma node id — provenance for the inspector's Wiring lens (maps a control back to its source node). NOT the host-param key; that is param_key.
   */
  source_node_id?: string;
  /**
   * Host-parameter binding key for a geometry-detected control (e.g. "filter.cutoff"). Maps to IRNode.interactive_elements[].param_key → DesignFrameElement::param_key; DesignFrameView routes gestures on it to the framework-agnostic HostParamSurface, so the view runs unchanged against whatever parameter system the host provides. Absent for an unbound knob.
   */
  param_key?: string;
};

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
   * Pulp Figma Library version metadata at time of export. Plugin reads tools/figma-plugin/library-manifest.json and copies the active section here. Emitted as null when no library snapshot is available (serialize.ts: ctx.libraryManifest ?? null).
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
  } | null;
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
  /**
   * Deduplicated catalog of every font (family + style + weight + italic) referenced by text nodes in the export. Drives Pulp's runtime font resolution: the consumer (Skia SkFontMgr) looks up each entry against system fonts, falls back to Pulp's bundled OFL set (Inter / Roboto / etc.), and emits a `recognition_unavailable` diagnostic when neither resolves. Note: the Figma plugin API intentionally does NOT expose font binaries, so `asset_id` stays unset for plain captures; it's populated only when the plugin's drag-drop escape hatch (issue follow-up) has carried a user-supplied TTF/OTF in `asset_manifest.assets`.
   */
  font_family_assets?: FontFamilyAsset[];
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
    | "unsupported_node"
    | "unresolved_asset"
    | "snapshot_semantics_warning"
    | "legacy_field_shortcut"
    | "capture_partial"
    | "fallback_used"
    | "recognition_unavailable";
  anchor_id?: string;
  property?: string;
}
/**
 * One row in the deduplicated font catalog carried on `font_family_assets`. Populated by extractScene().collectFontFamilyAssets — every (family, style, weight, italic) tuple referenced by a text node appears exactly once, in first-encounter order.
 */
export interface FontFamilyAsset {
  /**
   * Figma font family — e.g. "Inter", "Clash Grotesk".
   */
  family: string;
  /**
   * Figma style string — verbatim from TextNode.fontName.style ("Regular", "Semi Bold", "Italic", "Bold Italic", etc.).
   */
  style: string;
  /**
   * Numeric font-weight when figma exposes it on the text node (TextNode.fontWeight). Omitted when not numeric (mixed weight).
   */
  weight?: number;
  /**
   * True when the style string indicates italic (case-insensitive substring match on "italic"). Lets the runtime resolve italic variants without re-parsing the style string.
   */
  italic?: boolean;
  /**
   * Reference into asset_manifest.assets. Set only when the user has supplied a TTF/OTF via the plugin's drag-drop escape hatch (follow-up). For plain captures this is absent; the runtime falls back to system-font lookup.
   */
  asset_id?: string;
}
/**
 * An IR node as emitted by serialize.ts::toEnvelopeNode. Semantic audio-widget data lives at the NODE ROOT (audio_widget/label/min/max/default + attributes.binding), NOT in nested audio/binding sub-objects. The C++ parser (core/view/src/design_ir_json.cpp::parse_ir_node) reads these root-level fields directly.
 */
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
  /**
   * Recognized Pulp audio-widget kind. Emitted at the node root when serialize.ts sees node.library_widget_kind. The C++ parser maps this onto IRNode.audio_widget (enum). Equal to figma.library_widget_kind when present.
   */
  audio_widget?: "knob" | "fader" | "meter" | "xy_pad" | "waveform" | "spectrum";
  /**
   * Audio-widget label. Root-level; maps to IRNode.audio_label.
   */
  label?: string;
  /**
   * Audio-widget minimum value. Root-level; maps to IRNode.audio_min.
   */
  min?: number;
  /**
   * Audio-widget maximum value. Root-level; maps to IRNode.audio_max.
   */
  max?: number;
  /**
   * Audio-widget default value. Root-level; maps to IRNode.audio_default.
   */
  default?: number;
  attributes?: Attributes;
  style?: Style;
  layout?: Layout;
  /**
   * Resize constraints in Figma's Plugin-API spelling. Passed through untranslated; design_ir_json.cpp normalizes and codegen lowers to flex within the parent. Only emitted for nodes positioned in the parent's coordinate space (not flowing auto-layout children).
   */
  constraints?: {
    horizontal?: "MIN" | "MAX" | "CENTER" | "STRETCH" | "SCALE";
    vertical?: "MIN" | "MAX" | "CENTER" | "STRETCH" | "SCALE";
  };
  figma?: FigmaMetadata;
  /**
   * When type=image, references asset_manifest.assets[*].asset_id
   */
  asset_ref?: string;
  /**
   * Faithful-vector lane (Plan B). 'faithful_svg' makes the C++ materializer render svg_asset_id via DesignFrameView with interactive_elements overlays, instead of widget-recognition. Maps to IRNode.render_mode.
   */
  render_mode?: "normal" | "faithful_svg";
  /**
   * When render_mode=faithful_svg, references the asset_manifest entry (mime image/svg+xml) holding this node's SVG export. Maps to IRNode.svg_asset_id.
   */
  svg_asset_id?: string;
  /**
   * Source-identified interactive overlays for a faithful_svg render. Maps to IRNode.interactive_elements.
   */
  interactive_elements?: InteractiveElement[];
  /**
   * Alternate states of a faithful_svg node, each itself a faithful_svg node with its own svg_asset_id and interactive_elements. Maps to IRNode.alternate_frames; the importer materializes each via DesignFrameView::add_frame. ORDER IS SIGNIFICANT: this node is frame 0 and entry i is frame i+1, which is the index a swap element's target_frame names. Omit for a single-state design.
   */
  alternate_frames?: Node[];
  children?: Node[];
}
/**
 * Free-form string passthrough map emitted at the node root by serialize.ts. Each entry becomes an IRNode.attributes[*] entry in the C++ parser. Emitted only when at least one attribute is present.
 */
export interface Attributes {
  /**
   * Audio param / event / theme binding identifier (from node.audio_binding). Consumed by the binding resolver (bindParam JS helper + `pulp bindings check`).
   */
  binding?: string;
  /**
   * Display units for the audio-widget value (from node.audio_units).
   */
  units?: string;
  [k: string]: string;
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
 * Maps to IRLayout. Padding is nested as an object here; parser flattens to padding_top/right/bottom/left. The camelCase keys (rowGap, flexGrow, alignSelf, gridTemplate*, ...) use the consumer's spelling — the exact member names design_ir_json.cpp::parse_ir_layout reads — so the schema can never declare a key nobody consumes.
 */
export interface Layout {
  display?: "flex" | "grid" | "none";
  direction?: "row" | "column";
  gap?: number;
  rowGap?: number;
  columnGap?: number;
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
  alignContent?: "space-between";
  flexGrow?: number;
  alignSelf?: "flex-start" | "flex-end" | "center" | "stretch";
  aspectRatio?: number;
  gridTemplateColumns?: string;
  gridTemplateRows?: string;
  gridColumn?: string;
  gridRow?: string;
  width_mode?: "fixed" | "hug" | "fill";
  height_mode?: "fixed" | "hug" | "fill";
}
/**
 * Figma-specific identity / provenance, packed into the node's `figma` sub-object by serialize.ts. The C++ parser reads figma.library_widget_kind / figma.component_key as needed. The six positional fields below are always present; the component/library fields are emitted only when the node is an instance / recognized library widget.
 */
export interface FigmaMetadata {
  parent_id: string | null;
  z_order: number;
  /**
   * Node.relative_transform copied through as a 2x3 affine matrix.
   */
  absolute_transform: number[][];
  visible: boolean;
  locked: boolean;
  blend_mode: string;
  component_key?: string;
  component_set_name?: string;
  main_component_id?: string;
  main_component_name?: string;
  /**
   * True when the instance's main component lives in a remote team library. Emitted only when true.
   */
  remote_library?: boolean;
  /**
   * Recognized Pulp widget kind, mirrors the node-root audio_widget field.
   */
  library_widget_kind?: "knob" | "fader" | "meter" | "xy_pad" | "waveform" | "spectrum";
  library_version?: string;
  component_properties?: {
    [k: string]: unknown;
  };
  variant_properties?: {
    [k: string]: string;
  };
  [k: string]: unknown;
}
