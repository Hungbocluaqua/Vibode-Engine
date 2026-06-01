#pragma once

#include "rtv/NonCopyable.h"

#include <glm/glm.hpp>

struct GLFWwindow;

namespace rtv {

class PathTracerRenderer;

class CameraController final : private NonCopyable {
public:
    CameraController();

    bool update(GLFWwindow* window, float deltaSeconds, PathTracerRenderer& renderer, bool allowMouseCapture, bool allowKeyboardMove);
    void releaseMouse(GLFWwindow* window);
    void reset(PathTracerRenderer& renderer);
    void setPose(glm::vec3 position, glm::vec3 forward, PathTracerRenderer& renderer);
    void setMoveSpeed(float speed) { moveSpeed_ = speed; }

    [[nodiscard]] bool mouseCaptured() const { return mouseCaptured_; }
    [[nodiscard]] bool mouseCaptureMoved() const { return mouseCaptureMoved_; }
    [[nodiscard]] float releasedMouseCaptureDurationSeconds() const { return releasedMouseCaptureDurationSeconds_; }
    [[nodiscard]] bool releasedMouseCaptureMoved() const { return releasedMouseCaptureMoved_; }
    [[nodiscard]] glm::vec3 position() const { return position_; }
    [[nodiscard]] glm::vec3 direction() const { return forward(); }
    [[nodiscard]] float yawRadians() const { return yawRadians_; }
    [[nodiscard]] float pitchRadians() const { return pitchRadians_; }
    [[nodiscard]] float moveSpeed() const { return moveSpeed_; }

private:
    void captureMouse(GLFWwindow* window);
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 right() const;

    glm::vec3 position_{0.0f, 0.0f, 3.9f};
    float yawRadians_ = -1.57079632679f;
    float pitchRadians_ = 0.0f;
    float moveSpeed_ = 2.4f;
    float fastMoveSpeed_ = 7.5f;
    bool mouseCaptured_ = false;
    float mouseCaptureDurationSeconds_ = 0.0f;
    float releasedMouseCaptureDurationSeconds_ = -1.0f;
    bool mouseCaptureMoved_ = false;
    bool releasedMouseCaptureMoved_ = false;
    bool firstMouseSample_ = true;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
};

} // namespace rtv
