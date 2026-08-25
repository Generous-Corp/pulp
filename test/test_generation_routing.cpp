#include <pulp/format/generation_routing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace pulp::format;
using pulp::platform::DeviceCapabilityTier;
using pulp::platform::kDeviceCapabilityTierCount;
using pulp::platform::kThermalStateCount;
using pulp::platform::ThermalState;

namespace {

constexpr std::array<DeviceCapabilityTier, kDeviceCapabilityTierCount> kTiers{
    DeviceCapabilityTier::Constrained, DeviceCapabilityTier::Standard,
    DeviceCapabilityTier::Full};

constexpr std::array<ThermalState, kThermalStateCount> kThermals{
    ThermalState::Nominal, ThermalState::Fair, ThermalState::Serious, ThermalState::Critical};

constexpr std::array<GenerationJobClass, kGenerationJobClassCount> kJobs{
    GenerationJobClass::ProposalGeneration, GenerationJobClass::ArtifactPlayback,
    GenerationJobClass::ReceiptRendering};

constexpr std::array<GenerationHostContext, 2> kContexts{GenerationHostContext::Standalone,
                                                         GenerationHostContext::AudioUnitV3};

constexpr std::array<GenerationConnectivity, 2> kConnectivity{GenerationConnectivity::Online,
                                                              GenerationConnectivity::Offline};

constexpr GenerationRoute kExpectedProposalRoutes[kDeviceCapabilityTierCount]
                                                  [kThermalStateCount] = {
    {GenerationRoute::Server, GenerationRoute::Server, GenerationRoute::Server,
     GenerationRoute::QueueForLater},
    {GenerationRoute::Server, GenerationRoute::Server, GenerationRoute::Server,
     GenerationRoute::QueueForLater},
    {GenerationRoute::OnDevice, GenerationRoute::Server, GenerationRoute::Server,
     GenerationRoute::QueueForLater},
};

// Distance from the device, ascending. The two monotonicity properties are
// statements about this ordering, so naming it once keeps them readable.
constexpr int distance(GenerationRoute route) noexcept {
    switch (route) {
    case GenerationRoute::OnDevice:
        return 0;
    case GenerationRoute::Server:
        return 1;
    case GenerationRoute::QueueForLater:
        return 2;
    }
    return -1;
}

} // namespace

TEST_CASE("each capability tier routes each job class per the declared policy table",
          "[format][generation-routing]") {
    // Every combination has an answer. A routing policy that can decline to
    // decide would push the decision to a caller that has less context.
    for (auto tier : kTiers)
        for (auto thermal : kThermals)
            for (auto job : kJobs)
                for (auto context : kContexts)
                    for (auto conn : kConnectivity)
                        REQUIRE(distance(generation_route(tier, thermal, job, context, conn)) >= 0);

    // The declared table, read back cell by cell for proposal generation on a
    // standalone online device. This is the policy's face value; the properties
    // below constrain how it may ever be edited.
    const auto online = GenerationConnectivity::Online;
    const auto solo = GenerationHostContext::Standalone;
    const auto proposal = GenerationJobClass::ProposalGeneration;

    for (std::size_t tier = 0; tier < kDeviceCapabilityTierCount; ++tier)
        for (std::size_t thermal = 0; thermal < kThermalStateCount; ++thermal)
            REQUIRE(generation_route(kTiers[tier], kThermals[thermal], proposal, solo, online) ==
                    kExpectedProposalRoutes[tier][thermal]);

    // Constrained is review-and-perform with no local realtime, so no
    // temperature makes it a place to run inference.
    for (auto thermal : kThermals)
        REQUIRE(generation_route(DeviceCapabilityTier::Constrained, thermal, proposal, solo,
                                 online) != GenerationRoute::OnDevice);
}

TEST_CASE("heat only pushes generation work away and capability only pulls it closer",
          "[format][generation-routing]") {
    const auto online = GenerationConnectivity::Online;
    const auto solo = GenerationHostContext::Standalone;
    const auto proposal = GenerationJobClass::ProposalGeneration;

    // Along a row: never moves back toward the device as the machine heats up.
    for (auto tier : kTiers)
        for (std::size_t t = 1; t < kThermalStateCount; ++t)
            REQUIRE(distance(generation_route(tier, kThermals[t], proposal, solo, online)) >=
                    distance(generation_route(tier, kThermals[t - 1], proposal, solo, online)));

    // Down a column: never moves further from the device as the tier rises.
    for (auto thermal : kThermals)
        for (std::size_t i = 1; i < kDeviceCapabilityTierCount; ++i)
            REQUIRE(distance(generation_route(kTiers[i], thermal, proposal, solo, online)) <=
                    distance(generation_route(kTiers[i - 1], thermal, proposal, solo, online)));
}

TEST_CASE("an auv3 context never selects on device inference", "[format][generation-routing]") {
    // The load-bearing property: it holds over the entire input space, not one
    // row, because an AUv3 runs inside a memory budget it does not control.
    for (auto tier : kTiers)
        for (auto thermal : kThermals)
            for (auto job : kJobs)
                for (auto conn : kConnectivity) {
                    const auto route =
                        generation_route(tier, thermal, job, GenerationHostContext::AudioUnitV3,
                                         conn);
                    REQUIRE_FALSE(runs_inference_on_device(job, route));
                }

    // The substitution is real rather than vacuous: the same cell that an AUv3
    // is denied does run on device standalone, so the guard is what changed the
    // answer and not an input that could never reach OnDevice anyway.
    REQUIRE(generation_route(DeviceCapabilityTier::Full, ThermalState::Nominal,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::Standalone,
                             GenerationConnectivity::Online) == GenerationRoute::OnDevice);
    REQUIRE(generation_route(DeviceCapabilityTier::Full, ThermalState::Nominal,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::AudioUnitV3,
                             GenerationConnectivity::Online) == GenerationRoute::Server);
}

TEST_CASE("offline mode queues proposals and plays materialized artifacts without error",
          "[format][generation-routing]") {
    const auto offline = GenerationConnectivity::Offline;

    // Playback and receipts are what the product degrades *to*, so they stay
    // local and unaffected everywhere — including on a critical, constrained,
    // hosted device, which is the worst cell in the space.
    for (auto tier : kTiers)
        for (auto thermal : kThermals)
            for (auto context : kContexts)
                for (auto conn : kConnectivity) {
                    REQUIRE(generation_route(tier, thermal, GenerationJobClass::ArtifactPlayback,
                                             context, conn) == GenerationRoute::OnDevice);
                    REQUIRE(generation_route(tier, thermal, GenerationJobClass::ReceiptRendering,
                                             context, conn) == GenerationRoute::OnDevice);
                }

    // Offline never routes a proposal to a server it cannot reach.
    for (auto tier : kTiers)
        for (auto thermal : kThermals)
            for (auto context : kContexts)
                REQUIRE(generation_route(tier, thermal, GenerationJobClass::ProposalGeneration,
                                         context, offline) != GenerationRoute::Server);

    // Offline degrades rather than failing: a cool full-tier standalone device
    // still runs its own proposal, everything else holds the job.
    REQUIRE(generation_route(DeviceCapabilityTier::Full, ThermalState::Nominal,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::Standalone, offline) ==
            GenerationRoute::OnDevice);
    REQUIRE(generation_route(DeviceCapabilityTier::Full, ThermalState::Serious,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::Standalone, offline) ==
            GenerationRoute::QueueForLater);
    REQUIRE(generation_route(DeviceCapabilityTier::Standard, ThermalState::Nominal,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::Standalone, offline) ==
            GenerationRoute::QueueForLater);
}

TEST_CASE("an out of ladder value holds the job rather than running it",
          "[format][generation-routing]") {
    // A device we cannot characterize is not a device to spend inference on.
    const auto bogus_tier = static_cast<DeviceCapabilityTier>(kDeviceCapabilityTierCount + 7);
    const auto bogus_thermal = static_cast<ThermalState>(kThermalStateCount + 7);
    const auto bogus_job = static_cast<GenerationJobClass>(kGenerationJobClassCount + 7);
    const auto bogus_context =
        static_cast<GenerationHostContext>(kGenerationHostContextCount + 7);
    const auto bogus_connectivity =
        static_cast<GenerationConnectivity>(kGenerationConnectivityCount + 7);

    REQUIRE(generation_route(bogus_tier, ThermalState::Nominal,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::Standalone,
                             GenerationConnectivity::Online) == GenerationRoute::QueueForLater);
    REQUIRE(generation_route(DeviceCapabilityTier::Full, bogus_thermal,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::Standalone,
                             GenerationConnectivity::Online) == GenerationRoute::QueueForLater);
    REQUIRE(generation_route(DeviceCapabilityTier::Full, ThermalState::Nominal, bogus_job,
                             GenerationHostContext::Standalone,
                             GenerationConnectivity::Online) == GenerationRoute::QueueForLater);
    REQUIRE(generation_route(DeviceCapabilityTier::Full, ThermalState::Nominal,
                             GenerationJobClass::ProposalGeneration, bogus_context,
                             GenerationConnectivity::Online) == GenerationRoute::QueueForLater);
    REQUIRE(generation_route(DeviceCapabilityTier::Full, ThermalState::Nominal,
                             GenerationJobClass::ProposalGeneration,
                             GenerationHostContext::Standalone, bogus_connectivity) ==
            GenerationRoute::QueueForLater);
}
