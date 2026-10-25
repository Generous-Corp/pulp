#include "ui_script_gain.hpp"

#include <pulp/format/standalone.hpp>

int main() {
    pulp::format::StandaloneApp app(pulp::test_fixtures::create_ui_script_gain);
    return app.run_with_editor(false) ? 0 : 1;
}
