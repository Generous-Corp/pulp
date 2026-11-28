// Calls one compiled function from each installed Vellum integration package.

#include <pulp/format/third_party_notices.hpp>
#include <pulp/vellum/about_notices.hpp>
#include <pulp/vellum_audio/graph_scale.hpp>

#include <cmath>
#include <cstdio>

int main() {
    // pulp-vellum-audio: a log frequency axis puts the geometric midpoint of
    // 20 Hz and 20 kHz (about 632 Hz) at the middle of the axis.
    const pulp::vellum_audio::LogFrequencyScale axis{20.0f, 20000.0f, 0.0f, 1000.0f};
    const float middle = axis.to_x(std::sqrt(20.0f * 20000.0f));
    const bool axis_ok = std::fabs(middle - 500.0f) < 1.0f;

    // pulp-vellum: the About view's model builder, which lives in the package's
    // compiled library. One notice yields one row, collapsed.
    pulp::format::ThirdPartyNotices notices;
    notices.product = "Smoke";
    notices.notices.push_back({.name = "Example", .version = "1.0", .license = "MIT"});
    const auto model = pulp::vellum::make_about_model(notices);
    const bool model_ok = model.rows.size() == 1 && model.rows[0].name == "Example" &&
                          model.rows[0].license == "MIT" && !model.rows[0].expanded;

    std::printf("Pulp::vellum-audio -> LogFrequencyScale midpoint x=%.1f\n", middle);
    std::printf("Pulp::vellum -> make_about_model rows=%zu\n", model.rows.size());
    return axis_ok && model_ok ? 0 : 1;
}
