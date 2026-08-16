#include <pulp/format/clap_entry.hpp>

#include <memory>

std::unique_ptr<pulp::format::Processor> make_native_ui_link_floor();

PULP_CLAP_PLUGIN(make_native_ui_link_floor)
