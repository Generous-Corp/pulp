#include <catch2/catch_test_macros.hpp>
#include <pulp/view/sample_asset_drop_target.hpp>

#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("SampleAssetDropTarget classifies sample file drops without probing metadata",
          "[view][dnd][sample-assets]") {
    pulp::audio::SampleAssetPolicy policy;
    policy.allowed_audio_read_extensions = {".wav"};
    policy.allow_midi_drop = true;

    const std::vector<std::string> paths = {
        "/does/not/need/to/exist/KICK.WAV",
        "/does/not/need/to/exist/groove.mid",
        "/does/not/need/to/exist/readme.txt",
    };

    auto plan = pulp::view::classify_sample_asset_file_drop(paths, policy);

    REQUIRE(plan.items.size() == 3);
    REQUIRE(plan.accepted());
    REQUIRE(plan.accepted_count == 2);
    REQUIRE(plan.rejected_count == 1);
    REQUIRE(plan.audio_count == 1);
    REQUIRE(plan.midi_count == 1);
    REQUIRE(plan.needs_audio_probe());
    REQUIRE(plan.probe_audio_paths.size() == 1);
    REQUIRE(plan.probe_audio_paths[0] == paths[0]);
    REQUIRE(plan.items[0].status == pulp::audio::SampleAssetStatus::ok);
    REQUIRE(plan.items[0].kind == pulp::audio::SampleAssetKind::audio);
    REQUIRE(plan.items[1].status == pulp::audio::SampleAssetStatus::ok);
    REQUIRE(plan.items[1].kind == pulp::audio::SampleAssetKind::midi);
    REQUIRE(plan.items[2].status == pulp::audio::SampleAssetStatus::unsupported_extension);
}

TEST_CASE("SampleAssetDropTarget reports drag and drop plans and requests background probes",
          "[view][dnd][sample-assets]") {
    pulp::audio::SampleAssetPolicy policy;
    policy.allowed_audio_read_extensions = {".wav"};

    pulp::view::SampleAssetDropTarget target(policy);

    int drag_callbacks = 0;
    int drop_callbacks = 0;
    int probe_callbacks = 0;
    pulp::view::Point last_probe_position;
    std::vector<std::string> probe_paths;

    target.on_drag_classified = [&](const pulp::view::SampleAssetDropPlan& plan,
                                    pulp::view::Point position) {
        ++drag_callbacks;
        REQUIRE(position == pulp::view::Point{2.0f, 3.0f});
        REQUIRE(plan.accepted_count == 1);
        REQUIRE(plan.rejected_count == 1);
    };
    target.on_drop_classified = [&](const pulp::view::SampleAssetDropPlan& plan,
                                    pulp::view::Point position) {
        ++drop_callbacks;
        REQUIRE(position == pulp::view::Point{4.0f, 5.0f});
        REQUIRE(plan.accepted_count == 1);
        REQUIRE(plan.rejected_count == 1);
    };
    target.on_audio_probe_dispatch_requested = [&](const pulp::view::SampleAssetDropPlan& plan,
                                    pulp::view::Point position) {
        ++probe_callbacks;
        last_probe_position = position;
        probe_paths = plan.probe_audio_paths;
    };

    pulp::view::DropData data;
    data.type = pulp::view::DropData::Type::files;
    data.file_paths = {
        "/tmp/snare.wav",
        "/tmp/not-a-sample.txt",
    };

    REQUIRE(target.on_drag_enter(data, {2.0f, 3.0f}));
    REQUIRE(target.last_plan().accepted());
    REQUIRE(drag_callbacks == 1);

    REQUIRE(target.on_drop(data, {4.0f, 5.0f}));
    REQUIRE(drop_callbacks == 1);
    REQUIRE(probe_callbacks == 1);
    REQUIRE(last_probe_position == pulp::view::Point{4.0f, 5.0f});
    REQUIRE(probe_paths.size() == 1);
    REQUIRE(probe_paths[0] == "/tmp/snare.wav");
}

TEST_CASE("SampleAssetDropTarget keeps a stable drop plan across reentrant callbacks",
          "[view][dnd][sample-assets]") {
    pulp::audio::SampleAssetPolicy policy;
    policy.allowed_audio_read_extensions = {".wav"};
    pulp::view::SampleAssetDropTarget target(policy);

    bool probe_requested = false;
    std::vector<std::string> probe_paths;

    target.on_drop_classified = [&](const pulp::view::SampleAssetDropPlan& plan,
                                    pulp::view::Point) {
        REQUIRE(plan.accepted());
        target.on_drag_exit();
    };
    target.on_audio_probe_dispatch_requested = [&](const pulp::view::SampleAssetDropPlan& plan,
                                                   pulp::view::Point) {
        probe_requested = true;
        probe_paths = plan.probe_audio_paths;
    };

    pulp::view::DropData data;
    data.type = pulp::view::DropData::Type::files;
    data.file_paths = {"/tmp/kick.wav"};

    REQUIRE(target.on_drop(data, {0.0f, 0.0f}));
    REQUIRE(probe_requested);
    REQUIRE(probe_paths.size() == 1);
    REQUIRE(probe_paths[0] == "/tmp/kick.wav");
    REQUIRE_FALSE(target.last_plan().accepted());
}

TEST_CASE("SampleAssetDropTarget rejects non-file drops and clears stale plans",
          "[view][dnd][sample-assets]") {
    pulp::view::SampleAssetDropTarget target;

    pulp::view::DropData files;
    files.type = pulp::view::DropData::Type::files;
    files.file_paths = {"/tmp/kick.wav"};
    REQUIRE(target.on_drag_enter(files, {0.0f, 0.0f}));
    REQUIRE(target.last_plan().accepted());

    pulp::view::DropData text;
    text.type = pulp::view::DropData::Type::text;
    text.text = "not a file drop";

    REQUIRE_FALSE(target.on_drag_enter(text, {0.0f, 0.0f}));
    REQUIRE_FALSE(target.last_plan().accepted());
    REQUIRE_FALSE(target.on_drop(text, {0.0f, 0.0f}));

    REQUIRE(target.on_drag_enter(files, {0.0f, 0.0f}));
    target.on_drag_exit();
    REQUIRE_FALSE(target.last_plan().accepted());
}
