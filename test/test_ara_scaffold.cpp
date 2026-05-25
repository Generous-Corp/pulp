#include <catch2/catch_test_macros.hpp>
#include <pulp/format/ara.hpp>

#include <cstring>
#include <string_view>

using namespace pulp::format;

// ────────────────────────────────────────────────────────────────────────
// Scaffold validation (macOS plan item 3.6) — Pulp's ARA stub must
// compile + return documented defaults when PULP_ENABLE_ARA=OFF (the
// default). With PULP_ENABLE_ARA=ON + a developer-supplied Celemony
// SDK, ara_sdk_compiled_in() flips to true and ara_sdk_generation()
// returns the SDK's API generation number. The latter is a manual
// smoke (requires Celemony license + checkout) and is documented in
// CLAUDE.md rather than exercised in CI.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ARA scaffold reports SDK-not-compiled-in by default",
          "[format][ara][scaffold]") {
    // Default Pulp build (PULP_ENABLE_ARA=OFF) must report false here.
    // When a developer builds with PULP_ENABLE_ARA=ON + a valid
    // PULP_ARA_SDK_DIR, this returns true — switch the assertion to
    // verify the SDK path locally and document the procedure in
    // CLAUDE.md.
#ifdef PULP_HAS_ARA
    REQUIRE(ara_sdk_compiled_in() == true);
    REQUIRE(ara_sdk_generation() > 0);
#else
    REQUIRE(ara_sdk_compiled_in() == false);
    REQUIRE(ara_sdk_generation() == 0);
#endif
}

TEST_CASE("ARA host_supports_ara starts false (host query lives in adapters)",
          "[format][ara][scaffold]") {
    // The cross-format `host_supports_ara()` returns false until per-
    // format companion factories (VST3 / AU v3 / CLAP) land — see the
    // workstream 06 slices referenced in ara.hpp. Default behavior is
    // deterministic: false regardless of the SDK build-flag, because
    // *runtime* host-availability is independent of compile-time
    // SDK-inclusion.
    REQUIRE(host_supports_ara() == false);
}

TEST_CASE("AraDocumentController defaults are conservative",
          "[format][ara][scaffold]") {
    struct DefaultController : AraDocumentController {};
    DefaultController c;
    REQUIRE(c.is_ara_supported() == false);
    REQUIRE(c.supported_roles() == 0);
    REQUIRE(c.ara_factory_name() == "");

    // Virtual no-op methods must not crash and must not require ARA SDK
    // headers — Pulp plugins that don't declare ARA shouldn't pay any
    // setup cost.
    c.begin_editing();
    c.notify_audio_source_content_changed(42);
    c.end_editing();
}

TEST_CASE("AraDocumentController subclass can advertise roles",
          "[format][ara][scaffold]") {
    struct EditorController : AraDocumentController {
        int supported_roles() const override {
            return static_cast<int>(AraRole::EditorRenderer)
                 | static_cast<int>(AraRole::EditorView);
        }
        bool is_ara_supported() const override { return true; }
        std::string ara_factory_name() const override { return "PulpEditor"; }
    };
    EditorController c;
    REQUIRE(c.is_ara_supported() == true);
    const int roles = c.supported_roles();
    REQUIRE((roles & static_cast<int>(AraRole::EditorRenderer)) != 0);
    REQUIRE((roles & static_cast<int>(AraRole::EditorView)) != 0);
    REQUIRE((roles & static_cast<int>(AraRole::PlaybackRenderer)) == 0);
    REQUIRE(c.ara_factory_name() == "PulpEditor");
}

TEST_CASE("Companion-factory accessor returns nullptr without SDK",
          "[format][ara][scaffold]") {
    struct DummyController : AraDocumentController {
        bool is_ara_supported() const override { return true; }
    };
    DummyController c;
    const void* factory = ara_companion_factory_for(&c);
#ifdef PULP_HAS_ARA
    // With the SDK linked in, a real factory should be returned (or
    // nullptr if the controller doesn't advertise an ARA-aware role —
    // implementation choice; this test loosens to "either is valid").
    (void) factory;
#else
    // Without the SDK, no factory can exist.
    REQUIRE(factory == nullptr);
#endif
}

TEST_CASE("Adapter factory keys match Celemony conventions",
          "[format][ara][scaffold]") {
    // CLAP-side companion extension id.
    REQUIRE(std::string_view(kClapAraFactoryExtension)
            == "com.celemony.ara/clap-factory-v1");
    // VST3 host-context attribute key.
    REQUIRE(std::string_view(kVst3AraFactoryContextKey)
            == "com.celemony.ara/vst3-host-factory-v1");
    // AU v3 property key — Logic Pro expects this exact name.
    REQUIRE(std::string_view(kAuAraFactoryPropertyKey)
            == "audioUnitARAFactory");
}

TEST_CASE("AraRole enum values are stable single-bit flags",
          "[format][ara][scaffold]") {
    // PluginDescriptor / adapter code combines roles with bitwise OR.
    REQUIRE(static_cast<int>(AraRole::None) == 0);
    REQUIRE(static_cast<int>(AraRole::PlaybackRenderer) == 1);
    REQUIRE(static_cast<int>(AraRole::EditorRenderer) == 2);
    REQUIRE(static_cast<int>(AraRole::EditorView) == 4);
}
