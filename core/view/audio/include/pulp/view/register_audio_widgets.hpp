#pragma once

namespace pulp::view {
/// Registers the optional audio display factories. Safe and cheap to call on
/// every editor creation; direct applications should call it once at startup.
void register_audio_widgets();
} // namespace pulp::view
