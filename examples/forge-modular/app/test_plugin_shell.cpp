#include "plugin_shell.hpp"
#include <cstdio>
int main() {
    forge_modular::ForgeModularShell s;
    const auto d = s.descriptor();
    int bad = 0;
    auto ck = [&](bool ok, const char* what) {
        std::printf(ok ? "  ok     %s\n" : "  WRONG  %s\n", what);
        if (!ok) ++bad;
    };
    ck(d.category == pulp::format::PluginCategory::Effect,
       "declares itself an effect, not an instrument that owes a host audio");
    ck(s.latency_samples() == 0, "reports no latency");
    ck(s.tail_seconds() == 0.0, "reports no tail");
    const auto r = s.reach();
    ck(!r.can_launch_rack,
       "never claims it can launch Rack from inside a host — no plugin can "
       "instantiate another");
    ck(!r.note.empty(), "says what the user has to do instead");
    std::printf("\n%s: %d problem(s)\n", bad ? "FAIL" : "ok", bad);
    return bad ? 1 : 0;
}
