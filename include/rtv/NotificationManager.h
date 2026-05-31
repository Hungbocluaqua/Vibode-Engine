#pragma once

#include <deque>
#include <string>

namespace rtv {

class EditorLog;

enum class NotificationType {
    Info,
    Warning,
    Error,
    Success,
};

struct Notification {
    std::string message;
    NotificationType type = NotificationType::Info;
    float ageSeconds = 0.0f;
    float durationSeconds = 4.0f;
};

class NotificationManager {
public:
    void setLogSink(EditorLog* log) { log_ = log; }
    void notify(std::string message, NotificationType type = NotificationType::Info, float durationSeconds = 4.0f);
    void update(float deltaSeconds);
    void draw();

private:
    std::deque<Notification> active_;
    EditorLog* log_ = nullptr;
    int maxVisible_ = 4;
};

} // namespace rtv
