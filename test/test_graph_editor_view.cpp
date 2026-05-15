#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/widgets/graph_editor_view.hpp>

using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using pulp::host::SignalGraph;
using pulp::view::MouseEvent;
using pulp::view::Point;
using pulp::view::kModAlt;
using pulp::view::kModShift;
using pulp::view::widgets::GraphEditorView;

namespace {

constexpr float kNodeW = 160.0f;

Point output_port(float node_x, float node_y = 10.0f) {
    return {node_x + kNodeW, node_y + 24.0f};
}

Point input_port(float node_x, float node_y = 10.0f) {
    return {node_x, node_y + 24.0f};
}

MouseEvent modifier_event(uint16_t modifiers) {
    MouseEvent event;
    event.modifiers = modifiers;
    return event;
}

bool has_text(const RecordingCanvas& canvas, const std::string& text) {
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text && cmd.text == text)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("GraphEditorView paints graph nodes connections and drag ghost",
          "[view][graph_editor][issue-493]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(input, 0, output, 0));

    GraphEditorView editor(graph);
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);

    RecordingCanvas canvas;
    editor.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::begin_path) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::cubic_to) == 1);

    editor.on_mouse_down(output_port(10.0f));
    editor.on_mouse_drag({200.0f, 80.0f});

    RecordingCanvas drag_canvas;
    editor.paint(drag_canvas);
    REQUIRE(drag_canvas.count(DrawCommand::Type::cubic_to) == 2);
}

TEST_CASE("GraphEditorView drag connects output to input and reverse",
          "[view][graph_editor][issue-493]") {
    SignalGraph forward_graph;
    const auto forward_input = forward_graph.add_input_node(1, "Input");
    const auto forward_output = forward_graph.add_output_node(1, "Output");
    GraphEditorView forward(forward_graph);
    forward.set_node_position(forward_input, 10.0f, 10.0f);
    forward.set_node_position(forward_output, 240.0f, 10.0f);

    forward.on_mouse_down(output_port(10.0f));
    forward.on_mouse_up(input_port(240.0f));
    REQUIRE(forward_graph.connections().size() == 1);
    REQUIRE(forward_graph.connections()[0].source_node == forward_input);
    REQUIRE(forward_graph.connections()[0].dest_node == forward_output);
    REQUIRE_FALSE(forward_graph.connections()[0].feedback);
    REQUIRE_FALSE(forward_graph.connections()[0].midi);

    SignalGraph reverse_graph;
    const auto reverse_input = reverse_graph.add_input_node(1, "Input");
    const auto reverse_output = reverse_graph.add_output_node(1, "Output");
    GraphEditorView reverse(reverse_graph);
    reverse.set_node_position(reverse_input, 10.0f, 10.0f);
    reverse.set_node_position(reverse_output, 240.0f, 10.0f);

    reverse.on_mouse_down(input_port(240.0f));
    reverse.on_mouse_up(output_port(10.0f));
    REQUIRE(reverse_graph.connections().size() == 1);
    REQUIRE(reverse_graph.connections()[0].source_node == reverse_input);
    REQUIRE(reverse_graph.connections()[0].dest_node == reverse_output);
}

TEST_CASE("GraphEditorView modifier drags create feedback and midi edges",
          "[view][graph_editor][issue-493]") {
    SignalGraph feedback_graph;
    const auto feedback_input = feedback_graph.add_input_node(1, "Input");
    const auto feedback_output = feedback_graph.add_output_node(1, "Output");
    GraphEditorView feedback(feedback_graph);
    feedback.set_node_position(feedback_input, 10.0f, 10.0f);
    feedback.set_node_position(feedback_output, 240.0f, 10.0f);

    feedback.on_mouse_down(output_port(10.0f));
    feedback.on_mouse_event(modifier_event(kModShift));
    feedback.on_mouse_up(input_port(240.0f));
    REQUIRE(feedback_graph.connections().size() == 1);
    REQUIRE(feedback_graph.connections()[0].feedback);
    REQUIRE_FALSE(feedback_graph.connections()[0].midi);

    SignalGraph midi_graph;
    const auto midi_in = midi_graph.add_midi_input_node("Keys");
    const auto midi_out = midi_graph.add_midi_output_node("Sink");
    GraphEditorView midi(midi_graph);
    midi.set_node_position(midi_in, 10.0f, 10.0f);
    midi.set_node_position(midi_out, 240.0f, 10.0f);

    midi.on_mouse_down(output_port(10.0f));
    midi.on_mouse_event(modifier_event(kModAlt));
    midi.on_mouse_up(input_port(240.0f));
    REQUIRE(midi_graph.connections().size() == 1);
    REQUIRE(midi_graph.connections()[0].midi);
    REQUIRE_FALSE(midi_graph.connections()[0].feedback);
}

TEST_CASE("GraphEditorView reverse modifier drags create feedback and midi edges",
          "[view][graph_editor][coverage][issue-655]") {
    SignalGraph feedback_graph;
    const auto feedback_input = feedback_graph.add_input_node(1, "Input");
    const auto feedback_output = feedback_graph.add_output_node(1, "Output");
    GraphEditorView feedback(feedback_graph);
    feedback.set_node_position(feedback_input, 10.0f, 10.0f);
    feedback.set_node_position(feedback_output, 240.0f, 10.0f);

    feedback.on_mouse_down(input_port(240.0f));
    feedback.on_mouse_event(modifier_event(kModShift));
    feedback.on_mouse_up(output_port(10.0f));
    REQUIRE(feedback_graph.connections().size() == 1);
    REQUIRE(feedback_graph.connections()[0].feedback);

    SignalGraph midi_graph;
    const auto midi_in = midi_graph.add_midi_input_node("Keys");
    const auto midi_out = midi_graph.add_midi_output_node("Sink");
    GraphEditorView midi(midi_graph);
    midi.set_node_position(midi_in, 10.0f, 10.0f);
    midi.set_node_position(midi_out, 240.0f, 10.0f);

    midi.on_mouse_down(input_port(240.0f));
    midi.on_mouse_event(modifier_event(kModAlt));
    midi.on_mouse_up(output_port(10.0f));
    REQUIRE(midi_graph.connections().size() == 1);
    REQUIRE(midi_graph.connections()[0].midi);
}

TEST_CASE("GraphEditorView selection and miss handling are stable",
          "[view][graph_editor][issue-493]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");

    GraphEditorView editor(graph);
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);

    editor.on_mouse_down({20.0f, 20.0f});
    RecordingCanvas selected_canvas;
    editor.paint(selected_canvas);
    REQUIRE(selected_canvas.count(DrawCommand::Type::set_stroke_color) >= 2);

    editor.on_mouse_down({900.0f, 900.0f});
    editor.on_mouse_drag({1000.0f, 1000.0f});
    editor.on_mouse_up({1000.0f, 1000.0f});
    REQUIRE(graph.connections().empty());
}

TEST_CASE("GraphEditorView paints unnamed nodes and clears drag ghost after miss",
          "[view][graph_editor][coverage][issue-655]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "");
    const auto output = graph.add_output_node(1, "Output");

    GraphEditorView editor(graph);
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);

    RecordingCanvas initial;
    editor.paint(initial);
    REQUIRE(has_text(initial, "(unnamed)"));

    editor.on_mouse_down(output_port(10.0f));
    editor.on_mouse_drag({180.0f, 90.0f});
    RecordingCanvas dragging;
    editor.paint(dragging);
    REQUIRE(dragging.count(DrawCommand::Type::cubic_to) == 1);

    editor.on_mouse_up({900.0f, 900.0f});
    REQUIRE(graph.connections().empty());

    RecordingCanvas released;
    editor.paint(released);
    REQUIRE(released.count(DrawCommand::Type::cubic_to) == 0);
}
