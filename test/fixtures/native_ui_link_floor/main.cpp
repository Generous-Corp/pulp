#include <pulp/format/standalone.hpp>

#include <memory>

std::unique_ptr<pulp::format::Processor> make_native_ui_link_floor();

int main() {
    pulp::format::StandaloneApp app(&make_native_ui_link_floor);
    pulp::format::StandaloneConfig config;
    config.input_channels = 2;
    config.output_channels = 2;
    config.persist_settings = false;
    config.show_settings_tab = false;
    app.set_config(config);
    return app.run_with_editor(true) ? 0 : 1;
}
