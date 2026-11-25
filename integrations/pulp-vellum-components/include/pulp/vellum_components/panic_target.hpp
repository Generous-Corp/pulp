#pragma once

/// @file panic_target.hpp
/// Connects the stock `PanicButton` to a `format::Processor`'s panic request.
///
/// @code
/// root.add_child(std::make_unique<PanicButton>(panic_target(processor)));
/// @endcode
///
/// The target holds a reference to the processor, so the button must not
/// outlive it; an editor's views never do.

#include <pulp/format/panic.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/vellum_components/panic_button.hpp>

namespace pulp::vellum_components {

/// What `caps` lets a panic request do, in the button's terms.
inline PanicAvailability panic_availability(const format::PanicCapabilities& caps) noexcept {
    if (!caps.serviced)
        return PanicAvailability::unavailable;
    return caps.midi_out ? PanicAvailability::voices_and_midi : PanicAvailability::voices_only;
}

/// A `PanicTarget` that raises `processor.request_panic()` and reports
/// `processor.panic_capabilities()`.
inline PanicTarget panic_target(format::Processor& processor) {
    PanicTarget target;
    target.request = [&processor](bool release_only) {
        processor.request_panic(release_only ? format::PanicMode::release
                                             : format::PanicMode::immediate);
    };
    target.availability = [&processor] {
        return panic_availability(processor.panic_capabilities());
    };
    return target;
}

} // namespace pulp::vellum_components
