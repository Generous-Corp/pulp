#include <pulp/host/forge_catalog_json.hpp>

#include <choc/text/choc_JSON.h>

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace pulp::host {
namespace {

std::string json_string(std::string_view value) {
    return choc::json::getEscapedQuotedString(std::string(value));
}

const CustomNodeBakedParam&
require_baked_param(const ForgeCatalogExportNode& node,
                    const ForgeCatalogExportRealization& realization, state::ParamID id) {
    for (const auto& baked : realization.baked_params)
        if (baked.id == id)
            return baked;
    throw std::logic_error("Forge catalog serialization requires an audited descriptor/range "
                           "join for node '" +
                           std::string(node.descriptor.key) + "', parameter id " +
                           std::to_string(id) + ", realization '" + std::string(realization.mode) +
                           "'");
}

const ForgeRealization&
require_declared_realization(const ForgeCatalogExportNode& node, std::string_view mode) {
    for (const auto& declared : node.descriptor.realizations)
        if (declared.mode == mode)
            return declared;
    throw std::logic_error("Forge catalog serialization requires an audited realization join "
                           "for node '" +
                           std::string(node.descriptor.key) + "', mode '" + std::string(mode) +
                           "'");
}

void write_number(std::ostream& out, float value) {
    out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
}

} // namespace

std::string serialize_forge_catalog_json(const std::vector<ForgeCatalogExportNode>& nodes) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\n  \"schema\": \"pulp.forge-catalog.v1\",\n  \"nodes\": [";

    for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
        const auto& node = nodes[node_index];
        const auto& d = node.descriptor;
        out << (node_index == 0 ? "\n" : ",\n");
        out << "    {\n"
            << "      \"key\": " << json_string(d.key) << ",\n"
            << "      \"label\": " << json_string(d.label) << ",\n"
            << "      \"description\": " << json_string(d.description) << ",\n"
            << "      \"realization_axes\": [";

        for (std::size_t axis_index = 0; axis_index < d.axes.size(); ++axis_index) {
            const auto& axis = d.axes[axis_index];
            out << (axis_index == 0 ? "\n" : ",\n");
            out << "        {\"key\": " << json_string(axis.key)
                << ", \"label\": " << json_string(axis.label)
                << ", \"description\": " << json_string(axis.description) << ", \"values\": [";
            for (std::size_t value_index = 0; value_index < axis.values.size(); ++value_index) {
                const auto& value = axis.values[value_index];
                if (value_index != 0)
                    out << ", ";
                out << "{\"token\": " << json_string(value.token)
                    << ", \"label\": " << json_string(value.label) << ", \"value\": ";
                write_number(out, value.value);
                out << "}";
            }
            out << "]}";
        }
        if (!d.axes.empty())
            out << "\n      ";
        out << "],\n      \"realizations\": [";

        for (std::size_t realization_index = 0; realization_index < node.realizations.size();
             ++realization_index) {
            const auto& realization = node.realizations[realization_index];
            const auto& declared = require_declared_realization(node, realization.mode);
            if (realization_index != 0)
                out << ", ";
            out << "{\"mode\": " << json_string(realization.mode)
                << ", \"type_id\": " << json_string(realization.type_id)
                << ", \"settings\": {";
            for (std::size_t setting_index = 0; setting_index < declared.settings.size();
                 ++setting_index) {
                if (setting_index != 0)
                    out << ", ";
                const auto& setting = declared.settings[setting_index];
                out << json_string(setting.axis) << ": " << json_string(setting.value);
            }
            out << "}}";
        }
        out << "],\n      \"parameters\": [";

        for (std::size_t param_index = 0; param_index < d.params.size(); ++param_index) {
            const auto& param = d.params[param_index];
            out << (param_index == 0 ? "\n" : ",\n");
            out << "        {\"key\": " << json_string(param.key)
                << ", \"label\": " << json_string(param.label)
                << ", \"unit\": " << json_string(param.unit) << ", \"kind\": "
                << json_string(param.kind == ForgeParamKind::stepped ? "stepped"
                                                                     : "continuous")
                << ", \"curve\": "
                << json_string(param.curve == ForgeParamCurve::logarithmic ? "logarithmic"
                                                                           : "linear")
                << ", \"description\": " << json_string(param.description) << ", \"choices\": [";
            for (std::size_t choice_index = 0; choice_index < param.choices.size();
                 ++choice_index) {
                const auto& choice = param.choices[choice_index];
                if (choice_index != 0)
                    out << ", ";
                out << "{\"token\": " << json_string(choice.token)
                    << ", \"label\": " << json_string(choice.label) << ", \"value\": ";
                write_number(out, choice.value);
                out << ", \"realization_modes\": [";
                for (std::size_t mode_index = 0;
                     mode_index < choice.realization_modes.size(); ++mode_index) {
                    if (mode_index != 0)
                        out << ", ";
                    out << json_string(choice.realization_modes[mode_index]);
                }
                out << "]}";
            }
            out << "], \"contracts\": [";
            bool first_contract = true;
            for (const auto& realization : node.realizations) {
                if (!param_applies(param, realization.mode))
                    continue;
                const auto& baked = require_baked_param(node, realization, param.id);
                if (!first_contract)
                    out << ", ";
                first_contract = false;
                out << "{\"realization_mode\": " << json_string(realization.mode) << ", \"min\": ";
                write_number(out, baked.min_value);
                out << ", \"max\": ";
                write_number(out, baked.max_value);
                out << ", \"default\": ";
                write_number(out, baked.default_value);
                out << "}";
            }
            out << "], \"realization_modes\": [";
            for (std::size_t mode_index = 0; mode_index < param.realization_modes.size();
                 ++mode_index) {
                if (mode_index != 0)
                    out << ", ";
                out << json_string(param.realization_modes[mode_index]);
            }
            out << "]}";
        }
        if (!d.params.empty())
            out << "\n      ";
        out << "]\n    }";
    }

    if (!nodes.empty())
        out << "\n  ";
    out << "]\n}\n";
    return out.str();
}

} // namespace pulp::host
