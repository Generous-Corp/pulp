#pragma once

#include <pulp/view/view.hpp>

#include <memory>
#include <string>

namespace pulp::examples {

std::unique_ptr<view::View> build_elysium_rust_provider_ui(std::string* failure_reason = nullptr);

}  // namespace pulp::examples
