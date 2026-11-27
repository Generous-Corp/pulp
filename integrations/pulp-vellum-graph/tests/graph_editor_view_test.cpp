#include <catch2/catch_test_macros.hpp>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scan_blacklist.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/vellum_graph/graph_editor_view.hpp>
#include <pulp/vellum_graph/hosted_editor_attachment.hpp>
#include <pulp/vellum_graph/plugin_manager_panel.hpp>
#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/input_events.hpp>
#include <vellum/view/screenshot.hpp>
#include <vellum/view/window_host.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using pulp::host::NodeId;
using pulp::host::SignalGraph;
using pulp::vellum_graph::GraphEditorView;
using ::vellum::canvas::Color;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::kModAlt;
using ::vellum::view::kModShift;
using ::vellum::view::MouseEvent;
using ::vellum::view::MousePhase;
using ::vellum::view::Point;

namespace {

constexpr float kNodeW = GraphEditorView::kNodeWidth;

constexpr Color kAudioBlue = {0.30f, 0.55f, 0.95f, 1.0f};
constexpr Color kMidiGreen = {0.30f, 0.85f, 0.45f, 1.0f};
constexpr Color kFeedbackRed = {0.95f, 0.40f, 0.40f, 1.0f};
constexpr Color kSelectedYellow = {0.95f, 0.85f, 0.20f, 1.0f};

Point output_port(float node_x, float node_y = 10.0f) {
    return {node_x + kNodeW, node_y + 24.0f};
}

Point input_port(float node_x, float node_y = 10.0f) {
    return {node_x, node_y + 24.0f};
}

MouseEvent modifier_event(std::uint16_t modifiers) {
    MouseEvent event;
    event.modifiers = modifiers;
    return event;
}

bool has_fill_rect(const RecordingCanvas& canvas, float x, float y, float w, float h) {
    return std::any_of(canvas.commands().begin(), canvas.commands().end(),
                       [&](const DrawCommand& cmd) {
                           return cmd.type == DrawCommand::Type::fill_rect && cmd.f[0] == x &&
                                  cmd.f[1] == y && cmd.f[2] == w && cmd.f[3] == h;
                       });
}

bool has_text(const RecordingCanvas& canvas, const std::string& text) {
    return std::any_of(canvas.commands().begin(), canvas.commands().end(),
                       [&](const DrawCommand& cmd) {
                           return cmd.type == DrawCommand::Type::fill_text && cmd.text == text;
                       });
}

// The stroke colour in force when the n-th stroke_current_path was issued.
std::vector<Color> edge_stroke_colors(const RecordingCanvas& canvas) {
    std::vector<Color> colors;
    Color current{};
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_stroke_color)
            current = cmd.color;
        else if (cmd.type == DrawCommand::Type::stroke_current_path)
            colors.push_back(current);
    }
    return colors;
}

// The stroke colour in force when each stroke_rect (a node outline) was issued.
std::vector<Color> outline_stroke_colors(const RecordingCanvas& canvas) {
    std::vector<Color> colors;
    Color current{};
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_stroke_color)
            current = cmd.color;
        else if (cmd.type == DrawCommand::Type::stroke_rect)
            colors.push_back(current);
    }
    return colors;
}

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    struct Pixel {
        int r, g, b, a;
    };
    Pixel at(std::uint32_t x, std::uint32_t y) const {
        REQUIRE(!pixels.empty());
        REQUIRE(x < width);
        REQUIRE(y < height);
        const auto o = (static_cast<std::size_t>(y) * width + x) * 4;
        return {pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3]};
    }
};

Frame render(::vellum::view::View& view, std::uint32_t w, std::uint32_t h) {
    Frame frame;
    frame.pixels = ::vellum::view::render_to_rgba(view, w, h, 1.0f, &frame.width, &frame.height);
    return frame;
}

// PULP_VELLUM_GRAPH_PNG_DIR names a directory to receive rendered frames for
// review; unset (the default) writes nothing.
void maybe_write_png(::vellum::view::View& view, std::uint32_t w, std::uint32_t h,
                     const std::string& name) {
    if (const char* dir = std::getenv("PULP_VELLUM_GRAPH_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(view, w, h, std::string(dir) + "/" + name, 2.0f));
}

bool is_blue(const Frame::Pixel& p) {
    return p.b > 180 && p.b > p.r + 60 && p.b > p.g + 20;
}
bool is_green(const Frame::Pixel& p) {
    return p.g > 150 && p.g > p.r + 60 && p.g > p.b + 60;
}
bool is_red(const Frame::Pixel& p) {
    return p.r > 180 && p.r > p.g + 80 && p.r > p.b + 80;
}
bool is_yellow(const Frame::Pixel& p) {
    return p.r > 180 && p.g > 150 && p.b < 120;
}

} // namespace

TEST_CASE("GraphEditorView paints graph nodes connections and drag ghost",
          "[vellum-graph][graph_editor]") {
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
    // The curve is only visible once the path is stroked.
    REQUIRE(canvas.count(DrawCommand::Type::stroke_current_path) == 1);
    REQUIRE(edge_stroke_colors(canvas) == std::vector<Color>{kAudioBlue});
    // One outline per node.
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rect) == 2);

    editor.on_mouse_down(output_port(10.0f));
    editor.on_mouse_drag({200.0f, 80.0f});
    REQUIRE(editor.is_connecting());

    RecordingCanvas drag_canvas;
    editor.paint(drag_canvas);
    REQUIRE(drag_canvas.count(DrawCommand::Type::cubic_to) == 2);
    REQUIRE(drag_canvas.count(DrawCommand::Type::stroke_current_path) == 2);
}

TEST_CASE("GraphEditorView auto layout preserves positions and paints unnamed ports",
          "[vellum-graph][graph_editor]") {
    SignalGraph graph;
    const auto unnamed_input = graph.add_input_node(2, "");
    graph.add_output_node(2, "Output");

    GraphEditorView editor(graph);
    editor.set_node_position(unnamed_input, 100.0f, 20.0f);
    graph.add_gain_node("Gain");
    editor.auto_layout();

    RecordingCanvas canvas;
    editor.paint(canvas);

    REQUIRE(has_fill_rect(canvas, 100.0f, 20.0f, 160.0f, 80.0f));
    REQUIRE(has_fill_rect(canvas, 260.0f, 40.0f, 160.0f, 80.0f));
    REQUIRE(has_fill_rect(canvas, 480.0f, 40.0f, 160.0f, 80.0f));
    REQUIRE(has_text(canvas, "(unnamed)"));
    REQUIRE(has_text(canvas, "Output"));
    REQUIRE(has_text(canvas, "Gain"));
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 8);
}

TEST_CASE("GraphEditorView strokes feedback and midi edges in their own colours",
          "[vellum-graph][graph_editor]") {
    SignalGraph feedback_graph;
    const auto feedback_input = feedback_graph.add_input_node(1, "Input");
    const auto feedback_output = feedback_graph.add_output_node(1, "Output");
    REQUIRE(feedback_graph.connect_feedback(feedback_input, 0, feedback_output, 0));

    GraphEditorView feedback(feedback_graph);
    feedback.set_node_position(feedback_input, 10.0f, 10.0f);
    feedback.set_node_position(feedback_output, 240.0f, 10.0f);

    RecordingCanvas feedback_canvas;
    feedback.paint(feedback_canvas);
    const auto feedback_colors = edge_stroke_colors(feedback_canvas);
    REQUIRE(feedback_colors.size() == 1);
    REQUIRE(feedback_colors[0] == kFeedbackRed);

    SignalGraph midi_graph;
    const auto midi_in = midi_graph.add_midi_input_node("Keys");
    const auto midi_out = midi_graph.add_midi_output_node("Sink");
    REQUIRE(midi_graph.connect_midi(midi_in, midi_out));

    GraphEditorView midi(midi_graph);
    midi.set_node_position(midi_in, 10.0f, 10.0f);
    midi.set_node_position(midi_out, 240.0f, 10.0f);

    RecordingCanvas midi_canvas;
    midi.paint(midi_canvas);
    const auto midi_colors = edge_stroke_colors(midi_canvas);
    REQUIRE(midi_colors.size() == 1);
    REQUIRE(midi_colors[0] == kMidiGreen);
}

TEST_CASE("GraphEditorView drag connects output to input and reverse",
          "[vellum-graph][graph_editor]") {
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
    REQUIRE_FALSE(forward.is_connecting());

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
          "[vellum-graph][graph_editor]") {
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
          "[vellum-graph][graph_editor]") {
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
    REQUIRE(feedback_graph.connections()[0].source_node == feedback_input);

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
    REQUIRE(midi_graph.connections()[0].source_node == midi_in);
}

TEST_CASE("GraphEditorView selection follows clicks and a miss clears it",
          "[vellum-graph][graph_editor]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");

    GraphEditorView editor(graph);
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);

    REQUIRE(editor.selected_node() == 0);
    editor.on_mouse_down({60.0f, 60.0f});
    REQUIRE(editor.selected_node() == input);

    RecordingCanvas selected_canvas;
    editor.paint(selected_canvas);
    const auto outlines = outline_stroke_colors(selected_canvas);
    REQUIRE(outlines.size() == 2);
    REQUIRE(std::count(outlines.begin(), outlines.end(), kSelectedYellow) == 1);

    editor.on_mouse_down({900.0f, 900.0f});
    REQUIRE(editor.selected_node() == 0);
    editor.on_mouse_drag({1000.0f, 1000.0f});
    editor.on_mouse_up({1000.0f, 1000.0f});
    REQUIRE(graph.connections().empty());

    RecordingCanvas cleared_canvas;
    editor.paint(cleared_canvas);
    const auto cleared = outline_stroke_colors(cleared_canvas);
    REQUIRE(std::count(cleared.begin(), cleared.end(), kSelectedYellow) == 0);
}

TEST_CASE("GraphEditorView paints unnamed nodes and clears the drag ghost after a miss",
          "[vellum-graph][graph_editor]") {
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
    REQUIRE(dragging.count(DrawCommand::Type::stroke_current_path) == 1);

    editor.on_mouse_up({900.0f, 900.0f});
    REQUIRE(graph.connections().empty());
    REQUIRE_FALSE(editor.is_connecting());

    RecordingCanvas released;
    editor.paint(released);
    REQUIRE(released.count(DrawCommand::Type::cubic_to) == 0);
}

TEST_CASE("GraphEditorView keeps Alt through Vellum's drag routing",
          "[vellum-graph][graph_editor][routing]") {
    // Real dispatch: the press, the drag steps and the release arrive through
    // the same verbs a platform host calls, so the modifier must reach the
    // release the way a held key does, not through a hand-fed event.
    SignalGraph graph;
    const auto from = graph.add_midi_input_node("Keys");
    const auto to = graph.add_midi_output_node("Sink");
    GraphEditorView editor(graph);
    editor.set_bounds({0, 0, 480, 140});
    editor.set_node_position(from, 10.0f, 10.0f);
    editor.set_node_position(to, 240.0f, 10.0f);

    ::vellum::view::View::SimulatedPointer alt;
    alt.modifiers = kModAlt;
    editor.simulate_drag(output_port(10.0f), input_port(240.0f), 6, alt);
    REQUIRE(graph.connections().size() == 1);
    REQUIRE(graph.connections()[0].midi);
}

TEST_CASE("GraphEditorView audio drag through Vellum routing connects and honours Shift",
          "[vellum-graph][graph_editor][routing]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");
    GraphEditorView editor(graph);
    editor.set_bounds({0, 0, 480, 140});
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);

    ::vellum::view::View::SimulatedPointer plain;
    editor.simulate_drag(output_port(10.0f), input_port(240.0f), 6, plain);
    REQUIRE(graph.connections().size() == 1);
    REQUIRE_FALSE(graph.connections()[0].feedback);

    SignalGraph shifted_graph;
    const auto shifted_in = shifted_graph.add_input_node(1, "Input");
    const auto shifted_out = shifted_graph.add_output_node(1, "Output");
    GraphEditorView shifted(shifted_graph);
    shifted.set_bounds({0, 0, 480, 140});
    shifted.set_node_position(shifted_in, 10.0f, 10.0f);
    shifted.set_node_position(shifted_out, 240.0f, 10.0f);

    ::vellum::view::View::SimulatedPointer shift;
    shift.modifiers = kModShift;
    shifted.simulate_drag(output_port(10.0f), input_port(240.0f), 6, shift);
    REQUIRE(shifted_graph.connections().size() == 1);
    REQUIRE(shifted_graph.connections()[0].feedback);
}

TEST_CASE("GraphEditorView reports a double click on a node and only then",
          "[vellum-graph][graph_editor]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");
    GraphEditorView editor(graph);
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);

    std::vector<NodeId> activated;
    editor.set_node_activated_handler([&](NodeId id) { activated.push_back(id); });

    auto click = [&](Point at, int count, MousePhase phase) {
        MouseEvent event;
        event.position = at;
        event.click_count = count;
        event.phase = phase;
        editor.on_mouse_event(event);
    };

    click({60.0f, 60.0f}, 1, MousePhase::press);
    REQUIRE(activated.empty());

    click({900.0f, 900.0f}, 2, MousePhase::press);
    REQUIRE(activated.empty());

    click({300.0f, 60.0f}, 2, MousePhase::press);
    REQUIRE(activated == std::vector<NodeId>{output});

    // The release of the same double click does not report it again.
    click({300.0f, 60.0f}, 2, MousePhase::release);
    REQUIRE(activated.size() == 1);
}

TEST_CASE("GraphEditorView offscreen render draws edges, outlines and the selection",
          "[vellum-graph][graph_editor][raster]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(input, 0, output, 0));

    GraphEditorView editor(graph);
    editor.set_bounds({0, 0, 420, 120});
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);

    // The edge runs level between the two ports at y = 34, from x = 176 (the
    // output port's right edge) to x = 234 (the input port's left edge).
    const auto idle = render(editor, 420, 120);
    REQUIRE(idle.width == 420);
    const auto edge_px = idle.at(205, 34);
    INFO("edge pixel " << edge_px.r << "," << edge_px.g << "," << edge_px.b);
    CHECK(is_blue(edge_px));
    // Plain background above the edge.
    CHECK_FALSE(is_blue(idle.at(205, 20)));
    // The node body, then its outline, which is lighter than the fill.
    const auto body = idle.at(60, 80);
    CHECK(body.r < 70);
    const auto outline_px = idle.at(60, 10);
    INFO("outline pixel " << outline_px.r << "," << outline_px.g << "," << outline_px.b);
    CHECK(outline_px.r > body.r + 20);
    CHECK_FALSE(is_yellow(outline_px));
    maybe_write_png(editor, 420, 120, "graph_editor_audio_edge.png");

    editor.on_mouse_down({60.0f, 80.0f});
    const auto selected = render(editor, 420, 120);
    const auto selected_px = selected.at(60, 10);
    INFO("selected outline pixel " << selected_px.r << "," << selected_px.g << ","
                                   << selected_px.b);
    CHECK(is_yellow(selected_px));
    maybe_write_png(editor, 420, 120, "graph_editor_selected.png");
}

TEST_CASE("GraphEditorView offscreen render colours each edge kind and draws the drag ghost",
          "[vellum-graph][graph_editor][raster]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect_feedback(input, 0, output, 0));
    const auto keys = graph.add_midi_input_node("Keys");
    const auto sink = graph.add_midi_output_node("Sink");
    REQUIRE(graph.connect_midi(keys, sink));

    GraphEditorView editor(graph);
    editor.set_bounds({0, 0, 420, 260});
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 10.0f);
    editor.set_node_position(keys, 10.0f, 140.0f);
    editor.set_node_position(sink, 240.0f, 140.0f);

    const auto frame = render(editor, 420, 260);
    const auto feedback_px = frame.at(205, 34);
    const auto midi_px = frame.at(205, frame.height > 164 ? 164 : 34);
    INFO("feedback " << feedback_px.r << "," << feedback_px.g << "," << feedback_px.b);
    INFO("midi " << midi_px.r << "," << midi_px.g << "," << midi_px.b);
    CHECK(is_red(feedback_px));
    CHECK(is_green(midi_px));
    maybe_write_png(editor, 420, 260, "graph_editor_edge_kinds.png");

    // A port drag draws a grey ghost under the cursor.
    SignalGraph ghost_graph;
    const auto gi = ghost_graph.add_input_node(1, "Input");
    ghost_graph.add_output_node(1, "Output");
    GraphEditorView ghost(ghost_graph);
    ghost.set_bounds({0, 0, 420, 120});
    ghost.set_node_position(gi, 10.0f, 10.0f);
    ghost.on_mouse_down(output_port(10.0f));
    ghost.on_mouse_drag({300.0f, 34.0f});
    const auto ghost_frame = render(ghost, 420, 120);
    const auto ghost_px = ghost_frame.at(240, 34);
    INFO("ghost " << ghost_px.r << "," << ghost_px.g << "," << ghost_px.b << "," << ghost_px.a);
    CHECK(ghost_px.a > 40);
    CHECK(std::abs(ghost_px.r - ghost_px.g) < 25);
    CHECK(std::abs(ghost_px.g - ghost_px.b) < 25);
    maybe_write_png(ghost, 420, 120, "graph_editor_drag_ghost.png");
}

TEST_CASE("GraphEditorView offscreen render bends an edge between nodes at different heights",
          "[vellum-graph][graph_editor][raster]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Input");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(input, 0, output, 0));

    GraphEditorView editor(graph);
    editor.set_bounds({0, 0, 420, 140});
    editor.set_node_position(input, 10.0f, 10.0f);
    editor.set_node_position(output, 240.0f, 70.0f);

    // The curve leaves (170, 34) and enters (240, 94) with 40 px horizontal
    // handles; at t = 0.5 it passes through (205, 64).
    const auto frame = render(editor, 420, 140);
    const auto mid = frame.at(205, 64);
    INFO("curve midpoint " << mid.r << "," << mid.g << "," << mid.b);
    CHECK(is_blue(mid));
    // A straight line between the ports would cross (205, 64) too, so also
    // require the curve to be off the straight line near its start.
    CHECK_FALSE(is_blue(frame.at(190, 50)));
    maybe_write_png(editor, 420, 140, "graph_editor_curved_edge.png");
}

// ---- EditorAttachment: HostedEditor to WindowHost contract ----
// The RAII wrapper walks a PluginSlot's typed HostedEditor onto
// WindowHost::attach_native_child_view. These cases share this translation unit
// with the graph editor cases because both need the plug-in host headers.

namespace {

// Minimal slot fake that hands out a HostedEditor with a non-null handle.
class FakeSlot : public pulp::host::PluginSlot {
  public:
    bool created = false;
    bool destroyed = false;
    int fake_view = 0;
    uint32_t init_w = 800;
    uint32_t init_h = 600;
    bool resizable = true;

    const pulp::host::PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return true;
    }
    void release() override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int) override {}
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        return {};
    }
    float get_parameter(uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool b) override {
        bypassed_ = b;
    }
    bool is_bypassed() const override {
        return bypassed_;
    }
    std::vector<uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<uint8_t>&) override {
        return true;
    }
    bool has_editor() const override {
        return true;
    }

    // The legacy `void*` virtuals are pure in the base class, so we must
    // override them: but we route to the typed API below and use the legacy
    // entry points only via the deprecation-aware default impl.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    void* create_editor_view() override {
        return &fake_view;
    }
    void destroy_editor_view() override {}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    std::unique_ptr<HostedEditor> create_hosted_editor(void* /*parent*/) override {
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = &fake_view;
        ed->width = init_w;
        ed->height = init_h;
        ed->resizable = resizable;
        created = true;
        return ed;
    }
    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        if (ed)
            destroyed = true;
    }

    void set_editor_resize_request_handler(EditorResizeRequestHandler h) override {
        resize_handler = std::move(h);
    }
    /// Stand in for the plug-in asking its host for a different size.
    bool plugin_requests_resize(uint32_t w, uint32_t h) {
        return resize_handler && resize_handler(w, h);
    }
    EditorResizeRequestHandler resize_handler;

    int latency_samples() const override {
        return 0;
    }
    int tail_samples() const override {
        return 0;
    }

  private:
    pulp::host::PluginInfo info_;
    bool bypassed_ = false;
};

// Slot whose typed editor returns a null handle: should be treated as
// "no editor available" by EditorAttachment::create.
class NullHandleSlot : public FakeSlot {
  public:
    int destroy_typed_calls = 0;

    std::unique_ptr<HostedEditor> create_hosted_editor(void* /*parent*/) override {
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = nullptr; // simulate "couldn't open editor"
        ed->width = 200;
        ed->height = 100;
        created = true;
        return ed;
    }
    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        if (ed)
            ++destroy_typed_calls;
    }
};

// Fake host that records attach/detach/bounds traffic. It overrides only the
// pure virtuals plus the native-child seams EditorAttachment touches.

class FakeWindowHost : public ::vellum::view::WindowHost {
  public:
    // Pure virtuals on WindowHost we don't care about for this test.
    void show() override {}
    void hide() override {}
    bool is_visible() const override {
        return true;
    }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    // The two seams EditorAttachment touches.
    bool attach_native_child_view(void* v, float x, float y, float w, float h) override {
        attached_handle = v;
        attached_x = x;
        attached_y = y;
        attached_w = w;
        attached_h = h;
        attach_calls++;
        return attach_should_succeed;
    }
    bool set_native_child_view_bounds(void* v, float x, float y, float w, float h) override {
        bounds_handle = v;
        bounds_x = x;
        bounds_y = y;
        bounds_w = w;
        bounds_h = h;
        bounds_calls++;
        return true;
    }
    void detach_native_child_view(void* v) override {
        detached_handle = v;
        detach_calls++;
    }
    void* native_window_handle() const override {
        return reinterpret_cast<void*>(0xCAFEBABE);
    }

    int attach_calls = 0;
    int detach_calls = 0;
    int bounds_calls = 0;
    bool attach_should_succeed = true;
    void* attached_handle = nullptr;
    void* bounds_handle = nullptr;
    void* detached_handle = nullptr;
    float attached_x = -1, attached_y = -1, attached_w = -1, attached_h = -1;
    float bounds_x = -1, bounds_y = -1, bounds_w = -1, bounds_h = -1;
};

} // namespace

TEST_CASE("EditorAttachment attaches via the native-windows seam",
          "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host, 12.0f, 34.0f);
    REQUIRE(att);
    REQUIRE(slot->created);
    REQUIRE(host.attach_calls == 1);
    REQUIRE(host.attached_handle == &slot->fake_view);
    REQUIRE(host.attached_x == 12.0f);
    REQUIRE(host.attached_y == 34.0f);
    REQUIRE(host.attached_w == 800.0f);
    REQUIRE(host.attached_h == 600.0f);
    REQUIRE(att->is_attached());
    REQUIRE(att->editor()->resizable);
}

TEST_CASE("EditorAttachment destructor detaches and destroys", "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    {
        auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
        REQUIRE(att);
    }
    REQUIRE(host.detach_calls == 1);
    REQUIRE(host.detached_handle == &slot->fake_view);
    REQUIRE(slot->destroyed);
}

TEST_CASE("EditorAttachment release() is idempotent", "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
    REQUIRE(att);
    att->release();
    REQUIRE(host.detach_calls == 1);
    REQUIRE_FALSE(att->is_attached());
    // Second release is a no-op.
    att->release();
    REQUIRE(host.detach_calls == 1);
}

TEST_CASE("EditorAttachment set_bounds forwards to the host", "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
    REQUIRE(att);
    REQUIRE(att->set_bounds(5.0f, 6.0f, 200.0f, 150.0f));
    REQUIRE(host.bounds_calls == 1);
    REQUIRE(host.bounds_handle == &slot->fake_view);
    REQUIRE(host.bounds_w == 200.0f);
    REQUIRE(host.bounds_h == 150.0f);
    REQUIRE(att->width() == 200.0f);
    REQUIRE(att->height() == 150.0f);
    att->release();
    REQUIRE_FALSE(att->set_bounds(0, 0, 1, 1)); // post-release reject
}

TEST_CASE("EditorAttachment::create returns null when attach fails",
          "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    host.attach_should_succeed = false;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
    REQUIRE_FALSE(att);
    // The slot still gets destroy_hosted_editor() so it doesn't leak.
    REQUIRE(slot->destroyed);
    REQUIRE(host.attach_calls == 1);
    REQUIRE(host.detach_calls == 0);
}

TEST_CASE("EditorAttachment::create returns null on null handles",
          "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<NullHandleSlot>();
    FakeWindowHost host;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
    REQUIRE_FALSE(att);
    // The typed destroy is the path that ran (handle was null so we never
    // touched the legacy entry points).
    REQUIRE(slot->destroy_typed_calls == 1);
    REQUIRE(host.attach_calls == 0);
}

TEST_CASE("EditorAttachment::create rejects null inputs", "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    REQUIRE_FALSE(pulp::vellum_graph::EditorAttachment::create(nullptr, &host));
    REQUIRE_FALSE(pulp::vellum_graph::EditorAttachment::create(slot, nullptr));
}

TEST_CASE("EditorAttachment is move-constructible without re-detaching",
          "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    auto first = pulp::vellum_graph::EditorAttachment::create(slot, &host);
    REQUIRE(first);
    pulp::vellum_graph::EditorAttachment moved(std::move(*first));
    REQUIRE(moved.is_attached());
    // first is now hollow; releasing it must not double-detach.
    first->release();
    REQUIRE(host.detach_calls == 0);
    // Letting moved go out of scope does the single real detach.
    moved.release();
    REQUIRE(host.detach_calls == 1);
}

TEST_CASE("EditorAttachment keeps its slot alive after the graph drops it",
          "[host][hosted-editor][migration]") {
    // The signal graph owns nodes as shared_ptr<PluginSlot> and can remove a
    // node while its editor window is still open. When EditorAttachment held a
    // raw slot pointer, release() (run from the destructor) then dereferenced a
    // freed slot: a use-after-free. Co-owning the slot via shared_ptr fixes it:
    // the slot outlives the attachment, never the other way around.
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    std::weak_ptr<FakeSlot> weak = slot;

    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
    REQUIRE(att);
    REQUIRE(slot->created);

    // A test-only observer so we can inspect the slot after release() without
    // reading a freed object.
    std::shared_ptr<FakeSlot> observer = slot;

    // "Graph removes the node": drop the graph's reference while still attached.
    slot.reset();
    REQUIRE(weak.lock()); // the attachment (+observer) still own it: not freed.

    // release() must reach through the co-owned slot to destroy the editor.
    // With a raw slot_ this line was the use-after-free.
    att->release();
    REQUIRE(host.detach_calls == 1);
    REQUIRE(observer->destroyed); // the editor really was destroyed.

    // The attachment already dropped its slot ownership inside release(); once
    // the test observer goes too, the slot is freed cleanly (no leak).
    observer.reset();
    REQUIRE_FALSE(weak.lock());
}

TEST_CASE("EditorAttachment re-bounds the window on a plug-in resize request",
          "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host, 20.0f, 40.0f);
    REQUIRE(att);
    REQUIRE(host.attached_w == 800.0f);

    // A plug-in that sizes itself must move the child view the host placed,
    // not just the slot's private container.
    REQUIRE(slot->plugin_requests_resize(1024, 768));
    REQUIRE(host.bounds_calls == 1);
    REQUIRE(host.bounds_handle == &slot->fake_view);
    REQUIRE(host.bounds_w == 1024.0f);
    REQUIRE(host.bounds_h == 768.0f);
    // The origin the host chose is preserved: only the size was requested.
    REQUIRE(host.bounds_x == 20.0f);
    REQUIRE(host.bounds_y == 40.0f);
    REQUIRE(att->width() == 1024.0f);
    REQUIRE(att->height() == 768.0f);
}

TEST_CASE("EditorAttachment stops honoring resize requests after release",
          "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
    REQUIRE(att);

    att->release();
    // The handler captured the attachment; the slot outlives it, so a stale
    // handler here would be a use-after-free the moment the plug-in asked.
    REQUIRE_FALSE(slot->plugin_requests_resize(640, 480));
    REQUIRE(host.bounds_calls == 0);
    // The handler itself is gone, not merely inert.
    REQUIRE_FALSE(slot->resize_handler);
}

TEST_CASE("EditorAttachment destruction clears the slot's resize handler",
          "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    {
        auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host);
        REQUIRE(att);
        REQUIRE(slot->resize_handler);
    }
    // The slot outlives the attachment; a handler left behind would call into a
    // destroyed object.
    REQUIRE_FALSE(slot->resize_handler);
}

TEST_CASE("EditorAttachment moves its resize handler with it", "[host][hosted-editor][migration]") {
    auto slot = std::make_shared<FakeSlot>();
    FakeWindowHost host;
    auto att = pulp::vellum_graph::EditorAttachment::create(slot, &host, 5.0f, 6.0f);
    REQUIRE(att);

    // The handler captures `this`; a move must re-point it at the new owner.
    pulp::vellum_graph::EditorAttachment moved(std::move(*att));
    REQUIRE(slot->plugin_requests_resize(320, 240));
    REQUIRE(host.bounds_calls == 1);
    REQUIRE(host.bounds_w == 320.0f);
    REQUIRE(moved.width() == 320.0f);
    // The moved-from object no longer owns anything to resize.
    REQUIRE_FALSE(att->is_attached());
}

// ---- PluginManagerPanel ----
// Drives the widget against an in-memory InMemoryPluginManagerModel so the
// cases are deterministic and have no filesystem or subprocess dependencies:
// three buckets, search filter, rescan button, context menu, blacklist
// persistence round trip, drag-add and accessibility labels.

namespace plugin_manager_tests {
using namespace pulp::vellum_graph;
using pulp::host::PluginFormat;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::TextInputEvent;

PluginManagerRow make_row(PluginFormat fmt, std::string name, std::string path,
                          std::int64_t scan_time = 1700000000) {
    PluginManagerRow r;
    r.format = fmt;
    r.name = std::move(name);
    r.path = std::move(path);
    r.last_scan_unix = scan_time;
    return r;
}

void populate_model(InMemoryPluginManagerModel& m) {
    m.scanned_rows = {
        make_row(PluginFormat::CLAP, "Solid Bass", "/plugins/SolidBass.clap"),
        make_row(PluginFormat::VST3, "Glacier Reverb", "/plugins/Glacier.vst3"),
        make_row(PluginFormat::AudioUnit, "Airy Chorus",
                 "/Library/Audio/Plug-Ins/Components/Airy.component"),
    };
    PluginManagerRow failed = make_row(PluginFormat::CLAP, "Broken Synth", "/plugins/Broken.clap");
    failed.reason = "SIGSEGV during scan";
    m.failed_rows = {failed};
}

bool has_text_command(const ::vellum::canvas::RecordingCanvas& canvas, const std::string& needle) {
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == ::vellum::canvas::DrawCommand::Type::fill_text &&
            cmd.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST_CASE("PluginManagerPanel populates each bucket from the model", "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 3);
    REQUIRE(panel.visible_count(PluginManagerBucket::failed) == 1);
    REQUIRE(panel.visible_count(PluginManagerBucket::blacklisted) == 0);

    const auto& scanned = panel.rows(PluginManagerBucket::scanned);
    REQUIRE(scanned[0].name == "Solid Bass");
    REQUIRE(scanned[1].name == "Glacier Reverb");

    const auto& failed = panel.rows(PluginManagerBucket::failed);
    REQUIRE(failed[0].reason == "SIGSEGV during scan");
}

TEST_CASE("PluginManagerPanel search filter narrows all buckets", "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);

    panel.set_filter("reverb");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 1);
    REQUIRE(panel.visible_count(PluginManagerBucket::failed) == 0);

    panel.set_filter("broken");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 0);
    REQUIRE(panel.visible_count(PluginManagerBucket::failed) == 1);

    // Filter matches against the path too.
    panel.set_filter("/Library/Audio");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 1);

    panel.set_filter("");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 3);
}

TEST_CASE("PluginManagerPanel rescan button drives the model non-blockingly", "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);

    REQUIRE(model.rescan_count == 0);
    panel.trigger_rescan();
    REQUIRE(model.rescan_count == 1);

    model.scanning = true;
    model.progress = 0.42f;
    REQUIRE(panel.model().is_scanning());
    REQUIRE(panel.model().progress_fraction() == 0.42f);
}

TEST_CASE("PluginManagerPanel context menu drives per-row actions", "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);

    const std::string bass_path = "/plugins/SolidBass.clap";
    panel.open_context_menu(PluginManagerBucket::scanned, bass_path);
    REQUIRE(panel.context_menu_path() == bass_path);

    SECTION("rescan just this plugin") {
        panel.activate_context_item(PluginManagerPanel::ContextItem::rescan_this);
        REQUIRE(model.single_rescan_count == 1);
        REQUIRE(model.last_single_rescan_path == bass_path);
    }

    SECTION("toggle blacklist moves the row to the blacklisted bucket") {
        REQUIRE_FALSE(model.is_blacklisted(bass_path));
        panel.activate_context_item(PluginManagerPanel::ContextItem::toggle_blacklist);
        REQUIRE(model.is_blacklisted(bass_path));
        REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 2);
        REQUIRE(panel.visible_count(PluginManagerBucket::blacklisted) == 1);

        // The label reflects the current state so the second click
        // re-exposes the plugin.
        panel.open_context_menu(PluginManagerBucket::blacklisted, bass_path);
        REQUIRE(panel.context_menu_label(PluginManagerPanel::ContextItem::toggle_blacklist) ==
                "Remove from blacklist");
        panel.activate_context_item(PluginManagerPanel::ContextItem::toggle_blacklist);
        REQUIRE_FALSE(model.is_blacklisted(bass_path));
    }

    SECTION("reveal in file manager routes through the model") {
        panel.activate_context_item(PluginManagerPanel::ContextItem::reveal_in_file_manager);
        REQUIRE(model.reveal_count == 1);
        REQUIRE(model.last_reveal_path == bass_path);
    }
}

TEST_CASE("PluginManagerPanel mouse events select rows and close empty menus", "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    PluginManagerBucket selected_bucket = PluginManagerBucket::blacklisted;
    std::string selected_path;
    panel.on_row_selected = [&](PluginManagerBucket bucket, const PluginManagerRow& row) {
        selected_bucket = bucket;
        selected_path = row.path;
    };

    ::vellum::view::MouseEvent select;
    select.is_down = true;
    select.button = ::vellum::view::MouseButton::left;
    select.position = {12.0f, 63.0f};
    panel.on_mouse_event(select);

    REQUIRE(selected_bucket == PluginManagerBucket::scanned);
    REQUIRE(selected_path == "/plugins/SolidBass.clap");

    ::vellum::view::MouseEvent open_menu;
    open_menu.is_down = true;
    open_menu.button = ::vellum::view::MouseButton::right;
    open_menu.position = {320.0f, 63.0f};
    panel.on_mouse_event(open_menu);

    REQUIRE(panel.context_menu_bucket() == PluginManagerBucket::failed);
    REQUIRE(panel.context_menu_path() == "/plugins/Broken.clap");

    ::vellum::view::MouseEvent off_row;
    off_row.is_down = true;
    off_row.button = ::vellum::view::MouseButton::left;
    off_row.position = {320.0f, 180.0f};
    panel.on_mouse_event(off_row);

    REQUIRE(panel.context_menu_path().empty());
}

TEST_CASE("PluginManagerPanel paint records format labels and clamps progress",
          "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    model.scanned_rows.push_back(make_row(PluginFormat::AudioUnitV3, "", "/plugins/Mobile.auv3"));
    model.scanned_rows.push_back(make_row(PluginFormat::LV2, "Room LV2", "/plugins/Room.lv2"));
    model.scanning = true;
    model.progress = 1.75f;

    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 240});

    ::vellum::canvas::RecordingCanvas canvas;
    panel.paint(canvas);

    REQUIRE(has_text_command(canvas, "Scanning"));
    REQUIRE(has_text_command(canvas, "AUv3  /plugins/Mobile.auv3"));
    REQUIRE(has_text_command(canvas, "LV2  Room LV2"));

    int progress_rects = 0;
    bool saw_clamped_fill = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type != ::vellum::canvas::DrawCommand::Type::fill_rect)
            continue;
        if (cmd.f[0] == 676.0f && cmd.f[1] == 10.0f && cmd.f[3] == 8.0f) {
            ++progress_rects;
            REQUIRE(cmd.f[2] <= 110.0f);
            saw_clamped_fill = saw_clamped_fill || cmd.f[2] == 110.0f;
        }
    }
    REQUIRE(progress_rects == 2);
    REQUIRE(saw_clamped_fill);
}

TEST_CASE("PluginManagerPanel per-format search paths round-trip via the model",
          "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    PluginManagerPanel panel(model);

    panel.add_search_path(PluginFormat::VST3, "/Library/Audio/Plug-Ins/VST3");
    panel.add_search_path(PluginFormat::VST3, "/Users/me/VST3");
    panel.add_search_path(PluginFormat::CLAP, "/Library/Audio/Plug-Ins/CLAP");

    REQUIRE(panel.search_paths(PluginFormat::VST3).size() == 2);
    REQUIRE(panel.search_paths(PluginFormat::CLAP).size() == 1);

    // De-dup on repeat add.
    panel.add_search_path(PluginFormat::VST3, "/Library/Audio/Plug-Ins/VST3");
    REQUIRE(panel.search_paths(PluginFormat::VST3).size() == 2);

    panel.remove_search_path(PluginFormat::VST3, "/Users/me/VST3");
    REQUIRE(panel.search_paths(PluginFormat::VST3).size() == 1);
    REQUIRE(panel.search_paths(PluginFormat::VST3)[0] == "/Library/Audio/Plug-Ins/VST3");
}

TEST_CASE("PluginManagerPanel blacklist persists via ScanBlacklist "
          "text round-trip",
          "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);

    panel.toggle_blacklist("/plugins/SolidBass.clap");
    panel.toggle_blacklist("/plugins/Broken.clap");

    // Serialize the underlying blacklist to the on-disk format.
    const std::string serialized = model.blacklist.to_text();
    REQUIRE(!serialized.empty());

    // Fresh model (imagine a new session) can re-hydrate from the same
    // text blob.
    InMemoryPluginManagerModel next;
    REQUIRE(next.blacklist.from_text(serialized));

    PluginManagerPanel next_panel(next);
    REQUIRE(next.is_blacklisted("/plugins/SolidBass.clap"));
    REQUIRE(next.is_blacklisted("/plugins/Broken.clap"));
    REQUIRE(next_panel.visible_count(PluginManagerBucket::blacklisted) == 2);
}

TEST_CASE("PluginManagerPanel exposes screen-reader labels on each column", "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);

    REQUIRE(panel.access_role() == ::vellum::view::View::AccessRole::group);
    REQUIRE(panel.access_label() == "Plugin manager");

    REQUIRE(panel.column_access_label(PluginManagerBucket::scanned) ==
            "Scanned plugins — 3 entries");
    REQUIRE(panel.column_access_label(PluginManagerBucket::failed) == "Failed plugins — 1 entry");
    REQUIRE(panel.column_access_label(PluginManagerBucket::blacklisted) ==
            "Blacklisted plugins — 0 entries");
}

TEST_CASE("PluginManagerPanel text input appends to the filter "
          "and backspace removes characters",
          "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);

    TextInputEvent te;
    te.text = "gla";
    panel.on_text_input(te);
    REQUIRE(panel.filter() == "gla");
    REQUIRE(panel.visible_count(PluginManagerBucket::scanned) == 1);

    KeyEvent ke;
    ke.key = KeyCode::backspace;
    ke.is_down = true;
    REQUIRE(panel.on_key_event(ke));
    REQUIRE(panel.filter() == "gl");
}

// ── Drag-add into SignalGraph ────────────────────────────────────────────

TEST_CASE("PluginManagerRow round-trips into a PluginInfo for the graph", "[plugin-manager]") {
    PluginManagerRow row;
    row.format = PluginFormat::CLAP;
    row.name = "Solid Bass";
    row.path = "/plugins/SolidBass.clap";
    row.manufacturer = "Pulp Labs";
    row.version = "1.2.3";
    row.unique_id = "com.pulp.solidbass";
    row.num_inputs = 0;
    row.num_outputs = 2;
    row.is_instrument = true;
    row.is_effect = false;

    const auto info = row.to_plugin_info();
    REQUIRE(info.name == "Solid Bass");
    REQUIRE(info.path == "/plugins/SolidBass.clap");
    REQUIRE(info.manufacturer == "Pulp Labs");
    REQUIRE(info.unique_id == "com.pulp.solidbass");
    REQUIRE(info.num_inputs == 0);
    REQUIRE(info.num_outputs == 2);
    REQUIRE(info.is_instrument);
    REQUIRE_FALSE(info.is_effect);
    REQUIRE(info.format == PluginFormat::CLAP);
}

TEST_CASE("PluginManagerPanel fires on_row_drag_start when a scanned row "
          "is dragged past the threshold",
          "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    int drag_start_count = 0;
    int drag_end_count = 0;
    PluginManagerRow captured_row;
    PluginManagerBucket captured_bucket = PluginManagerBucket::failed;
    panel.on_row_drag_start = [&](PluginManagerBucket b, const PluginManagerRow& r,
                                  ::vellum::view::Point) {
        ++drag_start_count;
        captured_row = r;
        captured_bucket = b;
    };
    panel.on_row_drag_end = [&](PluginManagerBucket, const PluginManagerRow&,
                                ::vellum::view::Point) { ++drag_end_count; };

    // First scanned row sits just below the toolbar+header in column 0.
    const ::vellum::view::Point press{40.0f, 28.0f + 4.0f + 20.0f + 4.0f};
    panel.on_mouse_down(press);
    // Below threshold: must not start a drag.
    panel.on_mouse_drag({press.x + 1.0f, press.y + 1.0f});
    REQUIRE(drag_start_count == 0);
    // Past threshold (default 4 px): drag starts.
    panel.on_mouse_drag({press.x + 50.0f, press.y + 10.0f});
    panel.on_mouse_up({press.x + 50.0f, press.y + 10.0f});
    REQUIRE(drag_start_count == 1);
    REQUIRE(drag_end_count == 1);
    REQUIRE(captured_bucket == PluginManagerBucket::scanned);
    REQUIRE(captured_row.path == "/plugins/SolidBass.clap");
}

TEST_CASE("PluginManagerPanel suppresses drag-add for failed and "
          "blacklisted bucket rows",
          "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    int drag_starts = 0;
    panel.on_row_drag_start = [&](PluginManagerBucket, const PluginManagerRow&,
                                  ::vellum::view::Point) { ++drag_starts; };

    // simulate_row_drag bypasses the geometry hit-test so the test
    // doesn't have to know column coordinates.
    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::failed, "/plugins/Broken.clap"));
    REQUIRE(drag_starts == 0);

    // Scanned row IS allowed.
    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned, "/plugins/SolidBass.clap"));
    REQUIRE(drag_starts == 1);
}

TEST_CASE("Drag-emit from panel adds an unresolved plugin node to a "
          "SignalGraph",
          "[plugin-manager][host][signal_graph]") {
    InMemoryPluginManagerModel model;
    PluginManagerRow row;
    row.format = PluginFormat::CLAP;
    row.name = "Solid Bass";
    row.path = "/nonexistent/plugins/SolidBass.clap";
    row.unique_id = "com.pulp.solidbass";
    row.num_inputs = 0;
    row.num_outputs = 2;
    row.is_instrument = true;
    row.is_effect = false;
    model.scanned_rows = {row};

    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    pulp::host::SignalGraph graph;
    pulp::host::NodeId last_added = 0;
    bool last_loaded = true;
    panel.on_row_drag_start = [&](PluginManagerBucket bucket, const PluginManagerRow& r,
                                  ::vellum::view::Point /*drop_pos*/) {
        REQUIRE(bucket == PluginManagerBucket::scanned);
        last_added = pulp::host::add_plugin_node_from_drop(graph, r.to_plugin_info(), &last_loaded);
    };

    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned, row.path));

    REQUIRE(last_added != 0);
    // The path is bogus → PluginSlot::load returns null → unresolved fallback.
    REQUIRE_FALSE(last_loaded);

    const auto* n = graph.node(last_added);
    REQUIRE(n != nullptr);
    REQUIRE(n->type == pulp::host::NodeType::Plugin);
    REQUIRE(n->plugin == nullptr);            // unresolved
    REQUIRE(n->plugin_info.path == row.path); // identity preserved
    REQUIRE(n->plugin_info.unique_id == "com.pulp.solidbass");
    REQUIRE(n->num_input_ports == 0);
    REQUIRE(n->num_output_ports == 2);

    // Graph is now non-empty: drag-add really did integrate.
    REQUIRE(graph.nodes().size() == 1);
}

TEST_CASE("Multiple drag-emits stack additional graph nodes "
          "without disturbing earlier ones",
          "[plugin-manager][host][signal_graph]") {
    InMemoryPluginManagerModel model;
    PluginManagerRow a;
    a.format = PluginFormat::VST3;
    a.name = "Glacier";
    a.path = "/p/Glacier.vst3";
    a.unique_id = "glacier";
    a.num_inputs = 2;
    a.num_outputs = 2;
    PluginManagerRow b;
    b.format = PluginFormat::CLAP;
    b.name = "Solid";
    b.path = "/p/Solid.clap";
    b.unique_id = "solid";
    b.num_inputs = 2;
    b.num_outputs = 2;
    model.scanned_rows = {a, b};

    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});
    pulp::host::SignalGraph graph;

    std::vector<pulp::host::NodeId> ids;
    panel.on_row_drag_start = [&](PluginManagerBucket, const PluginManagerRow& r,
                                  ::vellum::view::Point) {
        ids.push_back(pulp::host::add_plugin_node_from_drop(graph, r.to_plugin_info(), nullptr));
    };

    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned, a.path));
    REQUIRE(panel.simulate_row_drag(PluginManagerBucket::scanned, b.path));

    REQUIRE(ids.size() == 2);
    REQUIRE(ids[0] != ids[1]);
    REQUIRE(graph.nodes().size() == 2);
    REQUIRE(graph.node(ids[0])->plugin_info.unique_id == "glacier");
    REQUIRE(graph.node(ids[1])->plugin_info.unique_id == "solid");
}

TEST_CASE("PluginManagerPanel labels a built-in device row", "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    model.scanned_rows = {make_row(PluginFormat::BuiltIn, "Pulp Gain", "builtin:gain")};
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 240});

    ::vellum::canvas::RecordingCanvas canvas;
    panel.paint(canvas);
    REQUIRE(has_text_command(canvas, "Built-in  Pulp Gain"));
    REQUIRE_FALSE(has_text_command(canvas, "?  Pulp Gain"));
}

TEST_CASE("PluginManagerPanel hit-tests the row that paint drew under the pointer",
          "[plugin-manager]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 400});

    // Rows paint from y = 28 (toolbar) + 4 + 20 (header) + 2 (gap) = 54, each
    // row_height() tall. The last pixel of row 0 and the first of row 1 are the
    // boundary a click must respect.
    const float first_row_top = 54.0f;
    const float rh = panel.row_height();
    std::string picked;
    panel.on_row_selected = [&](PluginManagerBucket, const PluginManagerRow& r) {
        picked = r.path;
    };

    auto click = [&](float y) {
        ::vellum::view::MouseEvent e;
        e.is_down = true;
        e.button = ::vellum::view::MouseButton::left;
        e.position = {12.0f, y};
        picked.clear();
        panel.on_mouse_event(e);
    };

    click(first_row_top + rh - 1.0f);
    REQUIRE(picked == "/plugins/SolidBass.clap");
    click(first_row_top + rh + 0.5f);
    REQUIRE(picked == "/plugins/Glacier.vst3");
    // The gap between the header and the first row belongs to no row.
    click(first_row_top - 1.0f);
    REQUIRE(picked.empty());
}

TEST_CASE("PluginManagerPanel offscreen render paints the toolbar, button, progress and columns",
          "[plugin-manager][render]") {
    InMemoryPluginManagerModel model;
    populate_model(model);
    model.scanning = true;
    model.progress = 0.5f;
    PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 900, 240});

    std::uint32_t w = 0, h = 0;
    const auto px = ::vellum::view::render_to_rgba(panel, 900, 240, 1.0f, &w, &h);
    REQUIRE(w == 900);
    REQUIRE(h == 240);
    REQUIRE(px.size() == static_cast<std::size_t>(w) * h * 4);
    auto at = [&](std::uint32_t x, std::uint32_t y) {
        const auto o = (static_cast<std::size_t>(y) * w + x) * 4;
        return std::array<int, 3>{px[o], px[o + 1], px[o + 2]};
    };
    auto near = [](std::array<int, 3> p, std::array<int, 3> want) {
        return std::abs(p[0] - want[0]) <= 3 && std::abs(p[1] - want[1]) <= 3 &&
               std::abs(p[2] - want[2]) <= 3;
    };

    // Toolbar surface 0x1a1a2e, Rescan button accent 0xe94560.
    CHECK(near(at(300, 6), {0x1a, 0x1a, 0x2e}));
    CHECK(near(at(799, 6), {0xe9, 0x45, 0x60}));
    // Progress bar: the left half is the green fill, the right half the track.
    CHECK(near(at(690, 14), {0x4a, 0xde, 0x80}));
    CHECK(near(at(770, 14), {0x0f, 0x34, 0x60}));
    // Below the last row each column shows its list background 0x0d1b33.
    CHECK(near(at(100, 200), {0x0d, 0x1b, 0x33}));
    CHECK(near(at(450, 200), {0x0d, 0x1b, 0x33}));
    CHECK(near(at(700, 200), {0x0d, 0x1b, 0x33}));
    // Column headers use the surface colour.
    CHECK(near(at(100, 40), {0x1a, 0x1a, 0x2e}));
    // The panel background fills the 4 px gutter between columns' edge and panel.
    CHECK(near(at(2, 100), {0x16, 0x21, 0x3e}));
    if (const char* dir = std::getenv("PULP_VELLUM_GRAPH_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(panel, 900, 240,
                                             std::string(dir) + "/plugin_manager_panel.png", 2.0f));
}

} // namespace plugin_manager_tests
