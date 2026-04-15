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

TEST_CASE("ara_companion_factory_for behaves correctly with/without the SDK",
          "[ara]") {
    // Without PULP_HAS_ARA the factory must be nullptr (no SDK, no
    // ARA::ARAFactory struct definition, hosts treat the plug-in as
    // non-ARA). With PULP_HAS_ARA the factory is a real pointer —
    // ara_factory.cpp owns construction. Guarded tests below cover
    // the real-factory field invariants.
#ifdef PULP_HAS_ARA
    REQUIRE(ara_companion_factory_for(nullptr) != nullptr);
#else
    REQUIRE(ara_companion_factory_for(nullptr) == nullptr);
#endif
}

TEST_CASE("kClapAraFactoryExtension id matches Celemony convention", "[ara]") {
    // The extension id is surfaced to CLAP hosts via
    // clap_plugin::get_extension. The string must stay stable because
    // hosts match on it exactly.
    REQUIRE(std::string(kClapAraFactoryExtension) == "com.celemony.ara/clap-factory-v1");
}

TEST_CASE("VST3 ARA factory context key is stable", "[ara][vst3]") {
    REQUIRE(std::string(kVst3AraFactoryContextKey) == "com.celemony.ara/vst3-host-factory-v1");
}

TEST_CASE("AU ARA factory property key matches Apple convention", "[ara][au]") {
    // Apple docs: AUAudioUnit.audioUnitARAFactory is the standard
    // property name that ARA-aware AU hosts observe.
    REQUIRE(std::string(kAuAraFactoryPropertyKey) == "audioUnitARAFactory");
}

#ifdef PULP_HAS_ARA
#include <ARA_API/ARAInterface.h>

TEST_CASE("ara_companion_factory_for returns a valid ARAFactory with SDK",
          "[ara][factory]") {
    const auto* raw = ara_companion_factory_for(nullptr);
    REQUIRE(raw != nullptr);
    auto* factory = static_cast<const ARA::ARAFactory*>(raw);
    REQUIRE(factory->structSize >= ARA::kARAFactoryMinSize);
    REQUIRE(factory->highestSupportedApiGeneration == ARA::kARAAPIGeneration_2_3_Final);
    REQUIRE(factory->lowestSupportedApiGeneration  == ARA::kARAAPIGeneration_2_0_Final);
    REQUIRE(factory->factoryID != nullptr);
    REQUIRE(std::string(factory->factoryID).find("pulp") != std::string::npos);
    REQUIRE(factory->initializeARAWithConfiguration != nullptr);
    REQUIRE(factory->uninitializeARA != nullptr);
    REQUIRE(factory->createDocumentControllerWithDocument != nullptr);
    REQUIRE(factory->plugInName != nullptr);
    REQUIRE(factory->manufacturerName != nullptr);
    REQUIRE(factory->documentArchiveID != nullptr);
}

TEST_CASE("factory createDocumentControllerWithDocument returns a valid instance",
          "[ara][factory]") {
    const auto* raw = ara_companion_factory_for(nullptr);
    REQUIRE(raw != nullptr);
    const auto* factory = static_cast<const ARA::ARAFactory*>(raw);
    factory->initializeARAWithConfiguration(nullptr);
    const auto* instance = factory->createDocumentControllerWithDocument(nullptr, nullptr);
    REQUIRE(instance != nullptr);
    REQUIRE(instance->documentControllerInterface != nullptr);
    REQUIRE(instance->documentControllerRef != nullptr);
    REQUIRE(instance->documentControllerInterface->destroyDocumentController != nullptr);
    REQUIRE(instance->documentControllerInterface->beginEditing != nullptr);
    REQUIRE(instance->documentControllerInterface->endEditing != nullptr);
    // Round-trip getFactory should return the same factory pointer.
    const auto* via_instance = instance->documentControllerInterface->getFactory(
        instance->documentControllerRef);
    REQUIRE(via_instance == factory);
    // Exercise begin/end to prove the stubs don't crash.
    instance->documentControllerInterface->beginEditing(instance->documentControllerRef);
    instance->documentControllerInterface->notifyModelUpdates(instance->documentControllerRef);
    instance->documentControllerInterface->endEditing(instance->documentControllerRef);
    instance->documentControllerInterface->destroyDocumentController(instance->documentControllerRef);
    factory->uninitializeARA();
}
#endif // PULP_HAS_ARA
