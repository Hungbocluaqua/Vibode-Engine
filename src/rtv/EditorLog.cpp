#include "rtv/EditorLog.h"

#include <fstream>
#include <iomanip>
#include <utility>

namespace rtv {

const char* editorLogCategoryName(EditorLogCategory category) {
    switch (category) {
    case EditorLogCategory::Info: return "Info";
    case EditorLogCategory::Warning: return "Warning";
    case EditorLogCategory::Error: return "Error";
    case EditorLogCategory::Import: return "Import";
    case EditorLogCategory::Render: return "Render";
    case EditorLogCategory::Project: return "Project";
    case EditorLogCategory::Scene: return "Scene";
    case EditorLogCategory::Command: return "Command";
    }
    return "Info";
}

void EditorLog::add(EditorLogCategory category, std::string message) {
    if (!entries_.empty() && entries_.back().category == category && entries_.back().message == message) {
        entries_.back().timestamp = std::chrono::system_clock::now();
        return;
    }
    entries_.push_back(EditorLogEntry{
        .category = category,
        .message = std::move(message),
        .timestamp = std::chrono::system_clock::now(),
    });
    if (entries_.size() > 2048) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - 2048));
    }
}

void EditorLog::clear() {
    entries_.clear();
}

bool EditorLog::saveText(const std::filesystem::path& path) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }
    for (const EditorLogEntry& entry : entries_) {
        const std::time_t time = std::chrono::system_clock::to_time_t(entry.timestamp);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        out << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] ["
            << editorLogCategoryName(entry.category) << "] " << entry.message << '\n';
    }
    return true;
}

} // namespace rtv
