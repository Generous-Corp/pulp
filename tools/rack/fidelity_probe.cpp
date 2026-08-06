// A Rack module that reports what the running engine actually holds.
//
// A generated patch is a file. Everything downstream of writing it -- Rack
// parsing it, building modules, restoring parameters, connecting cables,
// running the DSP -- is unobserved, so "the generator wrote four pitches" and
// "four pitches play" are two claims and only the first one has ever been
// checked. This module closes the gap from inside Rack, where both are simply
// readable:
//
//   * ENGINE STATE, once the patch has loaded: every module, every parameter's
//     live value, every cable's endpoints. Compared against the file, this
//     rejects a value the loader clamped, a parameter index that addressed the
//     wrong knob, a cable that landed on the wrong port, and a default that
//     overwrote what was written.
//
//   * THE SIGNAL, for a bounded window: whatever is patched into this module's
//     jacks, sample by sample. That is the only evidence that can ACCEPT --
//     engine state agreeing with the file still says nothing about what a
//     miswired signal path sounds like.
//
// It is a test instrument and lives with the test tooling: a separate plugin,
// built into a scratch directory the harness points a throwaway Rack at, so it
// is never installed and the module set never grows a diagnostic nobody asked
// for. CARTOG is the sibling that measures panel geometry; this one measures
// behaviour.
//
// Recording buffers into memory and writes once. Preallocating means the audio
// callback allocates nothing, and writing at the end means the file is one
// contiguous flush rather than a syscall per block. The write also runs from
// the destructor when the window never fills, so a run cut short still leaves
// whatever it captured rather than nothing at all.

#include <rack.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

rack::plugin::Plugin* pluginInstance = nullptr;

namespace {

constexpr int kInputs = 4;

/// Seconds of signal to keep, and where to put what this run observed.
///
/// Env rather than parameters: the harness launches Rack and cannot reach into
/// a patch it did not write. Both have defaults, so the module is still usable
/// dropped into a rack by hand.
double window_seconds() {
    if (const char* s = std::getenv("FORGE_PROBE_SECONDS")) {
        const double v = std::atof(s);
        if (v > 0.0) return v;
    }
    return 4.0;
}

std::string out_dir() {
    if (const char* s = std::getenv("FORGE_PROBE_OUT")) {
        if (*s) return s;
    }
    return rack::asset::user("");
}

/// JSON escaping for names that came out of other people's plugins.
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

/// A float as JSON, with enough digits to survive the round trip.
///
/// `std::to_string` gives six decimal places, which turns a parameter value
/// into a near miss and makes a faithful load look like a drifted one.
std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

struct ProbeModule : rack::engine::Module {
    std::vector<float> captured;      // interleaved, kInputs per frame
    std::size_t filled = 0;
    double rate = 0.0;
    bool written = false;

    ProbeModule() {
        config(0, kInputs, 0, 0);
        for (int i = 0; i < kInputs; ++i)
            configInput(i, "Tap " + std::to_string(i + 1));
    }

    ~ProbeModule() override { flush(); }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        // Sized here rather than in the constructor: the module is built
        // before the engine has told anyone what rate it runs at, so a buffer
        // allocated up front is a buffer sized from a guess.
        rate = e.sampleRate;
        const std::size_t frames =
            static_cast<std::size_t>(window_seconds() * e.sampleRate) + 1;
        captured.assign(frames * kInputs, 0.f);
        filled = 0;
        written = false;
    }

    void process(const ProcessArgs& args) override {
        if (filled + kInputs > captured.size()) {
            // The window is full. Write once, from here, so the recording
            // exists even if the process is killed rather than asked to exit.
            if (!written) flush();
            return;
        }
        for (int i = 0; i < kInputs; ++i)
            captured[filled + static_cast<std::size_t>(i)] =
                inputs[i].getVoltage();
        filled += kInputs;
    }

    /// Write the capture: a tiny JSON header beside a raw float32 block.
    ///
    /// Raw rather than WAV because nothing plays this -- it is read by an
    /// analyser, and a header format nobody has to agree on is one fewer place
    /// for a channel count to be wrong.
    void flush() {
        if (written) return;
        written = true;
        const std::string base = out_dir() + "/forge-probe-signal";
        if (FILE* f = std::fopen((base + ".f32").c_str(), "wb")) {
            if (filled) std::fwrite(captured.data(), sizeof(float), filled, f);
            std::fclose(f);
        }
        if (FILE* f = std::fopen((base + ".json").c_str(), "w")) {
            std::fprintf(f,
                         "{\"sampleRate\": %s, \"channels\": %d, "
                         "\"frames\": %zu}\n",
                         num(rate).c_str(), kInputs, filled / kInputs);
            std::fclose(f);
        }
        INFO("forge: probe wrote %zu frames at %s Hz",
             filled / kInputs, num(rate).c_str());
    }
};

/// Everything the running engine holds, written down.
///
/// Read through the Engine rather than through the widget tree: parameter
/// values and cable endpoints are engine state, and the widgets are a view of
/// it that a headless load never draws.
void dump_engine(const std::string& path) {
    rack::engine::Engine* e = APP ? APP->engine : nullptr;
    if (!e) return;

    std::string out = "{\n  \"modules\": [";
    bool first = true;
    for (int64_t id : e->getModuleIds()) {
        rack::engine::Module* m = e->getModule(id);
        if (!m || !m->model || !m->model->plugin) continue;
        out += first ? "\n" : ",\n";
        first = false;
        out += "    {\"id\": " + std::to_string(id) +
               ", \"plugin\": \"" + esc(m->model->plugin->slug) + "\"" +
               ", \"model\": \"" + esc(m->model->slug) + "\"" +
               ", \"params\": [";
        for (std::size_t i = 0; i < m->params.size(); ++i) {
            if (i) out += ", ";
            out += "{\"id\": " + std::to_string(i) +
                   ", \"value\": " + num(m->params[i].getValue()) + "}";
        }
        out += "]}";
    }
    out += first ? "],\n" : "\n  ],\n";

    out += "  \"cables\": [";
    first = true;
    for (int64_t id : e->getCableIds()) {
        rack::engine::Cable* c = e->getCable(id);
        if (!c || !c->inputModule || !c->outputModule) continue;
        out += first ? "\n" : ",\n";
        first = false;
        out += "    {\"id\": " + std::to_string(id) +
               ", \"outputModuleId\": " + std::to_string(c->outputModule->id) +
               ", \"outputId\": " + std::to_string(c->outputId) +
               ", \"inputModuleId\": " + std::to_string(c->inputModule->id) +
               ", \"inputId\": " + std::to_string(c->inputId) + "}";
    }
    out += first ? "]\n}\n" : "\n  ]\n}\n";

    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        INFO("forge: probe wrote engine state to %s", path.c_str());
    } else {
        WARN("forge: probe could not write %s", path.c_str());
    }
}

struct ProbeWidget : rack::app::ModuleWidget {
    explicit ProbeWidget(ProbeModule* m) {
        setModule(m);
        // No panel SVG. Nothing draws this in the run it exists for, and a
        // panel is a file that has to be installed beside the plugin -- one
        // more thing that can be missing on the machine under test.
        box.size = rack::math::Vec(rack::RACK_GRID_WIDTH * 6,
                                   rack::RACK_GRID_HEIGHT);
        for (int i = 0; i < kInputs; ++i) {
            // Real PortWidgets, because a cable in a patch attaches to the
            // WIDGET: an engine port with no widget behind it drops the cable
            // on load, which would read as a patch that never had it.
            addInput(rack::createInputCentered<rack::componentlibrary::PJ301MPort>(
                rack::math::Vec(box.size.x * 0.5f, 40.f + 30.f * i), m, i));
        }
    }

    /// Record the engine the moment this widget joins the rack.
    ///
    /// A headless load runs no frames, so anything driven from step() never
    /// happens -- the same constraint CARTOG met. Modules and cables are both
    /// built in the engine before any widget is created, so what this sees is
    /// the whole patch regardless of where the probe sits in it.
    void onAdd(const AddEvent& e) override {
        rack::app::ModuleWidget::onAdd(e);
        if (!APP || !APP->scene || !APP->scene->rack) return;
        const std::vector<rack::app::ModuleWidget*> placed =
            APP->scene->rack->getModules();
        if (std::find(placed.begin(), placed.end(), this) == placed.end())
            return;               // the browser builds previews; those are not the rack
        INFO("forge: probe placed alongside %d modules",
             static_cast<int>(placed.size()));
        dump_engine(out_dir() + "/forge-probe-engine.json");
    }
};

}  // namespace

rack::plugin::Model* modelPROBE =
    rack::createModel<ProbeModule, ProbeWidget>("PROBE");

extern "C" __attribute__((visibility("default")))
void init(rack::plugin::Plugin* p) {
    pluginInstance = p;
    p->addModel(modelPROBE);
}
