#pragma once

// Host-owned container view for an embedded plugin editor.
//
// CLAP `set_parent` and VST3 `IPlugView::attached` both CONSUME a parent view:
// the plugin inserts its own native view into what the host hands it, and never
// hands a view back. PluginSlot::HostedEditor is the other way round — the slot
// returns a `native_handle` that the host embeds. A host-owned container
// reconciles the two: the slot creates a container, gives it to the plugin as
// the parent, and returns the container as its HostedEditor::native_handle.
//
// AU v2's CocoaUI factory does return an NSView, so it does not need the
// container to bridge anything — it wraps its view in one anyway so that
// resize and teardown are uniform across formats.
//
// The container is created already inserted into `parent_window`'s content
// view. That ordering is deliberate: EditorAttachment::create() calls
// create_hosted_editor() BEFORE attach_native_child_view(), so a container that
// deferred insertion would have the plugin run set_parent()/attached() against
// a view with no window. Editors that bring up Metal/OpenGL layers on attach
// misbehave in that state. The later attach_native_child_view() re-parents the
// container within the same window, which is a cheap addSubview: move.
//
// Platforms without a native-window seam compile a stub whose create returns
// nullptr; create_hosted_editor() then returns nullptr and EditorAttachment
// already treats that as "no editor".

#include <cstdint>

namespace pulp::host {

/// Create a container view sized (width, height), inserted into
/// `parent_window`'s content view. Returns nullptr when `parent_window` is null
/// or the platform has no native-window seam. The caller owns the result and
/// must release it with destroy_editor_container().
void* create_editor_container(void* parent_window, uint32_t width, uint32_t height);

/// Resize an existing container. No-op when `container` is null.
void resize_editor_container(void* container, uint32_t width, uint32_t height);

/// Remove the container from its superview and release it. No-op when null.
void destroy_editor_container(void* container);

/// Backing scale factor of `parent_window` (2.0 on Retina), for the plugin's
/// scale negotiation. Returns 1.0 when unknown or unsupported.
double editor_container_scale(void* parent_window);

} // namespace pulp::host
