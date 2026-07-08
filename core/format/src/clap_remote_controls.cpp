#include "clap_remote_controls.hpp"

#include <pulp/format/clap_adapter.hpp>
#include <pulp/runtime/system.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::format::clap_adapter {

namespace {

const state::ParamGroup* find_group(std::span<const state::ParamGroup> groups,
                                    int group_id) {
    const auto it = std::find_if(groups.begin(), groups.end(),
        [group_id](const state::ParamGroup& group) { return group.id == group_id; });
    return it == groups.end() ? nullptr : &*it;
}

bool contains_group_id(const std::vector<int>& ids, int group_id) {
    return std::find(ids.begin(), ids.end(), group_id) != ids.end();
}

std::string fallback_group_name(int group_id) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "Group %d", group_id);
    return std::string(buffer);
}

std::string page_name_for_chunk(std::string_view base_name,
                                std::size_t page_index,
                                std::size_t page_count) {
    if (page_count <= 1) return std::string(base_name);
    return std::string(base_name) + " " + std::to_string(page_index + 1);
}

struct RemoteControlsPageSpec {
    std::string section_name;
    std::string page_name;
    std::array<clap_id, CLAP_REMOTE_CONTROLS_COUNT> param_ids{};
};

void append_remote_control_pages(std::vector<RemoteControlsPageSpec>& pages,
                                 std::string_view section_name,
                                 std::string_view page_name,
                                 const std::vector<clap_id>& param_ids) {
    if (param_ids.empty()) return;
    constexpr std::size_t kPageSize = CLAP_REMOTE_CONTROLS_COUNT;
    const std::size_t page_count = (param_ids.size() + kPageSize - 1) / kPageSize;
    for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
        RemoteControlsPageSpec page;
        page.section_name = section_name.empty() ? "Main" : std::string(section_name);
        page.page_name = page_name_for_chunk(
            page_name.empty() ? std::string_view("Main") : page_name,
            page_index,
            page_count);
        page.param_ids.fill(CLAP_INVALID_ID);
        const std::size_t start = page_index * kPageSize;
        const std::size_t end = std::min(param_ids.size(), start + kPageSize);
        for (std::size_t i = start; i < end; ++i) {
            page.param_ids[i - start] = param_ids[i];
        }
        pages.push_back(std::move(page));
    }
}

std::vector<RemoteControlsPageSpec> build_remote_control_pages(
    const PulpClapPlugin& self) {
    std::vector<RemoteControlsPageSpec> pages;
    const auto params = self.store.all_params();
    if (params.empty()) return pages;

    const auto groups = self.store.all_groups();

    std::vector<clap_id> ungrouped;
    for (const auto& param : params) {
        if (param.group_id == 0) ungrouped.push_back(param.id);
    }
    append_remote_control_pages(pages, "Main", "Main", ungrouped);

    std::vector<int> ordered_group_ids;
    ordered_group_ids.reserve(groups.size());
    for (const auto& group : groups) {
        if (group.id != 0 && !contains_group_id(ordered_group_ids, group.id)) {
            ordered_group_ids.push_back(group.id);
        }
    }
    for (const auto& param : params) {
        if (param.group_id != 0 &&
            !contains_group_id(ordered_group_ids, param.group_id)) {
            ordered_group_ids.push_back(param.group_id);
        }
    }

    std::vector<clap_id> grouped;
    for (int group_id : ordered_group_ids) {
        grouped.clear();
        for (const auto& param : params) {
            if (param.group_id == group_id) grouped.push_back(param.id);
        }
        if (grouped.empty()) continue;

        const auto* group = find_group(groups, group_id);
        const auto* parent = group ? find_group(groups, group->parent_id) : nullptr;
        const std::string fallback_name = fallback_group_name(group_id);
        const std::string page_name = group ? group->name : fallback_name;
        const std::string section_name =
            parent ? parent->name : (group ? group->name : fallback_name);
        append_remote_control_pages(pages, section_name, page_name, grouped);
    }

    return pages;
}

PulpClapPlugin* get_self(const clap_plugin_t* plugin) {
    if (!plugin) return nullptr;
    return static_cast<PulpClapPlugin*>(plugin->plugin_data);
}

uint32_t remote_controls_count(const clap_plugin_t* plugin) {
    auto* self = get_self(plugin);
    if (!self) return 0;
    return static_cast<uint32_t>(build_remote_control_pages(*self).size());
}

bool remote_controls_get(const clap_plugin_t* plugin,
                         uint32_t page_index,
                         clap_remote_controls_page_t* page) {
    if (!page) return false;
    auto* self = get_self(plugin);
    if (!self) return false;

    const auto pages = build_remote_control_pages(*self);
    if (page_index >= pages.size()) return false;

    const auto& src = pages[page_index];
    std::memset(page, 0, sizeof(*page));
    runtime::copy_c_string(page->section_name, src.section_name);
    runtime::copy_c_string(page->page_name, src.page_name);
    page->page_id = static_cast<clap_id>(page_index + 1);
    page->is_for_preset = false;
    for (std::size_t i = 0; i < src.param_ids.size(); ++i) {
        page->param_ids[i] = src.param_ids[i];
    }
    return true;
}

const clap_plugin_remote_controls_t s_remote_controls = {
    .count = remote_controls_count,
    .get = remote_controls_get,
};

} // namespace

const clap_plugin_remote_controls_t* remote_controls_extension() {
    return &s_remote_controls;
}

} // namespace pulp::format::clap_adapter
