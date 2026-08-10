#pragma once

// Where every panel and every cable end goes. No drawing, no Pulp, no Rack.
//
// Separated from the view deliberately. The painting is a dozen calls that
// either work or obviously do not; the geometry is where a preview goes
// subtly wrong -- a cable landing on the wrong jack, a panel at the wrong
// width shifting everything after it, an unmapped module silently drawn as
// though we knew where its ports were. That is the part worth testing, and
// testing it should not require linking a renderer.

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace forge_modular {

struct LayoutPort {
    int index = 0;
    bool is_input = false;
    std::string name;
    float x = 0.f;      ///< fraction of panel width
    float y = 0.f;      ///< px from the panel top
};

struct LayoutModule {
    std::string slug;
    int hp = 0;
    float width = 0.f;
    float height = 380.f;
    std::string image;      ///< empty when Rack has never rendered it
    bool mapped = false;    ///< false when its ports were never recorded
    std::vector<LayoutPort> ports;
};

struct LayoutPlaced {
    long id = 0;
    std::string slug;
    float order = 0.f;      ///< the patch's own x, used only to sort
    float x = 0.f;          ///< assigned: px from the rack's left edge
    float width = 0.f;
    float height = 380.f;
    bool known = false;     ///< is this module in the catalog at all
};

/// Where a cable meets a panel, and how much we actually know about it.
struct Endpoint {
    float x = 0.f, y = 0.f;
    bool exact = false;     ///< false when docked to the panel edge instead
    std::string port_name;  ///< empty unless the port map named it
};

/// Fallback width for a module we have never seen. Wrong, but wrong by a
/// bounded amount, and every panel after it would otherwise land nowhere.
constexpr float kUnknownWidth = 120.f;
constexpr float kPanelHeight = 380.f;

class PatchLayout {
public:
    void set_catalog(std::map<std::string, LayoutModule> c) {
        catalog_ = std::move(c);
    }

    /// Lay modules out left to right in the patch's own order, which is the
    /// order they will appear in Rack.
    void place(std::vector<LayoutPlaced> modules) {
        modules_ = std::move(modules);
        std::sort(modules_.begin(), modules_.end(),
                  [](const LayoutPlaced& a, const LayoutPlaced& b) {
                      return a.order < b.order;
                  });
        float pen = 0.f;
        height_ = 0.f;
        for (auto& m : modules_) {
            const auto* g = find(m.slug);
            m.known = g != nullptr;
            m.width = g ? g->width : kUnknownWidth;
            m.height = g ? g->height : kPanelHeight;
            m.x = pen;
            pen += m.width;
            height_ = std::max(height_, m.height);
        }
        width_ = pen;
    }

    float width() const { return width_; }
    float height() const { return height_; }
    const std::vector<LayoutPlaced>& modules() const { return modules_; }

    const LayoutPlaced* module_by_id(long id) const {
        for (const auto& m : modules_)
            if (m.id == id) return &m;
        return nullptr;
    }

    /// The scale that fits the rack into a pane, never magnifying past 1:1.
    ///
    /// A rack is a wide strip and a pane rarely is, so fitting beats cropping:
    /// the explanation beside it describes the whole patch, so the whole patch
    /// has to be visible.
    float fit_scale(float pane_w, float pane_h) const {
        if (width_ <= 0.f || height_ <= 0.f) return 1.f;
        return std::min(1.f, std::min(pane_w / width_, pane_h / height_));
    }

    /// Where a cable attaches.
    ///
    /// Exact when the port map recorded that jack. Otherwise it docks to the
    /// panel's bottom edge, spread by port index so several cables into one
    /// unmapped module stay apart -- and `exact` is false so a caller can say
    /// so rather than implying a precision it does not have. Drawing a cable
    /// confidently into a jack we cannot locate would teach somebody a wiring
    /// that is not there.
    Endpoint endpoint(long module_id, int port, bool is_input) const {
        Endpoint e;
        const auto* m = module_by_id(module_id);
        if (!m) return e;
        const auto* g = find(m->slug);
        if (g && g->mapped) {
            for (const auto& p : g->ports) {
                if (p.index == port && p.is_input == is_input) {
                    e.x = m->x + p.x * m->width;
                    e.y = p.y;
                    e.exact = true;
                    e.port_name = p.name;
                    return e;
                }
            }
        }
        const float slot = 0.2f + 0.15f * static_cast<float>(port % 5);
        e.x = m->x + m->width * slot;
        e.y = m->height - 6.f;
        e.exact = false;
        return e;
    }

    bool has_image(const std::string& slug) const {
        const auto* g = find(slug);
        return g && !g->image.empty();
    }

    /// How much of this patch we can draw faithfully, for an honest caption.
    struct Fidelity {
        int modules = 0, with_image = 0, mapped = 0, unknown = 0;
        bool exact() const {
            return unknown == 0 && with_image == modules && mapped == modules;
        }
    };

    Fidelity fidelity() const {
        Fidelity f;
        for (const auto& m : modules_) {
            ++f.modules;
            const auto* g = find(m.slug);
            if (!g) { ++f.unknown; continue; }
            if (!g->image.empty()) ++f.with_image;
            if (g->mapped) ++f.mapped;
        }
        return f;
    }

private:
    const LayoutModule* find(const std::string& slug) const {
        auto it = catalog_.find(slug);
        return it == catalog_.end() ? nullptr : &it->second;
    }

    std::map<std::string, LayoutModule> catalog_;
    std::vector<LayoutPlaced> modules_;
    float width_ = 0.f, height_ = 0.f;
};

}  // namespace forge_modular
