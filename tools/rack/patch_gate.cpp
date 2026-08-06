// Patch gate: run a whole patch's real DSP and find out whether it makes sound.
//
// A patch can satisfy every structural check and still be silent. The lint
// confirms modules resolve, cables land on real ports and something reaches an
// output; none of that can tell you a VCA never opens because nothing is
// patched to its CV and its level knob sits at zero. That is the single most
// common way a generated patch disappoints, and it is invisible to everything
// upstream of actually running it.
//
// Rack's own --headless mode would run it, but it loads the autosaved patch
// rather than a named one and drives a live audio device. Neither is wanted
// for a check that should be quiet and repeatable. Instead this loads the
// plugins directly: a Rack plugin is a shared library exporting `init`, its
// undefined rack:: symbols resolve against libRack, and Plugin::models is
// public -- so every module in a patch can be instantiated, wired and driven
// with no Rack process and no audio device anywhere.
//
// Core is the exception: it is compiled into Rack rather than shipped as a
// library, so the audio interface cannot be instantiated. It does not need to
// be. What matters is the voltage arriving at its inputs, which is measured on
// the cables that feed it.
//
// LOADING A PLUGIN IS NOT ENOUGH: THE RACK CONTEXT HAS TO EXIST FIRST.
// `APP` is `rack::contextGet()`, and it is NULL until something calls
// `contextSet()`. A great many third-party modules read the engine's sample
// rate in their CONSTRUCTOR -- Bogaudio's base module does it for every one of
// its 111 models -- so `APP->engine` dereferences NULL at offset 0x10 and the
// whole gate dies on SIGSEGV before it prints a single line. Modules that
// never touch `APP` are unaffected, which is why patches built only from this
// project's own modules were the only ones that ever passed. Install the
// context, an engine and the RNG before any plugin is loaded, in the order
// Rack itself uses.
//
// PRESENCE IS NOT THE PROPERTY. This gate answers "does it make a sound", and a
// held single note answers yes with a perfect score -- which is exactly how a
// request for a melody shipped as one pitch for six seconds. So alongside the
// presence verdict it now records the signal and MEASURES what it does: pitch
// variety, articulation, level over time, brightness over time. Those numbers
// are reported, never judged here. Which numbers a given request needs is a
// question about the request, and the request is not in this process.
//
// Usage: patch-gate <patch.vcv> <plugin-dir>
//
// Environment:
//   PATCH_GATE_TRACE=1              per-module output and param activity
//   PATCH_GATE_SECONDS=<n>          how long to run (default 6)
//   PATCH_GATE_SERIES=1             include the per-window tracks in the JSON
//   PATCH_GATE_SET=<name>=<v>,...   override a behaviour measurement setting

#include "patch_behaviour.hpp"
#include "patch_behaviour_json.hpp"

#include <rack.hpp>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr float kSr = 48000.f;
constexpr int kWarm = 512;

/// Long enough for a bar or two at any usable tempo. One second was enough to
/// tell whether a signal existed and far too short to tell whether it went
/// anywhere: a four-step sequence at 90 bpm has not finished its first pass.
constexpr double kDefaultSeconds = 6.0;

int failures = 0, warnings = 0;
void fail(const std::string& m) { std::printf("  FAIL  %s\n", m.c_str()); ++failures; }
void warn(const std::string& m) { std::printf("  warn  %s\n", m.c_str()); ++warnings; }
void pass(const std::string& m) { std::printf("  ok    %s\n", m.c_str()); }

/// Every measurement setting, addressable by the same name the report prints.
///
/// The values in `patch_behaviour::Settings` are guesses, and a guess that can
/// only be changed by editing and rebuilding a C++ file is a guess nobody will
/// ever tune. This is the tuning surface: `PATCH_GATE_SET=min_note_windows=3`
/// and the report says what it ran with, so an experiment leaves a record.
/// An unknown name is refused rather than ignored -- a typo that silently
/// changes nothing looks exactly like a threshold that does not matter.
bool apply_setting_overrides(patch_behaviour::Settings& s, const std::string& spec,
                             std::string& why) {
    const std::map<std::string, double*> doubles = {
        {"window_ms", &s.window_ms},
        {"envelope_window_ms", &s.envelope_window_ms},
        {"envelope_hop_ms", &s.envelope_hop_ms},
        {"f0_min_hz", &s.f0_min_hz},
        {"f0_max_hz", &s.f0_max_hz},
        {"voiced_window_fraction", &s.voiced_window_fraction},
        {"onset_threshold_mult", &s.onset_threshold_mult},
        {"onset_threshold_delta", &s.onset_threshold_delta},
        {"onset_min_gap_ms", &s.onset_min_gap_ms},
        {"active_floor_ratio", &s.active_floor_ratio},
        {"active_floor_v", &s.active_floor_v},
        {"tail_ms", &s.tail_ms},
        {"max_period_ms", &s.max_period_ms},
    };
    const std::map<std::string, int*> ints = {
        {"min_note_windows", &s.min_note_windows},
        {"onset_median_hops", &s.onset_median_hops},
        {"centroid_fft", &s.centroid_fft},
    };
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) { why = "no '=' in \"" + item + "\""; return false; }
        const std::string key = item.substr(0, eq);
        const double value = std::strtod(item.c_str() + eq + 1, nullptr);
        const auto d = doubles.find(key);
        if (d != doubles.end()) { *d->second = value; continue; }
        const auto i = ints.find(key);
        if (i != ints.end()) { *i->second = static_cast<int>(std::lround(value)); continue; }
        why = "no measurement setting is called \"" + key + "\"";
        return false;
    }
    return true;
}

// ── the smallest JSON reader that can read a patch ──────────────────────────
// Deliberately not a general parser: a patch has a fixed shape, and pulling a
// dependency into a test harness to read four field names is not worth it.

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Every value of `key` appearing inside `blob`, in order.
std::vector<std::string> strings_for(const std::string& blob, const std::string& key) {
    std::vector<std::string> out;
    const std::string pat = "\"" + key + "\"";
    for (size_t i = blob.find(pat); i != std::string::npos; i = blob.find(pat, i + 1)) {
        size_t q = blob.find('"', blob.find(':', i + pat.size()) + 1);
        if (q == std::string::npos) break;
        size_t e = blob.find('"', q + 1);
        out.push_back(blob.substr(q + 1, e - q - 1));
    }
    return out;
}

std::vector<long> ints_for(const std::string& blob, const std::string& key) {
    std::vector<long> out;
    const std::string pat = "\"" + key + "\"";
    for (size_t i = blob.find(pat); i != std::string::npos; i = blob.find(pat, i + 1)) {
        size_t c = blob.find(':', i + pat.size());
        if (c == std::string::npos) break;
        out.push_back(std::strtol(blob.c_str() + c + 1, nullptr, 10));
    }
    return out;
}

/// The same, for values that are not whole numbers. A param stored as 0.05
/// read through strtol becomes 0 -- the difference between a slow LFO and a
/// stopped one.
std::vector<double> doubles_for(const std::string& blob, const std::string& key) {
    std::vector<double> out;
    const std::string pat = "\"" + key + "\"";
    for (size_t i = blob.find(pat); i != std::string::npos; i = blob.find(pat, i + 1)) {
        size_t c = blob.find(':', i + pat.size());
        if (c == std::string::npos) break;
        out.push_back(std::strtod(blob.c_str() + c + 1, nullptr));
    }
    return out;
}

/// The `modules` and `cables` arrays, split into one blob per element.
std::vector<std::string> elements(const std::string& doc, const std::string& array) {
    std::vector<std::string> out;
    size_t a = doc.find("\"" + array + "\"");
    if (a == std::string::npos) return out;
    a = doc.find('[', a);
    if (a == std::string::npos) return out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = a; i < doc.size(); ++i) {
        if (doc[i] == '{') { if (depth++ == 0) start = i; }
        else if (doc[i] == '}') { if (--depth == 0) out.push_back(doc.substr(start, i - start + 1)); }
        else if (doc[i] == ']' && depth == 0) break;
    }
    return out;
}

// ── the Rack context ────────────────────────────────────────────────────────

/// Stand up the global state a module constructor is entitled to assume.
///
/// Rack does this before it loads a single plugin, and a module written
/// against Rack is right to expect it. Without it `APP` is NULL and
/// `APP->engine->getSampleRate()` -- an ordinary first line in a constructor --
/// is a null dereference at offset 0x10, the offset of `Context::engine`.
///
/// The engine is set to the rate this harness actually steps at, so a module
/// that caches coefficients at construction caches the right ones.
void install_rack_context() {
    // Deprecated only in the sense of "internal to Rack". This harness IS
    // standing in for Rack, so it is the one caller entitled to use them.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    rack::random::init();
    auto* ctx = new rack::Context;
    rack::contextSet(ctx);
    ctx->engine = new rack::engine::Engine;
    ctx->engine->setSampleRate(kSr);
#pragma clang diagnostic pop
}

// ── loading plugins ─────────────────────────────────────────────────────────

std::map<std::string, rack::plugin::Plugin*> g_plugins;

/// Load a plugin by slug and return it, or null if it has no library.
rack::plugin::Plugin* load_plugin(const std::string& slug, const std::string& dir) {
    auto it = g_plugins.find(slug);
    if (it != g_plugins.end()) return it->second;

    const std::string lib = dir + "/" + slug + "/plugin.dylib";
    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::printf("  ..    %s: dlopen failed: %s\n", slug.c_str(), dlerror());
        g_plugins[slug] = nullptr;
        return nullptr;
    }
    auto init = reinterpret_cast<void (*)(rack::plugin::Plugin*)>(dlsym(h, "init"));
    if (!init) {
        std::printf("  ..    %s: no init symbol\n", slug.c_str());
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

/// Do to a freshly created module what `Engine::addModule` does to it.
///
/// A CONSTRUCTED MODULE IS NOT A RUNNING ONE. `createModule()` runs the
/// constructor and stops; Rack then tells the module its sample rate and that
/// it has been added, and a great many modules allocate their DSP state in
/// exactly those callbacks. CV funk's Alloy sizes a delay line in
/// `onSampleRateChange`, so calling `process()` straight after the constructor
/// read through a null buffer pointer with a zero ring mask -- EXC_BAD_ACCESS
/// at 0x0, a second and completely separate crash from the missing context.
///
/// Sample rate first, then the add event, which is the order Engine uses.
void bring_up(rack::engine::Module* m) {
    if (!m) return;
    rack::engine::Module::SampleRateChangeEvent sr;
    sr.sampleRate = kSr;
    sr.sampleTime = 1.f / kSr;
    m->onSampleRateChange(sr);
    m->onAdd(rack::engine::Module::AddEvent{});
}

rack::plugin::Model* find_model(rack::plugin::Plugin* p, const std::string& slug) {
    if (!p) return nullptr;
    for (rack::plugin::Model* m : p->models)
        if (m && m->slug == slug) return m;
    return nullptr;
}

struct Node {
    long id = 0;
    std::string plugin, model;
    rack::engine::Module* mod = nullptr;   // null for Core, which has no library
    /// The knob positions the PATCH stores, by param id. Rack applies these on
    /// load; measuring with defaults instead measures a different patch than
    /// the one that will be opened.
    std::map<long, double> params;
};

struct Cable {
    long from = 0, from_port = 0, to = 0, to_port = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: patch-gate <patch.vcv> <plugin-dir>\n");
        return 2;
    }
    const std::string doc = slurp(argv[1]);
    const std::string dir = argv[2];

    // Before any dlopen: a plugin's own init() may touch APP too.
    install_rack_context();

    std::vector<Node> nodes;
    for (const std::string& e : elements(doc, "modules")) {
        Node n;
        auto ids = ints_for(e, "id");
        auto pl = strings_for(e, "plugin");
        auto mo = strings_for(e, "model");
        if (ids.empty() || pl.empty() || mo.empty()) continue;
        n.id = ids[0]; n.plugin = pl[0]; n.model = mo[0];
        // "params": [ {"id": 0, "value": 0.05}, ... ]
        for (const std::string& pe : elements(e, "params")) {
            auto pid = ints_for(pe, "id");
            auto pv = doubles_for(pe, "value");
            if (!pid.empty() && !pv.empty()) n.params[pid[0]] = pv[0];
        }
        if (n.plugin != "Core") {
            rack::plugin::Plugin* pl = load_plugin(n.plugin, dir);
            rack::plugin::Model* m = find_model(pl, n.model);
            if (!m && pl)
                std::printf("  ..    %s has no model %s (%zu models loaded)\n",
                            n.plugin.c_str(), n.model.c_str(), pl->models.size());
            if (m) n.mod = m->createModule();
            bring_up(n.mod);
        }
        nodes.push_back(n);
    }

    std::vector<Cable> cables;
    for (const std::string& e : elements(doc, "cables")) {
        Cable c;
        auto om = ints_for(e, "outputModuleId"), op = ints_for(e, "outputId");
        auto im = ints_for(e, "inputModuleId"), ip = ints_for(e, "inputId");
        if (om.empty() || im.empty()) continue;
        c.from = om[0]; c.from_port = op.empty() ? 0 : op[0];
        c.to = im[0];   c.to_port = ip.empty() ? 0 : ip[0];
        cables.push_back(c);
    }

    std::map<long, Node*> by_id;
    for (Node& n : nodes) by_id[n.id] = &n;

    int live = 0, unloadable = 0;
    for (Node& n : nodes) (n.mod ? live : unloadable)++;
    std::printf("patch gate: %zu modules (%d instantiated, %d not a library), "
                "%zu cables\n", nodes.size(), live, unloadable, cables.size());
    if (!live) { fail("no module could be instantiated"); return 1; }

    // Ports must look patched, or a module that guards on isConnected() writes
    // nothing. Assigned directly because the setter refuses to promote a port
    // out of the disconnected state.
    for (const Cable& c : cables) {
        Node* s = by_id.count(c.from) ? by_id[c.from] : nullptr;
        Node* d = by_id.count(c.to) ? by_id[c.to] : nullptr;
        if (s && s->mod && c.from_port < s->mod->getNumOutputs())
            s->mod->outputs[c.from_port].channels = 1;
        if (d && d->mod && c.to_port < d->mod->getNumInputs())
            d->mod->inputs[c.to_port].channels = 1;
    }
    for (Node& n : nodes) {
        if (!n.mod) continue;
        for (int p = 0; p < n.mod->getNumParams(); ++p) {
            auto* q = n.mod->getParamQuantity(p);
            if (!q) continue;
            // The patch's own value when it has one, the default otherwise --
            // which is exactly what Rack does on load. Using defaults for
            // everything measured a patch nobody would ever open: a mixer
            // level stored at zero reads as silence in Rack while the gate,
            // with that same fader at its default, hears the drone perfectly.
            const auto it = n.params.find(p);
            n.mod->params[p].setValue(it != n.params.end()
                                          ? static_cast<float>(it->second)
                                          : q->getDefaultValue());
        }
    }

    // What reaches the audio interface is the thing that matters.
    std::vector<const Cable*> to_audio;
    for (const Cable& c : cables) {
        Node* d = by_id.count(c.to) ? by_id[c.to] : nullptr;
        if (d && d->plugin == "Core" && d->model.rfind("Audio", 0) == 0)
            to_audio.push_back(&c);
    }
    if (to_audio.empty()) { fail("nothing is cabled to an audio interface"); return 1; }

    patch_behaviour::Settings settings;
    settings.sample_rate = kSr;
    if (const char* spec = getenv("PATCH_GATE_SET")) {
        std::string why;
        if (!apply_setting_overrides(settings, spec, why)) {
            std::printf("PATCH GATE FAILED: PATCH_GATE_SET is unusable: %s\n", why.c_str());
            return 2;
        }
    }
    double seconds = kDefaultSeconds;
    if (const char* s = getenv("PATCH_GATE_SECONDS")) {
        const double asked = std::strtod(s, nullptr);
        if (asked > 0.0 && std::isfinite(asked)) seconds = asked;
    }
    const int kRun = static_cast<int>(seconds * kSr);

    rack::engine::Module::ProcessArgs args;
    args.sampleRate = kSr;
    args.sampleTime = 1.f / kSr;
    args.frame = 0;

    std::vector<double> sum_abs(to_audio.size(), 0.0), peak(to_audio.size(), 0.0);
    std::vector<bool> finite(to_audio.size(), true);
    // The whole run, per cable, so the behaviour measurement sees the same
    // samples the presence verdict was computed from. Six seconds of one cable
    // at 48 kHz is about a megabyte; a patch has a handful of cables into the
    // interface, so this is a rounding error against the plugins already loaded.
    std::vector<std::vector<float>> recorded(to_audio.size());
    for (auto& r : recorded) r.reserve(static_cast<std::size_t>(kRun));
    // Peak magnitude over the measured window for every module output. A
    // terminal voltage is not activity: a healthy gate is commonly low on
    // the final frame. The trace feeds retry diagnosis, so it must summarize
    // the whole capture rather than whichever phase happened to end it.
    std::vector<std::vector<double>> output_peak(nodes.size());
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (nodes[n].mod)
            output_peak[n].assign(nodes[n].mod->getNumOutputs(), 0.0);
    }
    long counted = 0;

    for (int i = 0; i < kWarm + kRun; ++i) {
        for (Node& n : nodes)
            if (n.mod) n.mod->process(args);
        // Propagate after every module has run, which is how Rack does it: a
        // feedback path costs one sample of delay rather than being illegal.
        for (const Cable& c : cables) {
            Node* s = by_id.count(c.from) ? by_id[c.from] : nullptr;
            Node* d = by_id.count(c.to) ? by_id[c.to] : nullptr;
            if (!s || !s->mod || !d || !d->mod) continue;
            if (c.from_port >= s->mod->getNumOutputs()) continue;
            if (c.to_port >= d->mod->getNumInputs()) continue;
            d->mod->inputs[c.to_port].setVoltage(
                s->mod->outputs[c.from_port].getVoltage());
        }
        args.frame++;
        if (i < kWarm) continue;
        ++counted;
        for (size_t n = 0; n < nodes.size(); ++n) {
            if (!nodes[n].mod) continue;
            for (int o = 0; o < nodes[n].mod->getNumOutputs(); ++o) {
                const float v = nodes[n].mod->outputs[o].getVoltage();
                if (std::isfinite(v))
                    output_peak[n][o] = std::max(
                        output_peak[n][o], static_cast<double>(std::fabs(v)));
            }
        }
        for (size_t k = 0; k < to_audio.size(); ++k) {
            Node* s = by_id[to_audio[k]->from];
            if (!s || !s->mod) continue;
            if (to_audio[k]->from_port >= s->mod->getNumOutputs()) continue;
            const float v = s->mod->outputs[to_audio[k]->from_port].getVoltage();
            recorded[k].push_back(v);
            if (!std::isfinite(v)) { finite[k] = false; continue; }
            sum_abs[k] += std::fabs(v);
            peak[k] = std::max(peak[k], (double)std::fabs(v));
        }
    }

    if (getenv("PATCH_GATE_TRACE")) {
        std::printf("  --    per-module output activity:\n");
        for (size_t ni = 0; ni < nodes.size(); ++ni) {
            Node& n = nodes[ni];
            if (!n.mod) { std::printf("        %-18s (not instantiated)\n", n.model.c_str()); continue; }
            std::string line;
            for (int o = 0; o < n.mod->getNumOutputs(); ++o) {
                char b[64];
                std::snprintf(b, sizeof b, " out%d=%.3f", o, output_peak[ni][o]);
                line += b;
            }
            // Params too, and not as decoration. A VCA reading 0.000 with a
            // live envelope on its CV is not an untriggered envelope -- it is
            // a level knob at zero, because the knob MULTIPLIES the CV rather
            // than adding to it. Without the params in the trace the only
            // reading left is "its CV never rose", which is the advice this
            // gate gave, and the model spent five attempts re-triggering an
            // envelope that had been firing the whole time.
            std::string pline;
            for (int q = 0; q < n.mod->getNumParams(); ++q) {
                char b[64];
                std::snprintf(b, sizeof b, " p%d=%.3f", q,
                              n.mod->params[q].getValue());
                pline += b;
            }
            std::printf("        %-18s ch=%d%s%s%s\n", n.model.c_str(),
                        n.mod->getNumOutputs() ? n.mod->outputs[0].channels : -1,
                        line.c_str(), pline.empty() ? "" : "  |", pline.c_str());
        }
    }

    std::vector<std::string> names(to_audio.size(), "?");
    for (size_t k = 0; k < to_audio.size(); ++k) {
        Node* s = by_id.count(to_audio[k]->from) ? by_id[to_audio[k]->from] : nullptr;
        if (s) names[k] = s->model + " out " + std::to_string(to_audio[k]->from_port);
    }

    bool any_sound = false;
    for (size_t k = 0; k < to_audio.size(); ++k) {
        const std::string& who = names[k];
        const double mean = counted ? sum_abs[k] / counted : 0.0;
        if (!finite[k]) { fail(who + " produced NaN or Inf"); continue; }
        if (mean < 1e-4) {
            warn(who + " is silent (mean " + std::to_string(mean) + " V)");
        } else {
            any_sound = true;
            if (peak[k] > 20.0)
                fail(who + " peaks at " + std::to_string(peak[k]) + " V, well past line level");
            else
                pass(who + " carries signal (mean " + std::to_string(mean) +
                     " V, peak " + std::to_string(peak[k]) + " V)");
        }
    }
    if (!any_sound)
        fail("every cable into the audio interface is silent — this patch makes no sound");

    // WHAT THE PATCH DOES, reported and not judged. Nothing below adds a
    // failure: a measurement is not a verdict, and this process does not know
    // what was asked for. The caller that DOES know reads these numbers and
    // decides -- and when it decides no, it can say which number said so,
    // which is the difference between a retry with something to fix and a
    // retry with a shrug.
    std::vector<patch_behaviour::Behaviour> behaviours;
    for (size_t k = 0; k < to_audio.size(); ++k)
        behaviours.push_back(patch_behaviour::measure(recorded[k], settings, names[k]));

    std::printf("  --    behaviour over %.1f s:\n", seconds);
    for (const patch_behaviour::Behaviour& b : behaviours)
        std::printf("%s", patch_behaviour::cable_summary(b).c_str());
    std::printf("%s%s\n", patch_behaviour::kJsonMarker,
                patch_behaviour::report_json(behaviours, settings,
                                             getenv("PATCH_GATE_SERIES") != nullptr).c_str());

    std::printf("%s: %d failure(s), %d warning(s)\n",
                failures ? "PATCH GATE FAILED" : "patch gate passed", failures, warnings);
    return failures ? 1 : 0;
}
