// plugin_slot_auv3_internal.hpp - testable internals for the AUv3 host slot.
//
// Pure decisions the slot makes about an instantiated unit, kept free of
// AudioToolbox types so a test can check them without loading a component.
#pragma once

#include <cstddef>

namespace pulp::host::auv3_internal {

/// Logged when an out-of-process unit arrives without the buses its type
/// requires. The usual cause is a component registered with this process only
/// (AudioComponentRegister): the out-of-process host cannot see that
/// registration, hands back a unit with no buses, and allocateRenderResources
/// later fails with kAudioUnitErr_FailedInitialization (-10875).
inline constexpr const char* kRemoteUnitWithoutBuses =
    "the out-of-process unit has no audio buses; a component registered only in this "
    "process cannot be hosted out of process, so load it in process "
    "(AuV3LoadOptions::out_of_process = false)";

/// True when a unit loaded out of process reports no input and no output
/// buses although its component type always carries audio (an instrument,
/// effect, music effect or generator). A MIDI processor may legitimately have
/// none, so the caller passes `expects_audio` from the component type.
constexpr bool remote_unit_lost_its_buses(bool out_of_process, bool expects_audio,
                                          std::size_t inputs, std::size_t outputs) noexcept {
    return out_of_process && expects_audio && inputs == 0 && outputs == 0;
}

} // namespace pulp::host::auv3_internal
