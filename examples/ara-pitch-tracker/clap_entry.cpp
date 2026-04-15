// ara-pitch-tracker CLAP entry point.
#include "src/pitch_tracker.hpp"
#include <pulp/format/clap_entry.hpp>

namespace {
std::unique_ptr<pulp::format::Processor> make() {
    return std::make_unique<pulp::examples::ara_pitch_tracker::PitchTracker>();
}
} // namespace

PULP_CLAP_PLUGIN(make)
