#pragma once

#include <pulp/timeline/dawproject_import.hpp>

#include <pugixml.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::timeline::detail {

class DawProjectMediaSealer {
  public:
    DawProjectMediaSealer(DawProjectMediaResolver resolver, const DawProjectImportLimits& limits,
                          std::uint64_t& next_item_id);

    std::optional<DawProjectImportError> seal(const pugi::xml_node& audio, ItemId& asset_id,
                                              std::uint64_t& frame_count);

    std::vector<MediaAsset> take_assets();

  private:
    DawProjectMediaResolver resolver_;
    DawProjectImportLimits limits_;
    std::uint64_t& next_item_id_;
    std::unordered_map<std::string, std::string> hash_by_path_;
    std::unordered_map<std::string, ItemId> asset_by_hash_;
    std::vector<MediaAsset> assets_;
    std::size_t resolver_calls_ = 0;
    std::uint64_t resolved_bytes_ = 0;
};

} // namespace pulp::timeline::detail
