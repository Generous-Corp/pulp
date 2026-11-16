// The canonical scalar Control operations in control_math.hpp: pinned values
// for every edge class (NaN, infinity, signed zero, saturation, reversed
// bounds, reset, invalid configuration), bit-identical forwarding from the
// deprecated indexed_control.hpp spellings, and bit-identical agreement
// between the built-in graph Control nodes and the canonical functions.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/control_math.hpp>
#include <pulp/signal/indexed_control.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace pulp::host;
namespace sig = pulp::signal;

namespace {

// Bitwise equality: distinguishes -0 from +0 and never treats two NaNs as
// equal, so a canonical result that leaked a NaN fails as well.
template <typename T> bool same_bits(T a, T b) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    if (a != a || b != b)
        return false;
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

template <typename T> std::vector<T> edge_values() {
    using L = std::numeric_limits<T>;
    return {L::quiet_NaN(), L::infinity(), -L::infinity(),  T{0},
            -T{0},          T{1},          T{-1},           T{0.5},
            T{-3},          T{6},          L::max(),        L::lowest(),
            L::min(),       -L::min(),     L::denorm_min(), -L::denorm_min()};
}

template <typename T> void check_canonical_pins() {
    using L = std::numeric_limits<T>;
    const T nan = L::quiet_NaN();
    const T inf = L::infinity();
    const T pz = T{0};
    const T nz = -T{0};

    // Non-finite replacement keeps the sign of a real zero.
    CHECK(same_bits(sig::finite_control(nan), pz));
    CHECK(same_bits(sig::finite_control(inf), pz));
    CHECK(same_bits(sig::finite_control(-inf), pz));
    CHECK(same_bits(sig::finite_control(nz), nz));
    CHECK(same_bits(sig::finite_control(L::denorm_min()), L::denorm_min()));

    // min/max: non-finite operands become zero; equal operands return the first.
    CHECK(same_bits(sig::control_min(nz, pz), nz));
    CHECK(same_bits(sig::control_min(pz, nz), pz));
    CHECK(same_bits(sig::control_max(nz, pz), nz));
    CHECK(same_bits(sig::control_max(pz, nz), pz));
    CHECK(same_bits(sig::control_min(nan, T{-1}), T{-1}));
    CHECK(same_bits(sig::control_min(nan, T{1}), pz));
    CHECK(same_bits(sig::control_min(inf, T{2}), pz));
    CHECK(same_bits(sig::control_max(-inf, T{-2}), pz));
    CHECK(same_bits(sig::control_max(L::lowest(), L::max()), L::max()));

    // safe divide: zero denominator is +0, signs of zero quotients survive,
    // and overflow saturates at the finite endpoint.
    CHECK(same_bits(sig::safe_control_divide(T{1}, pz), pz));
    CHECK(same_bits(sig::safe_control_divide(T{1}, nz), pz));
    CHECK(same_bits(sig::safe_control_divide(T{-1}, nan), pz));
    CHECK(same_bits(sig::safe_control_divide(nan, nan), pz));
    CHECK(same_bits(sig::safe_control_divide(nz, T{1}), nz));
    CHECK(same_bits(sig::safe_control_divide(pz, T{-2}), nz));
    CHECK(same_bits(sig::safe_control_divide(inf, T{-2}), nz));
    CHECK(same_bits(sig::safe_control_divide(T{6}, T{3}), T{2}));
    CHECK(same_bits(sig::safe_control_divide(L::max(), T{0.5}), L::max()));
    CHECK(same_bits(sig::safe_control_divide(L::lowest(), T{0.5}), L::lowest()));
    CHECK(same_bits(sig::safe_control_divide(T{1}, L::denorm_min()), L::max()));
    CHECK(same_bits(sig::safe_control_divide(T{-1}, L::denorm_min()), L::lowest()));

    // clamp: non-finite value or bound becomes zero; reversed bounds reorder.
    CHECK(same_bits(sig::control_clamp(nan, T{-1}, T{1}), pz));
    CHECK(same_bits(sig::control_clamp(T{5}, T{1}, T{-1}), T{1}));
    CHECK(same_bits(sig::control_clamp(T{-5}, T{1}, T{-1}), T{-1}));
    CHECK(same_bits(sig::control_clamp(T{-5}, nan, T{1}), pz));
    CHECK(same_bits(sig::control_clamp(T{2}, -inf, inf), pz));
    CHECK(same_bits(sig::control_clamp(nz, T{-1}, T{1}), nz));

    // integrator step: bounded state, finite increment, saturating sum.
    CHECK(same_bits(sig::integrator_step(pz, inf, T{-1}, T{1}), pz));
    CHECK(same_bits(sig::integrator_step(T{0.25}, nan, T{-1}, T{1}), T{0.25}));
    CHECK(same_bits(sig::integrator_step(T{5}, pz, T{-1}, T{1}), T{1}));
    CHECK(same_bits(sig::integrator_step(pz, T{0.5}, T{1}, T{-1}), T{0.5}));
    CHECK(same_bits(sig::integrator_step(T{0.75}, T{4}, T{1}, T{-1}), T{1}));
    CHECK(same_bits(sig::integrator_step(L::max(), L::max(), L::lowest(), L::max()), L::max()));
    CHECK(same_bits(sig::integrator_step(L::lowest(), L::lowest(), L::lowest(), L::max()),
                    L::lowest()));
    CHECK(same_bits(sig::integrator_step(nz, nz, T{-1}, T{1}), nz));
    CHECK(same_bits(sig::integrator_step(nz, pz, T{-1}, T{1}), pz));
    CHECK(same_bits(sig::integrator_step(T{3}, T{1}, T{2}, T{2}), T{2}));
}

template <typename T> void check_integrator_lifecycle() {
    using L = std::numeric_limits<T>;
    const std::array<T, 8> increments{T{0.5}, L::infinity(), T{1},  L::quiet_NaN(),
                                      T{-4},  T{0.125},      -T{0}, L::max()};

    // An unconfigured integrator runs over [0, 1] from 0.
    sig::ControlIntegratorT<T> fresh;
    CHECK(same_bits(fresh.process(T{2}), T{1}));
    fresh.reset();
    CHECK(same_bits(fresh.value(), T{0}));

    sig::ControlIntegratorT<T> integrator;
    REQUIRE(integrator.configure(T{-1}, T{1}, T{0.25}));
    T state = T{0.25};
    for (const T increment : increments) {
        state = sig::integrator_step(state, increment, T{-1}, T{1});
        CHECK(same_bits(integrator.process(increment), state));
    }
    CHECK(same_bits(integrator.value(), T{1}));

    // Invalid configuration is refused and leaves range, state and reset
    // target untouched.
    CHECK_FALSE(integrator.configure(T{1}, T{-1}, T{0}));
    CHECK_FALSE(integrator.configure(L::quiet_NaN(), T{1}, T{0}));
    CHECK_FALSE(integrator.configure(T{-1}, L::infinity(), T{0}));
    CHECK_FALSE(integrator.configure(T{-1}, T{1}, L::infinity()));
    CHECK(same_bits(integrator.value(), T{1}));
    CHECK(same_bits(integrator.process(T{-0.5}), T{0.5}));
    integrator.reset();
    CHECK(same_bits(integrator.value(), T{0.25}));

    // An out-of-range initial value is clamped; a degenerate range holds.
    REQUIRE(integrator.configure(T{-1}, T{1}, T{5}));
    CHECK(same_bits(integrator.value(), T{1}));
    REQUIRE(integrator.configure(T{2}, T{2}, T{0}));
    CHECK(same_bits(integrator.process(T{-7}), T{2}));
    CHECK(same_bits(integrator.process(L::max()), T{2}));
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
template <typename T> void check_deprecated_spellings_forward() {
    const auto values = edge_values<T>();
    for (const T a : values) {
        INFO("a=" << a);
        CHECK(same_bits(sig::finite_control_sample(a), sig::finite_control(a)));
        for (const T b : values) {
            INFO("b=" << b);
            CHECK(same_bits(sig::control_minimum(a, b), sig::control_min(a, b)));
            CHECK(same_bits(sig::control_maximum(a, b), sig::control_max(a, b)));
            CHECK(same_bits(sig::control_safe_divide(a, b), sig::safe_control_divide(a, b)));
        }
    }
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

std::vector<float> run_binary(const CustomNodeType& type, const std::vector<float>& a,
                              const std::vector<float>& b) {
    std::vector<float> output(a.size(), -100.0f);
    const float* in_ptr[] = {a.data(), b.data()};
    float* out_ptr[] = {output.data()};
    pulp::audio::BufferView<const float> in(in_ptr, 2, a.size());
    pulp::audio::BufferView<float> out(out_ptr, 1, output.size());
    type.process(out, in, static_cast<int>(a.size()));
    return output;
}

std::vector<float> run_integrator_node(const std::vector<std::uint8_t>& state,
                                       const std::vector<float>& input) {
    const auto type = make_control_integrator_type();
    void* instance = type.create();
    REQUIRE(instance != nullptr);
    if (!state.empty())
        REQUIRE(type.load_state(instance, state));
    type.prepare(instance, 48000.0, static_cast<int>(input.size()));
    std::vector<float> output(input.size(), -100.0f);
    const float* in_ptr[] = {input.data()};
    float* out_ptr[] = {output.data()};
    pulp::audio::BufferView<const float> in(in_ptr, 1, input.size());
    pulp::audio::BufferView<float> out(out_ptr, 1, output.size());
    type.process_instance(instance, out, in, static_cast<int>(input.size()));
    type.destroy(instance);
    return output;
}

CustomNodeType make_audio_probe() {
    CustomNodeType type;
    type.type_id = "pulp.test.control-scalar-probe";
    type.num_input_ports = 2;
    type.num_output_ports = 1;
    type.input_ports = {{"control", PortKind::Control}, {"audio", PortKind::Audio}};
    type.output_ports = {{"audio", PortKind::Audio}};
    type.process = [](pulp::audio::BufferView<float>& out,
                      const pulp::audio::BufferView<const float>& in, int frames) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i] + in.channel_ptr(1)[i];
    };
    return type;
}

// Control index -> integrator node -> probe -> output, through the public
// graph. The probe adds silent audio, so the output is the node's value.
std::vector<float> render_integrator_graph(const std::array<float, 3>& configuration,
                                           const std::vector<float>& indices) {
    SignalGraph graph;
    const auto type = make_control_integrator_type();
    REQUIRE(graph.register_custom_node_type(type));
    REQUIRE(graph.register_custom_node_type(make_audio_probe()));
    const auto audio = graph.add_input_node(1);
    const auto index = graph.add_control_index_input_node();
    const auto node = graph.add_custom_node(type.type_id);
    const auto probe = graph.add_custom_node("pulp.test.control-scalar-probe");
    const auto sink = graph.add_output_node(1);
    REQUIRE(node != 0);
    REQUIRE(graph.connect(graph.node(index)->out("index"), graph.node(node)->in("value")));
    REQUIRE(graph.connect(graph.node(node)->out("value"), graph.node(probe)->in("control")));
    REQUIRE(graph.connect(graph.node(audio)->out("audio"), graph.node(probe)->in("audio")));
    REQUIRE(graph.connect(probe, 0, sink, 0));
    REQUIRE(graph.set_custom_node_state(node, make_control_scalar_state(configuration)));
    REQUIRE(graph.prepare(48000, static_cast<int>(indices.size())));
    std::vector<float> silence(indices.size(), 0.0f);
    std::vector<float> result(indices.size(), -100.0f);
    const float* in_ptr[] = {silence.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, indices.size());
    pulp::audio::BufferView<float> output(out_ptr, 1, indices.size());
    const auto position = graph.control_index_position();
    REQUIRE(position.bound);
    REQUIRE(
        graph.process_control_checked(output, input, {position.epoch, position.cursor, indices}) ==
        ControlProcessResult::Accepted);
    return result;
}

} // namespace

TEST_CASE("canonical scalar Control operations pin NaN, infinity, signed zero and saturation",
          "[signal][control-math]") {
    SECTION("float") {
        check_canonical_pins<float>();
    }
    SECTION("double") {
        check_canonical_pins<double>();
    }
}

TEST_CASE("ControlIntegratorT matches integrator_step through reset and invalid configuration",
          "[signal][control-math]") {
    SECTION("float") {
        check_integrator_lifecycle<float>();
    }
    SECTION("double") {
        check_integrator_lifecycle<double>();
    }
}

TEST_CASE("deprecated indexed spellings forward bit-identically to the canonical operations",
          "[signal][control-math][compat]") {
    SECTION("float") {
        check_deprecated_spellings_forward<float>();
    }
    SECTION("double") {
        check_deprecated_spellings_forward<double>();
    }
}

TEST_CASE("binary Control nodes agree bit-for-bit with the canonical scalar operations",
          "[graph][control-index][control-math]") {
    const auto values = edge_values<float>();
    std::vector<float> a;
    std::vector<float> b;
    for (const float x : values)
        for (const float y : values) {
            a.push_back(x);
            b.push_back(y);
        }
    struct Case {
        CustomNodeType type;
        float (*canonical)(float, float) noexcept;
    };
    const std::array<Case, 3> cases{
        {{make_control_min_type(), sig::control_min<float>},
         {make_control_max_type(), sig::control_max<float>},
         {make_control_safe_divide_type(), sig::safe_control_divide<float>}}};
    for (const auto& entry : cases) {
        INFO(entry.type.type_id);
        const auto rendered = run_binary(entry.type, a, b);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!same_bits(rendered[i], entry.canonical(a[i], b[i])))
                ++mismatches;
        CHECK(mismatches == 0);
    }
}

TEST_CASE("the integrator node agrees with the canonical integrator and refuses invalid state",
          "[graph][control-index][control-math]") {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> increments{0.5f,  inf,  1.0f, nan,     -4.0f, 0.125f,
                                        -0.0f, 0.0f, -inf, 1.0e30f, -0.3f, 0.3f};
    const std::array<float, 3> configuration{-1.0f, 1.0f, 0.25f};

    sig::ControlIntegrator reference;
    REQUIRE(reference.configure(configuration[0], configuration[1], configuration[2]));
    std::vector<float> expected;
    for (const float increment : increments)
        expected.push_back(reference.process(increment));

    const auto rendered = run_integrator_node(make_control_scalar_state(configuration), increments);
    REQUIRE(rendered.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("frame " << i);
        CHECK(same_bits(rendered[i], expected[i]));
    }

    // Prepare resets the node to its configured initial value, exactly as a
    // reset of the canonical integrator does.
    reference.reset();
    CHECK(same_bits(run_integrator_node(make_control_scalar_state(configuration), {0.0f}).front(),
                    reference.process(0.0f)));

    // The graph path publishes only finite indices; the node still reproduces
    // the canonical saturation sequence through the public graph.
    const std::vector<float> finite_increments{0.5f, 0.5f, 0.5f, -3.0f, 0.25f, 1.0e30f};
    reference.reset();
    std::vector<float> finite_expected;
    for (const float increment : finite_increments)
        finite_expected.push_back(reference.process(increment));
    CHECK(render_integrator_graph(configuration, finite_increments) == finite_expected);

    // Reversed and non-finite configurations are refused, as configure() does.
    const auto type = make_control_integrator_type();
    void* instance = type.create();
    const std::array<float, 3> reversed{1.0f, -1.0f, 0.0f};
    CHECK_FALSE(type.load_state(instance, make_control_scalar_state(reversed)));
    const std::array<float, 3> in_range{-2.0f, 2.0f, 9.0f};
    CHECK(type.load_state(instance, make_control_scalar_state(in_range)));
    type.destroy(instance);
}
