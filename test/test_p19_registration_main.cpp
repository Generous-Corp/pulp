#include <pulp/view/data_display.hpp>
#include <pulp/view/register_audio_widgets.hpp>

int main() {
    pulp::view::register_audio_widgets();
    return pulp::view::has_data_display("pulp.level") ? 0 : 1;
}
