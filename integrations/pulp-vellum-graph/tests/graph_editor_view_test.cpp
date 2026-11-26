#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph.hpp>
#include <pulp/vellum_graph/graph_editor_view.hpp>
#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/input_events.hpp>
#include <vellum/view/screenshot.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
