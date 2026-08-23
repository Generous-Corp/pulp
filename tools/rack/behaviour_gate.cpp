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
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

const char* forge_input_role(int i);
const char* forge_output_role(int i);
const char* forge_output_name(int i);
bool forge_require_clock_contract();
int forge_clock_output();
int forge_phase_output();
int forge_rate_param();
int forge_width_param();
int forge_reset_input();

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

int gate_transitions(const Trace& trace) {
    int transitions = 0;
    bool previous = false;
    for (size_t i = 0; i < trace.v.size(); ++i) {
        const bool state = trace.v[i] >= 5.f;
        if (i && state != previous) ++transitions;
        previous = state;
    }
    return transitions;
}

double high_fraction(const Trace& trace) {
    if (trace.v.empty()) return 0.0;
    size_t high = 0;
    for (float value : trace.v)
        high += value >= 5.f;
    return double(high) / trace.v.size();
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
float stimulus(const char* role, int i, int input_index) {
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
        // A connected 0 V pitch jack is electrically identical to the usual
        // disconnected 0 V normal. One octave above the reference proves the
        // port changes frequency while remaining ordinary 1 V/oct input.
        return 1.f;
    // A slow square around the quiet level, NOT a constant. Anything that
    // processes CV over time -- slew, lag, portamento, glide, envelope
    // followers -- produces an identical output for every setting when the
    // input never moves, so its rise and fall knobs are reported inert while
    // working perfectly in Rack. ~0.37 s halves: long enough for a slow slew
    // to finish, short enough that the probe sees many edges.
    // Give each CV jack a different quarter-cycle phase, centred on zero.
    // Driving the signal input and every modulation input with the exact same
    // 0..5 V square makes their edges perfectly correlated
    // and pins exponential modulation laws at an endpoint. A slew limiter can
    // then see its rise CV saturated on every rising signal edge, so moving the
    // corresponding knob cannot change the trace even though it is wired.
    // Every jack still receives a useful bipolar volt, without assuming that
    // input zero is the signal and every later CV is modulation; manifests do
    // not require that ordering.
    const int phased_sample = i + input_index * 4096;
    return ((phased_sample / 16384) % 2) ? 1.0f : -1.0f;
}

/// Drive the module for a while and record every output.
std::vector<Trace> run(rack::engine::Module& m, float in_v, bool clock_inputs,
                       int muted_input = -1, bool connect_inputs = true) {
    rack::engine::Module::ProcessArgs args;
    args.sampleRate = kSr;
    args.sampleTime = 1.f / kSr;
    args.frame = 0;

    std::vector<Trace> st(m.getNumOutputs());
    for (int i = 0; i < kWarm + kRun; ++i) {
        for (int p = 0; p < m.getNumInputs(); ++p) {
            if (!connect_inputs) {
                m.inputs[p].channels = 0;
                continue;
            }
            const float v = p == muted_input ? 0.f : clock_inputs
                          ? stimulus(forge_input_role(p), i, p) : in_v;
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
        // A clock/sync input can legitimately reset a phase generator.  The
        // role-aware stimulus above then keeps its phase so near zero that a
        // working WIDTH control appears inert.  Keep the driven probe for
        // controls that need input, but also compare an all-quiet run so
        // controls on self-running generators get a fair measurement.
        rack::engine::Module* quiet_baseline = forge_make_module();
        reset_params(*quiet_baseline);
        auto quiet_base = run(*quiet_baseline, 0.f, false, -1, false);
        delete quiet_baseline;
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

            rack::engine::Module* quiet_probe = forge_make_module();
            reset_params(*quiet_probe);
            quiet_probe->params[p].setValue(
                (std::fabs(hi - dv) > std::fabs(dv - lo)) ? hi : lo);
            auto quiet_moved = run(*quiet_probe, 0.f, false, -1, false);

            // Sample-wise, so a change of frequency, phase or timing counts
            // as an effect just as much as a change of level.
            double delta = 0;
            for (size_t o = 0; o < moved.size() && o < base.size(); ++o)
                delta = std::max(delta, rms_diff(moved[o], base[o]));
            for (size_t o = 0;
                 o < quiet_moved.size() && o < quiet_base.size(); ++o)
                delta = std::max(
                    delta, rms_diff(quiet_moved[o], quiet_base[o]));
            if (delta < 1e-6)
                fail("param '" + nm + "' changes no output across its full range — inert knob");
            else
                pass("param '" + nm + "' affects the output");
            delete quiet_probe;
            delete probe;
        }
    }

    // Why an inert input is inert, when the gate can tell.
    //
    // "input N changes no output" is true and unactionable: it names the
    // symptom and leaves the reason to be guessed. The commonest reason is not
    // a miswired jack at all — it is a MODULATED PARAM WHOSE DEFAULT SITS IN A
    // FLAT REGION OF ITS LAW, where the CV is added to a knob position that
    // cannot move. `AnalogVcfT`'s minimoog resonance is flat at 0.079 across
    // knob 0.0-0.50, so a resonance CV on a knob defaulted at 0.30 is
    // arithmetically incapable of changing the output, and every retry
    // reproduces it exactly.
    //
    // The param sweep in section 1 cannot see this: it moves each knob across
    // its FULL range, where such a law does move, so the knob reads as live.
    // Only the default position is dead.
    //
    // So: re-probe with every param pushed off its default. If the input
    // becomes observable there, the jack is wired and the DEFAULT is the
    // defect — which is a thing a generator can actually fix, unlike "no
    // output". Reported as a hint rather than a separate verdict, because the
    // run still failed and the remedy is a guess about which param.
    auto inert_input_hint = [&](int input) -> std::string {
        rack::engine::Module* moved_ref = forge_make_module();
        rack::engine::Module* moved_mute = forge_make_module();
        auto bias = [](rack::engine::Module& m) {
            reset_params(m);
            for (int q = 0; q < m.getNumParams(); ++q) {
                if (auto* pq = m.getParamQuantity(q)) {
                    // 0.75 of range: past minimoog's 0.60 knee, and off the
                    // default without pinning to an extreme that would make
                    // an unrelated nonlinearity look like the cause.
                    const float lo = pq->getMinValue(), hi = pq->getMaxValue();
                    m.params[q].setValue(lo + 0.75f * (hi - lo));
                }
            }
        };
        bias(*moved_ref);
        bias(*moved_mute);
        auto mb = run(*moved_ref, 2.5f, true);
        auto mm = run(*moved_mute, 2.5f, true, input);
        double moved_delta = 0;
        for (size_t o = 0; o < mm.size() && o < mb.size(); ++o)
            moved_delta = std::max(moved_delta, rms_diff(mm[o], mb[o]));
        delete moved_ref;
        delete moved_mute;
        if (moved_delta < 1e-6) return "";
        return " — but it DOES affect the output with the params moved off "
               "their defaults, so the jack is wired and a param it modulates "
               "has a default in a flat region of its law (e.g. AnalogVcfT "
               "minimoog resonance is flat below knob 0.50). Raise that "
               "param's default into the responsive region";
    };

    // ── 2. Inert inputs ──────────────────────────────────────────────────────
    // Compare the normal role-aware stimulus with one input muted while every
    // other input remains active. This exercises interaction-dependent ports
    // (reset beside clock, CV beside audio) without demanding that each jack
    // produce a signal in total isolation.
    {
        rack::engine::Module* reference = forge_make_module();
        reset_params(*reference);
        auto base = run(*reference, 2.5f, true);
        const int input_count = reference->getNumInputs();
        const bool measurable = reference->getNumOutputs() > 0;
        delete reference;
        for (int p = 0; measurable && p < input_count; ++p) {
            rack::engine::Module* probe = forge_make_module();
            reset_params(*probe);
            auto muted = run(*probe, 2.5f, true, p);
            double delta = 0;
            for (size_t o = 0; o < muted.size() && o < base.size(); ++o)
                delta = std::max(delta, rms_diff(muted[o], base[o]));
            if (delta < 1e-6)
                fail("input " + std::to_string(p) +
                     " changes no output when connected versus muted" +
                     inert_input_hint(p));
            else
                pass("input " + std::to_string(p) + " affects the output");
            delete probe;
        }
    }

    // ── 3-5. Output health ───────────────────────────────────────────────────
    {
        rack::engine::Module* m = forge_make_module();
        reset_params(*m);
        // The prompt-derived clock fixture drives RESET separately below.
        // Periodically resetting a slow clock here can keep it permanently in
        // its high half and create a false "no events" failure.
        auto st = run(*m, 2.5f,
                      !forge_require_clock_contract() && m->getNumInputs() > 0);
        for (int o = 0; o < m->getNumOutputs(); ++o) {
            const auto& s = st[o];
            const std::string nm = "output " + std::to_string(o);
            if (!s.finite) { fail(nm + " produced NaN or Inf"); continue; }
            if (s.max > 20.0 || s.min < -20.0)
                fail(nm + " leaves the sane voltage range (" +
                     std::to_string(s.min) + ".." + std::to_string(s.max) + " V)");
            else
                pass(nm + " stays in range");

            const std::string role = forge_output_role(o);
            if (role == "Clock" || role == "Gate" || role == "Trigger") {
                int transitions = 0;
                bool previous = false;
                bool saw_low = false, saw_high = false, levels_ok = true;
                for (size_t i = 0; i < s.v.size(); ++i) {
                    const float value = s.v[i];
                    const bool low = std::fabs(value) <= 0.15f;
                    const bool high = std::fabs(value - 10.f) <= 0.15f;
                    saw_low |= low;
                    saw_high |= high;
                    levels_ok &= low || high;
                    const bool state = value >= 5.f;
                    if (i && state != previous) ++transitions;
                    previous = state;
                }
                if (!levels_ok)
                    fail(nm + " declared " + role +
                         " but emitted an intermediate voltage instead of 0/10 V");
                else if (!saw_low || !saw_high || transitions < 2)
                    fail(nm + " declared " + role +
                         " but did not produce repeated low/high events");
                else
                    pass(nm + " obeys the 0/10 V " + role + " contract");
            }

            std::string declared_name = forge_output_name(o);
            for (char& c : declared_name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (declared_name.find("phase") != std::string::npos) {
                if (s.min < -0.05 || s.max > 10.05 || s.max - s.min < 1.0)
                    fail(nm + " named Phase must move across a useful part of 0..10 V");
                else
                    pass(nm + " obeys the moving 0..10 V phase contract");
            }
        }
        delete m;
    }

    // ── Prompt-derived clock contract ──────────────────────────────────────
    // Activation and indices come from the user's request after a separate
    // manifest-fidelity check. The model cannot evade these measurements by
    // omitting a role or renaming an output in its own manifest.
    if (forge_require_clock_contract()) {
        const int clock = forge_clock_output();
        const int phase = forge_phase_output();
        const int rate = forge_rate_param();
        const int width = forge_width_param();
        const int reset = forge_reset_input();

        rack::engine::Module* base_module = forge_make_module();
        const bool indices_ok = clock >= 0 && clock < base_module->getNumOutputs() &&
                                phase >= 0 && phase < base_module->getNumOutputs() &&
                                rate >= 0 && rate < base_module->getNumParams() &&
                                width >= 0 && width < base_module->getNumParams() &&
                                reset >= 0 && reset < base_module->getNumInputs();
        delete base_module;
        if (!indices_ok) {
            fail("prompt-derived clock contract could not resolve every requested control and port");
        } else {
            rack::engine::Module* nominal = forge_make_module();
            reset_params(*nominal);
            auto traces = run(*nominal, 0.f, false);
            const Trace& c = traces[clock];
            const Trace& p = traces[phase];
            bool levels_ok = true, saw_low = false, saw_high = false;
            for (float value : c.v) {
                const bool low = std::fabs(value) <= 0.15f;
                const bool high = std::fabs(value - 10.f) <= 0.15f;
                levels_ok &= low || high;
                saw_low |= low;
                saw_high |= high;
            }
            if (!levels_ok || !saw_low || !saw_high || gate_transitions(c) < 2)
                fail("requested CLOCK must emit repeated exact 0/10 V events");
            else
                pass("prompt requires and receives repeated 0/10 V CLOCK events");
            if (!p.finite || p.min < -0.05 || p.max > 10.05 || p.max - p.min < 1.0)
                fail("requested PHASE must move across a useful part of 0..10 V");
            else
                pass("prompt requires and receives moving 0..10 V PHASE");
            delete nominal;

            rack::engine::Module* slow = forge_make_module();
            rack::engine::Module* fast = forge_make_module();
            reset_params(*slow); reset_params(*fast);
            auto* rate_quantity = slow->getParamQuantity(rate);
            slow->params[rate].setValue(rate_quantity->getMinValue());
            fast->params[rate].setValue(rate_quantity->getMaxValue());
            auto slow_trace = run(*slow, 0.f, false)[clock];
            auto fast_trace = run(*fast, 0.f, false)[clock];
            if (gate_transitions(fast_trace) < gate_transitions(slow_trace) + 2)
                fail("RATE must monotonically increase CLOCK event frequency");
            else
                pass("RATE monotonically controls CLOCK event frequency");
            delete slow; delete fast;

            rack::engine::Module* narrow = forge_make_module();
            rack::engine::Module* wide = forge_make_module();
            reset_params(*narrow); reset_params(*wide);
            auto* width_quantity = narrow->getParamQuantity(width);
            narrow->params[width].setValue(width_quantity->getMinValue());
            wide->params[width].setValue(width_quantity->getMaxValue());
            const double narrow_duty = high_fraction(run(*narrow, 0.f, false)[clock]);
            const double wide_duty = high_fraction(run(*wide, 0.f, false)[clock]);
            if (wide_duty < narrow_duty + 0.2)
                fail("WIDTH must materially increase CLOCK high-time duty cycle");
            else
                pass("WIDTH materially controls CLOCK duty cycle");
            delete narrow; delete wide;

            rack::engine::Module* reset_probe = forge_make_module();
            reset_params(*reset_probe);
            rack::engine::Module::ProcessArgs args;
            args.sampleRate = kSr; args.sampleTime = 1.f / kSr; args.frame = 0;
            for (int i = 0; i < 2048; ++i) {
                for (int input = 0; input < reset_probe->getNumInputs(); ++input) {
                    reset_probe->inputs[input].channels = 1;
                    reset_probe->inputs[input].setVoltage(0.f);
                }
                for (int output = 0; output < reset_probe->getNumOutputs(); ++output)
                    reset_probe->outputs[output].channels = 1;
                reset_probe->process(args); ++args.frame;
            }
            const float before_reset = reset_probe->outputs[phase].getVoltage();
            reset_probe->inputs[reset].setVoltage(10.f);
            reset_probe->process(args);
            const float after_reset = reset_probe->outputs[phase].getVoltage();
            if (before_reset < 0.1f || after_reset > 0.2f ||
                    after_reset > before_reset - 0.05f)
                fail("RESET must return PHASE to zero on the triggering sample");
            else
                pass("RESET returns PHASE to zero on the triggering sample");
            delete reset_probe;
        }
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
