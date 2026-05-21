#pragma once

#include <filesystem>
#include <span>
#include <string_view>

namespace rtv {

struct ValidationSceneDescriptor {
    std::string_view name;
    std::string_view category;
    std::string_view purpose;
    std::string_view relativePath;
    std::string_view referenceChecksum;
};

[[nodiscard]] std::span<const ValidationSceneDescriptor> validationSceneSuite();
[[nodiscard]] std::filesystem::path validationScenePath(const ValidationSceneDescriptor& scene);

} // namespace rtv
