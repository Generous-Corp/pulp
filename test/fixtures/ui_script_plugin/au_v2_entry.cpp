#include "ui_script_gain.hpp"

#include <pulp/format/au_v2_entry.hpp>

// The AU class name fixes the exported factory symbol, which follows the
// plug-in target's name, so a second plug-in built from this entry names its own.
#ifndef PULP_UI_SCRIPT_FIXTURE_AU_CLASS
#define PULP_UI_SCRIPT_FIXTURE_AU_CLASS PulpUiScriptFixtureAU
#endif

PULP_AU_PLUGIN(PULP_UI_SCRIPT_FIXTURE_AU_CLASS, pulp::test_fixtures::create_ui_script_gain)
