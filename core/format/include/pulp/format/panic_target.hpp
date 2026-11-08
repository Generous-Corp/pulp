#pragma once

/// @file panic_target.hpp
/// Connects the stock `view::PanicButton` to a `Processor`'s panic request.
///
/// @code
/// auto panic = std::make_unique<view::PanicButton>(format::panic_target(processor));
/// root.add_child(std::move(panic));
/// @endcode
///
/// The target holds a reference to the processor, so the button must not
/// outlive it; an editor's views never do.

#include <pulp/format/panic.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/view/panic_button.hpp>

namespace pulp::format {

/// What `caps` lets a panic request do, in the button's terms.
inline view::PanicAvailability panic_availability(const PanicCapabilities& caps) noexcept {
    if (!caps.serviced)
        return view::PanicAvailability::unavailable;
    return caps.midi_out ? view::PanicAvailability::voices_and_midi
                         : view::PanicAvailability::voices_only;
}

/// A `view::PanicTarget` that raises `processor.request_panic()` and reports
/// `processor.panic_capabilities()`.
inline view::PanicTarget panic_target(Processor& processor) {
    view::PanicTarget target;
    target.request = [&processor](bool release_only) {
        processor.request_panic(release_only ? PanicMode::release : PanicMode::immediate);
    };
    target.availability = [&processor] {
        return panic_availability(processor.panic_capabilities());
    };
    return target;
}

} // namespace pulp::format
