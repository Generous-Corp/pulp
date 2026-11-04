// The standalone entry of the editor UI package fixture plug-ins that build a
// Standalone. pulp_add_plugin uses main.cpp in the plug-in's source directory.
// The tests read these targets' link closures; nothing runs this app.

#include "editor_probe_processor.hpp"

#include <pulp/format/standalone.hpp>

int main() {
    pulp::format::StandaloneApp app(pulp::test_fixtures::create_editor_probe);
    return app.run_with_editor(false) ? 0 : 1;
}
