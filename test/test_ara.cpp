// Unit tests for the ARA 2.x scaffolding (workstream 06 slice 6.1).
// These tests exercise only the Pulp-side stubs. Actual SDK integration +
// adapter companion factories land in slices 6.3..6.5.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/ara.hpp>

using namespace pulp::format;

TEST_CASE("host_supports_ara returns false without adapter companions", "[ara]") {
    REQUIRE(host_supports_ara() == false);
}

TEST_CASE("ara_sdk_compiled_in reflects build configuration", "[ara]") {
#ifdef PULP_HAS_ARA
    REQUIRE(ara_sdk_compiled_in() == true);
#else
    REQUIRE(ara_sdk_compiled_in() == false);
#endif
}

TEST_CASE("AraDocumentController subclass overrides work", "[ara]") {
    struct MyController : AraDocumentController {
        int supported_roles() const override {
            return static_cast<int>(AraRole::PlaybackRenderer)
                 | static_cast<int>(AraRole::EditorView);
        }
        bool is_ara_supported() const override { return true; }
        std::string ara_factory_name() const override { return "TestAraFactory"; }
    };
    MyController c;
    c.begin_editing();
    c.notify_audio_source_content_changed(42);
    c.end_editing();
    REQUIRE(c.is_ara_supported());
    REQUIRE(c.ara_factory_name() == "TestAraFactory");
    REQUIRE((c.supported_roles() & static_cast<int>(AraRole::PlaybackRenderer)) != 0);
    REQUIRE((c.supported_roles() & static_cast<int>(AraRole::EditorView)) != 0);
}

TEST_CASE("ara_sdk_generation reflects SDK headers", "[ara]") {
#ifdef PULP_HAS_ARA
    // Compiled with the Celemony SDK — generation constant is at least
    // the 2.3 Final enum value (6). A future header upgrade can only
    // raise this, never lower it.
    REQUIRE(ara_sdk_generation() >= 6);
#else
    REQUIRE(ara_sdk_generation() == 0);
#endif
}

TEST_CASE("ara_companion_factory_for returns nullptr without a controller", "[ara]") {
    // Until slices 6.3/6.4 land a real ARA::ARAFactory, the bridge is a
    // stub: it accepts any AraDocumentController pointer (including
    // nullptr) and returns nullptr. Hosts treat nullptr as "plugin is
    // not ARA-aware" instead of crashing on a garbage factory pointer.
    REQUIRE(ara_companion_factory_for(nullptr) == nullptr);
}

TEST_CASE("kClapAraFactoryExtension id matches Celemony convention", "[ara]") {
    // The extension id is surfaced to CLAP hosts via
    // clap_plugin::get_extension. The string must stay stable because
    // hosts match on it exactly.
    REQUIRE(std::string(kClapAraFactoryExtension) == "com.celemony.ara/clap-factory-v1");
}
