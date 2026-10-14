// Linked into pulp-test-audio-registration through a static library that links
// pulp::view-audio PRIVATE, so the build-tree registration shim never reaches
// the executable. See test_audio_registration_main.cpp.

#include <pulp/view/data_display.hpp>
#include <pulp/view/register_audio_widgets.hpp>

#include <cstdio>
#include <memory>

namespace pulp::test {

int check_explicit_audio_registration() {
    for (const char* id : {"pulp.level", "pulp.series", "pulp.bins"}) {
        if (pulp::view::has_data_display(id)) {
            std::fprintf(stderr, "'%s' was registered before main(); the shim reached this image\n",
                         id);
            return 2;
        }
    }
    pulp::view::register_audio_widgets();
    for (const char* id : {"pulp.level", "pulp.series", "pulp.bins"}) {
        auto view = pulp::view::make_data_display(id);
        if (!view) {
            std::fprintf(stderr, "register_audio_widgets() did not register '%s'\n", id);
            return 1;
        }
    }
    if (!dynamic_cast<pulp::view::LevelDisplay*>(
            pulp::view::make_data_display("pulp.level").get())) {
        std::fprintf(stderr, "'pulp.level' is not a LevelDisplay\n");
        return 1;
    }
    return 0;
}

} // namespace pulp::test
