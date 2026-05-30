#pragma once

#include <cstdint>

namespace rtv {

struct PhysicalCameraSettings {
    float aperture = 16.0f;
    float shutterSeconds = 1.0f / 125.0f;
    float iso = 100.0f;
    float exposureCompensation = 0.0f;
    float apertureRadius = 0.0f;
    float focusDistance = 10.0f;
    uint32_t bladeCount = 0;
    float bokehRotation = 0.0f;
};

class PhysicalCamera {
public:
    explicit PhysicalCamera(PhysicalCameraSettings settings = {});

    [[nodiscard]] const PhysicalCameraSettings& settings() const { return settings_; }
    void setSettings(PhysicalCameraSettings settings);

    [[nodiscard]] float ev100() const;
    [[nodiscard]] float exposureMultiplier() const;

private:
    PhysicalCameraSettings settings_{};
};

} // namespace rtv
