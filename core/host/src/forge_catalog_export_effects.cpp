#include "forge_catalog_export_detail.hpp"

#include <pulp/host/forge_fuzz_catalog.hpp>
#include <pulp/host/forge_saturator_catalog.hpp>
#include <pulp/host/forge_analog_vcf_catalog.hpp>
#include <pulp/host/forge_character_delay_catalog.hpp>
#include <pulp/host/forge_distortion_catalog.hpp>

namespace pulp::host::forge_catalog_export_detail {

void append_effects(Nodes& nodes) {
    add(nodes, fuzz::descriptor(),
        {
            realization("silicon_x1", fuzz::make_fuzz_node(fuzz::Device::silicon, false)),
            realization("silicon_x4", fuzz::make_fuzz_node(fuzz::Device::silicon, true)),
            realization("germanium_x1", fuzz::make_fuzz_node(fuzz::Device::germanium, false)),
            realization("germanium_x4", fuzz::make_fuzz_node(fuzz::Device::germanium, true)),
        });
    add(nodes, saturator::saturator_descriptor(),
        {
            realization("tanh_adaa", saturator::make_saturator_node(saturator::Shape::tanh_soft,
                                                       saturator::AliasPolicy::adaa)),
            realization("tanh_x2",
                        saturator::make_saturator_node(saturator::Shape::tanh_soft,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("tanh_off", saturator::make_saturator_node(saturator::Shape::tanh_soft,
                                                       saturator::AliasPolicy::off)),
            realization("atan_adaa", saturator::make_saturator_node(saturator::Shape::atan_soft,
                                                       saturator::AliasPolicy::adaa)),
            realization("atan_x2",
                        saturator::make_saturator_node(saturator::Shape::atan_soft,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("atan_off", saturator::make_saturator_node(saturator::Shape::atan_soft,
                                                       saturator::AliasPolicy::off)),
            realization("cubic_adaa", saturator::make_saturator_node(saturator::Shape::cubic_soft,
                                                       saturator::AliasPolicy::adaa)),
            realization("cubic_x2",
                        saturator::make_saturator_node(saturator::Shape::cubic_soft,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("cubic_off", saturator::make_saturator_node(saturator::Shape::cubic_soft,
                                                       saturator::AliasPolicy::off)),
            realization("sinh_arc_adaa",
                        saturator::make_saturator_node(saturator::Shape::sinh_arc,
                                                       saturator::AliasPolicy::adaa)),
            realization("sinh_arc_x2",
                        saturator::make_saturator_node(saturator::Shape::sinh_arc,
                                                       saturator::AliasPolicy::oversample_2x)),
            realization("sinh_arc_off",
                        saturator::make_saturator_node(saturator::Shape::sinh_arc,
                                                       saturator::AliasPolicy::off)),
        });

    add(nodes, forge_lofi::analog_vcf_descriptor(),
        {
            realization("juno", forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::juno)),
            realization("jupiter",
                        forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::jupiter)),
            realization("prophet5",
                        forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::prophet5)),
            realization("minimoog",
                        forge_lofi::make_analog_vcf_node(signal::AnalogVcf::Voicing::minimoog)),
        });

    add(nodes, character_delay::character_delay_descriptor(),
        {
            realization("clean", character_delay::make_character_delay_node(
                            character_delay::Character::clean)),
            realization("vintage", character_delay::make_character_delay_node(
                            character_delay::Character::vintage_digital)),
            realization("tape",
                        character_delay::make_character_delay_node(
                            character_delay::Character::tape, character_delay::TapeTier::standard)),
            realization("tape_physical",
                        character_delay::make_character_delay_node(
                            character_delay::Character::tape, character_delay::TapeTier::physical)),
            realization(
                "bbd", character_delay::make_character_delay_node(character_delay::Character::bbd)),
            realization("diffusion", character_delay::make_character_delay_node(
                            character_delay::Character::diffusion)),
        });

    add(nodes, distortion::distortion_descriptor(),
        {
            realization("to_ground_x1",
                        distortion::make_distortion_node(distortion::Topology::to_ground,
                            distortion::OversampleTier::x1)),
            realization("to_ground_x2",
                        distortion::make_distortion_node(distortion::Topology::to_ground,
                            distortion::OversampleTier::x2)),
            realization("to_ground_x4",
                        distortion::make_distortion_node(distortion::Topology::to_ground,
                            distortion::OversampleTier::x4)),
            realization("to_ground_x8",
                        distortion::make_distortion_node(distortion::Topology::to_ground,
                            distortion::OversampleTier::x8)),
            realization("in_loop_x1",
                        distortion::make_distortion_node(distortion::Topology::in_loop,
                            distortion::OversampleTier::x1)),
            realization("in_loop_x2",
                        distortion::make_distortion_node(distortion::Topology::in_loop,
                            distortion::OversampleTier::x2)),
            realization("in_loop_x4",
                        distortion::make_distortion_node(distortion::Topology::in_loop,
                            distortion::OversampleTier::x4)),
            realization("in_loop_x8",
                        distortion::make_distortion_node(distortion::Topology::in_loop,
                            distortion::OversampleTier::x8)),
        });
}

} // namespace pulp::host::forge_catalog_export_detail
