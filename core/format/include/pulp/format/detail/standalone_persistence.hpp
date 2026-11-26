#pragma once

namespace pulp::format::detail {

/// Disable AppKit's crash-recovery window restoration for headless launches.
///
/// This must run before the standalone creates or finishes launching its
/// NSApplication. Interactive launches leave the user's normal restoration
/// behavior unchanged.
void configure_standalone_persistence(bool headless);

} // namespace pulp::format::detail
