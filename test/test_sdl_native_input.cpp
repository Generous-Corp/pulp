#include <pulp/view/sdl_window_host.hpp>
#include <pulp/view/ui_components.hpp>
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace pulp::view;

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Root : View {
    void layout_children() override {}
};
struct Input : View {
    std::vector<MouseEvent> events;
    std::string text;
    bool wants_mouse_input() const override { return true; }
    bool accepts_text_input() const override { return true; }
    void on_mouse_event(const MouseEvent& event) override { events.push_back(event); }
    void on_text_input(const TextInputEvent& event) override { text += event.text; }
    int count(MousePhase phase) const {
        int n = 0;
        for (const auto& event : events) if (event.phase == phase) ++n;
        return n;
    }
};
void button(bool down, float x, float y) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x; event.button.y = y;
    require(SDL_PushEvent(&event), "queue button");
}
void key(SDL_Keycode code) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = code;
    event.key.down = true;
    require(SDL_PushEvent(&event), "queue key");
}
void move(float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x; event.motion.y = y;
    event.motion.state = SDL_BUTTON_LMASK;
    require(SDL_PushEvent(&event), "queue motion");
}
}

int main() {
    try {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        Root root;
        root.set_bounds({0, 0, 400, 300});
        auto combo_owner = std::make_unique<ComboBox>();
        auto* combo = combo_owner.get();
        combo->set_bounds({20, 20, 120, 30});
        combo->set_items({"One", "Two", "Three"});
        root.add_child(std::move(combo_owner));
        auto input_owner = std::make_unique<Input>();
        auto* input = input_owner.get();
        input->set_bounds({180, 90, 180, 130});
        input->set_focusable(true);
        root.add_child(std::move(input_owner));
        int clicks = 0;
        input->on_click = [&] { ++clicks; };
        auto host = SdlWindowHost::create(root, {});
        require(bool(host), "SDL host exists");
        int stage = 0;
        host->set_idle_callback([&] {
            switch (stage++) {
                case 0: button(true, 40, 35); button(false, 40, 35); break;
                case 1: require(combo->is_open(), "click opens dropdown"); key(SDLK_DOWN); break;
                case 2: require(combo->hovered_index() == 1, "Down advances dropdown highlight"); key(SDLK_RETURN); break;
                case 3:
                    require(!combo->is_open() && combo->selected() == 1, "Return commits dropdown");
                    button(true, 40, 35); button(false, 40, 35); break;
                case 4: require(combo->is_open(), "dropdown reopens"); key(SDLK_ESCAPE); break;
                case 5:
                    require(!combo->is_open() && combo->selected() == 1, "Escape cancels dropdown");
                    SDL_SetModState(SDL_KMOD_SHIFT);
                    button(true, 200, 110); break;
                case 6:
                    require(input->count(MousePhase::press) == 1, "one native press");
                    require(input->events.back().isShiftDown(), "Shift reaches press");
                    move(210, 110); break;
                case 7:
                    require(input->count(MousePhase::drag) == 1, "one first drag tick");
                    require(input->events.back().isShiftDown(), "Shift reaches drag");
                    move(220, 110); break;
                case 8:
                    require(input->count(MousePhase::press) == 1 && input->count(MousePhase::drag) == 2,
                            "motion does not restart press");
                    button(false, 220, 110); break;
                case 9:
                    require(input->count(MousePhase::release) == 1 && clicks == 1, "one release and click");
                    require(input->events.back().isShiftDown(), "Shift reaches release");
                    SDL_SetModState(SDL_KMOD_NONE);
                    button(true, 200, 110); break;
                case 10: {
                    SDL_Event event{}; event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
                    require(SDL_PushEvent(&event), "queue focus loss"); break;
                }
                case 11:
                    require(input->events.back().is_cancelled && clicks == 1,
                            "focus loss cancels without clicking");
                    move(220, 110); break;
                case 12: {
                    require(input->events.back().phase == MousePhase::hover, "uncaptured motion reaches hover");
                    input->set_cursor(View::CursorStyle::invisible);
                    SDL_Event event{}; event.type = SDL_EVENT_TEXT_INPUT;
                    event.text.text = "\xc3\xa9";
                    require(SDL_PushEvent(&event), "queue UTF-8 text"); break;
                }
                case 13: {
                    require(input->text == "\xc3\xa9", "native UTF-8 text reaches focused field once");
                    require(!SDL_CursorVisible(), "stationary cursor style change hides native cursor");
                    input->set_cursor(View::CursorStyle::pointer);
                    break;
                }
                case 14: {
                    require(SDL_CursorVisible(), "stationary cursor style change restores native cursor");
                    SDL_Event event{}; event.type = SDL_EVENT_QUIT;
                    require(SDL_PushEvent(&event), "queue quit"); break;
                }
                default: require(stage < 18, "bounded SDL test loop"); break;
            }
        });
        host->run_event_loop();
        require(stage >= 15, "event loop processed test sequence");
        std::cout << "PASS: native SDL dropdown, modifiers, drag, cancellation, hover and UTF-8\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
