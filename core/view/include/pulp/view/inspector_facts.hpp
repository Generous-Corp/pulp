#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

/// Lets a widget describe itself to the component inspector (ViewInspector)
/// without the inspector naming its class. A widget defined outside view-core
/// implements this, so view-core's inspector can show it while linking none of
/// that widget's code.
class InspectorFacts {
public:
    struct Fact {
        std::string key;         ///< JSON member name in ViewInspector::to_json
        std::string label;       ///< row label in ViewInspector::view_properties
        double value = 0.0;
        bool integral = false;   ///< serialized as an integer
        bool show_in_properties = false;
    };

    virtual ~InspectorFacts();
    virtual std::string_view inspector_type_name() const = 0;
    virtual void inspector_facts(std::vector<Fact>& out) const = 0;
};

} // namespace pulp::view
