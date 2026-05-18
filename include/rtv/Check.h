#pragma once

#include <Volk/volk.h>

#include <stdexcept>
#include <string>

namespace rtv {

inline void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

} // namespace rtv
