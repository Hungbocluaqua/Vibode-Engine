#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

enum class EditorLogCategory {
    Info,
    Warning,
    Error,
    Import,
    Render,
    Project,
    Scene,
    Command,
};

struct EditorLogEntry {
    EditorLogCategory category = EditorLogCategory::Info;
    std::string message;
    std::chrono::system_clock::time_point timestamp{};
};

class EditorLog final {
public:
    void add(EditorLogCategory category, std::string message);
    void clear();
    [[nodiscard]] const std::vector<EditorLogEntry>& entries() const { return entries_; }
    [[nodiscard]] bool saveText(const std::filesystem::path& path) const;

private:
    std::vector<EditorLogEntry> entries_;
};

[[nodiscard]] const char* editorLogCategoryName(EditorLogCategory category);

} // namespace rtv
