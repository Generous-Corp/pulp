#include "dc_processor.hpp"
#include "sync_processor.hpp"
#include <pulp/format/headless.hpp>
#include <pulp/view/screenshot.hpp>
#include <cstdio>
using namespace pulp;
using namespace pulp::examples::brew;

static void shoot(format::ProcessorFactory f, const char* path,
                  void (*setup)(format::HeadlessHost&)) {
    format::HeadlessHost host(f);
    host.prepare(48000.0, 512, 2, 2);
    if (setup) setup(host);
    auto v = host.processor()->create_view();
    v->set_bounds({0, 0, 360, 380});
    const bool ok = view::render_to_file(*v, 360, 380, path, 2.0f);
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", path);
}

int main() {
    shoot(create_dc, "/tmp/brewshots/dc.png", [](format::HeadlessHost& h) {
        h.state().set_value(DcProcessor::kValue, 0.62f);
    });
    shoot(create_dc, "/tmp/brewshots/dc-negative.png", [](format::HeadlessHost& h) {
        h.state().set_value(DcProcessor::kValue, -0.45f);
    });
    shoot(create_sync, "/tmp/brewshots/sync-stopped.png", nullptr);
    shoot(create_sync, "/tmp/brewshots/sync-running.png", [](format::HeadlessHost& h) {
        audio::Buffer<float> in(2, 512), out(2, 512); in.clear(); out.clear();
        const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> iv(ip, 2, 512);
        auto ov = out.view();
        format::ProcessContext c;
        c.sample_rate = 48000; c.num_samples = 512; c.is_playing = true;
        c.transport_started = true; c.tempo_bpm = 120; c.position_beats = 0;
        h.process(ov, iv, c);
    });
    return 0;
}
