#include "plugin.hpp"

#include <cmath>

#include <pulp/format/rack/module_descriptor.hpp>

#include "portmap_merge.hpp"

#include <cxxabi.h>

#include <cstdio>
#include <cstdlib>
#include <typeinfo>
#include <set>
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
// So this module is a scanner, not an instrument: add it, and every module
// currently in the rack is recorded to `<Rack user dir>/forge-portmap.json`,
// followed once per session by every model that is installed but not placed.
//
// Both halves are needed. Placed modules are the authoritative measurement
// and win where they overlap. But scanning ONLY what is on screen leaves a
// gap that cannot be seen: an unmapped module still draws, just as a
// faceplate with labels floating over nothing, so the map reads as complete
// while a preview quietly invents every panel it was never told about.

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
    bool swept_ = false;

    explicit CARTOGWidget(CARTOGModule* m) : mod(m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/CARTOG.svg"),
            rack::asset::plugin(pluginInstance, "res/CARTOG-dark.svg")));
        forge_modular::place_CARTOG(this, m);
    }

    /// A float as JSON, and whether it can BE JSON.
    ///
    /// `std::to_string(INFINITY)` is "inf", which no JSON parser accepts —
    /// and Rack uses infinite bounds for widgets that size themselves, so a
    /// single auto-sizing BlankPanel wrote `"w": inf` and the WHOLE port map
    /// stopped parsing. Not that module's entry: the whole file. Every vendor
    /// module then drew with no knobs and an UNMAPPED badge, because the app
    /// could not read the map at all, and nothing said why.
    static bool finite(float v) { return std::isfinite(v); }
    static std::string num(float v) { return std::to_string(v); }

    /// Walk the rack and write down every module's ports.
    ///
    /// Walk the widget tree, picking out what nothing enumerates.
    ///
    /// Params and ports have accessors; lights and displays do not, so the
    /// only way to find them is to descend. Positions accumulate on the way
    /// down because widgets nest -- a display's LEDs are children OF the
    /// display, and recording their local coordinates would stack every one
    /// of them in the panel's top-left corner.
    ///
    /// Screws and shadows are skipped: they are chrome every panel has, they
    /// tell a reader nothing, and including them would bury the widgets that
    /// do.
    static void collect(rack::widget::Widget* w, rack::math::Vec origin,
                        std::vector<std::string>& lights,
                        std::vector<std::string>& displays,
                        bool lights_only = false) {
        for (rack::widget::Widget* child : w->children) {
            if (!child) continue;
            const rack::math::Vec at = origin.plus(child->box.pos);
            const float cx = at.x + child->box.size.x * 0.5f;
            const float cy = at.y + child->box.size.y * 0.5f;

            if (dynamic_cast<rack::app::CircularShadow*>(child) ||
                dynamic_cast<rack::app::SvgPanel*>(child)) {
                continue;                                  // chrome, not content
            }
            const bool geometry_is_real =
                finite(cx) && finite(cy) &&
                finite(child->box.size.x) && finite(child->box.size.y);
            if (dynamic_cast<rack::app::ModuleLightWidget*>(child)) {
                if (!geometry_is_real) { collect(child, at, lights, displays,
                                                 lights_only); continue; }
                lights.push_back("{\"x\": " + std::to_string(cx) +
                                 ", \"y\": " + std::to_string(cy) +
                                 ", \"w\": " + std::to_string(child->box.size.x) +
                                 ", \"h\": " + std::to_string(child->box.size.y) + "}");
                continue;                                  // its children are its own
            }
            if (dynamic_cast<rack::app::ParamWidget*>(child) ||
                dynamic_cast<rack::app::PortWidget*>(child)) {
                // Already enumerated -- but DESCEND anyway, for LIGHTS ONLY.
                //
                // An LED button is a ParamWidget with its light as a CHILD, so
                // stopping here reported a panel covered in lit buttons as
                // having no lights at all: Fundamental's Mutes is ten of them,
                // and SEQ3 lost eleven.
                //
                // Lights only, because a knob's insides are the knob's. Rack
                // wraps a rotating SvgKnob in a TransformWidget, and treating
                // those as content put a 28x28 "display" under every knob in
                // the library -- 34 of them across three modules.
                collect(child, at, lights, displays, /*lights_only=*/true);
                continue;
            }

            // Wrappers, not content. A FramebufferWidget is Rack's render
            // cache and an SvgWidget is the panel image inside it; recording
            // either says "a display fills this panel", which is true of the
            // panel and useless. Descend through them to what they hold.
            if (dynamic_cast<rack::widget::FramebufferWidget*>(child) ||
                dynamic_cast<rack::widget::SvgWidget*>(child) ||
                dynamic_cast<rack::widget::TransparentWidget*>(child)) {
                collect(child, at, lights, displays, lights_only);
                continue;
            }

            // Screws, by NAME rather than by type. rack::app::SvgScrew is one
            // implementation and vendors subclass their own -- Fundamental's
            // ThemedScrew is not an SvgScrew and slipped straight through a
            // dynamic_cast. There is no common base to test, and four screws
            // per panel across a library is a lot of noise burying the one
            // widget that matters.
            const std::string tn = type_name(child);
            if (tn.find("Screw") != std::string::npos) continue;

            // Anything left that occupies real space is SOMETHING -- a screen,
            // a scope, a text field. Recorded by bounds and type name, because
            // a rectangle we can say "a display lives here" about beats a
            // blank we say nothing about. Tiny leftovers are layout glue.
            // `inf >= 6.0f` is true, so the size test alone let an
            // auto-sizing widget through.
            if (!lights_only && geometry_is_real &&
                child->box.size.x >= 6.0f && child->box.size.y >= 6.0f) {
                displays.push_back("{\"x\": " + std::to_string(cx) +
                                   ", \"y\": " + std::to_string(cy) +
                                   ", \"w\": " + std::to_string(child->box.size.x) +
                                   ", \"h\": " + std::to_string(child->box.size.y) +
                                   ", \"type\": \"" + esc(tn) + "\"}");
            }
            collect(child, at, lights, displays, lights_only);
        }
    }

    /// A widget's C++ class, demangled, for saying WHAT is in a rectangle.
    static std::string type_name(rack::widget::Widget* w) {
        const char* raw = typeid(*w).name();
        int status = 0;
        char* nice = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
        std::string out = (status == 0 && nice) ? nice : raw;
        std::free(nice);
        // Just the class, not the namespace chain: "LedDisplay" reads, and
        // "rack::app::LedDisplay" is the same fact with more of it.
        const auto at = out.rfind("::");
        return at == std::string::npos ? out : out.substr(at + 2);
    }

    /// Runs on the UI thread from step(), which is the only place the scene
    /// may be touched -- doing this from process() would be both a data race
    /// and a real-time violation.
    /// Path of the note naming the model currently being built.
    std::string pending_path() const {
        return rack::asset::user("forge-cartog-pending.txt");
    }

    /// Models that were mid-build when Rack last died, so they killed it.
    ///
    /// Reading this also clears it: a model gets one chance to be blamed, and
    /// the quarantine is the accumulated verdict.
    std::set<std::string> quarantined() const {
        std::set<std::string> out;
        const std::string qp = rack::asset::user("forge-cartog-skip.txt");
        auto slurp = [](const std::string& p) {
            std::string s;
            if (FILE* f = std::fopen(p.c_str(), "rb")) {
                char buf[4096];
                std::size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                    s.append(buf, n);
                std::fclose(f);
            }
            return s;
        };
        auto lines = [&out](const std::string& s) {
            std::string cur;
            for (char c : s) {
                if (c == '\n') {
                    if (!cur.empty()) out.insert(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) out.insert(cur);
        };
        lines(slurp(qp));
        // Anything still named as pending never finished: it is the culprit.
        const std::string culprit = slurp(pending_path());
        if (!culprit.empty()) {
            lines(culprit);
            if (FILE* f = std::fopen(qp.c_str(), "w")) {
                for (const std::string& k : out)
                    std::fwrite((k + "\n").data(), 1, k.size() + 1, f);
                std::fclose(f);
            }
            std::remove(pending_path().c_str());
        }
        return out;
    }

    void mark_pending(const std::string& key) const {
        if (FILE* f = std::fopen(pending_path().c_str(), "w")) {
            std::fwrite(key.data(), 1, key.size(), f);
            std::fflush(f);                 // it must survive a hard crash
            std::fclose(f);
        }
    }

    void clear_pending() const { std::remove(pending_path().c_str()); }

    /// One module's entry, written from a widget that has been laid out.
    ///
    /// Split out from the rack walk so one measurement serves both a
    /// module the user placed and one built only to be measured.
    void emit_module(rack::app::ModuleWidget* mw,
                     rack::plugin::Model* model, std::string& out) {
        out += "    {\n";
        out += "      \"plugin\": \"" + esc(model->plugin->slug) + "\",\n";
        out += "      \"model\": \"" + esc(model->slug) + "\",\n";
        out += "      \"pluginVersion\": \"" + esc(model->plugin->version) + "\",\n";
        // Which scanner measured this, so a later one can tell that an
        // entry it merged forward records less than it would have. A map
        // is merged, never rewritten, so entries outlive the scanner that
        // made them: without this, a module measured before controls were
        // recorded at all reports a matching plugin version and reads as
        // faithful. Keep in step with PortMap::kScanVersion.
        out += "      \"scan\": 3,\n";
        // Panel size lets a preview lay modules out at true width without
        // parsing anyone's artwork.
        out += "      \"size\": [" +
               num(finite(mw->box.size.x) ? mw->box.size.x : 0.0f) + ", " +
               num(finite(mw->box.size.y) ? mw->box.size.y : 0.0f) + "],\n";

        // Knobs, faders and switches, the same walk as the jacks.
        //
        // Recorded because a control's position exists ONLY here, exactly
        // like a jack's: a plugin.json says nothing about it and the panel
        // artwork usually does not draw it. Without this, a preview can
        // place a vendor module's cables correctly and still show a bare
        // faceplate with labels floating over nothing.
        //
        // The widget's own box size comes too. It is the size Rack DRAWS,
        // which beats inferring a diameter from a control's declared kind
        // -- vendors use sizes we have no table for, and a knob drawn at
        // the wrong size is a picture that looks right and is wrong.
        out += "      \"params\": [";
        {
            bool first = true;
            for (rack::app::ParamWidget* pw : mw->getParams()) {
                if (!pw) continue;
                if (!first) out += ",";
                first = false;
                std::string name;
                if (rack::engine::ParamQuantity* q = pw->getParamQuantity())
                    name = q->getLabel();
                // WHICH control, not just where. A fader, a switch and a
                // knob occupy the same field in a manifest and look
                // nothing alike, and guessing from the drawn aspect ratio
                // gets switches wrong. Rack's own class hierarchy already
                // answers this, so ask it.
                const char* kind = "knob";
                if (dynamic_cast<rack::app::SliderKnob*>(pw))      kind = "slider";
                else if (dynamic_cast<rack::app::Knob*>(pw))       kind = "knob";
                else if (dynamic_cast<rack::app::SvgButton*>(pw))  kind = "button";
                else if (dynamic_cast<rack::app::Switch*>(pw))     kind = "switch";
                else                                              kind = "other";
                // Centre, not corner: a knob turns about its middle and
                // box.pos is the widget's top-left.
                const float cx = pw->box.pos.x + pw->box.size.x * 0.5f;
                const float cy = pw->box.pos.y + pw->box.size.y * 0.5f;
                if (!finite(cx) || !finite(cy) ||
                    !finite(pw->box.size.x) || !finite(pw->box.size.y))
                    continue;
                out += "\n        {\"index\": " + std::to_string(pw->paramId) +
                       ", \"name\": \"" + esc(name) + "\"" +
                       ", \"x\": " + std::to_string(cx) +
                       ", \"y\": " + std::to_string(cy) +
                       ", \"w\": " + std::to_string(pw->box.size.x) +
                       ", \"h\": " + std::to_string(pw->box.size.y) +
                       ", \"kind\": \"" + kind + "\"}";
            }
            out += first ? "]" : "\n      ]";
            out += ",\n";
        }

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
                if (!finite(cx) || !finite(cy)) continue;
                out += "\n        {\"index\": " + std::to_string(pw->portId) +
                       ", \"name\": \"" + esc(name) + "\"" +
                       ", \"x\": " + std::to_string(cx) +
                       ", \"y\": " + std::to_string(cy) + "}";
            }
            out += first ? "]" : "\n      ]";
            out += inputs ? ",\n" : "\n";
        }
        // Lights and everything else on the face.
        //
        // ModuleWidget enumerates params and ports and nothing else, so an
        // LED or a screen is reachable only by walking the widget tree.
        // Both matter: a sequencer is mostly lights, and a scope is mostly
        // screen, and a drawing that omits them shows a panel of knobs
        // where the user sees a display.
        //
        // Anything not otherwise classified is recorded as a "display"
        // with its bounds and its C++ type. We cannot know what a vendor's
        // custom widget draws, but we can know that SOMETHING occupies
        // that rectangle -- which is the difference between a faithful
        // placeholder and a blank.
        {
            std::vector<std::string> lights, displays;
            collect(mw, rack::math::Vec(0, 0), lights, displays);
            out += ",\n      \"lights\": [";
            for (std::size_t i = 0; i < lights.size(); ++i)
                out += (i ? "," : "") + std::string("\n        ") + lights[i];
            out += lights.empty() ? "]" : "\n      ]";
            out += ",\n      \"displays\": [";
            for (std::size_t i = 0; i < displays.size(); ++i)
                out += (i ? "," : "") + std::string("\n        ") + displays[i];
            out += displays.empty() ? "]" : "\n      ]";
            out += "\n";
        }
        out += "    }";
    }

    /// Measure a model nobody has placed, by building one.
    ///
    /// The widget is asked for a real Module rather than the browser's
    /// null one: with no module a ParamWidget has no ParamQuantity and a
    /// PortWidget no PortInfo, so a null-module sweep records every
    /// position and not one NAME -- and names are the whole vocabulary.
    ///
    /// The module is then detached before the widget goes. ModuleWidget's
    /// destructor hands its module back to the ENGINE, and this one was
    /// never given to the engine -- letting it run asserts inside
    /// removeModule_NoLock and takes Rack down. Detaching means we own the
    /// module outright and free it ourselves, and the engine is never
    /// involved: nothing is added to the running graph, so no measured
    /// module ever processes a sample.
    bool emit_unplaced(rack::plugin::Model* model, std::string& out) {
        rack::engine::Module* m = model->createModule();
        rack::app::ModuleWidget* mw = model->createModuleWidget(m);
        if (!mw) {
            delete m;
            return false;
        }
        emit_module(mw, model, out);
        mw->module = nullptr;            // the engine never had it
        delete mw;
        delete m;
        return true;
    }

    void scan() {
        if (!APP || !APP->scene || !APP->scene->rack) return;

        std::string out = "{\n  \"modules\": [\n";
        bool first_mod = true;
        std::set<std::string> seen;

        for (rack::app::ModuleWidget* mw : APP->scene->rack->getModules()) {
            if (!mw || !mw->module || !mw->module->model) continue;
            rack::plugin::Model* model = mw->module->model;
            if (!model->plugin) continue;

            if (!first_mod) out += ",\n";
            first_mod = false;
            seen.insert(model->plugin->slug + "/" + model->slug);
            emit_module(mw, model, out);
        }

        // Then every model that is installed but not on screen.
        //
        // Scanning only what was placed sounds like the honest scope and is
        // actually a trap, because the gap it leaves is invisible: an
        // unmapped module still draws, just as a faceplate with labels over
        // nothing. Forge's own 30 modules sat unmapped for the whole life of
        // this file -- CARTOG had recorded itself and nothing else of ours --
        // while Fundamental read 39/39 purely because somebody had once
        // opened a patch holding all of them. Nobody could see the
        // difference, so nobody looked.
        //
        // A model does not need to be placed to be measured; it needs to
        // exist. Build one, measure it, throw it away.
        if (!swept_) {
            swept_ = true;                  // one sweep per session, not per rescan
            const std::set<std::string> bad = quarantined();
            for (rack::plugin::Plugin* plug : rack::plugin::plugins) {
                if (!plug) continue;
                for (rack::plugin::Model* model : plug->models) {
                    if (!model || !model->plugin) continue;
                    const std::string key =
                        model->plugin->slug + "/" + model->slug;
                    if (seen.count(key)) continue;
                    if (bad.count(key)) {
                        WARN("forge: skipping %s, it crashed a previous scan",
                             key.c_str());
                        continue;
                    }
                    seen.insert(key);
                    // Name it on disk BEFORE building it. Constructing a
                    // stranger's module runs their code, and a crash there
                    // takes Rack with it -- no catch runs, so the only way to
                    // learn which one did it is to have written it down
                    // first. Next launch reads this and steps around it.
                    mark_pending(key);
                    std::string one;
                    bool made = false;
                    try {
                        made = emit_unplaced(model, one);
                    } catch (const std::exception& e) {
                        WARN("forge: %s threw during scan: %s",
                             key.c_str(), e.what());
                    } catch (...) {
                        WARN("forge: %s threw during scan", key.c_str());
                    }
                    clear_pending();
                    if (!made || one.empty()) continue;
                    if (!first_mod) out += ",\n";
                    first_mod = false;
                    out += one;
                }
            }
        }
        out += "\n  ]\n}\n";

        const std::string path = rack::asset::user("forge-portmap.json");
        // Read what is already mapped and fold this scan into it, so a
        // batch adds to the library rather than replacing it.
        std::string existing;
        if (FILE* rf = std::fopen(path.c_str(), "rb")) {
            char buf[8192];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), rf)) > 0)
                existing.append(buf, n);
            std::fclose(rf);
        }
        out = forge_portmap::merge(existing, out);
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
            if (mod) mod->lit = 1.f;
            INFO("forge: wrote port map to %s", path.c_str());
        } else {
            WARN("forge: could not write %s", path.c_str());
        }
    }

    int last_count = -1;

    void step() override {
        rack::app::ModuleWidget::step();
        if (!mod) return;

        // Rescan whenever the rack's contents change, rather than only when
        // asked. Requiring a click would mean the map silently goes stale the
        // moment someone adds a module, and a stale map is worse than none --
        // it draws cables into jacks that have moved. The button stays for a
        // forced refresh after a plugin update, where the count is unchanged
        // but the layout may not be.
        if (APP && APP->scene && APP->scene->rack) {
            const int n = static_cast<int>(APP->scene->rack->getModules().size());
            if (n != last_count) {
                last_count = n;
                scan();
            }
        }

        const bool down = mod->params[CARTOGModule::L::SCAN_PARAM].getValue() > 0.5f;
        if (down && !held) scan();      // on the press, not every frame held
        held = down;
    }
};

}  // namespace

rack::plugin::Model* modelCARTOG =
    rack::createModel<CARTOGModule, CARTOGWidget>("CARTOG");
