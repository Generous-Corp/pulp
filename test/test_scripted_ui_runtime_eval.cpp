#include <catch2/catch_test_macros.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::state;
using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const char* stem) {
    const auto unique =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto dir = fs::temp_directory_path() / (std::string(stem) + "-" + unique);
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
}

} // namespace

TEST_CASE("ScriptedUiSession uses bounded cleanup grace after evaluation timeout",
          "[view][scripted-ui][inspector][runtime-eval][deadline]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-reset-deadline");
    const auto script = temp / "ui.js";
    write_text(script, "enableInspectClick(); createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());
    REQUIRE(root.on_global_click);

    const auto result = session.script_inspector()->evaluate(
        "for (;;) {}", std::chrono::milliseconds::zero());
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.timed_out);
    REQUIRE(result.error.find("evaluation timed out") != std::string::npos);
    REQUIRE_FALSE(session.script_inspector()->capabilities().engine.empty());
    REQUIRE(session.bridge() != nullptr);
    const auto recovered = session.script_inspector()->evaluate("40 + 2");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime evaluation cannot wedge cleanup or retain theme mutations",
          "[view][scripted-ui][inspector][runtime-eval][reset]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-hostile-cleanup");
    const auto script = temp / "ui.js";
    write_text(script, "enableInspectClick(); createLabel('status', 'ready', '');");

    View root;
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());
    REQUIRE(root.on_global_click);
    const auto last_good_background = root.theme().color("bg.primary");

    View other_root;
    auto other_combo_owner = std::make_unique<ComboBox>();
    auto* other_combo = other_combo_owner.get();
    other_combo->set_items({"other"});
    other_root.add_child(std::move(other_combo_owner));
    MouseEvent other_open_click;
    other_open_click.position = {1.0f, 1.0f};
    other_open_click.is_down = true;
    other_combo->on_mouse_event(other_open_click);
    REQUIRE(other_combo->is_open());

    const auto started = std::chrono::steady_clock::now();
    const auto result = session.script_inspector()->evaluate(R"(
        setTheme('light');
        __forgetWidgetCallbacks__ = function () { for (;;) {} };
        40 + 2;
    )");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result.ok);
    REQUIRE(result.json == "42");
    REQUIRE(elapsed < std::chrono::seconds(1));
    REQUIRE(root.theme().color("bg.primary") == last_good_background);
    REQUIRE(session.bridge() != nullptr);
    REQUIRE(session.bridge()->widget("status") != nullptr);
    REQUIRE(root.on_global_click);
    REQUIRE(other_combo->is_open());

    const auto recovered = session.script_inspector()->evaluate("6 * 7");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime evaluation retires every inline realm until the next poll",
          "[view][scripted-ui][inspector][runtime-eval][reset][lifetime]") {
    struct DestructorProbe final : View {
        explicit DestructorProbe(int& calls) : calls_(calls) {}
        ~DestructorProbe() override { ++calls_; }
        int& calls_;
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-eval-retired-realms");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    int destructor_calls = 0;
    root.add_child(std::make_unique<DestructorProbe>(destructor_calls));
    REQUIRE(session.script_inspector()->evaluate("1").ok);
    REQUIRE(destructor_calls == 0);

    root.add_child(std::make_unique<DestructorProbe>(destructor_calls));
    REQUIRE(session.script_inspector()->evaluate("2").ok);
    REQUIRE(destructor_calls == 0);

    session.poll();
    REQUIRE(destructor_calls == 2);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("retired realm destruction can reenter evaluation without corrupting its queue",
          "[view][scripted-ui][inspector][runtime-eval][reset][lifetime][reentrant]") {
    struct DestructorProbe final : View {
        explicit DestructorProbe(int& calls) : calls_(calls) {}
        ~DestructorProbe() override { ++calls_; }
        int& calls_;
    };
    struct ReentrantProbe final : View {
        explicit ReentrantProbe(std::function<void()> callback)
            : callback_(std::move(callback)) {}
        ~ReentrantProbe() override { callback_(); }
        std::function<void()> callback_;
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-eval-reentrant-retirement");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    bool reentrant_ok = false;
    root.add_child(std::make_unique<ReentrantProbe>([&] {
        reentrant_ok = session.script_inspector()->evaluate("3").ok;
    }));
    REQUIRE(session.script_inspector()->evaluate("1").ok);

    int second_batch_destructions = 0;
    root.add_child(
        std::make_unique<DestructorProbe>(second_batch_destructions));
    session.poll();
    REQUIRE(reentrant_ok);
    REQUIRE(second_batch_destructions == 0);

    session.poll();
    REQUIRE(second_batch_destructions == 1);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime reset reapplies cached theme override after source theme mutation",
          "[view][scripted-ui][inspector][runtime-eval][reset][theme]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-reset-theme-override");
    const auto script = temp / "ui.js";
    write_text(script, "setTheme('light'); createLabel('status', 'ready', '');");
    write_text(temp / "theme.json", R"({
        "colors": {
            "bg.primary": "#112233"
        }
    })");

    View root;
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    const auto overridden_background = root.theme().color("bg.primary");
    REQUIRE(overridden_background.has_value());
    REQUIRE(overridden_background->r8() == 0x11);
    REQUIRE(overridden_background->g8() == 0x22);
    REQUIRE(overridden_background->b8() == 0x33);

    // Deadline recovery must use the cached effective theme without touching
    // the filesystem; a newer on-disk value belongs to normal reload polling.
    write_text(temp / "theme.json", R"({
        "colors": {
            "bg.primary": "#abcdef"
        }
    })");

    const auto result = session.script_inspector()->evaluate("40 + 2");

    REQUIRE(result.ok);
    REQUIRE(result.json == "42");
    const auto reset_background = root.theme().color("bg.primary");
    REQUIRE(reset_background.has_value());
    REQUIRE(reset_background->r8() == 0x11);
    REQUIRE(reset_background->g8() == 0x22);
    REQUIRE(reset_background->b8() == 0x33);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime reset quarantines a partially built replacement until owner teardown",
          "[view][scripted-ui][inspector][runtime-eval][reset][quarantine]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-partial-realm");
    const auto script = temp / "ui.js";
    write_text(script, R"(
        createLabel('partial', 'first', '');
        createLabel('partial', 'second', '');
        createLabel('', 'empty-first', '');
        createLabel('', 'empty-second', '');
        if (listValueChannels().length) throw new Error('live-only failure');
    )");

    View root;
    root.set_visible(true);
    StateStore store;
    ValueChannelSet channels;
    REQUIRE(channels.declare_scalar("meter") != nullptr);
    int live_channel_visits = 0;
    {
        auto session = std::make_unique<ScriptedUiSession>(root, store, ScriptedUiOptions{
            .script_path = script,
            .granted_capabilities = CapabilitySet{},
            .value_channel_access = [&](const ValueChannelVisitor& visitor) {
                ++live_channel_visits;
                visitor(live_channel_visits == 1 ? nullptr : &channels);
            },
        });
        REQUIRE(session->load());
        REQUIRE(session->bridge() != nullptr);
        int repaint_requests = 0;
        session->set_repaint_callback([&] { ++repaint_requests; });

        const auto result = session->script_inspector()->evaluate("1");
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error.find("evaluated realm reset failed") != std::string::npos);
        REQUIRE(session->bridge() == nullptr);
        REQUIRE_FALSE(root.visible());
        REQUIRE(repaint_requests == 1);
        REQUIRE_FALSE(session->poll());
        session->attach_gpu_surface(nullptr);

        const auto replacement_script = temp / "replacement.js";
        write_text(replacement_script,
                   "enableInspectClick();"
                   "createCombo('partial', '');"
                   "setItems('partial', ['ready']);");
        auto replacement = std::make_unique<ScriptedUiSession>(
            root, store, ScriptedUiOptions{
                .script_path = replacement_script,
                .granted_capabilities = CapabilitySet{},
            });
        REQUIRE(replacement->load());
        REQUIRE(root.child_count() == 5);
        REQUIRE(static_cast<bool>(root.on_global_click));
        auto* replacement_combo =
            dynamic_cast<ComboBox*>(replacement->bridge()->widget("partial"));
        REQUIRE(replacement_combo != nullptr);
        MouseEvent open_click;
        open_click.position = {1.0f, 1.0f};
        open_click.is_down = true;
        replacement_combo->on_mouse_event(open_click);
        REQUIRE(replacement_combo->is_open());

        session.reset();
        REQUIRE(root.visible());
        REQUIRE(root.child_count() == 1);
        REQUIRE(replacement->bridge()->widget("partial") == replacement_combo);
        REQUIRE(static_cast<bool>(root.on_global_click));
        REQUIRE(replacement_combo->is_open());

        replacement->bridge()->clear_for_realm_replacement();
        replacement.reset();
    }

    REQUIRE(root.visible());
    REQUIRE(root.child_count() == 0);
    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime reset pairs last-good source with its original script base",
          "[view][scripted-ui][inspector][runtime-eval][reset][assets]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-last-good-base");
    const auto good_dir = temp / "good";
    const auto failed_dir = temp / "failed";
    fs::create_directories(good_dir);
    fs::create_directories(failed_dir);
    write_text(good_dir / "ui.js", "createLabel('status', 'ready', '');\n");

    View root;
    StateStore store;
    ScriptedUiSession session(
        root, store,
        {.script_path = good_dir / "ui.js",
         .granted_capabilities = CapabilitySet{}});
    std::string error;
    REQUIRE(session.load(&error));

    REQUIRE_FALSE(session.reload_from(failed_dir / "missing.js", &error));
    REQUIRE(session.script_path() == failed_dir / "missing.js");
    const auto result = session.script_inspector()->evaluate("1");
    REQUIRE(result.ok);

    REQUIRE(session.bridge()->widget("status") != nullptr);
    REQUIRE(session.bridge()->script_base_dir() == fs::absolute(good_dir));

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Ordinary reload retries after a live-only provisional failure",
          "[view][scripted-ui][reload][retry][value-channels]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-reload-live-only-retry");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'initial', '');");

    View root;
    StateStore store;
    ValueChannelSet channels;
    REQUIRE(channels.declare_scalar("meter") != nullptr);
    ScriptedUiSession session(root, store, ScriptedUiOptions{
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
        .value_channel_access = [&](const ValueChannelVisitor& visitor) {
            visitor(&channels);
        },
    });
    REQUIRE(session.load());

    write_text(script, R"(
        createLabel('partial', 'failed', '');
        __forgetWidgetCallbacks__ = function () { for (;;) {} };
        if (listValueChannels().length) throw new Error('live-only failure');
    )");
    std::string error;
    REQUIRE_FALSE(session.reload(&error));
    REQUIRE(error.find("live-only failure") != std::string::npos);
    REQUIRE(session.bridge() == nullptr);

    write_text(script, "createLabel('status', 'recovered', '');");
    error.clear();
    REQUIRE(session.reload(&error));
    REQUIRE(error.empty());
    REQUIRE(session.bridge() != nullptr);
    REQUIRE(session.bridge()->widget("status") != nullptr);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Ordinary reload detaches views before retaining their provider lifetime",
          "[view][scripted-ui][reload][lifetime][detach]") {
    struct DetachProbe final : View {
        DetachProbe(int& detaches, int& destructions)
            : detaches_(detaches), destructions_(destructions) {}
        ~DetachProbe() override { ++destructions_; }
        void on_detached() override { ++detaches_; }
        int& detaches_;
        int& destructions_;
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-reload-detach");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'initial', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    int detaches = 0;
    int destructions = 0;
    root.add_child(std::make_unique<DetachProbe>(detaches, destructions));
    write_text(script, "createLabel('status', 'replacement', '');");
    REQUIRE(session.reload());
    CHECK(detaches == 1);
    CHECK(destructions == 0);

    session.poll();
    CHECK(destructions == 1);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("ScriptedUiSession destruction contains throwing descendant detach hooks",
          "[view][scripted-ui][lifetime][exception-safety]") {
    struct ThrowingDetach final : View {
        void on_detached() override {
            throw std::runtime_error("detach failed");
        }
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-destroy-detach");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    {
        ScriptedUiSession session(root, store, {
            .script_path = script,
            .granted_capabilities = CapabilitySet{},
        });
        REQUIRE(session.load());
        auto* status = session.bridge()->widget("status");
        REQUIRE(status != nullptr);
        status->add_child(std::make_unique<ThrowingDetach>());
    }

    CHECK(root.child_count() == 0);
    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}
