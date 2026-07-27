#pragma once

#include <pulp/signal/detail/modular_sequencing_common.hpp>
#include <pulp/signal/trigger_kit.hpp>

namespace pulp::signal {

// ── Gate combinators ──────────────────────────────────────────────────────

/// Boolean operation performed by a `GateLogicT`. Ordered to match the catalog
/// node's `op` parameter.
enum class GateOp {
    logic_and,
    logic_or,
    logic_xor,
    logic_nand,
    logic_nor,
    logic_xnor,
};

/// Combinational logic on gate signals.
///
/// Genuinely stateless — a transport reset is a no-op, because there is nothing
/// to reset. That is why the level-domain overload compares against a plain
/// threshold rather than running each input through a `TriggerDetectT`:
/// hysteresis is memory, and memory in a combinational block would make its
/// output depend on the order inputs happened to arrive in.
///
/// The N-input form applies the base operation across all inputs and inverts for
/// the negated variants — `nand` is `not (a and b and c…)`, `xnor` is the
/// complement of parity — rather than folding the two-input operation pairwise,
/// which for the negated ops would not be associative and would make the answer
/// depend on argument order.
///
/// **USE.** `and` two clocks for an only-on-coincidence gate; `xor` two divided
/// clocks for a composite rhythm busier than either; `or` to merge two gate
/// streams. Three `ClockDividerT` (kit) taps at ÷2/÷3/÷5 through a tree of these
/// is a dense polymetric pattern from one master clock and zero randomness.
///
/// RT contract: pure arithmetic, no state, no allocation.
template <typename SampleType = float>
class GateLogicT {
public:
    /// Level above which an input counts as high in the level-domain overload.
    /// Matches the kit's rising threshold so a gate that opens a
    /// `TriggerDetectT` also reads high here.
    /// [design parameter] default `kTriggerHighThreshold` (0.5), range 0.01 .. 2.5.
    static constexpr double kLevelThreshold = kTriggerHighThreshold;

    void prepare(double) {}

    void set_op(GateOp op) { op_ = op; }
    GateOp op() const { return op_; }

    void apply_reset_edge() {}
    void reset() {}

    static constexpr int latency_samples() { return 0; }

    bool process(bool a, bool b) const {
        switch (op_) {
            case GateOp::logic_and: return a && b;
            case GateOp::logic_or: return a || b;
            case GateOp::logic_xor: return a != b;
            case GateOp::logic_nand: return !(a && b);
            case GateOp::logic_nor: return !(a || b);
            case GateOp::logic_xnor: return a == b;
        }
        return false;
    }

    /// N-input form. An empty input list returns the operation's identity, so a
    /// tree that loses a branch degrades predictably instead of returning false
    /// for everything.
    bool process(const bool* gates, int count) const {
        bool all = true;
        bool any = false;
        bool parity = false;
        for (int i = 0; i < count; ++i) {
            all = all && gates[i];
            any = any || gates[i];
            parity = parity != gates[i];
        }
        switch (op_) {
            case GateOp::logic_and: return all;
            case GateOp::logic_or: return any;
            case GateOp::logic_xor: return parity;
            case GateOp::logic_nand: return !all;
            case GateOp::logic_nor: return !any;
            case GateOp::logic_xnor: return !parity;
        }
        return false;
    }

    /// Level-domain form: thresholds both inputs and returns 0 or 1.
    SampleType process_levels(SampleType a, SampleType b) const {
        const bool ha = static_cast<double>(a) >= kLevelThreshold;
        const bool hb = static_cast<double>(b) >= kLevelThreshold;
        return process(ha, hb) ? SampleType{1} : SampleType{0};
    }

private:
    GateOp op_ = GateOp::logic_and;
};

using GateLogic = GateLogicT<float>;
using GateLogic64 = GateLogicT<double>;

}  // namespace pulp::signal
