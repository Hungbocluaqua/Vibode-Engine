#include "rtv/PhysicalCamera.h"

#include <algorithm>
#include <cmath>

namespace rtv {

PhysicalCamera::PhysicalCamera(PhysicalCameraSettings settings)
    : settings_(settings) {
    setSettings(settings);
}

void PhysicalCamera::setSettings(PhysicalCameraSettings settings) {
    settings.aperture = std::max(settings.aperture, 0.01f);
    settings.shutterSeconds = std::max(settings.shutterSeconds, 1.0e-6f);
    settings.iso = std::max(settings.iso, 1.0f);
    settings.exposureCompensation = std::clamp(settings.exposureCompensation, -10.0f, 10.0f);
    settings.apertureRadius = std::clamp(
        std::isfinite(settings.apertureRadius) ? settings.apertureRadius : 0.0f,
        0.0f,
        1.0f);
    settings.focusDistance = std::clamp(
        std::isfinite(settings.focusDistance) ? settings.focusDistance : 10.0f,
        0.01f,
        10000.0f);
    settings.bladeCount = settings.bladeCount == 0u ? 0u : std::clamp(settings.bladeCount, 3u, 16u);
    settings.bokehRotation = std::isfinite(settings.bokehRotation) ? settings.bokehRotation : 0.0f;
    settings_ = settings;
}

float PhysicalCamera::ev100() const {
    const float aperture = std::max(settings_.aperture, 0.01f);
    const float shutter = std::max(settings_.shutterSeconds, 1.0e-6f);
    const float iso = std::max(settings_.iso, 1.0f);
    return std::log2((aperture * aperture) / shutter * (100.0f / iso)) - settings_.exposureCompensation;
}

float PhysicalCamera::exposureMultiplier() const {
    return 1.0f / std::max(std::pow(2.0f, ev100()) * 1.2f, 1.0e-6f);
}

} // namespace rtv
