#pragma once

namespace pulp::view {
/// Registers the optional audio display factories. Safe and cheap to call on
/// every editor creation; direct applications should call it once at startup.
/// The build-tree INTERFACE_SOURCES shim covers simple direct consumers, but
/// installed consumers and applications that do not use a Pulp host must call
/// this function explicitly after linking `pulp::view-audio`.
void register_audio_widgets();
} // namespace pulp::view
