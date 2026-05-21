#include "rtv/ValidationSceneSuite.h"

#include <array>

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

} // namespace rtv
