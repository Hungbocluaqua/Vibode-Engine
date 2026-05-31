#include "rtv/NotificationManager.h"

#include "rtv/EditorLog.h"

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace rtv {

namespace {

ImU32 colorForType(NotificationType type) {
    switch (type) {
    case NotificationType::Success: return IM_COL32(74, 222, 128, 255);
    case NotificationType::Warning: return IM_COL32(250, 204, 21, 255);
    case NotificationType::Error: return IM_COL32(248, 113, 113, 255);
    case NotificationType::Info:
    default: return IM_COL32(96, 165, 250, 255);
    }
}

} // namespace

void NotificationManager::notify(std::string message, NotificationType type, float durationSeconds) {
    if (log_ != nullptr) {
        EditorLogCategory category = EditorLogCategory::Info;
        switch (type) {
        case NotificationType::Warning:
            category = EditorLogCategory::Warning;
            break;
        case NotificationType::Error:
            category = EditorLogCategory::Error;
            break;
        case NotificationType::Success:
        case NotificationType::Info:
            category = EditorLogCategory::Info;
            break;
        }
        log_->add(category, message);
    }
    if (!active_.empty() && active_.back().message == message && active_.back().type == type) {
        active_.back().ageSeconds = 0.0f;
        active_.back().durationSeconds = durationSeconds;
        return;
    }

    active_.push_back(Notification{
        .message = std::move(message),
        .type = type,
        .ageSeconds = 0.0f,
        .durationSeconds = std::max(durationSeconds, 0.5f),
    });
    while (active_.size() > 16) {
        active_.pop_front();
    }
}

void NotificationManager::update(float deltaSeconds) {
    for (Notification& notification : active_) {
        notification.ageSeconds += std::max(deltaSeconds, 0.0f);
    }
    while (!active_.empty() && active_.front().ageSeconds >= active_.front().durationSeconds) {
        active_.pop_front();
    }
}

void NotificationManager::draw() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || active_.empty()) {
        return;
    }

    const float width = 360.0f;
    const float padding = 12.0f;
    float y = viewport->WorkPos.y + padding;
    int visible = 0;
    for (auto it = active_.rbegin(); it != active_.rend() && visible < maxVisible_; ++it, ++visible) {
        const Notification& notification = *it;
        const float fadeIn = std::min(notification.ageSeconds / 0.15f, 1.0f);
        const float fadeOut = std::min((notification.durationSeconds - notification.ageSeconds) / 0.35f, 1.0f);
        const float alpha = std::clamp(std::min(fadeIn, fadeOut), 0.0f, 1.0f);
        ImGui::SetNextWindowBgAlpha(0.88f * alpha);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width - padding, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

        const std::string id = "##notification_" + std::to_string(visible);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove;
        if (ImGui::Begin(id.c_str(), nullptr, flags)) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 min = ImGui::GetWindowPos();
            const ImVec2 max = ImVec2(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight());
            drawList->AddRectFilled(min, ImVec2(min.x + 4.0f, max.y), colorForType(notification.type));
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::Indent(10.0f);
            ImGui::TextWrapped("%s", notification.message.c_str());
            ImGui::Unindent(10.0f);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }
        y += ImGui::GetWindowHeight() + 8.0f;
        ImGui::End();
    }
}

} // namespace rtv
