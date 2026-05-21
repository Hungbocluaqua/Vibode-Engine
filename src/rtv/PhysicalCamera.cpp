#include "rtv/PhysicalCamera.h"

#include <algorithm>
#include <cmath>

namespace rtv {

PhysicalCamera::PhysicalCamera(PhysicalCameraSettings settings)
    : settings_(settings) {}

void PhysicalCamera::setSettings(PhysicalCameraSettings settings) {
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
