// Module census: run ONE installed module's real DSP, alone, and record what
// every one of its output ports emits.
//
// WHY THIS EXISTS. The patch gate can already say a whole patch is silent, and
// its trace can already name the module whose outputs never left 0.000 V. What
// it cannot say is whether that module would EVER have emitted anything, or
// whether the patch around it was at fault -- so the same finding arrived
// repeatedly, module by module, one generation at a time. Static analysis of a
// patch provably cannot see this class at all: a module that is cabled,
// enabled, in range and still emits nothing is indistinguishable, on paper,
// from one that works.
//
// So measure it directly. A Rack plugin is a shared library exporting `init`,
// its undefined rack:: symbols resolve against libRack, and Plugin::models is
// public -- exactly the mechanism patch_gate.cpp uses -- so an arbitrary
// installed third-party module can be instantiated, driven and measured with
// no Rack process, no patch and no audio device.
//
// TWO THINGS THIS MEASURES THAT A SINGLE RUN AT DEFAULTS WOULD MISS:
//
//   * PER OUTPUT PORT, not per module. A module can be alive on one jack and
//     dead on another -- Bogaudio's SHAPER+ holds its Signal output at 0.000 V
//     while its envelope and stage gates are busy -- and a per-module verdict
//     averages that away in whichever direction the first live jack points.
//   * SEVERAL PARAMETER CONDITIONS, not just the constructor's defaults. A
//     defaults-only reading calls a module silent that only needs its CYCLE
//     switch on, and calls one audible whose single live jack needs a knob
//     nobody moved. The generator writes arbitrary in-range values, so the
//     census sweeps the range the generator writes into and a port counts as
//     dead only when it stayed at zero in every condition.
//
// Usage: module-census <spec.json>
//
// The spec names the plugin directory and the modules to measure:
//
//   {"plugin_dir": "...",
//    "modules": [{"plugin": "AS", "model": "SEQ16",
//                 "roles_in": ["Cv", "Clock", "Trigger", "Cv"],
//                 "data": {...} }]}
//
// `roles_in` is optional and comes from the same inventory the generator reads;
// without it every input is driven as CV. `data` is optional module-owned
// persistent state, applied exactly as Rack applies it on patch load.
//
// One JSON object per module is written to stdout and flushed immediately, so
// a module that crashes the process costs only its own row. Grouping a whole
// plugin's models into one invocation keeps the dlopen cost paid once.

#include <rack.hpp>
#include <patch.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kSr = 48000.f;
/// Two seconds. Long enough for a slow envelope to finish and for a sequencer
/// driven by the clock stimulus below to visit every step, short enough that a
/// few thousand modules remain measurable in an afternoon.
constexpr int kRun = 96000;

/// What one output port did over one condition.
struct PortResult {
    double peak_abs = 0.0;
    double mean_abs = 0.0;
    long nonzero = 0;          // samples with |v| > 1e-9
    bool finite = true;
};

/// The stimulus for one input, chosen from the role the inventory declared.
///
/// Copied in shape from behaviour_gate.cpp, and for the same reason: feeding
/// one identical square into every jack does not exercise a stateful module.
/// A sequencer whose reset shares the clock's waveform never leaves its first
/// step, and an envelope gated for 5 ms never reaches sustain -- both then
/// measure as silent while working perfectly in Rack.
float stimulus(const std::string& role, int i, int input_index) {
    if (role == "Clock")
        return ((i / 64) % 2) ? 10.f : 0.f;
    if (role == "Gate")
        return ((i / 16384) % 2) ? 10.f : 0.f;
    if (role == "Trigger")
        return (i % 16384) < 48 ? 10.f : 0.f;
    if (role == "Audio")
        return 5.f * std::sin(2.0 * M_PI * 110.0 * i / kSr);
    if (role == "Pitch")
        return 1.f;
    const int phased = i + input_index * 4096;
    return ((phased / 16384) % 2) ? 1.0f : -1.0f;
}

/// Stand up the global state a module constructor is entitled to assume.
///
/// `APP` is `rack::contextGet()` and is NULL until `contextSet()`. Many
/// third-party modules read the engine's sample rate in their CONSTRUCTOR, so
/// without this the census dies on SIGSEGV before measuring anything.
///
/// `user_dir` is what makes commercially licensed modules measurable, and
/// leaving it empty silently poisons the whole measurement. Plugins bought
/// through the VCV Library carry a licence check that resolves a cached key at
/// `<asset::userDir>/licenses/<plugin>.vcvkey`. `asset::userDir` is empty until
/// `asset::init()` runs, so the lookup goes to `/licenses/...`, finds nothing,
/// and the module concludes it is unlicensed -- whereupon it runs and writes
/// zero to every output. It does not log, does not refuse to construct and
/// does not fail: it is indistinguishable, from outside, from a module that
/// genuinely emits nothing. Uninitialised, this reads whole commercial plugins
/// as dead that produce audio the moment the key resolves.
void install_rack_context(const std::string& user_dir) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!user_dir.empty()) {
        rack::asset::userDir = user_dir;
        rack::asset::systemDir = user_dir;
        rack::asset::init();
    }
    rack::random::init();
    auto* ctx = new rack::Context;
    rack::contextSet(ctx);
    ctx->engine = new rack::engine::Engine;
    ctx->engine->setSampleRate(kSr);
    ctx->patch = new rack::patch::Manager;
    const auto autosave = std::filesystem::temp_directory_path() /
        ("forge-module-census-" + std::to_string(
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(ctx))));
    std::filesystem::create_directories(autosave);
    ctx->patch->autosavePath = autosave.string();
    ctx->patch->path = (autosave / "census.vcv").string();
#pragma clang diagnostic pop
}

std::map<std::string, rack::plugin::Plugin*> g_plugins;

rack::plugin::Plugin* load_plugin(const std::string& slug, const std::string& dir) {
    auto it = g_plugins.find(slug);
    if (it != g_plugins.end()) return it->second;
    const std::string lib = dir + "/" + slug + "/plugin.dylib";
    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        g_plugins[slug] = nullptr;
        return nullptr;
    }
    auto init = reinterpret_cast<void (*)(rack::plugin::Plugin*)>(dlsym(h, "init"));
    if (!init) {
        g_plugins[slug] = nullptr;
        return nullptr;
    }
    auto* p = new rack::plugin::Plugin;
    p->handle = h;
    p->path = dir + "/" + slug;
    init(p);
    g_plugins[slug] = p;
    return p;
}

rack::plugin::Model* find_model(rack::plugin::Plugin* p, const std::string& slug) {
    if (!p) return nullptr;
    for (rack::plugin::Model* m : p->models)
        if (m && m->slug == slug) return m;
    return nullptr;
}

/// One measured condition.
struct Condition {
    std::string name;
    /// Where in each param's range to put every knob, or <0 to leave defaults.
    double param_fraction = -1.0;
    bool drive_inputs = true;
    bool apply_data = false;
    /// Whether the sweep also moves the module's switches and buttons.
    ///
    /// Neither answer is safe on its own, so the census runs both and a port
    /// counts as silent only if it stayed at zero in every condition. Sweeping
    /// them presses mutes and holds resets, which silences a working module
    /// (ReBit); not sweeping them leaves a module whose output is gated by a
    /// switch that defaults to off looking dead (ReKey, in the same pack).
    bool sweep_switches = false;
};

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) continue;
        else out += c;
    }
    return out;
}

std::string number(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

/// Instantiate, configure, drive, and record. One fresh module per condition:
/// persistent DSP state carried from a previous condition would make the
/// reading depend on the order conditions happen to run in.
std::vector<PortResult> measure(rack::plugin::Model* model,
                                const Condition& condition,
                                const std::vector<std::string>& roles_in,
                                const std::string& data_json,
                                int settle_ms,
                                int& num_inputs, int& num_outputs,
                                int& num_params) {
    rack::engine::Module* m = model->createModule();
    if (!m) return {};
    m->id = 1;
    rack::contextGet()->engine->addModule(m);

    num_inputs = m->getNumInputs();
    num_outputs = m->getNumOutputs();
    num_params = m->getNumParams();

    // A SWEEP THAT TOUCHES SWITCHES MEASURES THE SWITCH, NOT THE MODULE.
    // Driving every parameter to the same fraction of its range also drives
    // the module's own mute, reset and enable buttons there, which silences a
    // working module through its front panel and reads back as a dead module.
    // AS-Seqs-n-Tools' ReBit is the worked example: at the default the four
    // divide amounts are 0, which is off, and at any swept fraction the four
    // Mute buttons and the Reset button go with them -- so it measured silent
    // on all five outputs in every condition, while setting only the dividers
    // and leaving the buttons alone gives 10 V on all five.
    //
    // The split is the SDK's own, not a guess about names: `configSwitch()`
    // and `configButton()` both return a `SwitchQuantity`, and `configParam()`
    // -- a continuous knob -- does not. Rack draws the same line itself, and
    // goes further for buttons by clearing `randomizeEnabled` so its own
    // Randomize command cannot press them either.
    for (int p = 0; p < num_params; ++p) {
        auto* q = m->getParamQuantity(p);
        if (!q) continue;
        const bool is_switch =
            dynamic_cast<rack::engine::SwitchQuantity*>(q) != nullptr;
        if (condition.param_fraction < 0.0
                || (is_switch && !condition.sweep_switches)) {
            m->params[p].setValue(q->getDefaultValue());
        } else {
            const float lo = q->getMinValue(), hi = q->getMaxValue();
            m->params[p].setValue(
                lo + static_cast<float>(condition.param_fraction) * (hi - lo));
        }
    }
    if (condition.apply_data && !data_json.empty()) {
        json_error_t err{};
        if (json_t* data = json_loads(data_json.c_str(), 0, &err)) {
            m->dataFromJson(data);
            json_decref(data);
        }
    }

    std::vector<PortResult> results(num_outputs);
    std::vector<double> sums(num_outputs, 0.0);

    rack::engine::Module::ProcessArgs args;
    args.sampleRate = kSr;
    args.sampleTime = 1.f / kSr;
    args.frame = 0;

    auto step = [&](int i) {
        for (int p = 0; p < num_inputs; ++p) {
            if (!condition.drive_inputs) { m->inputs[p].channels = 0; continue; }
            // Assigned directly: Port::setChannels() returns early when
            // channels == 0, because only the engine may promote a port out of
            // the disconnected state. Going through the setter leaves every
            // input reading 0 V, which makes every input-driven module look
            // dead.
            m->inputs[p].channels = 1;
            const std::string role =
                p < static_cast<int>(roles_in.size()) ? roles_in[p] : "Cv";
            m->inputs[p].setVoltage(stimulus(role, i, p));
        }
        // Outputs must look patched too, or a module that skips work for an
        // unconnected output -- which the module contract asks for -- writes
        // nothing and measures as dead for the wrong reason.
        for (int o = 0; o < num_outputs; ++o)
            m->outputs[o].channels = 1;
        m->process(args);
        args.frame++;
    };

    // WALL CLOCK IS A HIDDEN INPUT, and it is the one an offline census is
    // most likely to get wrong. A module that loads its wavetables, banks or
    // samples on a WORKER THREAD finishes that load in real seconds, while
    // this harness renders two seconds of audio in a few milliseconds -- so
    // the loader is still running when the measurement ends and the module
    // reads as permanently silent when it is merely not ready yet.
    // Synthesis Technology's E370 links pthread_create and std::thread::join
    // for exactly this. Priming with a few samples first matters too: a lazy
    // loader is commonly kicked off by the first process() call, not by the
    // constructor, so sleeping before any process() would wait on a thread
    // that has not started.
    if (settle_ms > 0) {
        for (int i = 0; i < 64; ++i) step(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
        args.frame = 0;
    }

    for (int i = 0; i < kRun; ++i) {
        step(i);
        for (int o = 0; o < num_outputs; ++o) {
            const float v = m->outputs[o].getVoltage();
            if (!std::isfinite(v)) { results[o].finite = false; continue; }
            const double a = std::fabs(v);
            results[o].peak_abs = std::max(results[o].peak_abs, a);
            sums[o] += a;
            if (a > 1e-9) ++results[o].nonzero;
        }
    }
    for (int o = 0; o < num_outputs; ++o)
        results[o].mean_abs = sums[o] / kRun;

    rack::contextGet()->engine->removeModule(m);
    delete m;
    return results;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: module-census <spec.json>\n");
        return 2;
    }
    json_error_t err{};
    json_t* spec = json_load_file(argv[1], 0, &err);
    if (!spec) {
        std::fprintf(stderr, "cannot read spec %s: %s\n", argv[1], err.text);
        return 2;
    }
    // Reading the spec needs no Rack context; installing the context needs the
    // spec's `user_dir`. Everything after this point may dlopen, so the
    // context has to be complete before the next statement, not after it.
    const char* user_dir = json_string_value(json_object_get(spec, "user_dir"));
    install_rack_context(user_dir ? user_dir : "");

    const char* dir_raw = json_string_value(json_object_get(spec, "plugin_dir"));
    if (!dir_raw) {
        std::fprintf(stderr, "spec has no plugin_dir\n");
        return 2;
    }
    const std::string dir = dir_raw;
    // Real seconds to wait, after priming, before measuring. Zero for the
    // sweep over everything; raised for the second pass over whatever looked
    // dead, which is the only way to tell a module that emits nothing from one
    // whose loader thread this harness outran.
    const int settle_ms = static_cast<int>(
        json_integer_value(json_object_get(spec, "settle_ms")));

    // Defaults first, so a module that is alive there is answered cheaply and
    // the sweep only has to run for the ones that look dead.
    const std::vector<Condition> conditions = {
        {"defaults_unpatched", -1.0, false, false},
        {"defaults_driven",    -1.0, true,  false},
        {"data_driven",        -1.0, true,  true},
        {"quarter_driven",     0.25, true,  true,  false},
        {"half_driven",        0.50, true,  true,  false},
        {"three_quarter_driven", 0.75, true, true,  false},
        {"max_driven",         1.00, true,  true,  false},
        // The same four sweeps with the switches and buttons moving too.
        {"quarter_switched",   0.25, true,  true,  true},
        {"half_switched",      0.50, true,  true,  true},
        {"three_quarter_switched", 0.75, true, true, true},
        {"max_switched",       1.00, true,  true,  true},
    };

    json_t* modules = json_object_get(spec, "modules");
    size_t index;
    json_t* entry;
    json_array_foreach(modules, index, entry) {
        const char* plugin = json_string_value(json_object_get(entry, "plugin"));
        const char* model_slug = json_string_value(json_object_get(entry, "model"));
        if (!plugin || !model_slug) continue;

        std::vector<std::string> roles_in;
        json_t* roles = json_object_get(entry, "roles_in");
        size_t r;
        json_t* role;
        json_array_foreach(roles, r, role)
            roles_in.push_back(json_string_value(role) ?: "Cv");

        std::string data_json;
        if (json_t* data = json_object_get(entry, "data")) {
            if (!json_is_null(data)) {
                if (char* encoded = json_dumps(data, JSON_COMPACT)) {
                    data_json = encoded;
                    std::free(encoded);
                }
            }
        }

        std::string out = std::string("{\"plugin\":\"") + escape(plugin) +
            "\",\"model\":\"" + escape(model_slug) + "\"";

        rack::plugin::Plugin* p = load_plugin(plugin, dir);
        rack::plugin::Model* model = find_model(p, model_slug);
        if (!model) {
            out += ",\"ok\":false,\"why\":\"" +
                std::string(p ? "the plugin exposes no model by that slug"
                              : "the plugin library could not be loaded") + "\"}";
            std::printf("%s\n", out.c_str());
            std::fflush(stdout);
            continue;
        }

        int num_inputs = 0, num_outputs = 0, num_params = 0;
        std::string conditions_json = ",\"conditions\":{";
        bool first = true;
        for (const Condition& condition : conditions) {
            if (condition.apply_data && data_json.empty() &&
                condition.name == "data_driven")
                continue;   // nothing to apply; the defaults run already covers it
            auto results = measure(model, condition, roles_in, data_json,
                                   settle_ms,
                                   num_inputs, num_outputs, num_params);
            if (!first) conditions_json += ',';
            first = false;
            conditions_json += "\"" + condition.name + "\":[";
            for (size_t o = 0; o < results.size(); ++o) {
                if (o) conditions_json += ',';
                conditions_json +=
                    "{\"peak_abs_v\":" + number(results[o].peak_abs) +
                    ",\"mean_abs_v\":" + number(results[o].mean_abs) +
                    ",\"nonzero_samples\":" + std::to_string(results[o].nonzero) +
                    ",\"finite\":" + (results[o].finite ? "true" : "false") + "}";
            }
            conditions_json += "]";
        }
        conditions_json += "}";

        out += ",\"ok\":true,\"num_inputs\":" + std::to_string(num_inputs) +
               ",\"num_outputs\":" + std::to_string(num_outputs) +
               ",\"num_params\":" + std::to_string(num_params) +
               ",\"samples_per_condition\":" + std::to_string(kRun) +
               ",\"settle_ms\":" + std::to_string(settle_ms) +
               conditions_json + "}";
        std::printf("%s\n", out.c_str());
        std::fflush(stdout);
    }

    json_decref(spec);
    return 0;
}
