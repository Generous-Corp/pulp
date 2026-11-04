#include "editor_probe_processor.hpp"

#include <pulp/format/au_v2_entry.hpp>

// Each AU target names its own component class (PULP_FIXTURE_AU_CLASS), since
// the class name is also its factory function's name.
PULP_AU_PLUGIN(PULP_FIXTURE_AU_CLASS, pulp::test_fixtures::create_editor_probe)
