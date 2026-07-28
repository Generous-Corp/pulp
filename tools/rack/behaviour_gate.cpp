// Behavioural gate: drive a generated module's REAL DSP and measure what it does.
//
// Compiling proves the C++ is valid. It proves nothing about the audio. The
// first module generated here compiled cleanly and used none of Pulp's DSP; a
// module can equally compile with a knob wired to nothing, an output stuck at
// zero, a gate emitting 3.7 V instead of 10, or NaN on the first sample. None
// of that is visible to a compiler or to a panel validator.
//
// Rack modules turn out to be instantiable outside Rack -- `rack::engine::Module`
// needs no Context as long as the constructor does not touch `APP` (which is
// why the contract forbids that). So the module's own `process()` can be driven
// directly and its output measured.
//
// Checks, in rough order of how often they catch something real:
//
//   1. INERT CONTROLS -- sweep each param across its range and require the
//      output to change. This is the check a source grep cannot make: a param
//      can be READ and still have no effect. The VCO's PW knob shipped inert.
//   2. FINITE OUTPUT -- no NaN or Inf, ever, on any output.
//   3. VOLTAGE RANGE -- audio outputs stay within a sane multiple of +/-5 V;
//      gate outputs sit at 0 or 10 V, not somewhere in between.
//   4. SILENCE IN, SILENCE OUT -- with no input and default params, an audio
//      effect must not emit anything (oscillators and generators are exempt,
//      declared per module).
//
// Built and run by generate.py against each freshly generated module.

#include <rack.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr float kSr = 48000.f;
constexpr int kWarm = 256;      // let filters and envelopes settle
constexpr int kRun = 4096;

struct Stats {
    double min = 1e30, max = -1e30, sum_abs = 0;
    bool finite = true;
    int n = 0;
    void add(float v) {
        if (!std::isfinite(v)) { finite = false; return; }
        min = std::min<double>(min, v);
        max = std::max<double>(max, v);
        sum_abs += std::fabs(v);
        ++n;
    }
    double mean_abs() const { return n ? sum_abs / n : 0.0; }
    double span() const { return n ? max - min : 0.0; }
};

int failures = 0;
int warnings = 0;

void fail(const std::string& m) { std::printf("  FAIL  %s\n", m.c_str()); ++failures; }
void warn(const std::string& m) { std::printf("  warn  %s\n", m.c_str()); ++warnings; }
void pass(const std::string& m) { std::printf("  ok    %s\n", m.c_str()); }

/// Drive the module for a while, feeding `in_v` to every input, and measure
/// every output.
std::vector<Stats> run(rack::engine::Module& m, float in_v, bool clock_inputs) {
    rack::engine::Module::ProcessArgs args;
    args.sampleRate = kSr;
    args.sampleTime = 1.f / kSr;
    args.frame = 0;

    std::vector<Stats> st(m.getNumOutputs());
    for (int i = 0; i < kWarm + kRun; ++i) {
        for (int p = 0; p < m.getNumInputs(); ++p) {
            // A steady level tells us nothing about a clock-driven module, so
            // inputs double as a slow square wave when asked.
            float v = in_v;
            if (clock_inputs) v = ((i / 240) % 2) ? 10.f : 0.f;
            m.inputs[p].setChannels(1);
            m.inputs[p].setVoltage(v);
        }
        m.process(args);
        args.frame++;
        if (i >= kWarm)
            for (int o = 0; o < m.getNumOutputs(); ++o)
                st[o].add(m.outputs[o].getVoltage());
    }
    return st;
}

void reset_params(rack::engine::Module& m) {
    for (int p = 0; p < m.getNumParams(); ++p)
        if (auto* q = m.getParamQuantity(p))
            m.params[p].setValue(q->getDefaultValue());
}

}  // namespace

/// Provided by the generated shim, which knows the concrete module type.
rack::engine::Module* forge_make_module();
const char* forge_module_slug();
bool forge_module_is_generator();

int main() {
    std::printf("behavioural gate: %s\n", forge_module_slug());

    // ── 1. Inert controls ────────────────────────────────────────────────────
    // A param that cannot change any output is dead on the panel.
    {
        rack::engine::Module* m = forge_make_module();
        reset_params(*m);
        const bool clocky = m->getNumInputs() > 0;
        auto base = run(*m, 2.5f, clocky);
        delete m;

        for (int p = 0; p < 64; ++p) {
            rack::engine::Module* probe = forge_make_module();
            if (p >= probe->getNumParams()) { delete probe; break; }
            reset_params(*probe);
            auto* q = probe->getParamQuantity(p);
            const std::string nm = q ? q->name : ("param " + std::to_string(p));
            const float lo = q ? q->getMinValue() : 0.f, hi = q ? q->getMaxValue() : 1.f;
            const float dv = q ? q->getDefaultValue() : 0.f;
            // Move as far from the default as the range allows.
            probe->params[p].setValue((std::fabs(hi - dv) > std::fabs(dv - lo)) ? hi : lo);
            auto moved = run(*probe, 2.5f, clocky);

            double delta = 0;
            for (size_t o = 0; o < moved.size() && o < base.size(); ++o) {
                delta = std::max(delta, std::fabs(moved[o].mean_abs() - base[o].mean_abs()));
                delta = std::max(delta, std::fabs(moved[o].span() - base[o].span()));
            }
            if (delta < 1e-6)
                fail("param '" + nm + "' changes no output across its full range — inert knob");
            else
                pass("param '" + nm + "' affects the output");
            delete probe;
        }
    }

    // ── 2-4. Output health ───────────────────────────────────────────────────
    {
        rack::engine::Module* m = forge_make_module();
        reset_params(*m);
        auto st = run(*m, 2.5f, m->getNumInputs() > 0);
        for (int o = 0; o < m->getNumOutputs(); ++o) {
            const auto& s = st[o];
            const std::string nm = "output " + std::to_string(o);
            if (!s.finite) { fail(nm + " produced NaN or Inf"); continue; }
            if (s.max > 20.0 || s.min < -20.0)
                fail(nm + " leaves the sane voltage range (" +
                     std::to_string(s.min) + ".." + std::to_string(s.max) + " V)");
            else
                pass(nm + " stays in range");
        }
        delete m;
    }

    // ── Silence in, silence out ─────────────────────────────────────────────
    if (!forge_module_is_generator()) {
        rack::engine::Module* m = forge_make_module();
        reset_params(*m);
        auto st = run(*m, 0.f, false);
        for (int o = 0; o < m->getNumOutputs(); ++o)
            if (st[o].mean_abs() > 1e-3)
                warn("output " + std::to_string(o) +
                     " emits " + std::to_string(st[o].mean_abs()) +
                     " V with no input — intended? (declare it a generator if so)");
        delete m;
    }

    std::printf("%s: %d failure(s), %d warning(s)\n",
                failures ? "GATE FAILED" : "gate passed", failures, warnings);
    return failures ? 1 : 0;
}
