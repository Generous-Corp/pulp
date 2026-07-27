#include "timeline_launch_session.hpp"

#include <cstdio>

// Worked example (d): drives a clip-launching session headless. Launches a
// scene, lets it sound, stops it on the bar, then captures the performance into
// the arrangement as ordinary clip-insert commands.
int main() {
    using namespace pulp;
    using namespace pulp::examples::timeline_session;

    constexpr double kSampleRate = 48'000.0;
    constexpr std::uint32_t kBlock = 256;

    TimelineLaunchSession session;
    if (!session.prepare(kSampleRate, kBlock)) {
        std::fprintf(stderr, "launch session: prepare failed\n");
        return 1;
    }

    if (!session.launch_scene({21})) { // scene B: launch_immediate
        std::fprintf(stderr, "launch session: scene launch refused\n");
        return 1;
    }
    for (std::uint32_t block = 0; block < 200; ++block)
        session.process(kBlock);
    std::printf("sounding slots: %zu\n", session.sounding_slot_count());

    session.stop_all({timebase::TickDuration{4 * timebase::kTicksPerQuarter},
                      timebase::TickPosition{0}});
    for (std::uint32_t block = 0; block < 500; ++block)
        session.process(kBlock);

    const auto* sequence = session.project().find_sequence({3});
    if (sequence == nullptr) {
        std::fprintf(stderr, "launch session: sequence missing\n");
        return 1;
    }
    std::uint64_t next_item_id = 500;
    const auto commands = flatten_launch_history(*sequence, session.history(), next_item_id);
    if (commands.empty() || !session.apply(commands)) {
        std::fprintf(stderr, "launch session: capture flatten failed\n");
        return 1;
    }
    std::printf("captured %zu launches into the arrangement\n", commands.size());
    return 0;
}
