#include <pulp/midi/mpe_synth_voice.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

using namespace pulp::midi;

// State-only probe: deliberately no oscillator, graph, or audio-quality oracle.
struct Voice final : MpeSynthVoice {
    void render(float*, int samples) override {
        for (int i = 0; i < samples; ++i)
            advance_smoothers();
    }
    void end_tail() {
        finish_release();
    }
};

using Pool = MpeVoiceAllocator<Voice>;
using Kind = MpeExpressionEvent::Kind;
int checks = 0;

void check(bool ok, const char* rule) {
    ++checks;
    if (!ok)
        throw std::runtime_error(rule);
}

MpeExpressionEvent event(Kind kind, MpeNoteGeneration id, int note = 60) {
    MpeExpressionEvent e;
    e.kind = kind;
    e.state.note_id = id;
    e.state.channel = 1;
    e.state.note = static_cast<uint8_t>(note);
    e.state.velocity = 80;
    return e;
}

void fill(Pool& pool) {
    for (std::size_t i = 0; i < pool.polyphony(); ++i) {
        pool.voice(i).set_smoothing(0.0f);
        pool.dispatch(event(Kind::NoteOn, i + 1, 60 + static_cast<int>(i)));
    }
}

void allocation_and_lifetime() {
    Pool pool(8);
    fill(pool);
    check(pool.active_count() == 8, "eight-active-voices");
    pool.dispatch(event(Kind::NoteOn, 9, 72));
    check(pool.voice(0).note_id() == 9 && pool.steal_count() == 1, "oldest-stolen");
    check(pool.active_count() == 8, "ninth-note-does-not-grow-pool");
    auto stale = event(Kind::Pressure, 1);
    stale.state.pressure = 0.75f;
    pool.dispatch(stale);
    pool.voice(0).render(nullptr, 1);
    check(pool.voice(0).pressure() == 0.0f, "stale-generation-expression");
    pool.dispatch(event(Kind::NoteOff, 1));
    check(!pool.voice(0).releasing(), "stale-generation-note-off");
    auto current = event(Kind::Pressure, 9);
    current.state.pressure = 0.5f;
    pool.dispatch(current);
    pool.voice(0).render(nullptr, 1);
    check(pool.voice(0).pressure() == 0.5f, "current-generation-expression");
    pool.dispatch(event(Kind::NoteOff, 9));
    check(pool.active_count() == 8 && pool.releasing_count() == 1, "tail-retains-slot");
    pool.voice(0).end_tail();
    check(pool.active_count() == 7, "voice-owned-tail-completion");
    pool.dispatch(event(Kind::NoteOn, 10));
    check(pool.voice(0).note_id() == 10 && pool.steal_count() == 1, "free-slot-reuse");
    check(!pool.voice(0).releasing() && pool.voice(0).pressure() == 0.0f,
          "reuse-resets-expression-and-tail");
    pool.reset_all();
    check(pool.active_count() == 0 && !pool.last_was_glide(), "reset-clears-voices");
    check(pool.steal_count() == 1, "reset-preserves-cumulative-steals");
    pool.reset_steal_count();
    check(pool.steal_count() == 0, "explicit-telemetry-reset");
}

void policies_and_ties() {
    constexpr std::array modes{MpeVoiceStealMode::Oldest, MpeVoiceStealMode::LowestVelocity,
                               MpeVoiceStealMode::LowestPitch, MpeVoiceStealMode::HighestPitch};
    constexpr std::array expected{0, 1, 1, 2};
    constexpr std::array notes{64, 60, 72};
    constexpr std::array velocities{90, 20, 70};
    for (std::size_t m = 0; m < modes.size(); ++m) {
        Pool pool(3);
        pool.set_steal_mode(modes[m]);
        for (int i = 0; i < 3; ++i) {
            auto e = event(Kind::NoteOn, i + 1, notes[i]);
            e.state.velocity = static_cast<uint8_t>(velocities[i]);
            pool.dispatch(e);
        }
        pool.dispatch(event(Kind::NoteOn, 4));
        check(pool.voice(expected[m]).note_id() == 4, "selected-steal-policy");
        Pool tied(2);
        tied.set_steal_mode(modes[m]);
        tied.dispatch(event(Kind::NoteOn, 1));
        tied.dispatch(event(Kind::NoteOn, 2));
        tied.dispatch(event(Kind::NoteOn, 3));
        check(tied.voice(0).note_id() == 3, "first-slot-wins-equal-priority");
    }
    Pool tail(2);
    fill(tail);
    tail.dispatch(event(Kind::NoteOff, 2, 61));
    tail.dispatch(event(Kind::NoteOn, 3));
    check(tail.voice(0).note_id() == 3 && tail.voice(1).releasing(),
          "oldest-policy-does-not-prefer-release-tail");
}

void boundaries_and_order() {
    Pool zero(0);
    zero.dispatch(event(Kind::NoteOn, 1));
    zero.dispatch(event(Kind::NoteOn, 2));
    check(zero.active_count() == 0 && !zero.last_was_glide(), "zero-pool-ignores-notes");
    Pool duplicate(2);
    duplicate.dispatch(event(Kind::NoteOn, 1));
    duplicate.dispatch(event(Kind::NoteOn, 2));
    duplicate.dispatch(event(Kind::NoteOff, 1));
    check(duplicate.voice(0).releasing() && !duplicate.voice(1).releasing(),
          "same-pitch-distinct-generations");
    Pool pool(1);
    pool.voice(0).set_smoothing(0.0f);
    auto on = event(Kind::NoteOn, 1);
    on.sample_offset = 31;
    pool.dispatch(on);
    check(pool.active_count() == 1, "dispatch-does-not-schedule-offset");
    MpeBuffer buffer;
    auto a = event(Kind::Pressure, 1);
    a.sample_offset = 8;
    a.state.pressure = 0.25f;
    auto b = a;
    b.state.pressure = 0.75f;
    check(buffer.add(a) && buffer.add(b), "buffer-admits-pair");
    buffer.sort();
    pool.dispatch_all(buffer);
    pool.voice(0).render(nullptr, 1);
    check(pool.voice(0).pressure() == 0.75f, "equal-offset-preserves-producer-order");
    buffer.clear();
    a.sample_offset = 20;
    b.sample_offset = 2;
    check(buffer.add(a) && buffer.add(b), "buffer-admits-unsorted-pair");
    pool.dispatch_all(buffer);
    pool.voice(0).render(nullptr, 1);
    check(pool.voice(0).pressure() == 0.75f, "dispatch-all-does-not-sort");
    buffer.sort();
    pool.dispatch_all(buffer);
    pool.voice(0).render(nullptr, 1);
    check(pool.voice(0).pressure() == 0.25f, "explicit-sort-changes-event-order");
}

int main() {
    try {
        allocation_and_lifetime();
        policies_and_ties();
        boundaries_and_order();
        std::cout << "g6-allocator-contract PASS checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "g6-allocator-contract FAIL rule=" << e.what() << '\n';
        return 1;
    }
}
