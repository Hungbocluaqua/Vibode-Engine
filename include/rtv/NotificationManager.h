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

enum class NotificationAction {
    None,
    OpenLog,
    OpenContent,
    OpenRenderSettings,
    OpenProjectManager,
    OpenOutputFolder,
};

struct Notification {
    std::string message;
    NotificationType type = NotificationType::Info;
    NotificationAction action = NotificationAction::OpenLog;
    std::string actionLabel = "Open Log";
    float ageSeconds = 0.0f;
    float durationSeconds = 4.0f;
};

class NotificationManager {
public:
    void setLogSink(EditorLog* log) { log_ = log; }
    void notify(std::string message, NotificationType type = NotificationType::Info, float durationSeconds = 4.0f);
    void notify(
        std::string message,
        NotificationType type,
        NotificationAction action,
        std::string actionLabel,
        float durationSeconds = 4.0f);
    void update(float deltaSeconds);
    void draw();
    [[nodiscard]] NotificationAction consumeRequestedAction();

private:
    std::deque<Notification> active_;
    EditorLog* log_ = nullptr;
    NotificationAction requestedAction_ = NotificationAction::None;
    int maxVisible_ = 4;
};

} // namespace rtv
