#pragma once

// rmlui_metal_golden.hpp — what the Metal-renderer golden test and its
// soft-renderer reference writer share: the documents, the frame sizes, the
// font, the reference files, and the comparison.
//
// The two sides run in separate processes: the reference writer links
// Vellum's Core module (its own RmlUi) and the test links the Native module
// (another RmlUi), and the two must not share a process. ctest joins them
// with a fixture.
//
// The documents use only what both renderers draw completely: boxes, borders,
// border radius, translucent fills, transforms and text. Vellum's
// SoftRenderer draws no clip mask, layer, filter or shader, so a document with
// one would differ by design.

#include <vellum/rmlui/assets.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace pulp_rmlui_golden {

struct Document {
    const char* name;
    const char* rml;
};

// Lengths are in px, so a frame at backing scale 2 lays out the same content
// in the same pixels as the reference opened at that pixel size.
inline constexpr Document kDocuments[] = {
    {"panel.rml", R"(<rml>
<head>
<style>
body { width: 100%; height: 100%; margin: 0; background-color: #20242c;
       font-family: Inter; font-size: 16px; color: #e8e8e8; }
#card { position: absolute; left: 16px; top: 16px; width: 200px; height: 110px;
        padding: 10px; background-color: #3a4150; border: 2px #8fb3ff;
        border-radius: 12px; }
h1 { display: block; font-size: 22px; color: #ffffff; margin-bottom: 8px; }
#tint { position: absolute; left: 150px; top: 100px; width: 140px; height: 100px;
        background-color: rgba(255, 120, 40, 128); }
</style>
</head>
<body><div id="card"><h1>Pulp + RmlUi</h1>Metal frame check</div><div id="tint"/></body>
</rml>)"},
    {"layout.rml", R"(<rml>
<head>
<style>
body { width: 100%; height: 100%; margin: 0; background-color: #f4f1ea;
       font-family: Inter; font-size: 14px; color: #202020; }
#row { display: flex; flex-direction: row; position: absolute; left: 10px; top: 20px; }
.box { width: 60px; height: 60px; margin: 10px; }
#a { background-color: #e04040; }
#b { background-color: #40c060; transform: rotate(15deg); }
#c { background-color: #4080e0; border: 3px #ffffff; border-radius: 30px; }
#label { position: absolute; left: 20px; top: 120px; width: 280px; }
</style>
</head>
<body><div id="row"><div class="box" id="a"/><div class="box" id="b"/><div class="box" id="c"/></div>
<div id="label">Gain -6.0 dB · Tone 1.2 kHz</div></body>
</rml>)"},
};

// The editor's size in points; frames are checked at backing scale 1 and 2.
inline constexpr std::uint32_t kWidthPoints = 320;
inline constexpr std::uint32_t kHeightPoints = 240;
inline constexpr std::uint32_t kScales[] = {1, 2};

// One face, registered on both sides as family "Inter", normal, weight 400.
inline constexpr const char* kFontAsset = "Inter-Regular.ttf";

// Serves the documents, and the font from the file the build points at.
class GoldenAssets final : public vellum::rmlui::AssetProvider {
  public:
    explicit GoldenAssets(std::filesystem::path font_file) : font_(std::move(font_file)) {}

    vellum::rmlui::Status load(std::string_view path, vellum::rmlui::Asset& out) override {
        out.path = std::string(path);
        for (const auto& document : kDocuments) {
            if (path == document.name) {
                out.bytes = document.rml;
                return vellum::rmlui::Status::ok;
            }
        }
        if (path == kFontAsset) {
            std::ifstream stream(font_, std::ios::binary);
            if (!stream)
                return vellum::rmlui::Status::missing_asset;
            out.bytes.assign(std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>());
            return vellum::rmlui::Status::ok;
        }
        return vellum::rmlui::Status::missing_asset;
    }

  private:
    std::filesystem::path font_;
};

inline std::string reference_name(std::string_view document, std::uint32_t width,
                                  std::uint32_t height) {
    std::string name(document);
    for (auto& c : name)
        if (c == '.')
            c = '_';
    return name + "_" + std::to_string(width) + "x" + std::to_string(height) + ".rgba";
}

// Premultiplied RGBA8, top row first.
inline bool write_rgba(const std::filesystem::path& file, const std::vector<std::uint8_t>& pixels) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(pixels.data()),
                 static_cast<std::streamsize>(pixels.size()));
    return static_cast<bool>(stream);
}

inline bool read_rgba(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
                      std::vector<std::uint8_t>& pixels) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return false;
    pixels.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return pixels.size() == static_cast<std::size_t>(width) * height * 4u;
}

// Fraction of pixels with any coverage; a frame that drew nothing reads 0.
inline double covered_fraction(const std::vector<std::uint8_t>& rgba) {
    if (rgba.empty())
        return 0.0;
    std::size_t covered = 0;
    for (std::size_t p = 0; p < rgba.size() / 4u; ++p)
        covered += rgba[p * 4 + 3] != 0 ? 1u : 0u;
    return static_cast<double>(covered) / static_cast<double>(rgba.size() / 4u);
}

} // namespace pulp_rmlui_golden
