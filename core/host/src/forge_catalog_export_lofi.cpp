#include "forge_catalog_export_detail.hpp"

#include <pulp/host/forge_fdn_reverb_catalog.hpp>
#include <pulp/host/forge_lofi_catalog.hpp>
#include <pulp/host/forge_modulation_catalog.hpp>

namespace pulp::host::forge_catalog_export_detail {

void append_lofi(Nodes& nodes) {
    add(nodes, forge_fdn::fdn_reverb_descriptor(),
        {
            realization("room", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::room)),
            realization("hall", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::hall)),
            realization("galaxy", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::galaxy)),
            realization("shimmer", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::shimmer)),
            realization("lofi", forge_fdn::make_fdn_reverb_node(forge_fdn::fdn::Mode::lofi)),
        });

    add(nodes, forge_lofi::delay_descriptor(), {realization("default", forge_lofi::make_delay_node())});
    add(nodes, forge_lofi::filter_descriptor(),
        {
            realization("lowpass", forge_lofi::make_filter_node(signal::Svf::Mode::lowpass)),
            realization("highpass", forge_lofi::make_filter_node(signal::Svf::Mode::highpass)),
            realization("bandpass", forge_lofi::make_filter_node(signal::Svf::Mode::bandpass)),
            realization("notch", forge_lofi::make_filter_node(signal::Svf::Mode::notch)),
        });
    add(nodes, forge_lofi::waveshaper_descriptor(),
        {realization("default", forge_lofi::make_waveshaper_node())});
    add(nodes, forge_lofi::drywet_descriptor(), {realization("default", forge_lofi::make_drywet_node())});
    add(nodes, forge_lofi::noise_descriptor(), {realization("default", forge_lofi::make_noise_node())});
    add(nodes, forge_lofi::bitcrush_descriptor(),
        {realization("legacy", forge_lofi::make_bitcrush_node()),
         realization("tpdf", forge_lofi::make_bitcrush_node(signal::DitherMode::tpdf)),
         realization("tpdf_first",
                     forge_lofi::make_bitcrush_node(signal::DitherMode::tpdf,
                                                    signal::NoiseShapingOrder::first)),
         realization("tpdf_second",
                     forge_lofi::make_bitcrush_node(signal::DitherMode::tpdf,
                                                    signal::NoiseShapingOrder::second))});
    add(nodes, forge_lofi::trim_descriptor(), {realization("default", forge_lofi::make_trim_node())});
    add(nodes, forge_lofi::ping_pong_descriptor(),
        {realization("default", forge_lofi::make_ping_pong_node())});
    add(nodes, forge_lofi::reverb_descriptor(), {realization("default", forge_lofi::make_reverb_node())});
    add(nodes, forge_lofi::compressor_descriptor(),
        {realization("default", forge_lofi::make_compressor_node())});
    add(nodes, forge_lofi::gate_descriptor(), {realization("default", forge_lofi::make_gate_node())});
    add(nodes, forge_lofi::lfo_descriptor(), {realization("default", forge_lofi::make_lfo_node())});
    add(nodes, forge_lofi::vca_descriptor(), {realization("default", forge_lofi::make_vca_node())});
    add(nodes, forge_lofi::env_follower_descriptor(),
        {realization("default", forge_lofi::make_env_follower_node())});
    add(nodes, forge_lofi::filter_cv_descriptor(),
        {realization("default", forge_lofi::make_filter_cv_node())});
    add(nodes, forge_lofi::delay_cv_descriptor(),
        {realization("default", forge_lofi::make_delay_cv_node())});
    add(nodes, forge_lofi::auto_pan_descriptor(),
        {realization("default", forge_lofi::make_auto_pan_node())});
    add(nodes, forge_lofi::width_descriptor(), {realization("default", forge_lofi::make_width_node())});
    add(nodes, forge_lofi::phaser_descriptor(), {realization("default", forge_lofi::make_phaser_node())});

    add(nodes, forge_modulation::mod_lfo_descriptor(),
        {realization("default", forge_modulation::make_mod_lfo_node())});
    add(nodes, forge_modulation::mod_lpg_descriptor(),
        {realization("default", forge_modulation::make_mod_lpg_node())});
    add(nodes, forge_modulation::mod_slew_descriptor(),
        {realization("default", forge_modulation::make_mod_slew_node())});
    add(nodes, forge_modulation::mod_transient_descriptor(),
        {realization("default", forge_modulation::make_mod_transient_node())});
    add(nodes, forge_modulation::mod_env_descriptor(),
        {realization("default", forge_modulation::make_mod_env_node())});
}

} // namespace pulp::host::forge_catalog_export_detail
