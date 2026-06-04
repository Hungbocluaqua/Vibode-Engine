#pragma once

#include <glm/glm.hpp>

#include <cmath>

namespace rtv {

inline glm::vec3 editorLinkedScaleFromReference(glm::vec3 reference, glm::vec3 edited) {
    constexpr float epsilon = 1.0e-5f;
    float selectedRatio = 1.0f;
    float selectedDelta = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(reference[axis]) <= epsilon) {
            continue;
        }
        const float ratio = edited[axis] / reference[axis];
        const float delta = std::abs(ratio - 1.0f);
        if (delta >= selectedDelta) {
            selectedDelta = delta;
            selectedRatio = ratio;
        }
    }
    if (selectedDelta <= epsilon) {
        return edited;
    }
    return reference * selectedRatio;
}

} // namespace rtv
