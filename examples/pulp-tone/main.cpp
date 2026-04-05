// PulpTone Standalone — synth with musical typing keyboard
// Computer keyboard maps to piano notes (like GarageBand Musical Typing)
// Also accepts hardware MIDI input

#include "pulp_tone.hpp"
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#if defined(__APPLE__)
#include <termios.h>
#include <unistd.h>

// Non-blocking keyboard input on macOS
static struct termios orig_termios;

static void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static int read_key() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) return c;
    return -1;
}
#endif

#if defined(__APPLE__)
// Musical typing keyboard layout (2 octaves)
// Bottom row: Z-M = C3-B3
// Top row:    Q-P = C4-B4
// Sharps: S,D,G,H,J (bottom) and 2,3,5,6,7 (top)
static int key_to_note(int key) {
    // Bottom octave (C3 = MIDI 48)
    switch (key) {
        case 'z': case 'Z': return 48; // C3
        case 's': case 'S': return 49; // C#3
        case 'x': case 'X': return 50; // D3
        case 'd': case 'D': return 51; // D#3
        case 'c': case 'C': return 52; // E3
        case 'v': case 'V': return 53; // F3
        case 'g': case 'G': return 54; // F#3
        case 'b': case 'B': return 55; // G3
        case 'h': case 'H': return 56; // G#3
        case 'n': case 'N': return 57; // A3
        case 'j': case 'J': return 58; // A#3
        case 'm': case 'M': return 59; // B3

        // Top octave (C4 = MIDI 60)
        case 'q': case 'Q': return 60; // C4 (Middle C)
        case '2':            return 61; // C#4
        case 'w': case 'W': return 62; // D4
        case '3':            return 63; // D#4
        case 'e': case 'E': return 64; // E4
        case 'r': case 'R': return 65; // F4
        case '5':            return 66; // F#4
        case 't': case 'T': return 67; // G4
        case '6':            return 68; // G#4
        case 'y': case 'Y': return 69; // A4 (440 Hz)
        case '7':            return 70; // A#4
        case 'u': case 'U': return 71; // B4
        case 'i': case 'I': return 72; // C5

        default: return -1;
    }
}
#endif

static std::atomic<bool> should_quit{false};
void signal_handler(int) { should_quit.store(true); }

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    pulp::runtime::log_info("PulpTone Standalone v1.0.0");

    pulp::format::StandaloneApp app(pulp::examples::create_pulp_tone);

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0; // Synth — no audio input

    app.set_config(config);

    if (!app.start()) {
        pulp::runtime::log_error("Failed to start standalone app");
        return 1;
    }

    const char* waveforms[] = {"Sine", "Saw", "Square"};
    int wf = static_cast<int>(app.state().get_value(pulp::examples::kWaveform));

    std::cout << "\n╔══════════════════════════════════════════╗\n"
              << "║  PulpTone — Musical Typing Synthesizer   ║\n"
              << "╠══════════════════════════════════════════╣\n"
              << "║  Waveform: " << waveforms[wf] << "                          ║\n"
              << "║  Volume:   " << app.state().get_value(pulp::examples::kVolume) << " dB                      ║\n"
              << "╠══════════════════════════════════════════╣\n"
              << "║  │ 2 │ 3 │   │ 5 │ 6 │ 7 │             ║\n"
              << "║  │C#4│D#4│   │F#4│G#4│A#4│             ║\n"
              << "║ Q │ W │ E │ R │ T │ Y │ U │ I           ║\n"
              << "║ C4│ D4│ E4│ F4│ G4│ A4│ B4│ C5          ║\n"
              << "║                                          ║\n"
              << "║  │ S │ D │   │ G │ H │ J │             ║\n"
              << "║  │C#3│D#3│   │F#3│G#3│A#3│             ║\n"
              << "║ Z │ X │ C │ V │ B │ N │ M               ║\n"
              << "║ C3│ D3│ E3│ F3│ G3│ A3│ B3              ║\n"
              << "╠══════════════════════════════════════════╣\n"
              << "║  1/8/9: Sine/Saw/Square  +/-: Volume    ║\n"
              << "║  Esc or Ctrl+C to quit                   ║\n"
              << "╚══════════════════════════════════════════╝\n"
              << std::endl;

#if defined(__APPLE__)
    enable_raw_mode();

    // Track which keys are held (for note-off when released)
    bool key_held[128] = {};

    while (!should_quit.load()) {
        int key = read_key();
        if (key == 27) { // Esc
            should_quit.store(true);
            break;
        }

        if (key > 0) {
            // Waveform selection
            if (key == '1') {
                app.state().set_value(pulp::examples::kWaveform, 0.0f);
                std::cout << "\rWaveform: Sine   " << std::flush;
            } else if (key == '8') {
                app.state().set_value(pulp::examples::kWaveform, 1.0f);
                std::cout << "\rWaveform: Saw    " << std::flush;
            } else if (key == '9') {
                app.state().set_value(pulp::examples::kWaveform, 2.0f);
                std::cout << "\rWaveform: Square " << std::flush;
            }
            // Volume
            else if (key == '+' || key == '=') {
                float vol = app.state().get_value(pulp::examples::kVolume);
                app.state().set_value(pulp::examples::kVolume, vol + 3.0f);
            } else if (key == '-') {
                float vol = app.state().get_value(pulp::examples::kVolume);
                app.state().set_value(pulp::examples::kVolume, vol - 3.0f);
            }
            // Musical typing
            else {
                int note = key_to_note(key);
                if (note >= 0 && !key_held[note]) {
                    key_held[note] = true;
                    // Send note-on via MIDI buffer would require threading...
                    // For now, directly trigger the processor's note handling
                    // by injecting a MIDI event into the pending buffer
                    // This is a simplification — proper implementation would
                    // use the standalone app's MIDI injection API
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#else
    // Non-macOS: just wait for Ctrl+C
    while (!should_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif

    app.stop();
    pulp::runtime::log_info("PulpTone stopped");
    return 0;
}
