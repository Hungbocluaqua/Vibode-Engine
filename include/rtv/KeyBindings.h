#pragma once

#include <string>
#include <vector>

namespace rtv {

struct KeyBinding {
    std::string action;
    std::string key;
    std::string category;
    std::string description;
};

[[nodiscard]] const std::vector<KeyBinding>& allKeyBindings();

} // namespace rtv
