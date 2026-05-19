// AudioFocusRegistry tests (#334).
//
// The registry is the cross-platform fan-out for OS audio-focus signals
// (Android's AudioManager.OnAudioFocusChangeListener today; iOS
// AVAudioSession interruptions in a future slice). Exercise the contract
// with a pure observer-pattern test — no JNI, no real audio device.

#include <pulp/audio/audio_focus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using pulp::audio::AudioFocusRegistry;
using pulp::audio::AudioFocusState;

TEST_CASE("AudioFocusRegistry: default state is gained",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    REQUIRE(AudioFocusRegistry::instance().current() == AudioFocusState::gained);
}

TEST_CASE("AudioFocusRegistry: publish updates current() snapshot",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    REQUIRE(AudioFocusRegistry::instance().current() == AudioFocusState::duck);
    AudioFocusRegistry::instance().publish(AudioFocusState::lost);
    REQUIRE(AudioFocusRegistry::instance().current() == AudioFocusState::lost);
    AudioFocusRegistry::instance().publish(AudioFocusState::gained);
    REQUIRE(AudioFocusRegistry::instance().current() == AudioFocusState::gained);
}

TEST_CASE("AudioFocusRegistry: subscribers see every publish",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    std::vector<AudioFocusState> observed;
    auto token = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState s) { observed.push_back(s); });

    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    AudioFocusRegistry::instance().publish(AudioFocusState::lost);
    AudioFocusRegistry::instance().publish(AudioFocusState::gained);

    REQUIRE(observed.size() == 3);
    REQUIRE(observed[0] == AudioFocusState::duck);
    REQUIRE(observed[1] == AudioFocusState::lost);
    REQUIRE(observed[2] == AudioFocusState::gained);
}

TEST_CASE("AudioFocusRegistry: RAII token unsubscribes on scope exit",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    int count = 0;
    {
        auto token = AudioFocusRegistry::instance().subscribe(
            [&](AudioFocusState) { ++count; });
        AudioFocusRegistry::instance().publish(AudioFocusState::duck);
        REQUIRE(count == 1);
    }
    // Token destructed → listener unsubscribed.
    AudioFocusRegistry::instance().publish(AudioFocusState::lost);
    REQUIRE(count == 1);
}

TEST_CASE("AudioFocusRegistry: token move transfers ownership",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    int count = 0;
    AudioFocusRegistry::Token outer;
    {
        auto inner = AudioFocusRegistry::instance().subscribe(
            [&](AudioFocusState) { ++count; });
        outer = std::move(inner);
    }
    // Inner destructed but ownership moved — subscription still live.
    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    REQUIRE(count == 1);
    outer.reset();
    AudioFocusRegistry::instance().publish(AudioFocusState::gained);
    REQUIRE(count == 1);  // outer now dropped → no more callbacks
}

TEST_CASE("AudioFocusRegistry: move construction empties source token",
          "[audio][focus][coverage][phase3]") {
    AudioFocusRegistry::instance().reset_for_test();
    int count = 0;
    auto source = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) { ++count; });
    const int id = source.id();

    AudioFocusRegistry::Token moved(std::move(source));
    REQUIRE(source.id() == 0);
    REQUIRE(moved.id() == id);

    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    REQUIRE(count == 1);
}

TEST_CASE("AudioFocusRegistry: move assignment replaces an existing subscription",
          "[audio][focus][issue-640]") {
    AudioFocusRegistry::instance().reset_for_test();
    int first_count = 0;
    int second_count = 0;

    auto first = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) { ++first_count; });
    auto second = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) { ++second_count; });

    first = std::move(second);
    AudioFocusRegistry::instance().publish(AudioFocusState::duck);

    REQUIRE(first.id() != 0);
    REQUIRE(second.id() == 0);
    REQUIRE(first_count == 0);
    REQUIRE(second_count == 1);
}

TEST_CASE("AudioFocusRegistry: default token reset is a no-op",
          "[audio][focus][issue-640]") {
    AudioFocusRegistry::Token token;

    REQUIRE(token.id() == 0);
    token.reset();
    REQUIRE(token.id() == 0);
}

TEST_CASE("AudioFocusRegistry: empty subscription is inert",
          "[audio][focus][issue-640]") {
    AudioFocusRegistry::instance().reset_for_test();

    auto token = AudioFocusRegistry::instance().subscribe({});

    REQUIRE(token.id() == 0);
    REQUIRE_NOTHROW(
        AudioFocusRegistry::instance().publish(AudioFocusState::duck));
    REQUIRE(AudioFocusRegistry::instance().current() == AudioFocusState::duck);
}

TEST_CASE("AudioFocusRegistry: callback that drops its own token does not deadlock",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    std::unique_ptr<AudioFocusRegistry::Token> token;
    token = std::make_unique<AudioFocusRegistry::Token>(
        AudioFocusRegistry::instance().subscribe(
            [&](AudioFocusState) { token.reset(); }));

    // If publish held the mutex during dispatch, token.reset() would
    // re-enter unsubscribe() and deadlock. The copy-and-dispatch-out-of-
    // lock pattern in publish() prevents that.
    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    SUCCEED("no deadlock");
}

TEST_CASE("AudioFocusRegistry: multiple subscribers all receive the signal",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    int count_a = 0, count_b = 0, count_c = 0;
    auto ta = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) { ++count_a; });
    auto tb = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) { ++count_b; });
    auto tc = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) { ++count_c; });

    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    REQUIRE(count_a == 1);
    REQUIRE(count_b == 1);
    REQUIRE(count_c == 1);
}

TEST_CASE("AudioFocusRegistry: subscribers added during publish wait for next signal",
          "[audio][focus][coverage][phase3]") {
    AudioFocusRegistry::instance().reset_for_test();
    int first_count = 0;
    int second_count = 0;
    AudioFocusRegistry::Token second;

    auto first = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) {
            ++first_count;
            if (second.id() == 0) {
                second = AudioFocusRegistry::instance().subscribe(
                    [&](AudioFocusState) { ++second_count; });
            }
        });

    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    REQUIRE(first_count == 1);
    REQUIRE(second_count == 0);

    AudioFocusRegistry::instance().publish(AudioFocusState::gained);
    REQUIRE(first_count == 2);
    REQUIRE(second_count == 1);
}

TEST_CASE("AudioFocusRegistry: current() is lock-free / audio-thread safe",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    std::atomic<bool> reader_started{false};
    std::atomic<bool> stop{false};
    std::atomic<int> samples{0};

    // Reader thread simulating the audio callback: spin on current()
    // while a publisher thread thrashes the state. This would hang or
    // tear if current() weren't an atomic snapshot.
    std::thread reader([&] {
        auto s = AudioFocusRegistry::instance().current();
        (void)s;
        samples.fetch_add(1, std::memory_order_relaxed);
        reader_started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            auto current = AudioFocusRegistry::instance().current();
            (void)current;
            samples.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (!reader_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::thread writer([&] {
        for (int i = 0; i < 10'000; ++i) {
            AudioFocusRegistry::instance().publish(
                (i & 1) ? AudioFocusState::duck : AudioFocusState::gained);
        }
    });
    writer.join();
    stop.store(true, std::memory_order_release);
    reader.join();
    REQUIRE(samples.load() > 0);
}

TEST_CASE("AudioFocusRegistry: lost_transient state roundtrips",
          "[audio][focus][issue-334]") {
    AudioFocusRegistry::instance().reset_for_test();
    std::vector<AudioFocusState> observed;
    auto token = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState s) { observed.push_back(s); });

    AudioFocusRegistry::instance().publish(AudioFocusState::lost_transient);
    AudioFocusRegistry::instance().publish(AudioFocusState::gained);

    REQUIRE(observed.size() == 2);
    REQUIRE(observed[0] == AudioFocusState::lost_transient);
    REQUIRE(observed[1] == AudioFocusState::gained);
}

TEST_CASE("AudioFocusRegistry: reset_for_test clears subscribers and state",
          "[audio][focus][issue-334]") {
    int count = 0;
    auto token = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState) { ++count; });
    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    REQUIRE(count == 1);

    AudioFocusRegistry::instance().reset_for_test();
    REQUIRE(AudioFocusRegistry::instance().current() == AudioFocusState::gained);

    AudioFocusRegistry::instance().publish(AudioFocusState::lost);
    // count should not increment — reset cleared subscribers.
    REQUIRE(count == 1);
    // The stale token's dtor is a no-op at this point (reset cleared cbs_
    // so unsubscribe can't find its id); just don't crash.
    token.reset();
    SUCCEED("no crash on stale token reset");
}

TEST_CASE("AudioFocusRegistry: publish with no subscribers still updates state",
          "[audio][focus][issue-640]") {
    AudioFocusRegistry::instance().reset_for_test();

    AudioFocusRegistry::instance().publish(AudioFocusState::lost_transient);

    REQUIRE(AudioFocusRegistry::instance().current()
            == AudioFocusState::lost_transient);
}

TEST_CASE("AudioFocusRegistry: subscriber added during publish waits for next signal",
          "[audio][focus][coverage][phase3-large]") {
    AudioFocusRegistry::instance().reset_for_test();

    std::vector<AudioFocusState> first_seen;
    std::vector<AudioFocusState> late_seen;
    AudioFocusRegistry::Token late_token;

    auto first_token = AudioFocusRegistry::instance().subscribe(
        [&](AudioFocusState state) {
            first_seen.push_back(state);
            if (late_token.id() == 0) {
                late_token = AudioFocusRegistry::instance().subscribe(
                    [&](AudioFocusState late_state) {
                        late_seen.push_back(late_state);
                    });
            }
        });

    AudioFocusRegistry::instance().publish(AudioFocusState::duck);
    REQUIRE(first_seen == std::vector<AudioFocusState>{AudioFocusState::duck});
    REQUIRE(late_seen.empty());

    AudioFocusRegistry::instance().publish(AudioFocusState::gained);
    REQUIRE(first_seen == std::vector<AudioFocusState>{
        AudioFocusState::duck, AudioFocusState::gained});
    REQUIRE(late_seen == std::vector<AudioFocusState>{AudioFocusState::gained});
}
