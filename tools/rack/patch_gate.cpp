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
// Usage: patch-gate <patch.vcv> <plugin-dir>

#include <rack.hpp>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr float kSr = 48000.f;
constexpr int kWarm = 512;
constexpr int kRun = 48000;          // a second, so slow envelopes finish

int failures = 0, warnings = 0;
void fail(const std::string& m) { std::printf("  FAIL  %s\n", m.c_str()); ++failures; }
void warn(const std::string& m) { std::printf("  warn  %s\n", m.c_str()); ++warnings; }
void pass(const std::string& m) { std::printf("  ok    %s\n", m.c_str()); }

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

    rack::engine::Module::ProcessArgs args;
    args.sampleRate = kSr;
    args.sampleTime = 1.f / kSr;
    args.frame = 0;

    std::vector<double> sum_abs(to_audio.size(), 0.0), peak(to_audio.size(), 0.0);
    std::vector<bool> finite(to_audio.size(), true);
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
        for (size_t k = 0; k < to_audio.size(); ++k) {
            Node* s = by_id[to_audio[k]->from];
            if (!s || !s->mod) continue;
            if (to_audio[k]->from_port >= s->mod->getNumOutputs()) continue;
            const float v = s->mod->outputs[to_audio[k]->from_port].getVoltage();
            if (!std::isfinite(v)) { finite[k] = false; continue; }
            sum_abs[k] += std::fabs(v);
            peak[k] = std::max(peak[k], (double)std::fabs(v));
        }
    }

    if (getenv("PATCH_GATE_TRACE")) {
        std::printf("  --    per-module output activity:\n");
        for (Node& n : nodes) {
            if (!n.mod) { std::printf("        %-18s (not instantiated)\n", n.model.c_str()); continue; }
            std::string line;
            for (int o = 0; o < n.mod->getNumOutputs(); ++o) {
                char b[64];
                std::snprintf(b, sizeof b, " out%d=%.3f", o, n.mod->outputs[o].getVoltage());
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

    bool any_sound = false;
    for (size_t k = 0; k < to_audio.size(); ++k) {
        Node* s = by_id[to_audio[k]->from];
        const std::string who = s ? (s->model + " out " +
                                     std::to_string(to_audio[k]->from_port)) : "?";
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

    std::printf("%s: %d failure(s), %d warning(s)\n",
                failures ? "PATCH GATE FAILED" : "patch gate passed", failures, warnings);
    return failures ? 1 : 0;
}
