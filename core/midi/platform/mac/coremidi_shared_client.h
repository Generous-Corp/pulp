#pragma once

// Internal to the macOS/iOS CoreMIDI backend. Exposed as a header so the
// one-client-per-process invariant can be asserted from a test rather than
// only reasoned about.

#include <CoreMIDI/CoreMIDI.h>

namespace pulp::midi::mac {

/// The process-wide CoreMIDI client every Pulp MIDI port is created against.
///
/// Returns the same handle for the lifetime of the process, or 0 if the
/// client could not be created. Never dispose the returned client: CoreMIDI
/// ties port and endpoint lifetime to it, so disposing it would invalidate
/// every port still open elsewhere in the process.
MIDIClientRef shared_client();

}  // namespace pulp::midi::mac
