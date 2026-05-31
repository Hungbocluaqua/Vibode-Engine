#pragma once

#include <filesystem>
#include <string>

namespace rtv {

struct ProjectContext {
    std::string projectGuid;
    std::string name;
    std::filesystem::path projectFile;
    std::filesystem::path projectRoot;
    std::filesystem::path contentRoot;
    std::filesystem::path scenesRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path savedRoot;
    std::filesystem::path configRoot;
    std::filesystem::path buildRoot;
    std::filesystem::path assetRegistryPath;
    std::filesystem::path startupScene;
    std::string defaultRenderPreset = "Editor";
    bool autosaveEnabled = true;
    int autosaveIntervalMinutes = 5;
};

struct CreateProjectRequest {
    std::string name;
    std::filesystem::path location;
    std::string templateName = "Path-Traced Level Editor Default";
    bool createDefaultScene = true;
    bool createDefaultContentFolders = true;
};

struct OpenProjectRequest {
    std::filesystem::path projectFile;
};

[[nodiscard]] ProjectContext makeProjectContext(
    std::string name,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& projectFile,
    std::string projectGuid = {});
[[nodiscard]] bool saveProjectFile(const ProjectContext& project);
[[nodiscard]] bool loadProjectFile(const std::filesystem::path& projectFile, ProjectContext& outProject, std::string* error = nullptr);
[[nodiscard]] bool createProjectFolders(const ProjectContext& project, bool createDefaultContentFolders, std::string* error = nullptr);
[[nodiscard]] std::string generateProjectGuid();

} // namespace rtv
