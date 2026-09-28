// widget_bridge.hpp reaches about 200 translation units and widgets.hpp is one
// of the most frequently edited view headers. Keeping the widget classes out of
// widget_bridge.hpp's include closure means a widgets.hpp edit recompiles only
// the translation units that use a widget class. This TU includes nothing but
// widget_bridge.hpp, so a widget class that is complete here arrived through it.
#include <pulp/view/widget_bridge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace pulp::view {
class Knob;
class Label;
}  // namespace pulp::view

namespace {
template <typename T, typename = void>
struct is_complete : std::false_type {};
template <typename T>
struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

// Evaluated here, before anything later in this TU could complete the types.
constexpr bool kViewComplete = is_complete<pulp::view::View>::value;
constexpr bool kKnobComplete = is_complete<pulp::view::Knob>::value;
constexpr bool kLabelComplete = is_complete<pulp::view::Label>::value;
}  // namespace

TEST_CASE("widget_bridge.hpp does not pull in the widget classes", "[view][include-fanout]") {
    // Control: widget_bridge.hpp does include view.hpp, so the probe must see
    // a complete type when one is there.
    REQUIRE(kViewComplete);
    REQUIRE_FALSE(kKnobComplete);
    REQUIRE_FALSE(kLabelComplete);
}
