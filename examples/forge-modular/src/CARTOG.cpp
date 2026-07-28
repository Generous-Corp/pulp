#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>

#include <cstdio>
#include <string>
#include <vector>

// Records where every module in the rack keeps its jacks.
//
// Nothing on disk describes a module's ports. A plugin.json lists a module's
// slug, name and tags and stops there; Core's manifest is no different. Port
// indices, port names and jack positions exist only in each module's compiled
// widget code. That leaves anything drawing or wiring a patch from outside
// Rack guessing -- which is why a generated patch reaches an audio interface
// on input 0 alone rather than as a stereo pair, and why a preview can only
// dock cables to the edge of a panel it cannot read.
//
// Inside Rack the same information is simply available. `RackWidget` hands out
// its ModuleWidgets, each of those hands out its PortWidgets, and a PortWidget
// knows its index, its position, and -- through PortInfo -- the name its
// author gave it. Walking that once and writing it down turns every module the
// user actually owns into something we can wire exactly.
//
// So this module is a scanner, not an instrument: add it, press SCAN, and
// every module currently in the rack is recorded to
// `<Rack user dir>/forge-portmap.json`. It records only what is on screen,
// which is the honest scope -- a module nobody has ever placed cannot be
// mapped, and the fallback for those stays what it was.

namespace {

/// JSON escaping for names that came from other people's plugins.
std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) continue;
        else o += c;
    }
    return o;
}

struct CARTOGModule : rack::engine::Module {
    using L = forge_modular::CARTOGLayout;

    // Set by the widget when a scan finishes, so the panel light can report it
    // without the widget reaching into audio-thread state.
    float lit = 0.f;

    CARTOGModule() { forge_modular::config_CARTOG(this); }

    void process(const ProcessArgs& args) override {
        // The button is read by the widget, which is where scene access is
        // legal. Nothing here is audio -- the light decay is the only work.
        lit = std::max(0.f, lit - args.sampleTime * 0.5f);
        lights[L::DONE_LIGHT].setBrightnessSmooth(lit, args.sampleTime);
    }
};

struct CARTOGWidget : rack::app::ModuleWidget {
    CARTOGModule* mod = nullptr;
    bool held = false;

    explicit CARTOGWidget(CARTOGModule* m) : mod(m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/CARTOG.svg"),
            rack::asset::plugin(pluginInstance, "res/CARTOG-dark.svg")));
        forge_modular::place_CARTOG(this, m);
    }

    /// Walk the rack and write down every module's ports.
    ///
    /// Runs on the UI thread from step(), which is the only place the scene
    /// may be touched -- doing this from process() would be both a data race
    /// and a real-time violation.
    void scan() {
        if (!APP || !APP->scene || !APP->scene->rack) return;

        std::string out = "{\n  \"modules\": [\n";
        bool first_mod = true;

        for (rack::app::ModuleWidget* mw : APP->scene->rack->getModules()) {
            if (!mw || !mw->module || !mw->module->model) continue;
            rack::plugin::Model* model = mw->module->model;
            if (!model->plugin) continue;

            if (!first_mod) out += ",\n";
            first_mod = false;

            out += "    {\n";
            out += "      \"plugin\": \"" + esc(model->plugin->slug) + "\",\n";
            out += "      \"model\": \"" + esc(model->slug) + "\",\n";
            out += "      \"pluginVersion\": \"" + esc(model->plugin->version) + "\",\n";
            // Panel size lets a preview lay modules out at true width without
            // parsing anyone's artwork.
            out += "      \"size\": [" + std::to_string(mw->box.size.x) + ", " +
                   std::to_string(mw->box.size.y) + "],\n";

            for (int pass = 0; pass < 2; ++pass) {
                const bool inputs = (pass == 0);
                out += inputs ? "      \"inputs\": [" : "      \"outputs\": [";
                std::vector<rack::app::PortWidget*> ports =
                    inputs ? mw->getInputs() : mw->getOutputs();
                bool first = true;
                for (rack::app::PortWidget* pw : ports) {
                    if (!pw) continue;
                    if (!first) out += ",";
                    first = false;
                    std::string name;
                    if (rack::engine::PortInfo* info = pw->getPortInfo())
                        name = info->getName();
                    // Centre, not corner: a cable plugs into the middle of a
                    // jack, and box.pos is the widget's top-left.
                    const float cx = pw->box.pos.x + pw->box.size.x * 0.5f;
                    const float cy = pw->box.pos.y + pw->box.size.y * 0.5f;
                    out += "\n        {\"index\": " + std::to_string(pw->portId) +
                           ", \"name\": \"" + esc(name) + "\"" +
                           ", \"x\": " + std::to_string(cx) +
                           ", \"y\": " + std::to_string(cy) + "}";
                }
                out += first ? "]" : "\n      ]";
                out += inputs ? ",\n" : "\n";
            }
            out += "    }";
        }
        out += "\n  ]\n}\n";

        const std::string path = rack::asset::user("forge-portmap.json");
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
            if (mod) mod->lit = 1.f;
            INFO("forge: wrote port map to %s", path.c_str());
        } else {
            WARN("forge: could not write %s", path.c_str());
        }
    }

    void step() override {
        rack::app::ModuleWidget::step();
        if (!mod) return;
        const bool down = mod->params[CARTOGModule::L::SCAN_PARAM].getValue() > 0.5f;
        if (down && !held) scan();      // on the press, not every frame held
        held = down;
    }
};

}  // namespace

rack::plugin::Model* modelCARTOG =
    rack::createModel<CARTOGModule, CARTOGWidget>("CARTOG");
