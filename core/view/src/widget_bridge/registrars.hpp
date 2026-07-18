#pragma once

// Internal seam for WidgetBridge's ~75 sub-API registrars.
//
// Why this lives under core/view/src/ and NOT in the public include tree:
// each registrar used to be a private member function declared inside the
// PUBLIC `widget_bridge.hpp`. Every time a new bridge API was added, that
// public header changed, so every translation unit that includes
// widget_bridge.hpp (the whole view/render/host surface) was recompiled --
// a large blast radius for what is a purely internal implementation detail.
//
// The registrars are now free-standing static methods of `BridgeRegistrars`,
// a single `friend` of WidgetBridge. Adding or removing a registrar touches
// only this internal header plus the owning .cpp and the register_api() table
// in widget_bridge.cpp -- never the public header. `BridgeRegistrars` is a
// friend so each static may reach WidgetBridge's private state through the
// `WidgetBridge&` it receives.

namespace pulp::view {

class WidgetBridge;

// All registrars take the owning bridge by reference. They are grouped and
// ORDER-iterated by WidgetBridge::register_api(); registration order is
// load-bearing (a later group may overwrite an earlier group's JS name), so
// the authoritative order lives in that table, not here.
struct BridgeRegistrars {
    static void register_accessibility_api(WidgetBridge& self);
    static void register_dom_api(WidgetBridge& self);
    static void register_hover_event_api(WidgetBridge& self);
    static void register_pointer_event_api(WidgetBridge& self);
    static void register_wheel_event_api(WidgetBridge& self);
    static void register_context_menu_event_api(WidgetBridge& self);
    static void register_drop_event_api(WidgetBridge& self);
    static void register_widget_style_background_color_api(WidgetBridge& self);
    static void register_widget_style_shadow_api(WidgetBridge& self);
    static void register_widget_style_opacity_api(WidgetBridge& self);
    static void register_widget_style_overflow_api(WidgetBridge& self);
    static void register_widget_style_background_gradient_api(WidgetBridge& self);
    static void register_widget_style_box_shadow_api(WidgetBridge& self);
    static void register_widget_style_cursor_direction_api(WidgetBridge& self);
    static void register_widget_style_filter_clip_api(WidgetBridge& self);
    static void register_widget_style_blend_api(WidgetBridge& self);
    static void register_widget_style_rn_compat_api(WidgetBridge& self);
    static void register_widget_style_state_api(WidgetBridge& self);
    static void register_widget_style_background_repeat_api(WidgetBridge& self);
    static void register_widget_style_mask_object_api(WidgetBridge& self);
    static void register_widget_style_background_subproperty_api(WidgetBridge& self);
    static void register_widget_style_visibility_api(WidgetBridge& self);
    static void register_widget_style_interaction_api(WidgetBridge& self);
    static void register_layout_grid_api(WidgetBridge& self);
    static void register_layout_flex_api(WidgetBridge& self);
    static void register_layout_query_api(WidgetBridge& self);
    static void register_layout_box_model_api(WidgetBridge& self);
    static void register_layout_position_api(WidgetBridge& self);
    static void register_list_style_api(WidgetBridge& self);
    static void register_metadata_removal_api(WidgetBridge& self);
    static void register_metadata_source_api(WidgetBridge& self);
    static void register_metadata_computed_api(WidgetBridge& self);
    static void register_platform_services_ai_api(WidgetBridge& self);
    static void register_platform_services_exec_api(WidgetBridge& self);
    static void register_query_service_api(WidgetBridge& self);
    static void register_platform_services_dialog_api(WidgetBridge& self);
    static void register_platform_services_clipboard_api(WidgetBridge& self);
    static void register_state_binding_api(WidgetBridge& self);
    static void register_storage_key_value_api(WidgetBridge& self);
    static void register_asset_loading_api(WidgetBridge& self);
    static void register_font_assets_api(WidgetBridge& self);
    static void register_svg_api(WidgetBridge& self);
    static void register_shader_widget_api(WidgetBridge& self);
    static void register_shader_canvas_api(WidgetBridge& self);
    static void register_theme_api(WidgetBridge& self);
    static void register_tokens_api(WidgetBridge& self);
    static void register_widget_assets_api(WidgetBridge& self);
    static void register_widget_schema_api(WidgetBridge& self);
    static void register_widget_factory_controls_api(WidgetBridge& self);
    static void register_widget_value_controls_api(WidgetBridge& self);
    static void register_widget_factory_form_api(WidgetBridge& self);
    static void register_widget_factory_container_api(WidgetBridge& self);
    static void register_widget_factory_composite_api(WidgetBridge& self);
    static void register_widget_value_list_api(WidgetBridge& self);
    static void register_widget_factory_text_editor_api(WidgetBridge& self);
    static void register_widget_factory_design_system_api(WidgetBridge& self);
    static void register_widget_value_label_api(WidgetBridge& self);
    static void register_widget_value_basic_api(WidgetBridge& self);
    static void register_widget_typography_api(WidgetBridge& self);
    static void register_widget_typography_color_api(WidgetBridge& self);
    static void register_widget_typography_decoration_api(WidgetBridge& self);
    static void register_widget_typography_overflow_api(WidgetBridge& self);
    static void register_widget_typography_extended_api(WidgetBridge& self);
    static void register_widget_typography_shadow_shorthand_api(WidgetBridge& self);
    static void register_widget_value_content_api(WidgetBridge& self);
    static void register_widget_text_runs_api(WidgetBridge& self);
    static void register_widget_border_box_api(WidgetBridge& self);
    static void register_widget_outline_api(WidgetBridge& self);
    static void register_widget_border_radius_api(WidgetBridge& self);
    static void register_widget_border_side_api(WidgetBridge& self);
    static void register_runtime_api(WidgetBridge& self);
    static void register_animation_api(WidgetBridge& self);
    static void register_animation_style_api(WidgetBridge& self);
    static void register_canvas2d_api(WidgetBridge& self);
    static void register_gpu_api(WidgetBridge& self);
};

} // namespace pulp::view
