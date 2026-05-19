// Verifies Processor::create_ara_document_controller() default + override
// shape (workstream 06 slice 6.3, Pulp-side Processor hook). No ARA SDK
// dependency — exercises the API surface that adapter companions rely on.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

using namespace pulp::format;

namespace {

class PlainProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override { return {}; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

class AraProcessor : public PlainProcessor {
public:
    struct MyController : AraDocumentController {
        bool is_ara_supported() const override { return true; }
        std::string ara_factory_name() const override { return "MyAraFactory"; }
    };
    std::unique_ptr<AraDocumentController>
    create_ara_document_controller() override {
        return std::make_unique<MyController>();
    }
};

} // namespace

TEST_CASE("default Processor returns nullptr for ARA controller",
          "[ara][processor-hook]") {
    PlainProcessor p;
    auto c = p.create_ara_document_controller();
    REQUIRE(c == nullptr);
}

TEST_CASE("ARA-aware processor returns a working controller",
          "[ara][processor-hook]") {
    AraProcessor p;
    auto c = p.create_ara_document_controller();
    REQUIRE(c != nullptr);
    REQUIRE(c->is_ara_supported());
    REQUIRE(c->ara_factory_name() == "MyAraFactory");
}

TEST_CASE("default AraDocumentController reports no ARA support and empty factory",
          "[ara][processor-hook][defaults]") {
    // The base AraDocumentController is what adapters see when a
    // plugin doesn't override the hook — these defaults are what
    // is_ara_supported-based gating reads. Pin them.
    AraDocumentController base;
    REQUIRE_FALSE(base.is_ara_supported());
    REQUIRE(base.ara_factory_name().empty());
    REQUIRE(base.supported_roles() == 0);
    // Default begin/end editing + audio-source-notify are no-ops.
    base.begin_editing();
    base.end_editing();
    base.notify_audio_source_content_changed(42);
    SUCCEED("default AraDocumentController virtuals complete without UB");
}

TEST_CASE("repeated create_ara_document_controller calls yield distinct owned instances",
          "[ara][processor-hook][ownership]") {
    // ARA host contract: the controller is owned per-instance; creating
    // one shouldn't alias a cached singleton. Catches a plausible
    // future regression where a plugin caches the controller and
    // returns the same pointer on every call.
    AraProcessor p;
    auto a = p.create_ara_document_controller();
    auto b = p.create_ara_document_controller();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a.get() != b.get());
}

TEST_CASE("Processor override that returns nullptr opts out of ARA",
          "[ara][processor-hook][opt-out]") {
    // A Processor may opt out of ARA at runtime (e.g. based on license
    // state or host support). Returning nullptr from the hook must be
    // reflected verbatim — adapters use nullptr to decide not to
    // advertise ARA to the host.
    class OptOutProcessor : public PlainProcessor {
    public:
        std::unique_ptr<AraDocumentController>
        create_ara_document_controller() override {
            return nullptr;
        }
    };
    OptOutProcessor p;
    REQUIRE(p.create_ara_document_controller() == nullptr);
}

TEST_CASE("AraRole enum encodes role bitmask as expected by hosts",
          "[ara][roles]") {
    // The ARA 2.x ABI uses AraRole as a bitmask. Adapter code ORs
    // these values together; verify the numeric layout matches what
    // an ARA SDK consumer would expect so a future refactor doesn't
    // silently re-number them.
    REQUIRE(static_cast<int>(AraRole::None) == 0);
    REQUIRE(static_cast<int>(AraRole::PlaybackRenderer) == 1);
    REQUIRE(static_cast<int>(AraRole::EditorRenderer) == 2);
    REQUIRE(static_cast<int>(AraRole::EditorView) == 4);

    int mask = static_cast<int>(AraRole::PlaybackRenderer)
             | static_cast<int>(AraRole::EditorRenderer)
             | static_cast<int>(AraRole::EditorView);
    REQUIRE(mask == 7);
}

// ─── Processor::suspend()/resume() (Tier B Slice 15) ────────────────────────

namespace {

class CountingProcessor : public PlainProcessor {
public:
    int suspends = 0;
    int resumes = 0;
    void suspend() override { ++suspends; }
    void resume() override { ++resumes; }
};

} // namespace

TEST_CASE("Processor::suspend() / resume() default to no-op",
          "[format][processor][suspend]") {
    // The base virtuals must compile and run as no-op so plug-ins that
    // don't need the heavy-op pause pattern pay nothing.
    PlainProcessor p;
    REQUIRE_NOTHROW(p.suspend());
    REQUIRE_NOTHROW(p.resume());
}

TEST_CASE("Processor::suspend() / resume() override dispatches via virtual",
          "[format][processor][suspend]") {
    CountingProcessor p;
    REQUIRE(p.suspends == 0);
    REQUIRE(p.resumes == 0);
    p.suspend();
    p.suspend();
    p.resume();
    REQUIRE(p.suspends == 2);
    REQUIRE(p.resumes == 1);
}
