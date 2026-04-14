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
