#include <catch2/catch_test_macros.hpp>

#include <RmlUi/Core.h>

#include <cstddef>

namespace {

class RecordingRender final : public Rml::RenderInterface {
  public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        if (vertices.empty() || indices.empty())
            return 0;
        ++compiled_geometry;
        return static_cast<Rml::CompiledGeometryHandle>(compiled_geometry);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f,
                        Rml::TextureHandle) override {
        if (geometry != 0)
            ++rendered_geometry;
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
        if (geometry != 0)
            ++released_geometry;
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i&, const Rml::String&) override {
        return 0;
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override {
        return 0;
    }

    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}

    std::size_t compiled_geometry = 0;
    std::size_t rendered_geometry = 0;
    std::size_t released_geometry = 0;
};

class RmlSession final {
  public:
    explicit RmlSession(Rml::RenderInterface& render) {
        Rml::SetRenderInterface(&render);
        Rml::SetFontEngineInterface(&font_);
        REQUIRE(Rml::Initialise());
    }

    ~RmlSession() {
        Rml::Shutdown();
    }

  private:
    Rml::FontEngineInterface font_;
};

} // namespace

TEST_CASE("RmlUi core parses, updates, renders, and tears down a document") {
    RecordingRender render;
    RmlSession session(render);

    auto* context = Rml::CreateContext("rmlui-core-smoke", {320, 200}, &render);
    REQUIRE(context != nullptr);

    const Rml::String document = "<rml><body><div style='display:block;width:120px;height:40px;"
                                 "background-color:#204060;'></div>"
                                 "</body></rml>";
    auto* loaded = context->LoadDocumentFromMemory(document, "smoke.rml");
    REQUIRE(loaded != nullptr);
    loaded->Show();
    REQUIRE(context->Update());
    REQUIRE(context->Render());
    REQUIRE(render.compiled_geometry > 0);
    REQUIRE(render.rendered_geometry > 0);

    REQUIRE(Rml::RemoveContext("rmlui-core-smoke"));
}
