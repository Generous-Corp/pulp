// PulpRustGain CLAP entry point.
#include <pulp/format/clap_entry.hpp>

#include <memory>

namespace pulp::format {
class Processor;
}
namespace pulp::examples {
std::unique_ptr<pulp::format::Processor> create_rust_gain();
}

PULP_CLAP_PLUGIN(pulp::examples::create_rust_gain)
