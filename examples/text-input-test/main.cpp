#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/window_host.hpp>
#include <iostream>
using namespace pulp::view;

int main() {
    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.flex().padding = 20;
    root.flex().gap = 10;

    auto label = std::make_unique<Label>("Click the text field and type:");
    label->set_id("label");
    label->flex().preferred_height = 20;
    root.add_child(std::move(label));

    auto editor = std::make_unique<TextEditor>();
    editor->set_id("editor");
    editor->placeholder = "Type here...";
    editor->flex().preferred_height = 30;
    editor->on_return = [](const std::string& text) {
        std::cout << "on_return: '" << text << "'\n";
    };
    editor->on_change = [](const std::string& text) {
        std::cout << "on_change: '" << text << "'\n";
    };
    root.add_child(std::move(editor));

    WindowOptions opts;
    opts.title = "TextEditor Test";
    opts.width = 400;
    opts.height = 150;
    auto window = WindowHost::create(root, opts);
    window->set_close_callback([] { std::cout << "Closed\n"; });
    std::cout << "Click the text field and type. Check console.\n";
    window->run_event_loop();
    return 0;
}
