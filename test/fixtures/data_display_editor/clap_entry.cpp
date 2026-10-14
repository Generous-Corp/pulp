#include <pulp/format/clap_entry.hpp>

#include <memory>

std::unique_ptr<pulp::format::Processor> make_data_display_fixture();

PULP_CLAP_PLUGIN(make_data_display_fixture)
