#include "rtv/Project.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

namespace rtv {

namespace {

std::string genericPath(const std::filesystem::path& path) {
    return path.generic_string();
}

std::filesystem::path relativeOrValue(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::path relative = std::filesystem::relative(path, root, error);
    return error ? path : relative;
}

std::filesystem::path projectPath(const std::filesystem::path& root, const nlohmann::json& json, const char* key, const char* fallback) {
    const std::string value = json.value(key, fallback);
    std::filesystem::path path{value};
    return path.is_absolute() ? path : root / path;
}

} // namespace

std::string generateProjectGuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    const auto now = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const uint64_t a = rng() ^ now;
    const uint64_t b = rng();

    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(a >> 32) << '-'
        << std::setw(4) << static_cast<uint16_t>(a >> 16) << '-'
        << std::setw(4) << static_cast<uint16_t>(a) << '-'
        << std::setw(4) << static_cast<uint16_t>(b >> 48) << '-'
        << std::setw(12) << (b & 0x0000FFFFFFFFFFFFull);
    return out.str();
}

ProjectContext makeProjectContext(
    std::string name,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& projectFile,
    std::string projectGuid) {
    ProjectContext project;
    project.projectGuid = projectGuid.empty() ? generateProjectGuid() : std::move(projectGuid);
    project.name = std::move(name);
    project.projectRoot = projectRoot;
    project.projectFile = projectFile;
    project.contentRoot = projectRoot / "Content";
    project.scenesRoot = projectRoot / "Scenes";
    project.cacheRoot = projectRoot / "DerivedDataCache";
    project.savedRoot = projectRoot / "Saved";
    project.configRoot = projectRoot / "Config";
    project.buildRoot = projectRoot / "Build";
    project.assetRegistryPath = project.contentRoot / "AssetRegistry.json";
    project.startupScene = project.scenesRoot / "Main.rtlevel";
    return project;
}

bool saveProjectFile(const ProjectContext& project) {
    std::error_code error;
    std::filesystem::create_directories(project.projectFile.parent_path(), error);
    if (error) {
        return false;
    }

    nlohmann::json json;
    json["version"] = 1;
    json["projectGuid"] = project.projectGuid;
    json["name"] = project.name;
    json["engineVersion"] = "0.1";
    json["startupScene"] = genericPath(relativeOrValue(project.startupScene, project.projectRoot));
    json["contentRoot"] = genericPath(relativeOrValue(project.contentRoot, project.projectRoot));
    json["scenesRoot"] = genericPath(relativeOrValue(project.scenesRoot, project.projectRoot));
    json["cacheRoot"] = genericPath(relativeOrValue(project.cacheRoot, project.projectRoot));
    json["savedRoot"] = genericPath(relativeOrValue(project.savedRoot, project.projectRoot));
    json["configRoot"] = genericPath(relativeOrValue(project.configRoot, project.projectRoot));
    json["assetRegistry"] = genericPath(relativeOrValue(project.assetRegistryPath, project.projectRoot));
    json["defaultRenderPreset"] = project.defaultRenderPreset;
    json["autosaveEnabled"] = project.autosaveEnabled;
    json["autosaveIntervalMinutes"] = project.autosaveIntervalMinutes;

    std::ofstream file(project.projectFile);
    if (!file.is_open()) {
        return false;
    }
    file << json.dump(2);
    return true;
}

bool loadProjectFile(const std::filesystem::path& projectFile, ProjectContext& outProject, std::string* error) {
    std::ifstream file(projectFile);
    if (!file.is_open()) {
        if (error != nullptr) *error = "Could not open project file";
        return false;
    }

    try {
        nlohmann::json json;
        file >> json;
        const int version = json.value("version", 0);
        if (version < 1) {
            if (error != nullptr) *error = "Unsupported project file version";
            return false;
        }

        const std::filesystem::path root = projectFile.parent_path();
        outProject.projectGuid = json.value("projectGuid", generateProjectGuid());
        outProject.name = json.value("name", projectFile.stem().string());
        outProject.projectFile = projectFile;
        outProject.projectRoot = root;
        outProject.contentRoot = projectPath(root, json, "contentRoot", "Content");
        outProject.scenesRoot = projectPath(root, json, "scenesRoot", "Scenes");
        outProject.cacheRoot = projectPath(root, json, "cacheRoot", "Cache");
        outProject.savedRoot = projectPath(root, json, "savedRoot", "Saved");
        outProject.configRoot = projectPath(root, json, "configRoot", "Config");
        outProject.buildRoot = root / "Build";
        outProject.assetRegistryPath = projectPath(root, json, "assetRegistry", "Content/AssetRegistry.json");
        outProject.startupScene = projectPath(root, json, "startupScene", "Scenes/Main.rtlevel");
        outProject.defaultRenderPreset = json.value("defaultRenderPreset", "Editor");
        outProject.autosaveEnabled = json.value("autosaveEnabled", true);
        outProject.autosaveIntervalMinutes = json.value("autosaveIntervalMinutes", 5);
        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) *error = ex.what();
        return false;
    }
}

bool createProjectFolders(const ProjectContext& project, bool createDefaultContentFolders, std::string* error) {
    std::error_code ec;
    const std::filesystem::path required[] = {
        project.contentRoot,
        project.scenesRoot,
        project.cacheRoot / "Meshes",
        project.cacheRoot / "Textures",
        project.cacheRoot / "Shaders",
        project.cacheRoot / "BLAS",
        project.cacheRoot / "Thumbnails",
        project.projectRoot / "Intermediate",
        project.savedRoot / "Autosaves",
        project.savedRoot / "Logs",
        project.savedRoot / "Backups",
        project.configRoot,
        project.buildRoot,
    };
    for (const auto& path : required) {
        std::filesystem::create_directories(path, ec);
        if (ec) {
            if (error != nullptr) *error = ec.message();
            return false;
        }
    }

    if (createDefaultContentFolders) {
        const char* folders[] = {"Models", "Materials", "Textures", "HDRI", "Prefabs", "VDB"};
        for (const char* folder : folders) {
            std::filesystem::create_directories(project.contentRoot / folder, ec);
            if (ec) {
                if (error != nullptr) *error = ec.message();
                return false;
            }
        }
    }

    if (!std::filesystem::exists(project.assetRegistryPath)) {
        std::ofstream registry(project.assetRegistryPath);
        if (registry.is_open()) {
            registry << "{\n  \"version\": 1,\n  \"assets\": []\n}\n";
        }
    }
    return true;
}

} // namespace rtv
