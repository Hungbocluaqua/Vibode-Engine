#include "rtv/CameraController.h"

#include "rtv/PathTracerRenderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {
constexpr float mouseSensitivity = 0.0025f;
constexpr float pitchLimit = 1.52f;
constexpr float maxCameraDeltaSeconds = 1.0f / 30.0f;

[[nodiscard]] bool keyDown(GLFWwindow* window, int key) {
    return glfwGetKey(window, key) == GLFW_PRESS;
}
} // namespace

CameraController::CameraController() = default;

bool CameraController::update(GLFWwindow* window, float deltaSeconds, PathTracerRenderer& renderer, bool allowMouseCapture, bool allowKeyboardMove) {
    deltaSeconds = std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : 0.0f, 0.0f, maxCameraDeltaSeconds);
    releasedMouseCaptureDurationSeconds_ = -1.0f;
    releasedMouseCaptureMoved_ = false;
    if (glfwGetWindowAttrib(window, GLFW_FOCUSED) != GLFW_TRUE) {
        if (mouseCaptured_) {
            releaseMouse(window);
        }
        return false;
    }

    const bool escapeDown = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    if (escapeDown && mouseCaptured_) {
        releaseMouse(window);
    }

    const bool rightMouseDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (mouseCaptured_ && !rightMouseDown) {
        releasedMouseCaptureDurationSeconds_ = mouseCaptureDurationSeconds_;
        releasedMouseCaptureMoved_ = mouseCaptureMoved_;
        releaseMouse(window);
    }
    if (allowMouseCapture && rightMouseDown && !escapeDown && !mouseCaptured_) {
        captureMouse(window);
    }

    bool changed = false;
    if (mouseCaptured_ && allowMouseCapture) {
        double mouseX = 0.0;
        double mouseY = 0.0;
        glfwGetCursorPos(window, &mouseX, &mouseY);
        if (firstMouseSample_) {
            lastMouseX_ = mouseX;
            lastMouseY_ = mouseY;
            firstMouseSample_ = false;
        }

        const double dx = mouseX - lastMouseX_;
        const double dy = mouseY - lastMouseY_;
        lastMouseX_ = mouseX;
        lastMouseY_ = mouseY;
        if (dx != 0.0 || dy != 0.0) {
            yawRadians_ += static_cast<float>(dx) * mouseSensitivity;
            pitchRadians_ = std::clamp(pitchRadians_ - static_cast<float>(dy) * mouseSensitivity, -pitchLimit, pitchLimit);
            mouseCaptureMoved_ = true;
            changed = true;
        }
    }

    glm::vec3 move{0.0f};
    if (mouseCaptured_ && allowKeyboardMove) {
        if (keyDown(window, GLFW_KEY_W)) { move += forward(); }
        if (keyDown(window, GLFW_KEY_S)) { move -= forward(); }
        if (keyDown(window, GLFW_KEY_D)) { move += right(); }
        if (keyDown(window, GLFW_KEY_A)) { move -= right(); }
        if (keyDown(window, GLFW_KEY_E) || keyDown(window, GLFW_KEY_SPACE)) { move += glm::vec3(0.0f, 1.0f, 0.0f); }
        if (keyDown(window, GLFW_KEY_Q) || keyDown(window, GLFW_KEY_LEFT_CONTROL)) { move -= glm::vec3(0.0f, 1.0f, 0.0f); }
    }

    if (glm::dot(move, move) > 0.0f) {
        const float speed = keyDown(window, GLFW_KEY_LEFT_SHIFT) ? fastMoveSpeed_ : moveSpeed_;
        position_ += glm::normalize(move) * speed * deltaSeconds;
        mouseCaptureMoved_ = true;
        changed = true;
    }

    if (mouseCaptured_) {
        mouseCaptureDurationSeconds_ += deltaSeconds;
    }

    if (changed) {
        renderer.setCameraPose(position_, forward());
    }
    return changed;
}

void CameraController::reset(PathTracerRenderer& renderer) {
    position_ = {0.0f, 0.0f, 3.9f};
    yawRadians_ = -1.57079632679f;
    pitchRadians_ = 0.0f;
    renderer.setCameraPose(position_, forward());
}

void CameraController::setPose(glm::vec3 position, glm::vec3 forwardDirection, PathTracerRenderer& renderer) {
    if (glm::dot(forwardDirection, forwardDirection) <= 0.0f) {
        return;
    }
    forwardDirection = glm::normalize(forwardDirection);
    position_ = position;
    pitchRadians_ = std::asin(std::clamp(forwardDirection.y, -1.0f, 1.0f));
    yawRadians_ = std::atan2(forwardDirection.z, forwardDirection.x);
    renderer.setCameraPose(position_, forward());
}

void CameraController::captureMouse(GLFWwindow* window) {
    mouseCaptured_ = true;
    mouseCaptureDurationSeconds_ = 0.0f;
    mouseCaptureMoved_ = false;
    firstMouseSample_ = true;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void CameraController::releaseMouse(GLFWwindow* window) {
    mouseCaptured_ = false;
    firstMouseSample_ = true;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

glm::vec3 CameraController::forward() const {
    const float cp = std::cos(pitchRadians_);
    return glm::normalize(glm::vec3(
        std::cos(yawRadians_) * cp,
        std::sin(pitchRadians_),
        std::sin(yawRadians_) * cp));
}

glm::vec3 CameraController::right() const {
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

} // namespace rtv
