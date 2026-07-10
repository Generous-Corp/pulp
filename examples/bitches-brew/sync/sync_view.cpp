// Out-of-line create_view() so the editor's headers stay out of the
// audio-only translation units.
#include "sync_processor.hpp"
#include "sync_ui.hpp"

namespace pulp::examples::brew {

std::unique_ptr<view::View> SyncProcessor::create_view() {
    return std::make_unique<SyncUi>(state(), *this);
}

}  // namespace pulp::examples::brew
