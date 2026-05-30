#include "rtv/ValidationSceneSuite.h"

#include <array>
#include <fstream>

namespace rtv {

namespace {

constexpr std::array<ValidationSceneDescriptor, 5> kValidationScenes{{
    {
        "Transform Stress",
        "AS refit",
        "Nested transforms, moving camera, and TLAS refit correctness.",
        "scenes/validation/transform_stress.rtlevel",
        "pending-reference-transform-stress",
    },
    {
        "Material Grid",
        "Material/update routing",
        "Roughness, metallic, emissive, and texture-slot debug view coverage.",
        "scenes/validation/material_grid.rtlevel",
        "pending-reference-material-grid",
    },
    {
        "Temporal Stability",
        "Velocity/history",
        "Camera motion, denoiser history, and future TAA reprojection validation.",
        "scenes/validation/temporal_stability.rtlevel",
        "pending-reference-temporal-stability",
    },
    {
        "MIS Validation",
        "Lighting",
        "Direct-light sampling, emissive contribution, PDF, and MIS debug views.",
        "scenes/validation/mis_validation.rtlevel",
        "pending-reference-mis-validation",
    },
    {
        "Atmosphere Validation",
        "Atmosphere",
        "Horizon, sun elevation, exposure transitions, and aerial perspective bring-up.",
        "scenes/validation/atmosphere_validation.rtlevel",
        "pending-reference-atmosphere-validation",
    },
}};

} // namespace

std::span<const ValidationSceneDescriptor> validationSceneSuite() {
    return kValidationScenes;
}

std::filesystem::path validationScenePath(const ValidationSceneDescriptor& scene) {
    return std::filesystem::current_path() / std::filesystem::path(scene.relativePath);
}

std::vector<ValidationSceneResult> validateAllScenes() {
    std::vector<ValidationSceneResult> results;
    results.reserve(kValidationScenes.size());
    for (const auto& scene : kValidationScenes) {
        ValidationSceneResult result;
        result.scene = scene;
        const auto path = validationScenePath(scene);
        if (!std::filesystem::exists(path)) {
            result.status = ValidationSceneStatus::FileMissing;
            result.message = "File not found: " + path.string();
        } else {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) {
                result.status = ValidationSceneStatus::FileMissing;
                result.message = "Cannot open: " + path.string();
            } else {
                result.status = ValidationSceneStatus::FileReadable;
                result.message = "OK (" + std::to_string(std::filesystem::file_size(path)) + " bytes)";
            }
        }
        results.push_back(result);
    }
    return results;
}

const char* validationStatusName(ValidationSceneStatus status) {
    switch (status) {
        case ValidationSceneStatus::NotChecked: return "Not Checked";
        case ValidationSceneStatus::FileMissing: return "File Missing";
        case ValidationSceneStatus::FileReadable: return "Readable";
        case ValidationSceneStatus::ChecksumMatch: return "Checksum OK";
        case ValidationSceneStatus::ChecksumMismatch: return "Checksum Mismatch";
    }
    return "Unknown";
}

} // namespace rtv
