// Musical Typing Keyboard — focused live demo. Opens a GPU window with JUST the
// MusicalTypingKeyboard primitive so it can be played/dragged directly (no app
// to build into). Notes + controls print to the terminal so the wiring is
// visible. Headless: `--screenshot out.png` renders without opening a window.
//
//   ./build/examples/mtk-demo/pulp-mtk-demo                 # interactive window
//   ./build/examples/mtk-demo/pulp-mtk-demo --screenshot /tmp/mtk.png
//
// In the window: click keys, type a w s e d f t g y h u j k o l p, z/x octave,
// c/v velocity, click 🎹/⌨ to toggle piano/typing, the on-screen octave/velocity
// buttons. Logic-faithful controls: keys 1/2 (or the −/+ pads) are momentary
// pitch bend; keys 3–8 (or the 6 pads) are a latched modulation selector (3 =
// off … 8 = max); tab (or the pad) is a momentary sustain hold. Readouts
// (OCTAVE / VEL / PITCH BEND) track state live.

#include <pulp/design/design_system.hpp>      // ink_signal_theme
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/window_host.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace pulp::view;

namespace {
std::string note_name(int midi) {
    static const char* pc[] = {"C", "C#", "D", "D#", "E", "F",
                               "F#", "G", "G#", "A", "A#", "B"};
    return std::string(pc[((midi % 12) + 12) % 12]) + std::to_string(midi / 12 - 2);
}
}  // namespace

int main(int argc, char** argv) {
    const char* screenshot = nullptr;
    int octave = 0;   // --octave N pre-shifts the octave (for render proofs)
    bool piano = false;
    bool demo = false;   // --demo drives the whole control surface, then renders
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--octave") && i + 1 < argc) octave = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--piano")) piano = true;
        else if (!std::strcmp(argv[i], "--demo")) demo = true;
    }

    auto kb = std::make_unique<MusicalTypingKeyboard>();
    kb->set_theme(pulp::design::ink_signal_theme(/*dark=*/true));
    if (piano) kb->set_mode(MusicalTypingKeyboard::Mode::piano);
    kb->on_note_on  = [](int n, float v) { std::printf("note on  %-3d %-3s  vel %.2f\n", n, note_name(n).c_str(), v); };
    kb->on_note_off = [](int n)          { std::printf("note off %-3d %-3s\n", n, note_name(n).c_str()); };
    kb->on_pitch_bend = [](float b)      { std::printf("pitch bend %+.2f\n", b); };
    kb->on_sustain    = [](bool on)      { std::printf("sustain %s\n", on ? "on" : "off"); };
    kb->on_modulation = [](float a)      { std::printf("modulation %.2f\n", a); };

    // Drive the octave the real way (z/x key events) so the readout label AND the
    // overview highlight both reflect it — used for render proofs of #80.
    for (int k = 0; k < std::abs(octave); ++k) {
        KeyEvent e{}; e.key = octave > 0 ? KeyCode::x : KeyCode::z; e.is_down = true;
        kb->on_key_event(e);
    }

    // --demo: exercise EVERY control through the real input paths and hold them so
    // a single render shows them all active at once (and the callbacks print). A
    // proof that the whole surface is wired, not just static chrome.
    if (demo) {
        auto hold = [&](KeyCode k) { KeyEvent e{}; e.key = k; e.is_down = true; kb->on_key_event(e); };
        std::puts("── driving the full control surface ──");
        hold(KeyCode::x); hold(KeyCode::x);   // octave +2 → C4 (highlight moves)
        hold(KeyCode::v);                     // velocity +1
        hold(KeyCode::a); hold(KeyCode::d); hold(KeyCode::g);  // a 3-note chord (keys light)
        hold(KeyCode::num2);                  // pitch bend UP (held → +20, pad lit)
        hold(KeyCode::num6);                  // modulation step (latched, pad lit)
        hold(KeyCode::tab);                   // sustain (held → lit)
        std::puts("── rendering held state ──");
    }

    const float w = kb->panel_width(), h = kb->panel_height();

    if (screenshot) {
        kb->set_bounds({0, 0, w, h});
        const bool ok = render_to_file(*kb, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                       screenshot, 2.0f, ScreenshotBackend::skia);
        std::printf(ok ? "wrote %s\n" : "render failed (no Skia?)\n", screenshot);
        return ok ? 0 : 1;
    }

    WindowOptions opts;
    opts.title = "Musical Typing Keyboard — Pulp demo";
    opts.width = w; opts.height = h;
    opts.min_width = w; opts.min_height = 176.0f;  // piano mode is shorter
    opts.resizable = true;
    opts.use_gpu = true;     // GPU (Skia Graphite) — required for the faithful SVG
    auto window = WindowHost::create(*kb, opts);
    if (!window) { std::fprintf(stderr, "failed to create window host\n"); return 1; }
    window->set_design_viewport(w, h);   // aspect-locked fit; piano mode letterboxes
    window->set_fixed_aspect_ratio(w / h);
    // Resize the window to fit the active frame on a piano⇄typing toggle: the
    // window shrinks/grows in height, top-aligned (the 🎹/⌨ toggles stay put).
    WindowHost* host = window.get();
    kb->on_intrinsic_size_changed = [host](float nw, float nh) {
        host->set_fixed_aspect_ratio(nw / nh);
        host->set_design_viewport(nw, nh);
        host->request_content_size(nw, nh);
    };
    window->set_close_callback([]() {});
    std::printf("Musical Typing Keyboard demo — GPU window. Play with the mouse/keys; "
                "close the window to exit.\n");
    window->run_event_loop();
    return 0;
}
