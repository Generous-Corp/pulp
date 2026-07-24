#pragma once

// Multirate FDN reverb — the bake-layer catalog node.
//
// Wraps signal::FdnReverbT as five registered node types (room / hall / galaxy
// / shimmer / lofi) that are ONE realization of ONE engine. A mode stamps the
// twelve baked params' defaults and the per-mode output lowpass; it never
// selects a code path, so every mode runs byte-identical DSP and the difference
// between a room and a galaxy is entirely in the numbers. That is what makes
// "five reverbs" cost one implementation to maintain and one suite to prove.
//
// TRUE STEREO, WET ONLY. The tank's fan-out already puts left into the even
// lines and right into the odd, so the node's two ports are the L and R halves
// of one logical wire (Forge instances it once across both rails) rather than a
// dual-mono pair. It emits wet only: dry/wet is the graph's `make_drywet_node`,
// so this block never has to guess what "dry" means for its caller.
//
// Header-only, like the rest of the catalog: FdnReverbT is a header-only
// template, so including this pulls in no new link dependency for pulp-host.
//
// PARAMETER GRANULARITY. The twelve knobs are read at the engine's own control
// cadence (32 samples) rather than once per block: the node walks the block in
// control-rate chunks and re-reads the BakedParamView at each one. That is the
// honest granularity — the engine re-derives its delay lengths, decay gains and
// damping coefficients on exactly that cadence and smooths continuous params
// over 5 ms, so a finer read would be discarded, and a block-rate read would
// make a knob sweep step audibly at large buffer sizes.

#include <pulp/host/signal_graph.hpp>

#include <pulp/signal/fdn_reverb.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pulp::host::forge_fdn {

namespace fdn = pulp::signal::fdn;

// ── Stable type ids ──────────────────────────────────────────────────────
// One per mode. The id is what a baked artifact stores, so it is frozen: a
// re-voicing of a mode changes its defaults, never its id.
inline constexpr const char* kRoomTypeId = "reverb.room";
inline constexpr const char* kHallTypeId = "reverb.hall";
inline constexpr const char* kGalaxyTypeId = "reverb.galaxy";
inline constexpr const char* kShimmerTypeId = "reverb.shimmer";
inline constexpr const char* kLofiTypeId = "reverb.lofi";

inline constexpr const char* type_id_for(fdn::Mode mode) {
    switch (mode) {
        case fdn::Mode::room: return kRoomTypeId;
        case fdn::Mode::hall: return kHallTypeId;
        case fdn::Mode::galaxy: return kGalaxyTypeId;
        case fdn::Mode::shimmer: return kShimmerTypeId;
        case fdn::Mode::lofi: return kLofiTypeId;
        case fdn::Mode::count: break;
    }
    return kHallTypeId;
}

inline constexpr const char* display_name_for(fdn::Mode mode) {
    switch (mode) {
        case fdn::Mode::room: return "Room";
        case fdn::Mode::hall: return "Hall";
        case fdn::Mode::galaxy: return "Galaxy";
        case fdn::Mode::shimmer: return "Shimmer";
        case fdn::Mode::lofi: return "Lo-Fi";
        case fdn::Mode::count: break;
    }
    return "Reverb";
}

// ── Injectable param ids ─────────────────────────────────────────────────
// Node-local (the framework namespaces per node), and deliberately laid out as
// `1 + the engine's own parameter index` so the two orders can never drift:
// param_id_for / engine_param_for are exact inverses, and the contract test
// walks the whole table rather than trusting the mapping.
inline constexpr state::ParamID param_id_for(fdn::Param p) {
    return static_cast<state::ParamID>(static_cast<int>(p) + 1);
}

inline constexpr fdn::Param engine_param_for(state::ParamID id) {
    return static_cast<fdn::Param>(static_cast<int>(id) - 1);
}

inline constexpr state::ParamID kDecay = param_id_for(fdn::Param::decay);
inline constexpr state::ParamID kSize = param_id_for(fdn::Param::size);
inline constexpr state::ParamID kPredelay = param_id_for(fdn::Param::predelay);
inline constexpr state::ParamID kDampHi = param_id_for(fdn::Param::damp_hi);
inline constexpr state::ParamID kDampLo = param_id_for(fdn::Param::damp_lo);
inline constexpr state::ParamID kDiffusion = param_id_for(fdn::Param::diffusion);
inline constexpr state::ParamID kMod = param_id_for(fdn::Param::mod);
inline constexpr state::ParamID kShimmer = param_id_for(fdn::Param::shimmer);
inline constexpr state::ParamID kDrive = param_id_for(fdn::Param::drive);
inline constexpr state::ParamID kBloom = param_id_for(fdn::Param::bloom);
inline constexpr state::ParamID kWidth = param_id_for(fdn::Param::width);
inline constexpr state::ParamID kTankRate = param_id_for(fdn::Param::tank_rate);

// Worst-case linear gain the node can present, for the path-gain lint. The tank
// is unitary and the realized per-pass gain is provably below kGainCeil, so the
// wet tail cannot exceed its own steady state; the output soft limiter caps the
// wet return at kWetLimiterHeadroom regardless, which is the hard bound.
inline constexpr float kWorstCaseGain = static_cast<float>(fdn::kWetLimiterHeadroom);

struct ReverbInstance {
    signal::FdnReverbT<float> engine;
    fdn::Mode mode = fdn::Mode::hall;
};

// Declare the twelve baked params with the mode's stamped defaults. Ranges come
// from the engine's own parameter table and defaults from the mode table, so
// there is no second copy of either to drift.
inline void declare_params(CustomNodeType& t, fdn::Mode mode) {
    for (int p = 0; p < fdn::kNumParams; ++p) {
        const fdn::ParamSpec& spec = fdn::kParamSpecs[static_cast<std::size_t>(p)];
        const auto param = static_cast<fdn::Param>(p);
        t.baked_params.push_back({param_id_for(param), static_cast<float>(spec.min),
                                  static_cast<float>(spec.max),
                                  static_cast<float>(fdn::mode_default(mode, param))});
    }
}

inline CustomNodeType make_fdn_reverb_node(fdn::Mode mode) {
    CustomNodeType t;
    t.type_id = type_id_for(mode);
    t.version = 1;
    t.num_input_ports = 2;
    t.num_output_ports = 2;
    t.default_name = display_name_for(mode);
    t.lowerable = true;

    const auto captured_mode = mode;
    t.create = [captured_mode]() -> void* {
        auto* s = new ReverbInstance{};
        s->mode = captured_mode;
        return s;
    };
    t.destroy = [](void* p) { delete static_cast<ReverbInstance*>(p); };
    t.prepare = [](void* p, double sr, int max_block) {
        auto* s = static_cast<ReverbInstance*>(p);
        // prepare() is the only allocating call; it sizes for the worst case
        // (96 kHz tank at this block size) so a live tank_rate injection never
        // reaches the allocator.
        s->engine.prepare(sr, std::max(max_block, 1));
        s->engine.set_mode(s->mode);
    };
    t.reset = [](void* p) { static_cast<ReverbInstance*>(p)->engine.reset(); };

    declare_params(t, mode);

    t.process_instance_baked_param =
        [](void* p, audio::BufferView<float>& out,
           const audio::BufferView<const float>& in, int n, const BakedParamView& params) {
            auto* s = static_cast<ReverbInstance*>(p);
            const float* il = in.channel_ptr(0);
            const float* ir = in.channel_ptr(1);
            float* ol = out.channel_ptr(0);
            float* or_ = out.channel_ptr(1);

            for (int offset = 0; offset < n; offset += fdn::kControlRateSamples) {
                const int chunk = std::min(n - offset, fdn::kControlRateSamples);
                const auto at = static_cast<std::int32_t>(offset);
                for (int k = 0; k < fdn::kNumParams; ++k) {
                    const auto param = static_cast<fdn::Param>(k);
                    s->engine.set_parameter(
                        param, static_cast<double>(params.value_at(param_id_for(param), at)));
                }
                s->engine.process_block(il + offset, ir + offset, ol + offset,
                                        or_ + offset, chunk);
            }
        };
    return t;
}

}  // namespace pulp::host::forge_fdn
