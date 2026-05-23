#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtv {

struct ValidationSceneDescriptor {
    std::string_view name;
    std::string_view category;
    std::string_view purpose;
    std::string_view relativePath;
    std::string_view referenceChecksum;
};

enum class ValidationSceneStatus {
    NotChecked,
    FileMissing,
    FileReadable,
    ChecksumMatch,
    ChecksumMismatch,
};

struct ValidationSceneResult {
    ValidationSceneDescriptor scene;
    ValidationSceneStatus status = ValidationSceneStatus::NotChecked;
    std::string message;
};

[[nodiscard]] std::span<const ValidationSceneDescriptor> validationSceneSuite();
[[nodiscard]] std::filesystem::path validationScenePath(const ValidationSceneDescriptor& scene);
[[nodiscard]] std::vector<ValidationSceneResult> validateAllScenes();
[[nodiscard]] const char* validationStatusName(ValidationSceneStatus status);

} // namespace rtv
