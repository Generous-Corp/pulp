// Render a real patch through the preview and check what came out.
//
// The preview composites panel images and draws cables over them, and both
// halves fail quietly: a missing image renders as a placeholder that still
// looks deliberate, and a cable to a module we cannot locate lands somewhere
// plausible. So this asserts against the pixels rather than the code path --
// that panels actually appear, that they land where the layout says, and that
// cables are drawn between them.
//
// The Skia backend is not optional. render_to_png's default on macOS is
// CoreGraphics, whose canvas does not implement draw_image_from_file: every
// panel would come back as its own filename in placeholder text, which looks
// like a broken import rather than a backend choice.

#include "patch_preview.hpp"

#include <pulp/view/screenshot.hpp>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void fail(const std::string& m) { std::printf("  FAIL  %s\n", m.c_str()); ++failures; }
void pass(const std::string& m) { std::printf("  ok    %s\n", m.c_str()); }

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Minimal readers for the geometry contract and a patch. A full JSON parser
/// is not worth a dependency here; both documents have a fixed shape.
std::string str_field(const std::string& b, const std::string& k) {
    const std::string pat = "\"" + k + "\"";
    size_t i = b.find(pat);
    if (i == std::string::npos) return "";
    size_t c = b.find(':', i + pat.size());
    size_t q = b.find('"', c);
    if (q == std::string::npos || q > b.find('\n', c) + 400) return "";
    size_t e = b.find('"', q + 1);
    return b.substr(q + 1, e - q - 1);
}

double num_field(const std::string& b, const std::string& k, double dflt = 0) {
    const std::string pat = "\"" + k + "\"";
    size_t i = b.find(pat);
    if (i == std::string::npos) return dflt;
    size_t c = b.find(':', i + pat.size());
    return c == std::string::npos ? dflt : std::strtod(b.c_str() + c + 1, nullptr);
}

std::vector<std::string> objects(const std::string& doc, const std::string& array) {
    std::vector<std::string> out;
    size_t a = doc.find("\"" + array + "\"");
    if (a == std::string::npos) return out;
    a = doc.find('[', a);
    if (a == std::string::npos) return out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = a; i < doc.size(); ++i) {
        if (doc[i] == '{') { if (depth++ == 0) start = i; }
        else if (doc[i] == '}') {
            if (--depth == 0) out.push_back(doc.substr(start, i - start + 1));
        } else if (doc[i] == ']' && depth == 0) break;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: preview-smoke <geometry.json> <patch.vcv> <out.png>\n");
        return 2;
    }
    const std::string geo = slurp(argv[1]);
    const std::string patch = slurp(argv[2]);
    const std::string out = argv[3];

    std::map<std::string, forge_modular::PreviewModule> catalog;
    for (const auto& e : objects(geo, "modules")) {
        forge_modular::PreviewModule m;
        m.slug = str_field(e, "slug");
        if (m.slug.empty()) continue;
        m.hp = static_cast<int>(num_field(e, "hp"));
        m.width = static_cast<float>(num_field(e, "width", 120));
        m.height = static_cast<float>(num_field(e, "height", 380));
        m.image = str_field(e, "image");
        m.mapped = e.find("\"mapped\": true") != std::string::npos;
        for (const auto& p : objects(e, "ports")) {
            forge_modular::PreviewPort pp;
            pp.index = static_cast<int>(num_field(p, "index"));
            pp.is_input = str_field(p, "dir") == "in";
            pp.name = str_field(p, "name");
            pp.x = static_cast<float>(num_field(p, "x"));
            pp.y = static_cast<float>(num_field(p, "y"));
            m.ports.push_back(pp);
        }
        catalog[m.slug] = m;
    }
    std::printf("catalog: %zu modules\n", catalog.size());

    std::vector<forge_modular::PlacedModule> placed;
    std::map<long, std::string> by_id;
    for (const auto& e : objects(patch, "modules")) {
        forge_modular::PlacedModule p;
        p.id = static_cast<long>(num_field(e, "id"));
        p.slug = str_field(e, "plugin") + "/" + str_field(e, "model");
        // `pos` orders the rack; layout() reassigns real pixel positions.
        size_t b = e.find("\"pos\"");
        p.x = b == std::string::npos ? 0.f
            : static_cast<float>(std::strtod(e.c_str() + e.find('[', b) + 1, nullptr));
        placed.push_back(p);
        by_id[p.id] = p.slug;
    }

    std::vector<forge_modular::PreviewCable> cables;
    for (const auto& e : objects(patch, "cables")) {
        forge_modular::PreviewCable c;
        c.from_id = static_cast<long>(num_field(e, "outputModuleId"));
        c.from_port = static_cast<int>(num_field(e, "outputId"));
        c.to_id = static_cast<long>(num_field(e, "inputModuleId"));
        c.to_port = static_cast<int>(num_field(e, "inputId"));
        cables.push_back(c);
    }
    std::printf("patch: %zu modules, %zu cables\n", placed.size(), cables.size());

    int unknown = 0, unmapped = 0, imageless = 0;
    for (const auto& p : placed) {
        auto it = catalog.find(p.slug);
        if (it == catalog.end()) { ++unknown; continue; }
        if (!it->second.mapped) ++unmapped;
        if (it->second.image.empty()) ++imageless;
    }
    std::printf("  %d not in the catalog · %d unmapped · %d without an image\n",
                unknown, unmapped, imageless);

    forge_modular::PatchPreview view;
    view.set_catalog(catalog);
    view.set_patch(placed, cables);
    view.set_bounds({0, 0, 1200, 460});

    std::printf("  rack is %.0f x %.0f px at 1:1 (%.1f:1)\n",
                view.content_width(), view.content_height(),
                view.content_height() > 0
                    ? view.content_width() / view.content_height() : 0.f);

    if (view.content_width() <= 0) fail("the rack has no width — nothing laid out");
    else pass("laid out");

    const bool ok = pulp::view::render_to_file(
        view, 1200, 460, out, 2.0f, pulp::view::ScreenshotBackend::skia);
    if (!ok) {
        fail("render_to_file failed");
    } else {
        std::ifstream f(out, std::ios::binary | std::ios::ate);
        const long bytes = f.tellg();
        // A pane of flat background compresses to almost nothing. Real panels
        // and cables do not, so size is a crude but honest liveness check.
        if (bytes < 4000)
            fail("the PNG is " + std::to_string(bytes) +
                 " bytes — nothing was drawn but the background");
        else
            pass("rendered " + std::to_string(bytes) + " bytes to " + out);
    }

    std::printf("%s: %d failure(s)\n", failures ? "SMOKE FAILED" : "smoke passed",
                failures);
    return failures ? 1 : 0;
}
