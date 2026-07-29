#include "delay_params.hpp"
#include "pulp_delay_editor.hpp"

#include <pulp/view/screenshot.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace pulp;
using namespace pulp::examples::delay;

int main(int argc, char** argv) {
    const std::filesystem::path output_dir =
        argc > 1 ? argv[1] : "/tmp/pulp-delay-ui-shots";
    std::error_code error;
    std::filesystem::create_directories(output_dir, error);
    if (error) {
        std::fprintf(stderr, "Unable to create %s: %s\n",
                     output_dir.string().c_str(), error.message().c_str());
        return 2;
    }

    state::StateStore store;
    define_delay_parameters(store);
    auto editor = ui::build_pulp_delay_editor(store);

    constexpr std::array<float, 5> values = {
        0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    constexpr std::array<const char*, 5> names = {
        "000", "025", "050", "075", "100"};
    bool ok = true;
    for (std::size_t index = 0; index < values.size(); ++index) {
        store.set_normalized(kFeedback, values[index]);
        store.pump_listeners();
        const auto path = output_dir
            / (std::string("pulp-delay-feedback-") + names[index] + ".png");
        const bool captured = view::render_to_file(
            *editor, 1120, 740, path.string(), 1.0f,
            view::ScreenshotBackend::skia);
        std::printf("%s  feedback=%.2f  %s\n",
                    captured ? "OK" : "FAILED", values[index],
                    path.string().c_str());
        ok = ok && captured;
    }

    constexpr std::array<Character, 4> characters = {
        Character::clean, Character::vintage, Character::tape, Character::bbd};
    constexpr std::array<const char*, 4> character_names = {
        "clean", "vintage", "tape", "bbd"};
    store.set_value(kFeedback, 62.0f);
    for (std::size_t index = 0; index < characters.size(); ++index) {
        store.set_value(kCharacter, static_cast<float>(characters[index]));
        store.pump_listeners();
        const auto path = output_dir
            / (std::string("pulp-delay-character-")
               + character_names[index] + ".png");
        const bool captured = view::render_to_file(
            *editor, 1120, 740, path.string(), 1.0f,
            view::ScreenshotBackend::skia);
        std::printf("%s  character=%s  %s\n",
                    captured ? "OK" : "FAILED", character_names[index],
                    path.string().c_str());
        ok = ok && captured;
    }
    return ok ? 0 : 1;
}
