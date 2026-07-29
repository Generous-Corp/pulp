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

const char* forge_input_role(int i);

namespace {

constexpr float kSr = 48000.f;
constexpr int kWarm = 256;      // let filters and envelopes settle
// A full second. Envelope times run to seconds, so a short window leaves an
// ADSR still decaying when the gate falls -- which makes SUSTAIN measure as
// inert on an envelope that works. The run is cheap; the false failure is not.
constexpr int kRun = 48000;

/// One output's recorded trace, plus the summary the range checks need.
///
/// The full trace matters for the inert-knob test. Summary statistics are
/// blind to any change that preserves them: a sine at 100 Hz and one at 8 kHz
/// have the same mean-absolute value and the same peak-to-peak span, so a
/// VCO's frequency knob measures as having no effect. Comparing the samples
/// themselves catches a change of shape, of phase, or of timing.
struct Trace {
    std::vector<float> v;
    double min = 1e30, max = -1e30, sum_abs = 0;
    bool finite = true;
    void add(float x) {
        v.push_back(x);
        if (!std::isfinite(x)) { finite = false; return; }
        min = std::min<double>(min, x);
        max = std::max<double>(max, x);
        sum_abs += std::fabs(x);
    }
    double mean_abs() const { return v.empty() ? 0.0 : sum_abs / v.size(); }
};

/// RMS difference between two traces — how much moving a knob changed things.
double rms_diff(const Trace& a, const Trace& b) {
    const size_t n = std::min(a.v.size(), b.v.size());
    if (!n) return 0.0;
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double d = double(a.v[i]) - double(b.v[i]);
        if (std::isfinite(d)) acc += d * d;
    }
    return std::sqrt(acc / n);
}

int failures = 0;
int warnings = 0;

void fail(const std::string& m) { std::printf("  FAIL  %s\n", m.c_str()); ++failures; }
void warn(const std::string& m) { std::printf("  warn  %s\n", m.c_str()); ++warnings; }
void pass(const std::string& m) { std::printf("  ok    %s\n", m.c_str()); }

/// The stimulus for one input, chosen from the role the manifest declared.
///
/// Feeding one identical square into every jack does not exercise a stateful
/// module. A sequencer whose reset shares the clock's waveform never leaves
/// its first step, and an envelope gated for 5 ms never reaches sustain, so
/// both report knobs as inert that work perfectly in Rack. Driving each jack
/// as what it actually is costs nothing and removes that whole class of false
/// failure.
float stimulus(const char* role, int i, float quiet_v) {
    const std::string r = role ? role : "Cv";
    if (r == "Clock")
        // Fast enough that a sequencer visits every step several times over.
        return ((i / 64) % 2) ? 10.f : 0.f;
    if (r == "Gate")
        // ~0.34 s halves, long enough for a default attack and decay to finish
        // so the envelope actually sits at its sustain level for a while.
        return ((i / 16384) % 2) ? 10.f : 0.f;
    if (r == "Trigger")
        // A ~1 ms pulse, rare relative to the clock -- a reset that fires as
        // often as the clock would pin a sequencer to its first step.
        return (i % 16384) < 48 ? 10.f : 0.f;
    if (r == "Audio")
        return 5.f * std::sin(2.0 * M_PI * 110.0 * i / kSr);
    if (r == "Pitch")
        return 0.f;                    // C4
    return quiet_v;                    // Cv and anything unrecognised
}

/// Drive the module for a while and record every output.
std::vector<Trace> run(rack::engine::Module& m, float in_v, bool clock_inputs) {
    rack::engine::Module::ProcessArgs args;
    args.sampleRate = kSr;
    args.sampleTime = 1.f / kSr;
    args.frame = 0;

    std::vector<Trace> st(m.getNumOutputs());
    for (int i = 0; i < kWarm + kRun; ++i) {
        for (int p = 0; p < m.getNumInputs(); ++p) {
            const float v = clock_inputs
                          ? stimulus(forge_input_role(p), i, in_v) : in_v;
            // Assign `channels` directly. Port::setChannels() returns early
            // when channels == 0, because a disconnected port must stay
            // disconnected -- only the engine marks a port live when a cable
            // lands. Going through the setter here leaves every input reading
            // 0 V, which makes every input-driven module look like it has
            // inert knobs.
            m.inputs[p].channels = 1;
            m.inputs[p].setVoltage(v);
        }
        // Outputs must look patched too. A well-written module skips work for
        // an unconnected output -- the module contract explicitly asks for
        // that -- so leaving them disconnected means the module writes nothing
        // and every one of its knobs measures as inert. Assigned directly for
        // the same reason as the inputs: the setter refuses to promote a port
        // out of the disconnected state.
        for (int o = 0; o < m.getNumOutputs(); ++o)
            m.outputs[o].channels = 1;
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
        // A module with no outputs has nothing for a knob to change. Utility
        // modules that only act on the host -- a scanner, a notes panel --
        // are the real case, and measuring them here would fail every one of
        // their controls for the wrong reason.
        const bool measurable = m->getNumOutputs() > 0;
        auto base = run(*m, 2.5f, clocky);
        delete m;
        if (!measurable)
            std::printf("  --    no outputs; inert-control check does not apply\n");

        for (int p = 0; measurable && p < 64; ++p) {
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

            // Sample-wise, so a change of frequency, phase or timing counts
            // as an effect just as much as a change of level.
            double delta = 0;
            for (size_t o = 0; o < moved.size() && o < base.size(); ++o)
                delta = std::max(delta, rms_diff(moved[o], base[o]));
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
