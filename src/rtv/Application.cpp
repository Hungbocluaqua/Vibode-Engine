#include "rtv/Application.h"

#include "rtv/AssetImport.h"
#include "rtv/CommandSystem.h"
#include "rtv/BufferUploader.h"
#include "rtv/DiagnosticImageExport.h"
#include "rtv/EditorCommands.h"
#include "rtv/EditorLog.h"
#include "rtv/FileDialog.h"
#include "rtv/GltfLoader.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/PipelineDemo.h"
#include "rtv/Prefab.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ResourceDemo.h"
#include "rtv/SceneOperations.h"
#include "rtv/SceneRenderSettingsSync.h"
#include "rtv/SceneUpdateRouter.h"
#include "rtv/SunController.h"
#include "rtv/Swapchain.h"
#include "rtv/UiOverlay.h"
#include "rtv/UploadContext.h"
#include "rtv/VulkanContext.h"

#include <Volk/volk.h>
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#if defined(_WIN32)
#include <Windows.h>
#include <Shellapi.h>
#include <dwmapi.h>
#endif

namespace rtv {

namespace {
constexpr int initialWidth = 1280;
constexpr int initialHeight = 720;
constexpr uint64_t largeSceneTriangleThreshold = 1'000'000ull;
constexpr float defaultMaxFrameDeltaSeconds = 1.0f / 30.0f;

constexpr RendererDebugView intermediateViews[] = {
    RendererDebugView::Beauty,
    RendererDebugView::DirectLighting,
    RendererDebugView::IndirectLighting,
    RendererDebugView::Variance,
    RendererDebugView::Normals,
    RendererDebugView::Depth,
    RendererDebugView::MotionVectors,
};

std::string quoteShellPath(const std::filesystem::path& path) {
    std::string value = path.string();
    std::string quoted;
    quoted.reserve(value.size() + 2u);
    quoted.push_back('"');
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::filesystem::path currentExecutablePath() {
#if defined(_WIN32)
    std::array<char, MAX_PATH> buffer{};
    const DWORD size = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size > 0 && size < buffer.size()) {
        return std::filesystem::path(buffer.data());
    }
#endif
    return std::filesystem::current_path() / "rtvulkan.exe";
}

std::string lowercaseAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

const AssetRecord* findAssetRecordByGuid(const AssetRegistry& registry, const AssetGuid& guid) {
    for (const AssetRecord& record : registry.records()) {
        if (record.guid == guid) {
            return &record;
        }
    }
    return nullptr;
}

std::vector<AssetType> expectedDependencyAssetTypesForRepair(const std::string& role) {
    const std::string lowerRole = lowercaseAscii(role);
    if (lowerRole.find("material") != std::string::npos) {
        return {AssetType::Material};
    }
    if (lowerRole.find("texture") != std::string::npos ||
        lowerRole.find("image") != std::string::npos ||
        lowerRole.find("hdr") != std::string::npos ||
        lowerRole.find("environment") != std::string::npos) {
        return {AssetType::Texture, AssetType::HDRI};
    }
    if (lowerRole.find("mesh") != std::string::npos) {
        return {AssetType::Mesh};
    }
    if (lowerRole.find("prefab") != std::string::npos || lowerRole.find("model") != std::string::npos) {
        return {AssetType::Prefab};
    }
    if (lowerRole.find("anim") != std::string::npos) {
        return {AssetType::Animation};
    }
    if (lowerRole.find("skeleton") != std::string::npos || lowerRole.find("skin") != std::string::npos) {
        return {AssetType::Skeleton};
    }
    return {};
}

bool dependencyRoleMatchesRepairAssetType(const std::string& role, AssetType type) {
    const std::vector<AssetType> expectedTypes = expectedDependencyAssetTypesForRepair(role);
    return expectedTypes.empty() || std::find(expectedTypes.begin(), expectedTypes.end(), type) != expectedTypes.end();
}

std::vector<std::pair<int, const AssetRecord*>> rankedMissingDependencyRepairCandidates(
    const AssetRegistry& registry,
    const AssetRecord& owner,
    const AssetDependency& dependency) {
    std::vector<std::pair<int, const AssetRecord*>> scored;
    for (const AssetRecord& candidate : registry.records()) {
        if (candidate.guid.empty() || candidate.guid == owner.guid || candidate.guid == dependency.guid) {
            continue;
        }
        if (!dependencyRoleMatchesRepairAssetType(dependency.kind, candidate.type)) {
            continue;
        }
        int score = 0;
        if (!owner.importGroupId.empty() && candidate.importGroupId == owner.importGroupId) {
            score += 4;
        }
        if (!owner.importRootGuid.empty() && candidate.importRootGuid == owner.importRootGuid) {
            score += 3;
        }
        if (!owner.importGroupName.empty() && candidate.importGroupName == owner.importGroupName) {
            score += 2;
        }
        if (score == 0 && !expectedDependencyAssetTypesForRepair(dependency.kind).empty()) {
            score = 1;
        }
        if (score > 0) {
            scored.push_back({score, &candidate});
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first) return lhs.first > rhs.first;
        const std::string lhsName = lhs.second != nullptr ? lhs.second->displayName : std::string{};
        const std::string rhsName = rhs.second != nullptr ? rhs.second->displayName : std::string{};
        return lhsName < rhsName;
    });
    return scored;
}

#if defined(_WIN32)
std::wstring quoteWindowsArg(std::wstring_view value) {
    std::wstring result;
    result.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::string narrowWindowsCommandLine(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring cookProcessCommandLineWide(
    const std::filesystem::path& exe,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath) {
    return quoteWindowsArg(exe.wstring()) +
        L" --cook-project " + quoteWindowsArg(projectFile.wstring()) +
        L" --cook-output " + quoteWindowsArg(outputDir.wstring()) +
        L" --cook-manifest " + quoteWindowsArg(manifestPath.wstring());
}

int runCookProjectProcess(
    const std::filesystem::path& exe,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& logPath,
    std::string* commandLineOut) {
    const std::wstring commandLine = cookProcessCommandLineWide(exe, projectFile, outputDir, manifestPath);
    if (commandLineOut != nullptr) {
        *commandLineOut = narrowWindowsCommandLine(commandLine);
    }

    std::error_code ec;
    std::filesystem::create_directories(logPath.parent_path(), ec);
    HANDLE logHandle = CreateFileW(
        logPath.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = logHandle;
    startup.hStdError = logHandle;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const std::wstring applicationName = exe.wstring();
    const BOOL created = CreateProcessW(
        applicationName.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);
    if (!created) {
        const DWORD error = GetLastError();
        DWORD written = 0;
        const std::string message = "CreateProcessW failed for cook worker. GetLastError=" + std::to_string(error) + "\r\n";
        (void)WriteFile(logHandle, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        CloseHandle(logHandle);
        return -1;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    (void)GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(logHandle);
    return static_cast<int>(exitCode);
}
#else
std::string cookProcessCommandLine(
    const std::filesystem::path& exe,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& logPath) {
    return quoteShellPath(exe) +
        " --cook-project " + quoteShellPath(projectFile) +
        " --cook-output " + quoteShellPath(outputDir) +
        " --cook-manifest " + quoteShellPath(manifestPath) +
        " > " + quoteShellPath(logPath) + " 2>&1";
}

int runCookProjectProcess(
    const std::filesystem::path& exe,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& logPath,
    std::string* commandLineOut) {
    std::error_code logEc;
    std::filesystem::create_directories(logPath.parent_path(), logEc);
    const std::string command = cookProcessCommandLine(exe, projectFile, outputDir, manifestPath, logPath);
    if (commandLineOut != nullptr) {
        *commandLineOut = command;
    }
    return std::system(command.c_str());
}
#endif

std::string cookArtifactPathString(const std::filesystem::path& path) {
    return path.empty() ? std::string{} : path.generic_string();
}

bool writeCookJsonArtifact(const std::filesystem::path& path, const nlohmann::json& json, std::string* error = nullptr) {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "path is empty";
        }
        return false;
    }
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error != nullptr) {
                *error = "could not create directory " + parent.string() + ": " + ec.message();
            }
            return false;
        }
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        if (error != nullptr) {
            *error = "could not open " + path.string();
        }
        return false;
    }
    out << json.dump(2);
    return true;
}

void ensureCookFailureArtifacts(
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& validationReportPath,
    const std::filesystem::path& logPath,
    int exitCode,
    const std::string& status,
    const std::string& commandLine = {}) {
    std::error_code ec;
    if (!outputDir.empty()) {
        std::filesystem::create_directories(outputDir, ec);
    }

    const std::string detail = status.empty() ? "Cook failed before writing artifacts." : status;
    const nlohmann::json errors = nlohmann::json::array({detail});

    if (!validationReportPath.empty() && !std::filesystem::exists(validationReportPath)) {
        const nlohmann::json report = {
            {"version", 1},
            {"kind", "CookAssetValidationReport"},
            {"project", {{"projectFile", cookArtifactPathString(projectFile)}}},
            {"assetCount", 0},
            {"errorCount", 1},
            {"warningCount", 0},
            {"validationErrorCount", 1},
            {"validationWarningCount", 0},
            {"cookErrors", errors},
            {"cookWarnings", nlohmann::json::array()},
            {"status", "failed"},
            {"policy", "This fallback report was written by the editor because the cook worker exited before producing its normal artifacts."},
        };
        std::string writeError;
        if (!writeCookJsonArtifact(validationReportPath, report, &writeError)) {
            std::cerr << "Cook fallback report write failed: " << writeError << '\n';
        }
    }

    if (!manifestPath.empty() && !std::filesystem::exists(manifestPath)) {
        const nlohmann::json manifest = {
            {"schema", "TransparentCookManifestV1"},
            {"project", {{"projectFile", cookArtifactPathString(projectFile)}}},
            {"outputRoot", cookArtifactPathString(outputDir)},
            {"assetCount", 0},
            {"assets", nlohmann::json::array()},
            {"plannedFiles", nlohmann::json::array()},
            {"copiedFiles", nlohmann::json::array()},
            {"warnings", nlohmann::json::array()},
            {"errors", errors},
            {"validationReport", validationReportPath.empty() ? std::string{} : validationReportPath.filename().generic_string()},
            {"validationErrorCount", 1},
            {"validationWarningCount", 0},
            {"status", "failed"},
        };
        std::string writeError;
        if (!writeCookJsonArtifact(manifestPath, manifest, &writeError)) {
            std::cerr << "Cook fallback manifest write failed: " << writeError << '\n';
        }
    }

    if (!logPath.empty() && !std::filesystem::exists(logPath)) {
        std::error_code logEc;
        std::filesystem::create_directories(logPath.parent_path(), logEc);
        std::ofstream log(logPath, std::ios::trunc);
        if (log.is_open()) {
            log << detail << '\n'
                << "Exit code: " << exitCode << '\n'
                << "Project: " << projectFile.string() << '\n'
                << "Output: " << outputDir.string() << '\n';
            if (!commandLine.empty()) {
                log << "Command: " << commandLine << '\n';
            }
        }
    }
}

struct CookManifestProgress {
    bool available = false;
    std::string status;
    size_t plannedFileCount = 0;
    size_t copiedFileCount = 0;
};

CookManifestProgress readCookManifestProgress(const std::filesystem::path& manifestPath) {
    CookManifestProgress progress;
    if (manifestPath.empty()) {
        return progress;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifestPath, ec)) {
        return progress;
    }

    std::ifstream file(manifestPath);
    if (!file.is_open()) {
        return progress;
    }
    nlohmann::json manifest;
    try {
        file >> manifest;
    } catch (...) {
        return progress;
    }

    progress.available = true;
    progress.status = manifest.value("status", std::string{});
    if (manifest.contains("plannedFiles") && manifest["plannedFiles"].is_array()) {
        progress.plannedFileCount = manifest["plannedFiles"].size();
    }
    if (manifest.contains("copiedFiles") && manifest["copiedFiles"].is_array()) {
        progress.copiedFileCount = manifest["copiedFiles"].size();
    }
    return progress;
}

bool assetPlacementBlocked(const AssetRecord& record) {
    return record.missing || record.status == AssetImportStatus::Missing || record.status == AssetImportStatus::Failed ||
        record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing;
}

const char* assetPlacementBlockReason(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Failed) return "Asset import failed; repair or reimport before placement.";
    if (record.importedMetadataMissing) return "Asset metadata is missing; repair or reimport before placement.";
    if (record.cookedPayloadMissing) return "Asset cooked/runtime payload is missing; rebuild or repair before placement.";
    if (record.dependenciesMissing) return "Asset dependency records are missing; repair references before placement.";
    if (record.missing || record.status == AssetImportStatus::Missing) return "Asset is marked missing; repair before placement.";
    return "Asset is ready for placement.";
}

SceneUpdateKind createEntityUpdateKind(EditorEntityCreateKind kind) {
    switch (kind) {
    case EditorEntityCreateKind::Empty:
        return SceneUpdateKind::None;
    case EditorEntityCreateKind::Camera:
        return SceneUpdateKind::CameraOnly;
    case EditorEntityCreateKind::Light:
    case EditorEntityCreateKind::Sun:
    case EditorEntityCreateKind::SpotLight:
    case EditorEntityCreateKind::AreaLight:
        return SceneUpdateKind::LightOnly;
    case EditorEntityCreateKind::EnvironmentLight:
    case EditorEntityCreateKind::SkyAtmosphere:
    case EditorEntityCreateKind::HeightFog:
    case EditorEntityCreateKind::VolumetricCloud:
    case EditorEntityCreateKind::PostProcessVolume:
        return SceneUpdateKind::RendererSettingsOnly;
    }
    return SceneUpdateKind::TopologyChanged;
}

#if defined(_WIN32)
void enableDarkWindowFrame(GLFWwindow* window) {
    if (window == nullptr) {
        return;
    }
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr) {
        return;
    }
    BOOL enabled = TRUE;
    constexpr DWORD darkModeAttribute = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE on current Windows SDKs.
    if (FAILED(DwmSetWindowAttribute(hwnd, darkModeAttribute, &enabled, sizeof(enabled)))) {
        constexpr DWORD legacyDarkModeAttribute = 19;
        (void)DwmSetWindowAttribute(hwnd, legacyDarkModeAttribute, &enabled, sizeof(enabled));
    }
}
#endif

RendererDebugView nextDebugView(RendererDebugView view) {
    const auto& views = editorDebugViews();
    for (size_t i = 0; i < views.size(); ++i) {
        if (views[i] == view) {
            return views[(i + 1u) % views.size()];
        }
    }
    return RendererDebugView::Beauty;
}

std::optional<std::filesystem::path> startupProjectOverridePath() {
#if defined(_WIN32)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "RTV_EDITOR_STARTUP_PROJECT") != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path path(value);
    std::free(value);
    if (path.empty()) {
        return std::nullopt;
    }
    return path;
#else
    const char* value = std::getenv("RTV_EDITOR_STARTUP_PROJECT");
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::filesystem::path(value);
#endif
}

bool editorRecoveryPromptSuppressed() {
#if defined(_WIN32)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT") != 0 || value == nullptr) {
        return false;
    }
    const std::string text(value);
    std::free(value);
#else
    const char* value = std::getenv("RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT");
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
#endif
    return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON";
}

std::string normalizedCameraName(std::string_view name) {
    std::string result{name};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return ch == '_' ? '-' : static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string lowerPathExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

float clampFrameDeltaSeconds(float rawDeltaSeconds, const PathTracerRenderer* renderer) {
    const float maxDelta = renderer != nullptr
        ? std::max(0.001f, renderer->settings().maxFrameDeltaSeconds)
        : defaultMaxFrameDeltaSeconds;
    return std::clamp(std::isfinite(rawDeltaSeconds) ? rawDeltaSeconds : 0.0f, 0.0f, maxDelta);
}

std::filesystem::path nearestExistingParentForProject(std::filesystem::path path) {
    std::error_code ec;
    while (!path.empty() && !std::filesystem::exists(path, ec)) {
        path = path.parent_path();
    }
    return path;
}

bool pathLooksWritableForProject(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return false;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    const std::filesystem::perms permissions = std::filesystem::status(path, ec).permissions();
    if (ec) {
        return false;
    }
    return (permissions & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
#endif
}

std::filesystem::path normalizedPathForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return normalized;
    }
    normalized = std::filesystem::absolute(path, ec);
    return ec ? path.lexically_normal() : normalized.lexically_normal();
}

bool pathIsInsideDirectory(const std::filesystem::path& child, const std::filesystem::path& directory) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(
        normalizedPathForCompare(child),
        normalizedPathForCompare(directory),
        ec);
    if (ec || relative.empty()) {
        return false;
    }
    for (const std::filesystem::path& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string lowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool projectReferenceRewriteFileCandidate(const std::filesystem::path& path) {
    const std::string filename = lowerAscii(path.filename().string());
    const std::string ext = lowerAscii(path.extension().string());
    if (ext == ".rtlevel" || ext == ".mscene" || ext == ".vproject") {
        return true;
    }
    auto endsWith = [&](const char* suffix) {
        const std::string value(suffix);
        return filename.size() >= value.size() && filename.compare(filename.size() - value.size(), value.size(), value) == 0;
    };
    return endsWith(".rtprefab.json") ||
        endsWith(".rtmesh.json") ||
        endsWith(".rtmaterial.json") ||
        endsWith(".rttexture.json") ||
        endsWith(".rthdri.json") ||
        endsWith(".rtanim.json") ||
        endsWith(".rtskeleton.json");
}

void appendUniqueExistingDirectory(std::vector<std::filesystem::path>& roots, const std::filesystem::path& root) {
    if (root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }
    const std::filesystem::path normalized = normalizedPathForCompare(root);
    for (const std::filesystem::path& existing : roots) {
        if (normalizedPathForCompare(existing) == normalized) {
            return;
        }
    }
    roots.push_back(normalized);
}

std::vector<std::filesystem::path> collectProjectReferenceRewriteFiles(const ProjectContext& project, nlohmann::json& checkedRoots) {
    std::vector<std::filesystem::path> roots;
    appendUniqueExistingDirectory(roots, project.contentRoot);
    appendUniqueExistingDirectory(roots, project.scenesRoot);

    std::vector<std::filesystem::path> files;
    for (const std::filesystem::path& root : roots) {
        checkedRoots.push_back(root.generic_string());
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            std::error_code entryError;
            if (entry.is_regular_file(entryError) && projectReferenceRewriteFileCandidate(entry.path())) {
                files.push_back(normalizedPathForCompare(entry.path()));
            }
        }
    }
    if (!project.projectFile.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(project.projectFile, ec) && projectReferenceRewriteFileCandidate(project.projectFile)) {
            files.push_back(normalizedPathForCompare(project.projectFile));
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::string jsonPathChild(std::string parent, const std::string& child) {
    if (parent.empty()) {
        parent = "$";
    }
    return parent + "/" + child;
}

bool jsonPathContainsSegment(const std::string& jsonPath, std::string_view segment) {
    const std::string path = lowerAscii(jsonPath);
    const std::string needle = "/" + std::string(segment) + "/";
    return path.find(needle) != std::string::npos;
}

bool isSavedProjectAssetReferenceField(const std::string& jsonPath, const std::string& key) {
    const std::string lowerKey = lowerAscii(key);
    if (lowerKey == "meshguid" ||
        lowerKey == "materialguid" ||
        lowerKey == "overridematerialguid" ||
        lowerKey == "animationguid" ||
        lowerKey == "prefabguid" ||
        lowerKey == "materialguids") {
        return true;
    }
    if (lowerKey == "assetguid") {
        return jsonPathContainsSegment(jsonPath, "assetreferences") || jsonPathContainsSegment(jsonPath, "dependencies");
    }
    if (lowerKey == "guid") {
        return jsonPathContainsSegment(jsonPath, "dependencies");
    }
    if (lowerKey == "references") {
        return true;
    }
    return false;
}

bool replaceGuidReferenceOccurrences(
    nlohmann::json& value,
    const AssetGuid& oldGuid,
    const AssetGuid& newGuid,
    const std::string& jsonPath,
    const std::string& key,
    nlohmann::json& occurrences) {
    bool changed = false;
    if (value.is_string()) {
        if (value.get<std::string>() == oldGuid && isSavedProjectAssetReferenceField(jsonPath, key)) {
            value = newGuid;
            occurrences.push_back({{"jsonPath", jsonPath.empty() ? "$" : jsonPath}, {"field", key}});
            return true;
        }
        return false;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            changed = replaceGuidReferenceOccurrences(it.value(), oldGuid, newGuid, jsonPathChild(jsonPath, it.key()), it.key(), occurrences) || changed;
        }
        return changed;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            changed = replaceGuidReferenceOccurrences(value[i], oldGuid, newGuid, jsonPathChild(jsonPath, std::to_string(i)), key, occurrences) || changed;
        }
    }
    return changed;
}

std::filesystem::path uniqueReferenceRewriteBackupPath(const std::filesystem::path& path) {
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
        std::filesystem::path candidate = path.string() + ".before_replace_refs." + std::to_string(stamp);
        if (attempt > 0) {
            candidate = candidate.string() + "." + std::to_string(attempt);
        }
        candidate = candidate.string() + ".bak";
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return path.string() + ".before_replace_refs." + std::to_string(stamp) + ".fallback.bak";
}

std::string safeReportName(std::string value) {
    if (value.empty()) {
        return "asset";
    }
    for (char& c : value) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || std::isspace(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    return value;
}

struct ProjectReferenceRewriteResult {
    size_t scannedFileCount = 0;
    size_t changedFileCount = 0;
    size_t occurrenceCount = 0;
    bool refreshedReferenceIndex = false;
    nlohmann::json checkedRoots = nlohmann::json::array();
    nlohmann::json changedFiles = nlohmann::json::array();
    nlohmann::json skippedFiles = nlohmann::json::array();
    nlohmann::json parseErrors = nlohmann::json::array();
    nlohmann::json writeErrors = nlohmann::json::array();
    std::filesystem::path reportPath;
    std::filesystem::path refreshedReferenceIndexPath;
    std::string refreshedReferenceIndexError;
};

void appendPersistentReferenceIndexEntries(
    const nlohmann::json& value,
    const std::unordered_set<AssetGuid>& registryGuids,
    const std::filesystem::path& filePath,
    const std::string& jsonPath,
    const std::string& key,
    nlohmann::json& references,
    nlohmann::json& unknownGuidFields) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            appendPersistentReferenceIndexEntries(it.value(), registryGuids, filePath, jsonPathChild(jsonPath, it.key()), it.key(), references, unknownGuidFields);
        }
        return;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            appendPersistentReferenceIndexEntries(value[i], registryGuids, filePath, jsonPathChild(jsonPath, std::to_string(i)), key, references, unknownGuidFields);
        }
        return;
    }
    if (!value.is_string()) {
        return;
    }

    const std::string guid = value.get<std::string>();
    if (guid.empty() || !isSavedProjectAssetReferenceField(jsonPath, key)) {
        return;
    }
    if (registryGuids.find(guid) != registryGuids.end()) {
        references.push_back({
            {"file", filePath.generic_string()},
            {"jsonPath", jsonPath.empty() ? "$" : jsonPath},
            {"key", key},
            {"guid", guid},
        });
    } else {
        unknownGuidFields.push_back({
            {"file", filePath.generic_string()},
            {"jsonPath", jsonPath.empty() ? "$" : jsonPath},
            {"key", key},
            {"guid", guid},
        });
    }
}

bool refreshPersistentAssetReferenceIndex(
    const ProjectContext& project,
    const AssetRegistry& registry,
    std::filesystem::path& outPath,
    std::string& outError,
    std::string generatedBy = "ReplaceProjectReferences",
    std::string persistenceReason = "This index was refreshed after Replace Project References rewrote saved project metadata.") {
    outPath = project.savedRoot / "AssetReferenceIndex.json";
    std::unordered_set<AssetGuid> registryGuids;
    std::unordered_map<AssetGuid, size_t> savedReferenceCounts;
    registryGuids.reserve(registry.records().size());
    for (const AssetRecord& record : registry.records()) {
        if (!record.guid.empty()) {
            registryGuids.insert(record.guid);
            savedReferenceCounts.emplace(record.guid, 0u);
        }
    }

    nlohmann::json checkedRoots = nlohmann::json::array();
    nlohmann::json scannedFiles = nlohmann::json::array();
    nlohmann::json references = nlohmann::json::array();
    nlohmann::json unknownGuidFields = nlohmann::json::array();
    nlohmann::json parseErrors = nlohmann::json::array();
    const std::vector<std::filesystem::path> files = collectProjectReferenceRewriteFiles(project, checkedRoots);
    for (const std::filesystem::path& path : files) {
        scannedFiles.push_back(path.generic_string());
        std::ifstream input(path);
        if (!input.is_open()) {
            parseErrors.push_back({
                {"file", path.generic_string()},
                {"detail", "Could not open JSON metadata file while refreshing the persistent reference index."},
            });
            continue;
        }
        nlohmann::json json;
        try {
            input >> json;
        } catch (const std::exception& error) {
            parseErrors.push_back({
                {"file", path.generic_string()},
                {"detail", error.what()},
            });
            continue;
        }
        const size_t before = references.size();
        appendPersistentReferenceIndexEntries(json, registryGuids, path, "$", {}, references, unknownGuidFields);
        for (size_t i = before; i < references.size(); ++i) {
            const AssetGuid guid = references[i].value("guid", std::string{});
            if (!guid.empty()) {
                ++savedReferenceCounts[guid];
            }
        }
    }

    nlohmann::json assets = nlohmann::json::array();
    for (const AssetRecord& record : registry.records()) {
        assets.push_back({
            {"guid", record.guid},
            {"displayName", record.displayName},
            {"assetType", assetTypeName(record.type)},
            {"savedProjectReferenceCount", savedReferenceCounts[record.guid]},
        });
    }

    const nlohmann::json index = {
        {"version", 1},
        {"kind", "AssetProjectReferenceIndexReport"},
        {"generatedBy", generatedBy},
        {"registryPath", registry.state().path.empty() ? std::string{} : registry.state().path.generic_string()},
        {"persistentIndexPath", outPath.generic_string()},
        {"assetCount", assets.size()},
        {"checkedRoots", checkedRoots},
        {"scannedFileCount", scannedFiles.size()},
        {"registeredReferenceCount", references.size()},
        {"unknownGuidFieldCount", unknownGuidFields.size()},
        {"parseErrorCount", parseErrors.size()},
        {"assets", assets},
        {"references", references},
        {"unknownGuidFields", unknownGuidFields},
        {"parseErrors", parseErrors},
        {"scannedFiles", scannedFiles},
        {"checkedFileTypes", nlohmann::json::array({".rtlevel", ".mscene", ".vproject", ".rtprefab.json", ".rtmesh.json", ".rtmaterial.json", ".rttexture.json", ".rthdri.json", ".rtanim.json", ".rtskeleton.json"})},
        {"persistence", persistenceReason},
        {"limitation", "This is a saved-file JSON index refresh triggered by explicit editor persistence workflows. It is not a continuously maintained background index/watcher and does not inspect generated cache payload internals or opaque packages."},
    };

    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create persistent reference index folder: " + ec.message();
        return false;
    }
    std::ofstream output(outPath, std::ios::trunc);
    if (!output.is_open()) {
        outError = "Could not write persistent reference index: " + outPath.string();
        return false;
    }
    output << index.dump(2);
    return true;
}

ProjectReferenceRewriteResult rewriteSavedProjectAssetReferences(
    const ProjectContext& project,
    const AssetRegistry& registry,
    const AssetGuid& oldGuid,
    const AssetGuid& newGuid,
    const std::optional<std::filesystem::path>& currentScenePath,
    bool currentSceneDirty) {
    ProjectReferenceRewriteResult result;
    const std::vector<std::filesystem::path> files = collectProjectReferenceRewriteFiles(project, result.checkedRoots);
    result.scannedFileCount = files.size();
    const std::filesystem::path currentSceneKey = currentScenePath.has_value() ? normalizedPathForCompare(*currentScenePath) : std::filesystem::path{};

    for (const std::filesystem::path& path : files) {
        if (currentSceneDirty && !currentSceneKey.empty() && normalizedPathForCompare(path) == currentSceneKey) {
            result.skippedFiles.push_back({
                {"path", path.generic_string()},
                {"reason", "CurrentSceneHasUnsavedChanges"},
            });
            continue;
        }
        std::ifstream input(path);
        if (!input.is_open()) {
            result.parseErrors.push_back({
                {"path", path.generic_string()},
                {"detail", "Could not open JSON metadata file for reference rewrite."},
            });
            continue;
        }
        nlohmann::json json;
        try {
            input >> json;
        } catch (const std::exception& error) {
            result.parseErrors.push_back({
                {"path", path.generic_string()},
                {"detail", error.what()},
            });
            continue;
        }

        nlohmann::json occurrences = nlohmann::json::array();
        if (!replaceGuidReferenceOccurrences(json, oldGuid, newGuid, "$", {}, occurrences)) {
            continue;
        }

        std::error_code ec;
        const std::filesystem::path backupPath = uniqueReferenceRewriteBackupPath(path);
        std::filesystem::copy_file(path, backupPath, std::filesystem::copy_options::none, ec);
        if (ec) {
            result.writeErrors.push_back({
                {"path", path.generic_string()},
                {"detail", "Could not write backup before reference rewrite: " + ec.message()},
            });
            continue;
        }
        std::ofstream output(path, std::ios::trunc);
        if (!output.is_open()) {
            result.writeErrors.push_back({
                {"path", path.generic_string()},
                {"backupPath", backupPath.generic_string()},
                {"detail", "Could not open metadata file for writing after backup."},
            });
            continue;
        }
        output << json.dump(2);
        ++result.changedFileCount;
        result.occurrenceCount += occurrences.size();
        result.changedFiles.push_back({
            {"path", path.generic_string()},
            {"backupPath", backupPath.generic_string()},
            {"referenceCount", occurrences.size()},
            {"occurrences", occurrences},
        });
    }

    result.refreshedReferenceIndex = refreshPersistentAssetReferenceIndex(project, registry, result.refreshedReferenceIndexPath, result.refreshedReferenceIndexError);
    result.reportPath = project.savedRoot / "Reports" / ("asset_reference_rewrite_" + safeReportName(oldGuid) + "_to_" + safeReportName(newGuid) + ".json");
    std::error_code ec;
    std::filesystem::create_directories(result.reportPath.parent_path(), ec);
    if (!ec) {
        std::ofstream report(result.reportPath, std::ios::trunc);
        if (report.is_open()) {
            const nlohmann::json reportJson = {
                {"version", 1},
                {"kind", "AssetReferenceRewriteReport"},
                {"oldGuid", oldGuid},
                {"newGuid", newGuid},
                {"checkedRoots", result.checkedRoots},
                {"scannedFileCount", result.scannedFileCount},
                {"changedFileCount", result.changedFileCount},
                {"referenceOccurrenceCount", result.occurrenceCount},
                {"changedFiles", result.changedFiles},
                {"skippedFiles", result.skippedFiles},
                {"parseErrors", result.parseErrors},
                {"writeErrors", result.writeErrors},
                {"refreshedReferenceIndex", result.refreshedReferenceIndex},
                {"refreshedReferenceIndexPath", result.refreshedReferenceIndexPath.empty() ? std::string{} : result.refreshedReferenceIndexPath.generic_string()},
                {"refreshedReferenceIndexError", result.refreshedReferenceIndexError},
                {"backupPolicy", "Each changed metadata file is copied to a unique <file>.before_replace_refs.<timestamp>[.<n>].bak before rewriting; existing backups are not overwritten."},
                {"rewritePolicy", "Only known asset-reference fields are rewritten, including scene/prefab component GUID fields, assetReferences entries, AssetRegistry dependencies, and AssetRegistry references. Identity/provenance fields such as guid, runtimePayload.assetGuid, and importRootGuid are preserved."},
            };
            report << reportJson.dump(2);
        }
    }
    return result;
}

bool projectRootLooksSafeToDelete(const std::filesystem::path& projectRoot, const std::filesystem::path& projectFile) {
    if (projectRoot.empty() || projectFile.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(projectRoot, ec)) {
        return false;
    }
    const std::filesystem::path normalizedRoot = normalizedPathForCompare(projectRoot);
    if (!normalizedRoot.has_filename() || normalizedRoot == normalizedRoot.root_path()) {
        return false;
    }
    const std::filesystem::path extension = projectFile.extension();
    if (extension != ".vproject" && extension != ".rtproject") {
        return false;
    }
    return pathIsInsideDirectory(projectFile, projectRoot);
}

bool sameRegistryPath(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    return normalizedPathForCompare(lhs) == normalizedPathForCompare(rhs);
}

uint64_t countSceneTriangles(const SceneAsset& scene, const AssetManager& assets) {
    uint64_t triangles = 0;
    for (MeshAssetHandle handle : scene.meshes) {
        if (const MeshAsset* mesh = assets.mesh(handle)) {
            triangles += mesh->indices.size() / 3u;
        }
    }
    return triangles;
}

RendererSettings interactiveSettingsForScene(RendererSettings settings, const SceneAsset& scene, const AssetManager& assets, bool importSafeRuntime, bool& changed) {
    changed = false;
    const uint64_t triangleCount = countSceneTriangles(scene, assets);
    bool runtimeSettingsChanged = false;
    bool largeSceneSettingsChanged = false;

    auto setBool = [&](bool& field, bool value, bool& localChanged) {
        if (field != value) {
            field = value;
            changed = true;
            localChanged = true;
        }
    };
    auto setUint = [&](uint32_t& field, uint32_t value, bool& localChanged) {
        if (field != value) {
            field = value;
            changed = true;
            localChanged = true;
        }
    };
    auto capUint = [&](uint32_t& field, uint32_t maxValue, bool& localChanged) {
        const uint32_t value = std::min(field, maxValue);
        if (field != value) {
            field = value;
            changed = true;
            localChanged = true;
        }
    };
    auto capFloat = [&](float& field, float maxValue, bool& localChanged) {
        const float value = std::min(field, maxValue);
        if (std::abs(field - value) > 0.0001f) {
            field = value;
            changed = true;
            localChanged = true;
        }
    };
    auto setDenoiserBackend = [&](DenoiserBackend value, bool& localChanged) {
        if (settings.denoiserBackend != value) {
            settings.denoiserBackend = value;
            changed = true;
            localChanged = true;
        }
    };
    auto setTemporalUpscaler = [&](TemporalUpscaler value, bool& localChanged) {
        if (settings.temporalUpscaler != value) {
            settings.temporalUpscaler = value;
            changed = true;
            localChanged = true;
        }
    };
    auto setAdaptiveQualityMode = [&](AdaptiveQualityMode value, bool& localChanged) {
        if (settings.adaptiveQualityMode != value) {
            settings.adaptiveQualityMode = value;
            changed = true;
            localChanged = true;
        }
    };
    auto setCustomPresetIfNeeded = [&](bool& localChanged) {
        if (settings.renderPreset != RenderPreset::Custom) {
            settings.renderPreset = RenderPreset::Custom;
            changed = true;
            localChanged = true;
        }
    };

    if (importSafeRuntime) {
        setBool(settings.denoiserEnabled, true, runtimeSettingsChanged);
        setDenoiserBackend(DenoiserBackend::Engine, runtimeSettingsChanged);
        setBool(settings.denoiseWhileMoving, true, runtimeSettingsChanged);
        setBool(settings.taaEnabled, true, runtimeSettingsChanged);
        setTemporalUpscaler(TemporalUpscaler::TaaTsr, runtimeSettingsChanged);
        setBool(settings.dlssFrameGenerationEnabled, false, runtimeSettingsChanged);
        setBool(settings.dlssRayReconstructionEnabled, false, runtimeSettingsChanged);
        if (runtimeSettingsChanged) {
            setCustomPresetIfNeeded(runtimeSettingsChanged);
            std::cout << "glTF import runtime settings: using engine denoiser and TAA/TSR; "
                      << "DLSS/NRD disabled for initial scene load. Enable them after the scene is loaded if needed.\n";
        }
    }

    if (triangleCount >= largeSceneTriangleThreshold) {
        capUint(settings.maxBounces, 2u, largeSceneSettingsChanged);
        setUint(settings.samplesPerPixel, 1u, largeSceneSettingsChanged);
        setBool(settings.limitSamplesPerPixel, true, largeSceneSettingsChanged);
        setUint(settings.environmentDirectSamples, 1u, largeSceneSettingsChanged);
        capUint(settings.atrousIterations, 2u, largeSceneSettingsChanged);
        capUint(settings.denoiserMaxHistoryLength, 32u, largeSceneSettingsChanged);
        capFloat(settings.renderResolutionScale, 0.35f, largeSceneSettingsChanged);
        capFloat(settings.materialTextureAnisotropy, 2.0f, largeSceneSettingsChanged);
        capFloat(settings.fireflyClamp, 7.0f, largeSceneSettingsChanged);
        setBool(settings.denoiserEnabled, true, largeSceneSettingsChanged);
        setDenoiserBackend(DenoiserBackend::Engine, largeSceneSettingsChanged);
        setBool(settings.denoiseWhileMoving, true, largeSceneSettingsChanged);
        setBool(settings.taaEnabled, true, largeSceneSettingsChanged);
        setTemporalUpscaler(TemporalUpscaler::TaaTsr, largeSceneSettingsChanged);
        setBool(settings.dlssFrameGenerationEnabled, false, largeSceneSettingsChanged);
        setBool(settings.dlssRayReconstructionEnabled, false, largeSceneSettingsChanged);
        setBool(settings.restirGiEnabled, false, largeSceneSettingsChanged);
        setBool(settings.restirGiHalfResolution, true, largeSceneSettingsChanged);
        setUint(settings.restirGiSpatialRounds, 1u, largeSceneSettingsChanged);
        capFloat(settings.restirGiSpatialRadius, 3.0f, largeSceneSettingsChanged);
        setUint(settings.restirGiVisibilityRayBudget, 1u, largeSceneSettingsChanged);
        setBool(settings.opacityMicromapsEnabled, false, largeSceneSettingsChanged);
        setUint(settings.opacityMicromapSubdivisionLevel, 0u, largeSceneSettingsChanged);
        setBool(settings.shaderExecutionReorderingEnabled, false, largeSceneSettingsChanged);
        setBool(settings.motionBlurEnabled, false, largeSceneSettingsChanged);
        setBool(settings.homogeneousVolumeEnabled, false, largeSceneSettingsChanged);
        setBool(settings.mneeCausticsEnabled, false, largeSceneSettingsChanged);
        setAdaptiveQualityMode(AdaptiveQualityMode::Aggressive, largeSceneSettingsChanged);
        if (largeSceneSettingsChanged) {
            setCustomPresetIfNeeded(largeSceneSettingsChanged);
        }
    }

    if (largeSceneSettingsChanged) {
        std::cout << "Large glTF scene detected (" << triangleCount
                  << " triangles); using import-safe interactive defaults: bounces="
                  << settings.maxBounces << " resolution_scale="
                  << settings.renderResolutionScale
                  << " restir_gi=" << (settings.restirGiEnabled ? "on" : "off")
                  << " dlss=" << (settings.temporalUpscaler == TemporalUpscaler::Dlss ? "on" : "off")
                  << " omm=" << (settings.opacityMicromapsEnabled ? "on" : "off")
                  << ". Raise quality in Render Settings after the scene is loaded.\n";
    }
    return settings;
}

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity);

std::wstring widenAscii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

void syncDocumentRenderSettings(SceneDocument& document, const RendererSettings& settings) {
    RenderSettings& render = document.renderSettings();
    render.renderPreset = settings.renderPreset;
    render.pathTracingEnabled = settings.pathTracingEnabled;
    render.cameraJitterEnabled = settings.cameraJitterEnabled;
    render.directLightingEnabled = settings.directLightingEnabled;
    render.maxBounces = settings.maxBounces;
    render.environmentDirectSamples = settings.environmentDirectSamples;
    render.toneMapper = settings.toneMapper;
    render.exposure = settings.exposure;
    render.gamma = settings.gamma;
    render.contrast = settings.contrast;
    render.saturation = settings.saturation;
    render.brightness = settings.brightness;
    render.whitePoint = settings.whitePoint;
    render.autoExposureEnabled = settings.autoExposureEnabled;
    render.targetLuminance = settings.targetLuminance;
    render.minExposure = settings.minExposure;
    render.maxExposure = settings.maxExposure;
    render.adaptationSpeed = settings.adaptationSpeed;
    render.histogramMinLogLuminance = settings.histogramMinLogLuminance;
    render.histogramMaxLogLuminance = settings.histogramMaxLogLuminance;
    render.histogramLowPercentile = settings.histogramLowPercentile;
    render.histogramHighPercentile = settings.histogramHighPercentile;
    render.histogramTargetPercentile = settings.histogramTargetPercentile;
    render.skyIntensity = settings.skyIntensity;
    render.indirectStrength = settings.indirectStrength;
    render.restirMode = settings.restirMode;
    render.restirGiEnabled = settings.restirGiEnabled;
    render.denoiserEnabled = settings.denoiserEnabled;
    render.denoiserBackend = settings.denoiserBackend;
    render.denoiseWhileMoving = settings.denoiseWhileMoving;
    render.samplesPerPixel = settings.samplesPerPixel;
    render.limitSamplesPerPixel = settings.limitSamplesPerPixel;
    render.atrousIterations = settings.atrousIterations;
    render.denoiserStrength = settings.denoiserStrength;
    render.denoiserMaxHistoryLength = settings.denoiserMaxHistoryLength;
    render.momentValidityThreshold = settings.momentValidityThreshold;
    render.taaEnabled = settings.taaEnabled;
    render.temporalUpscaler = settings.temporalUpscaler;
    render.dlssFrameGenerationEnabled = settings.dlssFrameGenerationEnabled;
    render.dlssRayReconstructionEnabled = settings.dlssRayReconstructionEnabled;
    render.dlssSharpeningStrength = settings.dlssSharpeningStrength;
    render.taaFeedback = settings.taaFeedback;
    render.taaMotionFeedback = settings.taaMotionFeedback;
    render.taaReactiveFeedback = settings.taaReactiveFeedback;
    render.taaSharpeningStrength = settings.taaSharpeningStrength;
    render.debugView = settings.debugView;
    render.resolutionScale = settings.renderResolutionScale;
    render.materialTextureAnisotropy = settings.materialTextureAnisotropy;
    render.specularAaEnabled = settings.specularAaEnabled;
    render.opacityMicromapsEnabled = settings.opacityMicromapsEnabled;
    render.shadowRayBias = settings.shadowRayBias;
    render.shadowDistanceBias = settings.shadowDistanceBias;
    render.fireflyClamp = settings.fireflyClamp;
    render.restirGiTemporalMaxAge = settings.restirGiTemporalMaxAge;
    render.restirGiSpatialRounds = settings.restirGiSpatialRounds;
    render.restirGiSpatialRadius = settings.restirGiSpatialRadius;
    render.restirGiDepthThresholdScale = settings.restirGiDepthThresholdScale;
    render.restirGiSpatialCompatibilityThreshold = settings.restirGiSpatialCompatibilityThreshold;
    render.restirGiHalfResolution = settings.restirGiHalfResolution;
    render.restirGiVisibilityRayBudget = settings.restirGiVisibilityRayBudget;
    render.restirGiFinalStabilizationEnabled = settings.restirGiFinalStabilizationEnabled;
    render.adaptiveQualityMode = settings.adaptiveQualityMode;
    render.adaptiveGpuFrameTargetMs = settings.adaptiveGpuFrameTargetMs;
    render.usePhysicalCamera = settings.usePhysicalCamera;
    render.physicalAperture = settings.physicalAperture;
    render.physicalShutterSeconds = settings.physicalShutterSeconds;
    render.physicalIso = settings.physicalIso;
    render.physicalExposureCompensation = settings.physicalExposureCompensation;
    render.dofApertureRadius = settings.dofApertureRadius;
    render.dofFocusDistance = settings.dofFocusDistance;
    render.dofBladeCount = settings.dofBladeCount;
    render.dofBokehRotation = settings.dofBokehRotation;
    render.motionBlurEnabled = settings.motionBlurEnabled;
    render.motionBlurShutterOpen = settings.motionBlurShutterOpen;
    render.motionBlurShutterClose = settings.motionBlurShutterClose;
    render.homogeneousVolumeEnabled = settings.homogeneousVolumeEnabled;
    render.homogeneousVolumeScattering = settings.homogeneousVolumeScattering;
    render.homogeneousVolumeAbsorption = settings.homogeneousVolumeAbsorption;
    render.homogeneousVolumeAnisotropy = settings.homogeneousVolumeAnisotropy;
    render.mneeCausticsEnabled = settings.mneeCausticsEnabled;
    Environment& environment = document.environment();
    environment.enabled = settings.environmentEnabled;
    environment.intensity = settings.environmentIntensity;
    environment.rotation = settings.environmentRotation;
    environment.backgroundIntensity = settings.environmentBackgroundIntensity;
    applySceneWorldComponentsToDocumentSettings(document);
    document.markDirty(SceneUpdateKind::RendererSettingsOnly);
}

RendererSettings rendererSettingsFromDocument(const SceneDocument& document, RendererSettings settings) {
    const RenderSettings& render = document.renderSettings();
    const Environment& environment = document.environment();
    settings.renderPreset = render.renderPreset;
    settings.pathTracingEnabled = render.pathTracingEnabled;
    settings.cameraJitterEnabled = render.cameraJitterEnabled;
    settings.directLightingEnabled = render.directLightingEnabled;
    settings.maxBounces = render.maxBounces;
    settings.environmentDirectSamples = render.environmentDirectSamples;
    settings.toneMapper = render.toneMapper;
    settings.exposure = render.exposure;
    settings.gamma = render.gamma;
    settings.contrast = render.contrast;
    settings.saturation = render.saturation;
    settings.brightness = render.brightness;
    settings.whitePoint = render.whitePoint;
    settings.autoExposureEnabled = render.autoExposureEnabled;
    settings.targetLuminance = render.targetLuminance;
    settings.minExposure = render.minExposure;
    settings.maxExposure = render.maxExposure;
    settings.adaptationSpeed = render.adaptationSpeed;
    settings.histogramMinLogLuminance = render.histogramMinLogLuminance;
    settings.histogramMaxLogLuminance = render.histogramMaxLogLuminance;
    settings.histogramLowPercentile = render.histogramLowPercentile;
    settings.histogramHighPercentile = render.histogramHighPercentile;
    settings.histogramTargetPercentile = render.histogramTargetPercentile;
    settings.skyIntensity = render.skyIntensity;
    settings.indirectStrength = render.indirectStrength;
    settings.restirMode = render.restirMode;
    settings.restirGiEnabled = render.restirGiEnabled;
    settings.denoiserEnabled = render.denoiserEnabled;
    settings.denoiserBackend = render.denoiserBackend;
    settings.denoiseWhileMoving = render.denoiseWhileMoving;
    settings.samplesPerPixel = render.samplesPerPixel;
    settings.limitSamplesPerPixel = render.limitSamplesPerPixel;
    settings.atrousIterations = render.atrousIterations;
    settings.denoiserStrength = render.denoiserStrength;
    settings.denoiserMaxHistoryLength = render.denoiserMaxHistoryLength;
    settings.momentValidityThreshold = render.momentValidityThreshold;
    settings.taaEnabled = render.taaEnabled;
    settings.temporalUpscaler = render.temporalUpscaler;
    settings.dlssFrameGenerationEnabled = render.dlssFrameGenerationEnabled;
    settings.dlssRayReconstructionEnabled = render.dlssRayReconstructionEnabled;
    settings.dlssSharpeningStrength = render.dlssSharpeningStrength;
    settings.taaFeedback = render.taaFeedback;
    settings.taaMotionFeedback = render.taaMotionFeedback;
    settings.taaReactiveFeedback = render.taaReactiveFeedback;
    settings.taaSharpeningStrength = render.taaSharpeningStrength;
    settings.debugView = render.debugView;
    settings.renderResolutionScale = render.resolutionScale;
    settings.materialTextureAnisotropy = render.materialTextureAnisotropy;
    settings.specularAaEnabled = render.specularAaEnabled;
    settings.opacityMicromapsEnabled = render.opacityMicromapsEnabled;
    settings.shadowRayBias = render.shadowRayBias;
    settings.shadowDistanceBias = render.shadowDistanceBias;
    settings.fireflyClamp = render.fireflyClamp;
    settings.restirGiTemporalMaxAge = render.restirGiTemporalMaxAge;
    settings.restirGiSpatialRounds = render.restirGiSpatialRounds;
    settings.restirGiSpatialRadius = render.restirGiSpatialRadius;
    settings.restirGiDepthThresholdScale = render.restirGiDepthThresholdScale;
    settings.restirGiSpatialCompatibilityThreshold = render.restirGiSpatialCompatibilityThreshold;
    settings.restirGiHalfResolution = render.restirGiHalfResolution;
    settings.restirGiVisibilityRayBudget = render.restirGiVisibilityRayBudget;
    settings.restirGiFinalStabilizationEnabled = render.restirGiFinalStabilizationEnabled;
    settings.adaptiveQualityMode = render.adaptiveQualityMode;
    settings.adaptiveGpuFrameTargetMs = render.adaptiveGpuFrameTargetMs;
    settings.usePhysicalCamera = render.usePhysicalCamera;
    settings.physicalAperture = render.physicalAperture;
    settings.physicalShutterSeconds = render.physicalShutterSeconds;
    settings.physicalIso = render.physicalIso;
    settings.physicalExposureCompensation = render.physicalExposureCompensation;
    settings.dofApertureRadius = render.dofApertureRadius;
    settings.dofFocusDistance = render.dofFocusDistance;
    settings.dofBladeCount = render.dofBladeCount;
    settings.dofBokehRotation = render.dofBokehRotation;
    settings.motionBlurEnabled = render.motionBlurEnabled;
    settings.motionBlurShutterOpen = render.motionBlurShutterOpen;
    settings.motionBlurShutterClose = render.motionBlurShutterClose;
    settings.homogeneousVolumeEnabled = render.homogeneousVolumeEnabled;
    settings.homogeneousVolumeScattering = render.homogeneousVolumeScattering;
    settings.homogeneousVolumeAbsorption = render.homogeneousVolumeAbsorption;
    settings.homogeneousVolumeAnisotropy = render.homogeneousVolumeAnisotropy;
    settings.mneeCausticsEnabled = render.mneeCausticsEnabled;
    settings.environmentEnabled = environment.enabled;
    settings.environmentIntensity = environment.intensity;
    settings.environmentRotation = environment.rotation;
    settings.environmentBackgroundIntensity = environment.backgroundIntensity;
    SunController::applyToRendererSettings(document, settings);
    applySceneWorldComponentsToRendererSettings(document, settings);
    return settings;
}

bool rendererSettingsRequestDlss(const RendererSettings& settings) {
    return settings.temporalUpscaler == TemporalUpscaler::Dlss ||
        settings.dlssRayReconstructionEnabled ||
        settings.dlssFrameGenerationEnabled;
}

bool disableDlssForRendererReplacement(RendererSettings& settings) {
    if (!rendererSettingsRequestDlss(settings)) {
        return false;
    }
    settings.taaEnabled = true;
    settings.temporalUpscaler = TemporalUpscaler::TaaTsr;
    settings.dlssRayReconstructionEnabled = false;
    settings.dlssFrameGenerationEnabled = false;
    settings.denoiserEnabled = true;
    settings.denoiserBackend = DenoiserBackend::Engine;
    settings.denoiseWhileMoving = true;
    settings.renderPreset = RenderPreset::Custom;
    return true;
}

void applyDocumentMaterialAssignments(const SceneDocument& document, AssetManager& assets) {
    (void)document;
    (void)assets;
}

struct ImportedAssetHandleRemap {
    std::vector<TextureAssetHandle> textures;
    std::vector<MaterialAssetHandle> materials;
    std::vector<MeshAssetHandle> meshes;
};

std::filesystem::path editorRenderOutputRoot(const std::optional<ProjectContext>& project) {
    if (project.has_value()) {
        return project->savedRoot / "Renders";
    }
    return std::filesystem::current_path() / "out" / "editor_renders";
}

std::filesystem::path editorSceneAutosavePath(const ProjectContext& project, const std::optional<std::filesystem::path>& scenePath, const std::optional<std::filesystem::path>& gltfPath) {
    const std::string sceneName = scenePath.has_value()
        ? scenePath->stem().string()
        : (gltfPath.has_value() ? gltfPath->stem().string() : std::string("Untitled"));
    return project.savedRoot / "Autosaves" / (sceneName + "_autosave.rtlevel");
}

std::filesystem::path editorProjectAutosavePath(const ProjectContext& project) {
    const std::string name = project.name.empty() ? std::string("Project") : project.name;
    return project.savedRoot / "Autosaves" / (name + "_project_autosave.vproject");
}

std::filesystem::path editorAssetRegistryAutosavePath(const ProjectContext& project) {
    const std::string name = project.name.empty() ? std::string("Project") : project.name;
    return project.savedRoot / "Autosaves" / (name + "_asset_registry_autosave.json");
}

std::string safeAutosaveStem(std::string value) {
    if (value.empty()) {
        value = "material";
    }
    for (char& c : value) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch) && c != '_' && c != '-') {
            c = '_';
        }
    }
    return value;
}

std::filesystem::path editorMaterialAssetAutosavePath(const ProjectContext& project, const AssetRecord& record) {
    const std::string label = safeAutosaveStem(record.displayName.empty() ? record.guid : record.displayName);
    const std::string guidSuffix = record.guid.size() > 12u ? record.guid.substr(0, 12u) : record.guid;
    return project.savedRoot / "Autosaves" / (label + "_" + safeAutosaveStem(guidSuffix) + "_material_autosave.rtmaterial.json");
}

std::string editorTimestampString() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return stream.str();
}

nlohmann::json jsonVec3(glm::vec3 value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json jsonVec4(glm::vec4 value) {
    return nlohmann::json::array({value.x, value.y, value.z, value.w});
}

const char* materialAlphaModeName(uint32_t alphaMode) {
    switch (alphaMode) {
    case kMaterialAlphaModeMask: return "Mask";
    case kMaterialAlphaModeBlend: return "Blend";
    case kMaterialAlphaModeOpaque:
    default:
        return "Opaque";
    }
}

nlohmann::json textureHandleJson(TextureAssetHandle handle) {
    return handle.valid() ? nlohmann::json(handle.index) : nlohmann::json(nullptr);
}

nlohmann::json materialEditorOverrideJson(const MaterialAsset& material) {
    return {
        {"schema", "TransparentMaterialEditorOverrideV1"},
        {"name", material.name},
        {"baseColorFactor", jsonVec4(material.baseColorFactor)},
        {"metallicFactor", material.metallicFactor},
        {"roughnessFactor", material.roughnessFactor},
        {"emissiveFactor", jsonVec3(material.emissiveFactor)},
        {"emissiveStrength", material.emissiveStrength},
        {"alphaMode", materialAlphaModeName(material.alphaMode)},
        {"alphaCutoff", material.alphaCutoff},
        {"doubleSided", material.doubleSided != 0u},
        {"conductorOptics", {
            {"enabled", material.useConductorOptics != 0u},
            {"eta", jsonVec3(material.conductorEta)},
            {"k", jsonVec3(material.conductorK)},
        }},
        {"textures", {
            {"baseColor", textureHandleJson(material.baseColorTexture)},
            {"normal", textureHandleJson(material.normalTexture)},
            {"metallicRoughness", textureHandleJson(material.metallicRoughnessTexture)},
            {"emissive", textureHandleJson(material.emissiveTexture)},
            {"occlusion", textureHandleJson(material.occlusionTexture)},
        }},
    };
}

float jsonArrayFloat(const nlohmann::json& value, size_t index, float fallback) {
    return value.is_array() && index < value.size() && value[index].is_number()
        ? value[index].get<float>()
        : fallback;
}

uint32_t materialAlphaModeFromName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "mask") return kMaterialAlphaModeMask;
    if (name == "blend") return kMaterialAlphaModeBlend;
    return kMaterialAlphaModeOpaque;
}

TextureAssetHandle textureHandleFromJson(const nlohmann::json& value) {
    if (!value.is_number_unsigned()) {
        return {};
    }
    return TextureAssetHandle{value.get<uint32_t>()};
}

std::optional<MaterialAsset> materialAssetFromEditorOverrideJson(const nlohmann::json& root) {
    const nlohmann::json* source = nullptr;
    if (root.contains("editorMaterialOverride") && root["editorMaterialOverride"].is_object()) {
        source = &root["editorMaterialOverride"];
    } else if (root.contains("pbr") && root["pbr"].is_object() && root["pbr"].value("schema", std::string{}) == "TransparentMaterialEditorOverrideV1") {
        source = &root["pbr"];
    }
    if (source == nullptr) {
        return std::nullopt;
    }

    MaterialAsset material;
    material.name = source->value("name", root.value("displayName", std::string{}));
    if (source->contains("baseColorFactor")) {
        const nlohmann::json& color = (*source)["baseColorFactor"];
        material.baseColorFactor = glm::vec4(
            jsonArrayFloat(color, 0, material.baseColorFactor.x),
            jsonArrayFloat(color, 1, material.baseColorFactor.y),
            jsonArrayFloat(color, 2, material.baseColorFactor.z),
            jsonArrayFloat(color, 3, material.baseColorFactor.w));
    }
    material.metallicFactor = source->value("metallicFactor", material.metallicFactor);
    material.roughnessFactor = source->value("roughnessFactor", material.roughnessFactor);
    if (source->contains("emissiveFactor")) {
        const nlohmann::json& emissive = (*source)["emissiveFactor"];
        material.emissiveFactor = glm::vec3(
            jsonArrayFloat(emissive, 0, material.emissiveFactor.x),
            jsonArrayFloat(emissive, 1, material.emissiveFactor.y),
            jsonArrayFloat(emissive, 2, material.emissiveFactor.z));
    }
    material.emissiveStrength = source->value("emissiveStrength", material.emissiveStrength);
    material.alphaMode = materialAlphaModeFromName(source->value("alphaMode", std::string("Opaque")));
    material.alphaCutoff = source->value("alphaCutoff", material.alphaCutoff);
    material.doubleSided = source->value("doubleSided", false) ? 1u : 0u;
    if (source->contains("conductorOptics") && (*source)["conductorOptics"].is_object()) {
        const nlohmann::json& conductor = (*source)["conductorOptics"];
        material.useConductorOptics = conductor.value("enabled", false) ? 1u : 0u;
        if (conductor.contains("eta")) {
            const nlohmann::json& eta = conductor["eta"];
            material.conductorEta = glm::vec3(jsonArrayFloat(eta, 0, 0.0f), jsonArrayFloat(eta, 1, 0.0f), jsonArrayFloat(eta, 2, 0.0f));
        }
        if (conductor.contains("k")) {
            const nlohmann::json& k = conductor["k"];
            material.conductorK = glm::vec3(jsonArrayFloat(k, 0, 0.0f), jsonArrayFloat(k, 1, 0.0f), jsonArrayFloat(k, 2, 0.0f));
        }
    }
    if (source->contains("textures") && (*source)["textures"].is_object()) {
        const nlohmann::json& textures = (*source)["textures"];
        material.baseColorTexture = textureHandleFromJson(textures.value("baseColor", nlohmann::json{}));
        material.normalTexture = textureHandleFromJson(textures.value("normal", nlohmann::json{}));
        material.metallicRoughnessTexture = textureHandleFromJson(textures.value("metallicRoughness", nlohmann::json{}));
        material.emissiveTexture = textureHandleFromJson(textures.value("emissive", nlohmann::json{}));
        material.occlusionTexture = textureHandleFromJson(textures.value("occlusion", nlohmann::json{}));
    }
    return material;
}

bool applyEditorMaterialOverrideJson(MaterialAsset& material, const nlohmann::json& root) {
    std::optional<MaterialAsset> edited = materialAssetFromEditorOverrideJson(root);
    if (!edited.has_value()) {
        return false;
    }
    if (!edited->name.empty()) {
        material.name = edited->name;
    }
    material.baseColorFactor = edited->baseColorFactor;
    material.emissiveFactor = edited->emissiveFactor;
    material.metallicFactor = edited->metallicFactor;
    material.roughnessFactor = edited->roughnessFactor;
    material.emissiveStrength = edited->emissiveStrength;
    material.alphaCutoff = edited->alphaCutoff;
    material.alphaMode = edited->alphaMode;
    material.doubleSided = edited->doubleSided;
    material.useConductorOptics = edited->useConductorOptics;
    material.conductorEta = edited->conductorEta;
    material.conductorK = edited->conductorK;
    return true;
}

bool applyMaterialMetadataOverrideForGuid(
    const AssetRegistry* registry,
    const std::filesystem::path& root,
    const AssetGuid& guid,
    MaterialAsset& material) {
    if (registry == nullptr || guid.empty()) {
        return false;
    }
    const auto recordIt = std::find_if(registry->records().begin(), registry->records().end(), [&](const AssetRecord& record) {
        return record.guid == guid && record.type == AssetType::Material;
    });
    if (recordIt == registry->records().end() || recordIt->importedPath.empty()) {
        return false;
    }

    std::filesystem::path metadataPath = recordIt->importedPath;
    if (!metadataPath.is_absolute()) {
        metadataPath = root / metadataPath;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(metadataPath, ec)) {
        return false;
    }
    try {
        std::ifstream file(metadataPath);
        nlohmann::json json;
        file >> json;
        const AssetGuid metadataGuid = json.value("guid", guid);
        if (!metadataGuid.empty() && metadataGuid != guid) {
            return false;
        }
        return applyEditorMaterialOverrideJson(material, json);
    } catch (const std::exception& error) {
        std::cerr << "Material metadata override load failed: " << metadataPath.string() << " " << error.what() << '\n';
        return false;
    }
}

bool openDirectoryInShell(const std::filesystem::path& directory) {
    if (directory.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(directory, ec);
    const std::filesystem::path target = ec ? directory : absolute;
    if (!std::filesystem::is_directory(target, ec)) {
        return false;
    }
#if defined(_WIN32)
    return reinterpret_cast<intptr_t>(ShellExecuteA(nullptr, "open", target.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    (void)target;
    return false;
#endif
}

bool openFileInShell(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path target = ec ? path : absolute;
    if (!std::filesystem::is_regular_file(target, ec)) {
        return false;
    }
#if defined(_WIN32)
    return reinterpret_cast<intptr_t>(ShellExecuteA(nullptr, "open", target.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    (void)target;
    return false;
#endif
}

void appendRenderHistoryEvent(const std::filesystem::path& outputRoot, const char* action, const SceneDocument& scene, const RendererSettings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(outputRoot, ec);
    if (ec) {
        return;
    }

    nlohmann::json event;
    event["action"] = action;
    event["scene_dirty"] = scene.dirty();
    event["debug_view"] = rendererDebugViewName(settings.debugView);
    event["samples_per_pixel"] = settings.samplesPerPixel;
    event["limit_samples_per_pixel"] = settings.limitSamplesPerPixel;
    event["render_resolution_scale"] = settings.renderResolutionScale;

    nlohmann::json history = nlohmann::json::array();
    const std::filesystem::path historyPath = outputRoot / "render_history.json";
    if (std::filesystem::exists(historyPath, ec)) {
        try {
            std::ifstream in(historyPath);
            in >> history;
            if (!history.is_array()) {
                history = nlohmann::json::array();
            }
        } catch (...) {
            history = nlohmann::json::array();
        }
    }
    history.push_back(std::move(event));
    std::ofstream out(historyPath);
    if (out.is_open()) {
        out << history.dump(2);
    }
}

const char* editorRenderJobAction(EditorRenderJobKind kind) {
    switch (kind) {
    case EditorRenderJobKind::CurrentViewport: return "RenderCurrentViewport";
    case EditorRenderJobKind::Image: return "RenderImage";
    case EditorRenderJobKind::Sequence: return "RenderSequence";
    case EditorRenderJobKind::None:
    default: return "Render";
    }
}

const char* editorRenderJobTitle(EditorRenderJobKind kind) {
    switch (kind) {
    case EditorRenderJobKind::CurrentViewport: return "Render Current Viewport";
    case EditorRenderJobKind::Image: return "Render Image";
    case EditorRenderJobKind::Sequence: return "Render Sequence";
    case EditorRenderJobKind::None:
    default: return "Render";
    }
}

std::string editorRenderTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif
    std::ostringstream out;
    out << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return out.str();
}

std::filesystem::path editorSequenceFramePath(const std::filesystem::path& outputRoot, uint32_t frameIndex) {
    std::ostringstream name;
    name << "frame_" << std::setw(4) << std::setfill('0') << frameIndex << ".png";
    return outputRoot / name.str();
}

uint32_t editorRenderImageTargetFrames(const PathTracerRenderer& renderer) {
    const RendererSettings& settings = renderer.settings();
    if (settings.accumulationLimit == 0u) {
        return 64u;
    }
    const uint32_t effectiveSpp = std::max(1u, renderer.effectiveSamplesPerPixel());
    return std::max(1u, (settings.accumulationLimit + effectiveSpp - 1u) / effectiveSpp);
}

constexpr uint32_t kMaxEditorRenderSequenceFramesPerTimelineFrame = 65536u;

class AppSceneDocumentSnapshotCommand final : public ICommand {
public:
    AppSceneDocumentSnapshotCommand(SceneDocument& document, SceneDocument before, SceneDocument after, SceneUpdateKind updateKind, std::string label)
        : document_(document), before_(std::move(before)), after_(std::move(after)), updateKind_(updateKind), label_(std::move(label)) {}

    void undo() override { document_ = before_; document_.markDirty(updateKind_); }
    void redo() override { document_ = after_; document_.markDirty(updateKind_); }
    [[nodiscard]] const std::string& label() const override { return label_; }

private:
    SceneDocument& document_;
    SceneDocument before_;
    SceneDocument after_;
    SceneUpdateKind updateKind_ = SceneUpdateKind::TopologyChanged;
    std::string label_;
};

class AssetManagerSnapshotCommand final : public ICommand {
public:
    AssetManagerSnapshotCommand(AssetManager& assets, SceneDocument& document, AssetManager before, AssetManager after, SceneUpdateKind updateKind, std::string label)
        : assets_(assets), document_(document), before_(std::move(before)), after_(std::move(after)), updateKind_(updateKind), label_(std::move(label)) {}

    void undo() override { assets_ = before_; document_.markDirty(updateKind_); }
    void redo() override { assets_ = after_; document_.markDirty(updateKind_); }
    [[nodiscard]] const std::string& label() const override { return label_; }

private:
    AssetManager& assets_;
    SceneDocument& document_;
    AssetManager before_;
    AssetManager after_;
    SceneUpdateKind updateKind_ = SceneUpdateKind::MaterialOnly;
    std::string label_;
};

class SceneAndAssetsSnapshotCommand final : public ICommand {
public:
    SceneAndAssetsSnapshotCommand(
        SceneDocument& document,
        AssetManager& assets,
        SceneDocument beforeDocument,
        AssetManager beforeAssets,
        SceneDocument afterDocument,
        AssetManager afterAssets,
        SceneUpdateKind updateKind,
        std::string label)
        : document_(document),
          assets_(assets),
          beforeDocument_(std::move(beforeDocument)),
          beforeAssets_(std::move(beforeAssets)),
          afterDocument_(std::move(afterDocument)),
          afterAssets_(std::move(afterAssets)),
          updateKind_(updateKind),
          label_(std::move(label)) {}

    void undo() override {
        document_ = beforeDocument_;
        assets_ = beforeAssets_;
        document_.markDirty(updateKind_);
    }

    void redo() override {
        document_ = afterDocument_;
        assets_ = afterAssets_;
        document_.markDirty(updateKind_);
    }

    [[nodiscard]] const std::string& label() const override { return label_; }

private:
    SceneDocument& document_;
    AssetManager& assets_;
    SceneDocument beforeDocument_;
    AssetManager beforeAssets_;
    SceneDocument afterDocument_;
    AssetManager afterAssets_;
    SceneUpdateKind updateKind_ = SceneUpdateKind::MaterialOnly;
    std::string label_;
};

void ensureMaterialSlotsForRenderer(MeshRenderer& renderer, const AssetManager& assets) {
    if (!renderer.materialSlots.empty()) {
        return;
    }
    const MeshAsset* mesh = assets.mesh(renderer.mesh);
    if (mesh == nullptr) {
        return;
    }
    renderer.materialSlots.reserve(mesh->primitives.size());
    for (size_t i = 0; i < mesh->primitives.size(); ++i) {
        renderer.materialSlots.push_back(MaterialSlot{
            .name = "Primitive " + std::to_string(i),
            .material = mesh->primitives[i].material,
        });
    }
}

TextureAssetHandle remapTextureHandle(const ImportedAssetHandleRemap& remap, TextureAssetHandle handle) {
    return handle.valid() && handle.index < remap.textures.size() ? remap.textures[handle.index] : TextureAssetHandle{};
}

MaterialAssetHandle remapMaterialHandle(const ImportedAssetHandleRemap& remap, MaterialAssetHandle handle) {
    return handle.valid() && handle.index < remap.materials.size() ? remap.materials[handle.index] : MaterialAssetHandle{};
}

MeshAssetHandle remapMeshHandle(const ImportedAssetHandleRemap& remap, MeshAssetHandle handle) {
    return handle.valid() && handle.index < remap.meshes.size() ? remap.meshes[handle.index] : MeshAssetHandle{};
}

void remapMaterialTextures(MaterialAsset& material, const ImportedAssetHandleRemap& remap) {
    auto remapTexture = [&](TextureAssetHandle& handle) { handle = remapTextureHandle(remap, handle); };
    remapTexture(material.baseColorTexture);
    remapTexture(material.normalTexture);
    remapTexture(material.metallicRoughnessTexture);
    remapTexture(material.emissiveTexture);
    remapTexture(material.clearcoatTexture);
    remapTexture(material.clearcoatRoughnessTexture);
    remapTexture(material.clearcoatNormalTexture);
    remapTexture(material.transmissionTexture);
    remapTexture(material.volumeThicknessTexture);
    remapTexture(material.specularTexture);
    remapTexture(material.specularColorTexture);
    remapTexture(material.sheenColorTexture);
    remapTexture(material.sheenRoughnessTexture);
    remapTexture(material.iridescenceTexture);
    remapTexture(material.iridescenceThicknessTexture);
    remapTexture(material.anisotropyTexture);
    remapTexture(material.occlusionTexture);
}

ImportedAssetHandleRemap appendImportedAssets(AssetManager& destination, const AssetManager& source) {
    ImportedAssetHandleRemap remap;
    remap.textures.reserve(source.textures().size());
    remap.materials.reserve(source.materials().size());
    remap.meshes.reserve(source.meshes().size());

    for (const TextureAsset& texture : source.textures()) {
        remap.textures.push_back(destination.addTexture(texture));
    }
    for (MaterialAsset material : source.materials()) {
        remapMaterialTextures(material, remap);
        remap.materials.push_back(destination.addMaterial(std::move(material)));
    }
    for (MeshAsset mesh : source.meshes()) {
        for (MeshPrimitiveAsset& primitive : mesh.primitives) {
            primitive.material = remapMaterialHandle(remap, primitive.material);
            for (auto& variant : primitive.materialVariants) {
                variant.material = remapMaterialHandle(remap, variant.material);
            }
        }
        remap.meshes.push_back(destination.addMesh(std::move(mesh)));
    }
    return remap;
}

void remapSceneAssetHandles(SceneAsset& scene, const ImportedAssetHandleRemap& remap) {
    for (TextureAssetHandle& texture : scene.textures) {
        texture = remapTextureHandle(remap, texture);
    }
    for (MaterialAssetHandle& material : scene.materials) {
        material = remapMaterialHandle(remap, material);
    }
    for (MeshAssetHandle& mesh : scene.meshes) {
        mesh = remapMeshHandle(remap, mesh);
    }
    for (SceneNodeAsset& node : scene.nodes) {
        node.mesh = remapMeshHandle(remap, node.mesh);
    }
}

std::string importModeForRecord(const AssetRecord& record, const std::filesystem::path& root);

bool appendCachedPrefabRuntimeAssets(
    const AssetRecord& prefabRecord,
    const std::filesystem::path& root,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& explicitCachePath,
    const AssetRegistry* registry,
    AssetManager& destination,
    PrefabRuntimeBindings& bindings,
    std::string* error) {
    std::filesystem::path cachePath = explicitCachePath.empty() ? SceneCache::cachePathFor(sourcePath) : explicitCachePath;
    if (!cachePath.is_absolute()) {
        cachePath = root / cachePath;
    }
    const bool sourceExists = std::filesystem::exists(sourcePath);
    if (sourceExists && !SceneCache::isCacheValid(sourcePath, cachePath)) {
        return false;
    }
    auto cached = SceneCache::load(cachePath);
    if (!cached.has_value()) {
        return false;
    }

    std::vector<TextureAssetHandle> textures;
    textures.reserve(cached->textures.size());
    for (const CachedTextureData& cachedTex : cached->textures) {
        TextureAsset texture;
        texture.name = cachedTex.name;
        texture.sourcePath = cachedTex.sourcePath.empty() ? sourcePath : std::filesystem::path(cachedTex.sourcePath);
        texture.width = cachedTex.width;
        texture.height = cachedTex.height;
        texture.channels = cachedTex.channels;
        texture.mipLevels = cachedTex.mipLevels;
        texture.srgb = cachedTex.srgb;
        texture.fallback = cachedTex.fallback;
        texture.isCompressed = cachedTex.isCompressed;
        texture.linearColorSpace = cachedTex.linearColorSpace;
        texture.format = static_cast<VkFormat>(cachedTex.format);
        texture.compressedFormat = static_cast<VkFormat>(cachedTex.compressedFormat);
        texture.rgba8 = cachedTex.rgba8;
        texture.mipData = cachedTex.mipData;
        texture.sampler.minFilter = static_cast<TextureFilter>(cachedTex.minFilter);
        texture.sampler.magFilter = static_cast<TextureFilter>(cachedTex.magFilter);
        texture.sampler.wrapS = static_cast<TextureWrap>(cachedTex.wrapS);
        texture.sampler.wrapT = static_cast<TextureWrap>(cachedTex.wrapT);
        textures.push_back(destination.addTexture(std::move(texture)));
    }

    auto textureHandleFor = [&](int32_t index) -> TextureAssetHandle {
        if (index < 0 || static_cast<size_t>(index) >= textures.size()) {
            return TextureAssetHandle{};
        }
        return textures[static_cast<size_t>(index)];
    };

    const std::string sourceHash = prefabRecord.sourceHash.empty()
        ? assetSourceHashForPath(sourcePath)
        : prefabRecord.sourceHash;
    AssetImportRequest hashRequest;
    hashRequest.sourcePath = sourcePath;
    hashRequest.mode = importModeForRecord(prefabRecord, root);
    hashRequest.settings = prefabRecord.importSettings;
    const std::string settingsHash = prefabRecord.importSettingsHash.empty()
        ? assetImportSettingsHashForRequest(hashRequest)
        : prefabRecord.importSettingsHash;

    std::vector<MaterialAssetHandle> materials;
    materials.reserve(cached->materials.size());
    for (size_t materialIndex = 0; materialIndex < cached->materials.size(); ++materialIndex) {
        const CachedMaterialData& cachedMat = cached->materials[materialIndex];
        MaterialAsset material;
        material.name = cachedMat.name;
        material.baseColorFactor = cachedMat.baseColorFactor;
        material.emissiveFactor = cachedMat.emissiveFactor;
        material.metallicFactor = cachedMat.metallicFactor;
        material.roughnessFactor = cachedMat.roughnessFactor;
        material.iorFactor = cachedMat.iorFactor;
        material.alphaCutoff = cachedMat.alphaCutoff;
        material.alphaMode = cachedMat.alphaMode;
        material.doubleSided = cachedMat.doubleSided;
        material.hasIor = cachedMat.hasIor;
        material.hasClearcoat = cachedMat.hasClearcoat;
        material.clearcoatFactor = cachedMat.clearcoatFactor;
        material.clearcoatRoughnessFactor = cachedMat.clearcoatRoughnessFactor;
        material.hasTransmission = cachedMat.hasTransmission;
        material.transmissionFactor = cachedMat.transmissionFactor;
        material.hasVolume = cachedMat.hasVolume;
        material.volumeThicknessFactor = cachedMat.volumeThicknessFactor;
        material.volumeAttenuationDistance = cachedMat.volumeAttenuationDistance;
        material.volumeAttenuationColor = cachedMat.volumeAttenuationColor;
        material.hasDispersion = cachedMat.hasDispersion;
        material.dispersionFactor = cachedMat.dispersionFactor;
        material.hasSpecular = cachedMat.hasSpecular;
        material.specularFactor = cachedMat.specularFactor;
        material.specularColorFactor = cachedMat.specularColorFactor;
        material.hasSheen = cachedMat.hasSheen;
        material.sheenColorFactor = cachedMat.sheenColorFactor;
        material.sheenRoughnessFactor = cachedMat.sheenRoughnessFactor;
        material.hasIridescence = cachedMat.hasIridescence;
        material.iridescenceFactor = cachedMat.iridescenceFactor;
        material.iridescenceIor = cachedMat.iridescenceIor;
        material.iridescenceThicknessMinimum = cachedMat.iridescenceThicknessMinimum;
        material.iridescenceThicknessMaximum = cachedMat.iridescenceThicknessMaximum;
        material.hasEmissiveStrength = cachedMat.hasEmissiveStrength;
        material.emissiveStrength = cachedMat.emissiveStrength;
        material.hasAnisotropy = cachedMat.hasAnisotropy;
        material.anisotropyStrength = cachedMat.anisotropyStrength;
        material.anisotropyRotation = cachedMat.anisotropyRotation;
        material.occlusionStrength = cachedMat.occlusionStrength;
        material.useConductorOptics = cachedMat.useConductorOptics;
        material.conductorEta = cachedMat.conductorEta;
        material.conductorK = cachedMat.conductorK;
        material.baseColorTexture = textureHandleFor(cachedMat.baseColorTextureIndex);
        material.normalTexture = textureHandleFor(cachedMat.normalTextureIndex);
        material.metallicRoughnessTexture = textureHandleFor(cachedMat.metallicRoughnessTextureIndex);
        material.emissiveTexture = textureHandleFor(cachedMat.emissiveTextureIndex);
        material.clearcoatTexture = textureHandleFor(cachedMat.clearcoatTextureIndex);
        material.clearcoatRoughnessTexture = textureHandleFor(cachedMat.clearcoatRoughnessTextureIndex);
        material.clearcoatNormalTexture = textureHandleFor(cachedMat.clearcoatNormalTextureIndex);
        material.transmissionTexture = textureHandleFor(cachedMat.transmissionTextureIndex);
        material.volumeThicknessTexture = textureHandleFor(cachedMat.volumeThicknessTextureIndex);
        material.specularTexture = textureHandleFor(cachedMat.specularTextureIndex);
        material.specularColorTexture = textureHandleFor(cachedMat.specularColorTextureIndex);
        material.sheenColorTexture = textureHandleFor(cachedMat.sheenColorTextureIndex);
        material.sheenRoughnessTexture = textureHandleFor(cachedMat.sheenRoughnessTextureIndex);
        material.iridescenceTexture = textureHandleFor(cachedMat.iridescenceTextureIndex);
        material.iridescenceThicknessTexture = textureHandleFor(cachedMat.iridescenceThicknessTextureIndex);
        material.anisotropyTexture = textureHandleFor(cachedMat.anisotropyTextureIndex);
        material.occlusionTexture = textureHandleFor(cachedMat.occlusionTextureIndex);
        material.baseColorTextureTransform = cachedMat.baseColorTextureTransform;
        material.metallicRoughnessTextureTransform = cachedMat.metallicRoughnessTextureTransform;
        material.normalTextureTransform = cachedMat.normalTextureTransform;
        material.emissiveTextureTransform = cachedMat.emissiveTextureTransform;
        material.occlusionTextureTransform = cachedMat.occlusionTextureTransform;
        material.clearcoatTextureTransform = cachedMat.clearcoatTextureTransform;
        material.clearcoatRoughnessTextureTransform = cachedMat.clearcoatRoughnessTextureTransform;
        material.clearcoatNormalTextureTransform = cachedMat.clearcoatNormalTextureTransform;
        material.transmissionTextureTransform = cachedMat.transmissionTextureTransform;
        material.volumeThicknessTextureTransform = cachedMat.volumeThicknessTextureTransform;
        material.specularTextureTransform = cachedMat.specularTextureTransform;
        material.specularColorTextureTransform = cachedMat.specularColorTextureTransform;
        material.sheenColorTextureTransform = cachedMat.sheenColorTextureTransform;
        material.sheenRoughnessTextureTransform = cachedMat.sheenRoughnessTextureTransform;
        material.iridescenceTextureTransform = cachedMat.iridescenceTextureTransform;
        material.iridescenceThicknessTextureTransform = cachedMat.iridescenceThicknessTextureTransform;
        material.anisotropyTextureTransform = cachedMat.anisotropyTextureTransform;
        material.shaderCompatibilityMask = cachedMat.shaderCompatibilityMask;
        (void)applyMaterialMetadataOverrideForGuid(
            registry,
            root,
            importedAssetGuidFor(sourceHash, settingsHash, "Material", materialIndex),
            material);
        materials.push_back(destination.addMaterial(std::move(material)));
    }

    std::vector<MeshAssetHandle> meshes;
    meshes.reserve(cached->meshes.size());
    for (const CachedMeshData& cachedMesh : cached->meshes) {
        MeshAsset mesh;
        mesh.name = cachedMesh.name;
        mesh.vertices = cachedMesh.vertices;
        mesh.indices = cachedMesh.indices;
        mesh.defaultMorphWeights = cachedMesh.defaultMorphWeights;
        for (const CachedPrimitiveData& cachedPrim : cachedMesh.primitives) {
            MeshPrimitiveAsset primitive;
            primitive.firstVertex = cachedPrim.firstVertex;
            primitive.vertexCount = cachedPrim.vertexCount;
            primitive.firstIndex = cachedPrim.firstIndex;
            primitive.indexCount = cachedPrim.indexCount;
            primitive.morphTargets = cachedPrim.morphTargets;
            if (cachedPrim.materialIndex >= 0 && static_cast<size_t>(cachedPrim.materialIndex) < materials.size()) {
                primitive.material = materials[static_cast<size_t>(cachedPrim.materialIndex)];
            } else if (!materials.empty()) {
                primitive.material = materials.front();
            }
            for (const auto& cachedVariant : cachedPrim.materialVariants) {
                if (cachedVariant.materialIndex >= 0 && static_cast<size_t>(cachedVariant.materialIndex) < materials.size()) {
                    primitive.materialVariants.push_back(MeshPrimitiveAsset::MaterialVariant{
                        .variantIndex = cachedVariant.variantIndex,
                        .variantName = cachedVariant.variantName,
                        .material = materials[static_cast<size_t>(cachedVariant.materialIndex)],
                    });
                }
            }
            updatePrimitiveAlphaClassification(primitive, destination.material(primitive.material));
            mesh.primitives.push_back(primitive);
        }
        meshes.push_back(destination.addMesh(std::move(mesh)));
    }

    for (size_t i = 0; i < meshes.size(); ++i) {
        bindings.meshes[importedAssetGuidFor(sourceHash, settingsHash, "Mesh", i)] = meshes[i];
    }
    for (size_t i = 0; i < materials.size(); ++i) {
        bindings.materials[importedAssetGuidFor(sourceHash, settingsHash, "Material", i)] = materials[i];
    }

    if (bindings.meshes.empty() && bindings.materials.empty()) {
        if (error != nullptr) {
            *error = "Prefab cache contains no runtime mesh/material payload: " + cachePath.string();
        }
        return false;
    }
    std::cout << "Prefab runtime assets restored from scene cache: " << cachePath.string() << '\n';
    return true;
}

std::filesystem::path resolveAssetRecordPath(const AssetRecord& record, const std::filesystem::path& root) {
    std::filesystem::path path = record.importedPath;
    if (path.empty()) {
        return path;
    }
    if (!path.is_absolute()) {
        path = root / path;
    }
    return path;
}

std::filesystem::path resolveAssetCachePath(const AssetRecord& record, const std::filesystem::path& root) {
    std::filesystem::path path = record.cachePath;
    if (path.empty()) {
        return path;
    }
    if (!path.is_absolute()) {
        path = root / path;
    }
    return path;
}

std::filesystem::path resolveAssetSourcePath(const AssetRecord& record, const std::filesystem::path& root) {
    std::filesystem::path path = record.sourcePath;
    if (path.empty()) {
        return path;
    }
    if (!path.is_absolute()) {
        path = root / path;
    }
    return path;
}

std::string assetRegistryPathValue(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (!ec) {
        bool escapesRoot = false;
        for (const auto& part : relative) {
            if (part == "..") {
                escapesRoot = true;
                break;
            }
        }
        if (!escapesRoot) {
            return relative.generic_string();
        }
    }
    return path.generic_string();
}

std::string importModeForRecord(const AssetRecord& record, const std::filesystem::path& root) {
    const std::filesystem::path importedPath = resolveAssetRecordPath(record, root);
    try {
        std::ifstream file(importedPath);
        if (file.is_open()) {
            nlohmann::json json;
            file >> json;
            return json.value("mode", std::string("ImportAsset"));
        }
    } catch (...) {
    }
    return "ImportAsset";
}

bool appendPrefabRuntimeAssets(
    const AssetRecord& prefabRecord,
    const PrefabAsset& prefab,
    const std::filesystem::path& root,
    const AssetRegistry* registry,
    AssetManager& destination,
    PrefabRuntimeBindings& bindings,
    std::string* error) {
    const std::filesystem::path sourcePath = resolveAssetSourcePath(prefabRecord, root);
    if (appendCachedPrefabRuntimeAssets(prefabRecord, root, sourcePath, prefab.runtimeCachePath, registry, destination, bindings, error)) {
        return true;
    }
    if (error != nullptr) {
        const std::filesystem::path cachePath = prefab.runtimeCachePath.empty()
            ? resolveAssetCachePath(prefabRecord, root)
            : (prefab.runtimeCachePath.is_absolute() ? prefab.runtimeCachePath : root / prefab.runtimeCachePath);
        *error = "Prefab cooked payload is unavailable or stale; reimport the asset to rebuild cache";
        if (!cachePath.empty()) {
            *error += ": " + cachePath.string();
        }
    }
    return false;
}

uint32_t rebindGuidBackedRenderers(SceneDocument& document, const PrefabRuntimeBindings& bindings) {
    uint32_t rebound = 0;
    for (Entity* entity : document.registry().entities()) {
        if (!entity->meshRenderer.has_value()) {
            continue;
        }
        MeshRenderer& renderer = *entity->meshRenderer;
        if (!renderer.meshGuid.empty()) {
            const auto meshIt = bindings.meshes.find(renderer.meshGuid);
            if (meshIt != bindings.meshes.end()) {
                renderer.mesh = meshIt->second;
                ++rebound;
            }
        }
        for (MaterialSlot& slot : renderer.materialSlots) {
            if (!slot.materialGuid.empty()) {
                const auto materialIt = bindings.materials.find(slot.materialGuid);
                if (materialIt != bindings.materials.end()) {
                    slot.material = materialIt->second;
                }
            }
            if (slot.overrideMaterialGuid.has_value() && !slot.overrideMaterialGuid->empty()) {
                const auto materialIt = bindings.materials.find(*slot.overrideMaterialGuid);
                if (materialIt != bindings.materials.end()) {
                    slot.overrideMaterial = materialIt->second;
                }
            }
        }
    }
    return rebound;
}

std::filesystem::path resolveProjectRoot() {
    std::filesystem::path candidate = std::filesystem::current_path();
    while (!candidate.empty()) {
        auto shadersDir = candidate / "native" / "vulkan" / "shaders";
        if (std::filesystem::exists(shadersDir / "pathtrace.rgen")) {
            return candidate;
        }
        candidate = candidate.parent_path();
    }
    return std::filesystem::current_path();
}

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    const Entity* current = &entity;
    glm::mat4 result(1.0f);
    constexpr int maxDepth = 512;
    for (int depth = 0; depth < maxDepth && current != nullptr; ++depth) {
        result = current->transform.localMatrix() * result;
        if (!current->parent.valid()) {
            break;
        }
        const Entity* parent = registry.entity(current->parent);
        if (parent == nullptr) {
            break;
        }
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return result;
}

float activeCameraFovRadians(const SceneDocument& document) {
    if (const Entity* cameraEntity = document.registry().entity(document.activeCamera())) {
        if (cameraEntity->camera.has_value()) {
            return cameraEntity->camera->verticalFovRadians;
        }
    }
    return glm::radians(60.0f);
}

void expandEntityBounds(
    const SceneRegistry& registry,
    const AssetManager& assets,
    const Entity& entity,
    glm::vec3& minBounds,
    glm::vec3& maxBounds,
    bool& hasBounds) {
    const glm::mat4 world = entityWorldMatrix(registry, entity);
    if (entity.meshRenderer.has_value()) {
        if (const MeshAsset* mesh = assets.mesh(entity.meshRenderer->mesh)) {
            for (const MeshVertex& vertex : mesh->vertices) {
                const glm::vec3 point = glm::vec3(world * glm::vec4(vertex.position, 1.0f));
                minBounds = glm::min(minBounds, point);
                maxBounds = glm::max(maxBounds, point);
                hasBounds = true;
            }
        }
    }
    if (!hasBounds) {
        const glm::vec3 point = glm::vec3(world[3]);
        minBounds = glm::min(minBounds, point - glm::vec3(0.5f));
        maxBounds = glm::max(maxBounds, point + glm::vec3(0.5f));
        hasBounds = true;
    }
    for (EntityId childId : entity.children) {
        if (const Entity* child = registry.entity(childId)) {
            expandEntityBounds(registry, assets, *child, minBounds, maxBounds, hasBounds);
        }
    }
}

EntityId duplicateEntityRecursive(SceneRegistry& registry, Entity source, EntityId parent) {
    const EntityId copyId = registry.createEntity(source.name.empty() ? "Entity Copy" : source.name + " Copy");
    Entity* copy = registry.entity(copyId);
    if (copy == nullptr) {
        return {};
    }

    copy->transform = source.transform;
    copy->transform.dirty = true;
    copy->defaultTransform = copy->transform;
    copy->meshRenderer = source.meshRenderer;
    copy->light = source.light;
    copy->sun = source.sun;
    copy->camera = source.camera;
    if (copy->camera.has_value()) {
        copy->camera->active = false;
    }
    copy->parent = parent;
    copy->children.clear();
    if (Entity* parentEntity = registry.entity(parent)) {
        parentEntity->children.push_back(copyId);
    }

    const std::vector<EntityId> children = source.children;
    for (EntityId childId : children) {
        if (const Entity* child = registry.entity(childId)) {
            (void)duplicateEntityRecursive(registry, *child, copyId);
        }
    }
    return copyId;
}

void windowFocusCallback(GLFWwindow* window, int focused) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->onWindowFocusChanged(focused == GLFW_TRUE);
    }
}

void fileDropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->onFilesDropped(count, paths);
    }
}
}

Application::Application(
    RendererDebugView debugView,
    std::optional<std::filesystem::path> gltfPath,
    std::optional<std::filesystem::path> hdrPath,
    std::optional<std::filesystem::path> scenePath,
    std::optional<bool> denoiserOverride,
    std::optional<RestirMode> restirModeOverride,
    std::optional<RenderPreset> renderPresetOverride,
    std::optional<bool> restirGiOverride,
    std::optional<bool> opacityMicromapOverride,
    std::optional<uint32_t> opacityMicromapSubdivisionOverride,
    bool debugViewOverride,
    bool validationCameraMotion,
    bool validationObjectMotion,
    bool headless,
    bool disableAsyncCompute,
    bool singleQueueFallback,
    bool disableResourceAliasing)
    : debugView_(debugView),
      gltfPath_(std::move(gltfPath)),
      hdrPath_(std::move(hdrPath)),
      scenePath_(std::move(scenePath)),
      denoiserOverride_(denoiserOverride),
      restirModeOverride_(restirModeOverride),
      renderPresetOverride_(renderPresetOverride),
      restirGiOverride_(restirGiOverride),
      opacityMicromapOverride_(opacityMicromapOverride),
      opacityMicromapSubdivisionOverride_(opacityMicromapSubdivisionOverride),
      debugViewOverride_(debugViewOverride),
      validationCameraMotion_(validationCameraMotion),
      validationObjectMotion_(validationObjectMotion),
      disableAsyncCompute_(disableAsyncCompute),
      singleQueueFallback_(singleQueueFallback),
      disableResourceAliasing_(disableResourceAliasing),
      headless_(headless) {
    if (!headless_) {
        initWindow();
    }
    initVulkan();
}

Application::~Application() {
    asyncSceneLoader_.requestCancel();
    asyncSceneLoader_.wait();
    waitForAssetImportWorker();
    if (commandSystem_) {
        commandSystem_->waitIdle();
    }

    if (uiOverlay_) {
        (void)saveActiveEditorPreferences();
    }
    writeCrashMarker(false);
    if (commandSystem_) {
        commandSystem_->setPathTracer(nullptr);
    }
    retiredPathTracers_.clear();
    pathTracer_.reset();
    commandSystem_.reset();
    uiOverlay_.reset();
    pipelineDemo_.reset();
    resourceDemo_.reset();
    swapchain_.reset();
    uploader_.reset();
    uploadContext_.reset();
    allocator_.reset();
    context_.reset();

    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
    }
    if (!headless_) {
        glfwTerminate();
    }
}

void Application::run(uint32_t maxFrames) {
    mainLoop(maxFrames);
}

void Application::runHeadless(uint32_t warmupFrames, uint32_t totalFrames) {
    warmupFrameCount_ = warmupFrames;
    totalFrameCount_ = totalFrames;
    nextDiagnosticFrameIndex_ = 0;
    cpuFrameTimings_.clear();
    gpuFrameTimings_.clear();
    perFrameGpuTimings_.clear();
    cpuFrameTimings_.reserve(totalFrames);
    gpuFrameTimings_.reserve(totalFrames);
    perFrameGpuTimings_.reserve(totalFrames);

    const uint32_t renderedFrames = std::max(warmupFrames, totalFrames);
    const auto start = std::chrono::steady_clock::now();
    float seconds = 0.0f;

    for (uint32_t frameCount = 0; frameCount < renderedFrames; ++frameCount) {
        const auto frameStart = std::chrono::steady_clock::now();

        const float rawDeltaSeconds = 1.0f / 60.0f;
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;
        applyValidationObjectMotion(nextDiagnosticFrameIndex_);
        applyValidationCameraMotion(nextDiagnosticFrameIndex_++);
        updateAnimationPlayers(deltaSeconds);
        if (beginFrameCapture_) {
            beginFrameCapture_(frameCount + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(frameCount + 1u);
        }
        seconds += deltaSeconds;

        const auto frameEnd = std::chrono::steady_clock::now();
        const float cpuMs = std::chrono::duration<float, std::milli>(frameEnd - frameStart).count();
        cpuFrameTimings_.push_back(cpuMs);

        if (pathTracer_) {
            const auto& timings = pathTracer_->timings();
            gpuFrameTimings_.push_back(timings.totalMs());
            perFrameGpuTimings_.push_back(timings);
        }
    }
    commandSystem_->waitIdle();
}

void Application::renderFrames(uint32_t count) {
    float seconds = lastFrameSeconds_ + 1.0f / 60.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const float rawDeltaSeconds = 1.0f / 60.0f;
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;
        applyValidationObjectMotion(nextDiagnosticFrameIndex_);
        applyValidationCameraMotion(nextDiagnosticFrameIndex_++);
        updateAnimationPlayers(deltaSeconds);
        if (beginFrameCapture_) {
            beginFrameCapture_(i + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(i + 1u);
        }
        seconds += deltaSeconds;
    }
    commandSystem_->waitIdle();
}

std::filesystem::path Application::assetResolutionRoot() const {
    if (project_.has_value()) {
        return project_->projectRoot;
    }
    if (!assetRegistry_.state().path.empty() && assetRegistry_.state().path.has_parent_path()) {
        return assetRegistry_.state().path.parent_path();
    }
    return std::filesystem::current_path();
}

std::optional<std::filesystem::path> Application::resolveAnimationClipPath(const AnimationPlayer& player) const {
    const std::filesystem::path root = assetResolutionRoot();
    if (!player.animationGuid.empty()) {
        const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
            return record.guid == player.animationGuid && record.type == AssetType::Animation;
        });
        if (recordIt != assetRegistry_.records().end()) {
            const std::filesystem::path path = resolveAssetRecordPath(*recordIt, root);
            if (!path.empty()) {
                return path.lexically_normal();
            }
        }
    }
    if (player.animationPath.empty()) {
        return std::nullopt;
    }
    std::filesystem::path path = player.animationPath;
    if (!path.is_absolute()) {
        path = root / path;
    }
    return path.lexically_normal();
}

const AnimationClip* Application::animationClipForPlayer(const AnimationPlayer& player) {
    const std::optional<std::filesystem::path> clipPath = resolveAnimationClipPath(player);
    if (!clipPath.has_value() || clipPath->empty()) {
        return nullptr;
    }
    const std::string key = clipPath->string();
    if (const auto cached = animationClipCache_.find(key); cached != animationClipCache_.end()) {
        return &cached->second;
    }
    if (failedAnimationClipLoads_.find(key) != failedAnimationClipLoads_.end()) {
        return nullptr;
    }

    std::vector<std::string> warnings;
    AnimationClip clip = AnimationClip::loadRtanimJson(*clipPath, &warnings);
    for (const std::string& warning : warnings) {
        std::cerr << warning << '\n';
    }
    if (!clip.valid()) {
        failedAnimationClipLoads_.insert(key);
        std::cerr << "Animation clip load failed or had no decoded tracks: " << key << '\n';
        return nullptr;
    }
    auto [it, inserted] = animationClipCache_.emplace(key, std::move(clip));
    return inserted ? &it->second : nullptr;
}

void Application::updateAnimationPlayers(float deltaSeconds) {
    if (!pathTracer_) {
        return;
    }
    const std::vector<Entity*> entities = sceneDocument_.registry().entities();
    if (entities.empty()) {
        return;
    }

    auto entityKey = [](EntityId id) -> uint64_t {
        return (static_cast<uint64_t>(id.index) << 32u) | static_cast<uint64_t>(id.generation);
    };
    auto collectSubtree = [&](auto&& self, EntityId id, std::unordered_set<uint64_t>& out) -> void {
        Entity* entity = sceneDocument_.registry().entity(id);
        if (entity == nullptr) {
            return;
        }
        const uint64_t key = entityKey(id);
        if (!out.insert(key).second) {
            return;
        }
        for (EntityId child : entity->children) {
            self(self, child, out);
        }
    };
    auto entityInScope = [&](const Entity* entity, const std::unordered_set<uint64_t>& scope) -> bool {
        return scope.empty() || (entity != nullptr && scope.find(entityKey(entity->id)) != scope.end());
    };
    auto differentVec3 = [](glm::vec3 a, glm::vec3 b) {
        constexpr float eps = 1.0e-5f;
        return std::abs(a.x - b.x) > eps || std::abs(a.y - b.y) > eps || std::abs(a.z - b.z) > eps;
    };
    auto differentFloatVector = [](const std::vector<float>& a, const std::vector<float>& b) {
        constexpr float eps = 1.0e-5f;
        if (a.size() != b.size()) {
            return true;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::abs(a[i] - b[i]) > eps) {
                return true;
            }
        }
        return false;
    };
    auto pathKey = [](int32_t node, AnimationTrackPath path) -> uint64_t {
        return (static_cast<uint64_t>(static_cast<uint32_t>(node)) << 32u) | static_cast<uint64_t>(static_cast<uint32_t>(path));
    };
    auto rootMotionSample = [&](const AnimationSample& sample, int32_t node) -> const AnimationNodeSample* {
        const auto it = sample.nodes.find(node);
        return it != sample.nodes.end() ? &it->second : nullptr;
    };
    auto applyRootMotionDelta = [&](Entity& playerEntity, const AnimationClip& clip, const AnimationSample& previousSample, const AnimationSample& currentSample) -> bool {
        bool changed = false;
        glm::vec3 translationDelta{0.0f};
        glm::quat rotationDelta{1.0f, 0.0f, 0.0f, 0.0f};
        bool hasTranslationDelta = false;
        bool hasRotationDelta = false;
        std::unordered_set<int32_t> translationNodes;
        std::unordered_set<int32_t> rotationNodes;
        for (const AnimationClip::RootMotionCandidate& candidate : clip.rootMotionCandidates()) {
            if (candidate.path == AnimationTrackPath::Translation) {
                translationNodes.insert(candidate.node);
            } else if (candidate.path == AnimationTrackPath::Rotation) {
                rotationNodes.insert(candidate.node);
            }
        }
        for (int32_t node : translationNodes) {
            const AnimationNodeSample* previous = rootMotionSample(previousSample, node);
            const AnimationNodeSample* current = rootMotionSample(currentSample, node);
            if (previous != nullptr && current != nullptr && previous->hasTranslation && current->hasTranslation) {
                translationDelta += current->translation - previous->translation;
                hasTranslationDelta = true;
            }
        }
        for (int32_t node : rotationNodes) {
            const AnimationNodeSample* previous = rootMotionSample(previousSample, node);
            const AnimationNodeSample* current = rootMotionSample(currentSample, node);
            if (previous != nullptr && current != nullptr && previous->hasRotation && current->hasRotation) {
                rotationDelta = glm::normalize(current->rotation * glm::inverse(previous->rotation) * rotationDelta);
                hasRotationDelta = true;
            }
        }
        if (hasTranslationDelta && glm::dot(translationDelta, translationDelta) > 1.0e-10f) {
            playerEntity.transform.position += playerEntity.transform.rotation() * translationDelta;
            changed = true;
        }
        if (hasRotationDelta) {
            const glm::quat playerRotation = playerEntity.transform.rotation();
            const glm::quat nextRotation = glm::normalize(playerRotation * rotationDelta);
            const glm::vec3 nextEuler = glm::eulerAngles(nextRotation);
            if (differentVec3(playerEntity.transform.rotationEuler, nextEuler)) {
                playerEntity.transform.rotationEuler = nextEuler;
                changed = true;
            }
        }
        if (changed) {
            playerEntity.transform.dirty = true;
        }
        return changed;
    };

    bool sceneTransformChanged = false;
    bool sceneTopologyChanged = false;
    const float clampedDelta = std::max(0.0f, deltaSeconds);
    for (Entity* playerEntity : entities) {
        if (playerEntity == nullptr || !playerEntity->animationPlayer.has_value()) {
            continue;
        }
        AnimationPlayer& player = *playerEntity->animationPlayer;
        if (!player.enabled) {
            continue;
        }
        const AnimationClip* clip = animationClipForPlayer(player);
        if (clip == nullptr) {
            continue;
        }

        const double previousTime = player.currentTimeSeconds;
        double sampleTime = previousTime;
        if (player.playing) {
            sampleTime += static_cast<double>(clampedDelta) * static_cast<double>(player.playbackSpeed);
        }
        if (!player.loop && clip->duration() > 0.0) {
            sampleTime = std::clamp(sampleTime, clip->startTime(), clip->endTime());
        }
        const AnimationSample previousSample = clip->sample(previousTime, player.loop);
        const AnimationSample sample = clip->sample(sampleTime, player.loop);
        player.currentTimeSeconds = sample.timeSeconds;
        std::unordered_set<uint64_t> rootMotionChannels;
        if (player.applyRootMotion && player.playing && clip->rootMotionCandidateCount() > 0) {
            const AnimationSample rootPreviousSample = previousSample.timeSeconds <= sample.timeSeconds || !player.loop
                ? previousSample
                : clip->sample(clip->startTime(), false);
            if (applyRootMotionDelta(*playerEntity, *clip, rootPreviousSample, sample)) {
                sceneTransformChanged = true;
            }
            for (const AnimationClip::RootMotionCandidate& candidate : clip->rootMotionCandidates()) {
                rootMotionChannels.insert(pathKey(candidate.node, candidate.path));
            }
        }

        std::unordered_set<uint64_t> scope;
        collectSubtree(collectSubtree, playerEntity->id, scope);
        std::unordered_map<int32_t, Entity*> entityForSourceNode;
        for (Entity* entity : entities) {
            if (entity == nullptr || entity->sourceNodeIndex < 0 || !entityInScope(entity, scope)) {
                continue;
            }
            entityForSourceNode.try_emplace(entity->sourceNodeIndex, entity);
        }

        std::unordered_set<int32_t> skinDrivenTransformNodes;
        const std::vector<SceneSkinAsset>& sceneSkins = sceneDocument_.sceneSkins();
        for (Entity* entity : entities) {
            if (entity == nullptr || !entity->meshRenderer.has_value() || !entityInScope(entity, scope)) {
                continue;
            }
            const int32_t skinIndex = entity->meshRenderer->skinIndex;
            if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= sceneSkins.size()) {
                continue;
            }
            if (entity->sourceNodeIndex >= 0) {
                skinDrivenTransformNodes.insert(entity->sourceNodeIndex);
            }
            for (uint32_t jointNode : sceneSkins[static_cast<size_t>(skinIndex)].joints) {
                if (jointNode <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
                    skinDrivenTransformNodes.insert(static_cast<int32_t>(jointNode));
                }
            }
        }

        for (const auto& [nodeIndex, nodeSample] : sample.nodes) {
            const auto entityIt = entityForSourceNode.find(nodeIndex);
            if (entityIt == entityForSourceNode.end() || entityIt->second == nullptr) {
                continue;
            }
            Entity& target = *entityIt->second;
            bool targetChanged = false;
            if (nodeSample.hasTranslation && rootMotionChannels.find(pathKey(nodeIndex, AnimationTrackPath::Translation)) == rootMotionChannels.end() &&
                differentVec3(target.transform.position, nodeSample.translation)) {
                target.transform.position = nodeSample.translation;
                targetChanged = true;
            }
            if (nodeSample.hasRotation && rootMotionChannels.find(pathKey(nodeIndex, AnimationTrackPath::Rotation)) == rootMotionChannels.end()) {
                const glm::vec3 rotationEuler = glm::eulerAngles(glm::normalize(nodeSample.rotation));
                if (differentVec3(target.transform.rotationEuler, rotationEuler)) {
                    target.transform.rotationEuler = rotationEuler;
                    targetChanged = true;
                }
            }
            if (nodeSample.hasScale && differentVec3(target.transform.scale, nodeSample.scale)) {
                target.transform.scale = nodeSample.scale;
                targetChanged = true;
            }
            if (targetChanged) {
                target.transform.dirty = true;
                if (skinDrivenTransformNodes.find(nodeIndex) != skinDrivenTransformNodes.end()) {
                    sceneTopologyChanged = true;
                } else {
                    sceneTransformChanged = true;
                }
            }
            if (player.applyMorphWeights && nodeSample.hasMorphWeights && target.meshRenderer.has_value() &&
                differentFloatVector(target.meshRenderer->morphWeights, nodeSample.morphWeights)) {
                target.meshRenderer->morphWeights = nodeSample.morphWeights;
                sceneTopologyChanged = true;
            }
        }
    }

    if (sceneTopologyChanged) {
        sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
        (void)applyPendingSceneUpdate(true);
    } else if (sceneTransformChanged) {
        sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
        (void)applyPendingSceneUpdate(false);
    }
}

bool Application::runDescriptorLifetimeStress(
    const std::filesystem::path& outputPath,
    uint32_t cycles,
    uint32_t framesPerCycle) {
    if (!headless_) {
        throw std::runtime_error("--descriptor-lifetime-stress requires --headless");
    }
    if (!pathTracer_ || !allocator_ || !commandSystem_) {
        throw std::runtime_error("Descriptor lifetime stress requires an initialized renderer");
    }

    cycles = std::max(1u, cycles);
    framesPerCycle = std::max(1u, framesPerCycle);

    const RendererSettings originalSettings = pathTracer_->settings();
    const auto initialDescriptorStats = pathTracer_->descriptorPoolStats();
    const auto initialBudget = allocator_->memoryBudgetReport();

    uint32_t maxPoolCount = initialDescriptorStats.poolCount;
    uint32_t maxCapacitySets = initialDescriptorStats.capacitySets;
    uint32_t maxAllocatedSets = initialDescriptorStats.allocatedSets;
    uint32_t maxPeakAllocatedSets = initialDescriptorStats.peakAllocatedSets;
    uint32_t maxFailedAllocations = initialDescriptorStats.failedAllocations;
    uint32_t maxFragmentedPoolFailures = initialDescriptorStats.fragmentedPoolFailures;
    uint64_t maxVmaUsageBytes = initialBudget.totalUsageBytes;
    uint64_t maxVmaAllocationBytes = initialBudget.totalAllocationBytes;

    nlohmann::json samples = nlohmann::json::array();

    auto descriptorJson = [](const DescriptorAllocator::Stats& stats) {
        return nlohmann::json{
            {"sets_per_pool", stats.setsPerPool},
            {"max_pools", stats.maxPools},
            {"used_pools", stats.usedPools},
            {"free_pools", stats.freePools},
            {"pool_count", stats.poolCount},
            {"capacity_sets", stats.capacitySets},
            {"allocated_sets", stats.allocatedSets},
            {"peak_allocated_sets", stats.peakAllocatedSets},
            {"failed_allocations", stats.failedAllocations},
            {"fragmented_pool_failures", stats.fragmentedPoolFailures},
            {"pool_growth_count", stats.poolGrowthCount},
        };
    };
    auto budgetJson = [](const ResourceAllocator::MemoryBudgetReport& budget) {
        nlohmann::json heaps = nlohmann::json::array();
        for (const auto& heap : budget.heaps) {
            heaps.push_back({
                {"heap_index", heap.heapIndex},
                {"usage_bytes", heap.usageBytes},
                {"budget_bytes", heap.budgetBytes},
                {"allocation_bytes", heap.allocationBytes},
                {"block_bytes", heap.blockBytes},
                {"allocation_count", heap.allocationCount},
                {"block_count", heap.blockCount},
                {"usage_ratio", heap.usageRatio},
                {"pressure", heap.pressure},
            });
        }
        return nlohmann::json{
            {"supported", budget.supported},
            {"total_usage_bytes", budget.totalUsageBytes},
            {"total_budget_bytes", budget.totalBudgetBytes},
            {"total_allocation_bytes", budget.totalAllocationBytes},
            {"total_block_bytes", budget.totalBlockBytes},
            {"peak_usage_bytes", budget.peakUsageBytes},
            {"usage_delta_bytes", budget.usageDeltaBytes},
            {"allocation_count", budget.allocationCount},
            {"block_count", budget.blockCount},
            {"max_usage_ratio", budget.maxUsageRatio},
            {"pressure", budget.pressure},
            {"override_active", budget.overrideActive},
            {"warnings", budget.warnings},
            {"heaps", heaps},
        };
    };
    auto takeSample = [&](uint32_t cycle, const char* phase) {
        const auto descriptorStats = pathTracer_->descriptorPoolStats();
        const auto budget = allocator_->memoryBudgetReport();
        maxPoolCount = std::max(maxPoolCount, descriptorStats.poolCount);
        maxCapacitySets = std::max(maxCapacitySets, descriptorStats.capacitySets);
        maxAllocatedSets = std::max(maxAllocatedSets, descriptorStats.allocatedSets);
        maxPeakAllocatedSets = std::max(maxPeakAllocatedSets, descriptorStats.peakAllocatedSets);
        maxFailedAllocations = std::max(maxFailedAllocations, descriptorStats.failedAllocations);
        maxFragmentedPoolFailures = std::max(maxFragmentedPoolFailures, descriptorStats.fragmentedPoolFailures);
        maxVmaUsageBytes = std::max(maxVmaUsageBytes, budget.totalUsageBytes);
        maxVmaAllocationBytes = std::max(maxVmaAllocationBytes, budget.totalAllocationBytes);
        samples.push_back({
            {"cycle", cycle},
            {"phase", phase},
            {"frame_serial", frameSerial_},
            {"retired_renderer_count", retiredPathTracers_.size()},
            {"descriptors", descriptorJson(descriptorStats)},
            {"vma_budget", budgetJson(budget)},
        });
    };

    takeSample(0, "initial");
    const float originalScale = std::clamp(originalSettings.renderResolutionScale, 0.1f, 1.0f);
    const float stressScale = std::max(0.5f, std::min(originalScale, 0.75f));

    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        renderFrames(framesPerCycle);
        takeSample(cycle, "steady_frame");

        RendererSettings resizedSettings = pathTracer_->settings();
        resizedSettings.renderResolutionScale = (cycle % 2u == 0u) ? stressScale : originalScale;
        resizedSettings.renderPreset = RenderPreset::Custom;
        applyRendererSettingsSafely(resizedSettings, true);
        pathTracer_->resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
        renderFrames(framesPerCycle);
        takeSample(cycle, "render_scale_toggle");

        if (gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty()) {
            if (!pathTracer_->updateMaterials(*gpuSceneAsset_, assets_)) {
                pathTracer_->resetAccumulation(AccumulationResetReason::MaterialChanged);
            }
            renderFrames(framesPerCycle);
            takeSample(cycle, "material_update");
        }

        if (scenePath_.has_value()) {
            const RendererSettings previousSettings = pathTracer_->settings();
            if (!sceneDocument_.loadJson(*scenePath_)) {
                throw std::runtime_error("Descriptor lifetime stress scene reload failed: " + scenePath_->string());
            }
            gltfPath_ = sceneDocument_.sourceGltfPath();
            hdrPath_ = sceneDocument_.sourceHdrPath();
            if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
                assets_.clear();
                GltfLoader loader(assets_);
                importedScene_ = loader.loadWithCache(*gltfPath_);
            }
            rebuildGpuSceneAsset();
            preparePathTracerForRendererReplacement(previousSettings);
            std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
                gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
                gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
                currentSceneCachePathForRenderer(),
                &previousSettings);
            retirePathTracer(std::move(pathTracer_));
            pathTracer_ = std::move(nextPathTracer);
            applyActiveSceneCamera();
            pathTracer_->resetAccumulation(AccumulationResetReason::SceneChanged);
            commandSystem_->setPathTracer(pathTracer_.get());
            renderFrames(CommandSystem::framesInFlight + framesPerCycle + 1u);
            takeSample(cycle, "scene_reload");
        }

        reloadShadersFromEditor();
        takeSample(cycle, "renderer_recreated");
        renderFrames(CommandSystem::framesInFlight + framesPerCycle + 1u);
        takeSample(cycle, "retirement_drained");
    }

    applyRendererSettingsSafely(originalSettings, true);
    renderFrames(CommandSystem::framesInFlight + framesPerCycle + 1u);
    takeSample(cycles, "final");

    const auto finalDescriptorStats = pathTracer_->descriptorPoolStats();
    const auto finalBudget = allocator_->memoryBudgetReport();
    std::vector<std::string> failures;
    if (maxFailedAllocations > initialDescriptorStats.failedAllocations) {
        failures.push_back("descriptor allocation failures increased");
    }
    if (maxFragmentedPoolFailures > initialDescriptorStats.fragmentedPoolFailures) {
        failures.push_back("fragmented descriptor-pool failures increased");
    }
    if (!retiredPathTracers_.empty()) {
        failures.push_back("retired renderer queue was not drained");
    }
    const bool passed = failures.empty();

    nlohmann::json report;
    report["schema"] = "rtv_descriptor_lifetime_stress_v1";
    report["passed"] = passed;
    report["cycles"] = cycles;
    report["frames_per_cycle"] = framesPerCycle;
    report["operations"] = {
        "steady_frame",
        "render_scale_toggle",
        "material_update_when_scene_assets_exist",
        "scene_reload_when_rtlevel_path_exists",
        "renderer_recreated_via_shader_reload_path",
        "retirement_drained"
    };
    report["initial_descriptors"] = descriptorJson(initialDescriptorStats);
    report["final_descriptors"] = descriptorJson(finalDescriptorStats);
    report["max_descriptors"] = {
        {"pool_count", maxPoolCount},
        {"capacity_sets", maxCapacitySets},
        {"allocated_sets", maxAllocatedSets},
        {"peak_allocated_sets", maxPeakAllocatedSets},
        {"failed_allocations", maxFailedAllocations},
        {"fragmented_pool_failures", maxFragmentedPoolFailures},
    };
    report["initial_vma_budget"] = budgetJson(initialBudget);
    report["final_vma_budget"] = budgetJson(finalBudget);
    report["max_vma_usage_bytes"] = maxVmaUsageBytes;
    report["max_vma_allocation_bytes"] = maxVmaAllocationBytes;
    report["retired_renderer_count_final"] = retiredPathTracers_.size();
    report["failure_reasons"] = failures;
    report["samples"] = std::move(samples);

    const auto parent = outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream file(outputPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open descriptor lifetime stress output: " + outputPath.string());
    }
    file << report.dump(2);
    return passed;
}

void Application::resetDiagnosticFrameCounter(uint32_t frameIndex) {
    nextDiagnosticFrameIndex_ = frameIndex;
}

void Application::setFrameCaptureCallbacks(std::function<void(uint32_t)> begin, std::function<void(uint32_t)> end) {
    beginFrameCapture_ = std::move(begin);
    endFrameCapture_ = std::move(end);
}

void Application::resetAccumulation() {
    if (pathTracer_) {
        pathTracer_->resetAccumulation(AccumulationResetReason::Manual);
    }
}

bool Application::applyNamedCamera(std::string_view cameraName) {
    if (pathTracer_ == nullptr || cameraName.empty()) {
        return false;
    }

    const std::string normalized = normalizedCameraName(cameraName);
    auto applyPose = [&](glm::vec3 position, glm::vec3 target, float fovY, std::string_view label) {
        glm::vec3 forward = target - position;
        if (glm::dot(forward, forward) <= 1.0e-8f) {
            return false;
        }
        pathTracer_->setCameraProjection(0u, fovY, 0.0f, 1.0f, 1.0f, 0.01f, 1000.0f);
        cameraController_.setPose(position, glm::normalize(forward), *pathTracer_);
        std::cout << "Applied camera: " << label << '\n';
        return true;
    };

    if (normalized == "sponza-foliage" || normalized == "lightweight-sponza-foliage") {
        return applyPose(
            glm::vec3{2.0f, 1.05f, 1.55f},
            glm::vec3{-0.52f, 0.82f, -0.30f},
            1.15f,
            "sponza-foliage");
    }
    if (normalized == "sponza-courtyard" || normalized == "lightweight-sponza-courtyard") {
        return applyPose(
            glm::vec3{0.0f, 1.25f, 6.0f},
            glm::vec3{0.0f, 0.95f, 0.0f},
            0.872665f,
            "sponza-courtyard");
    }

    for (const Entity* entity : sceneDocument_.registry().entities()) {
        if (entity == nullptr || !entity->camera.has_value()) {
            continue;
        }
        if (normalizedCameraName(entity->name) != normalized) {
            continue;
        }
        const glm::mat4 transform = entityWorldMatrix(sceneDocument_.registry(), *entity);
        const glm::vec3 position = glm::vec3(transform[3]);
        glm::vec3 forward = glm::mat3(transform) * glm::vec3(0.0f, 0.0f, -1.0f);
        if (glm::dot(forward, forward) <= 1.0e-8f) {
            forward = glm::vec3{0.0f, 0.0f, -1.0f};
        }
        pathTracer_->setCameraProjection(
            entity->camera->projection,
            entity->camera->verticalFovRadians,
            entity->camera->aspectRatio,
            entity->camera->orthographicXmag,
            entity->camera->orthographicYmag,
            entity->camera->nearPlane,
            entity->camera->farPlane);
        cameraController_.setPose(position, glm::normalize(forward), *pathTracer_);
        std::cout << "Applied scene camera: " << entity->name << '\n';
        return true;
    }

    return false;
}

void Application::applyDebugView(RendererDebugView view) {
    if (pathTracer_) {
        RendererSettings settings = pathTracer_->settings();
        settings.debugView = view;
        pathTracer_->applySettings(settings);
        pathTracer_->resetAccumulation(AccumulationResetReason::DebugViewChanged);
    }
}

void Application::initWindow() {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    const bool explicitStartupScene = scenePath_.has_value() || gltfPath_.has_value();
    mainWindowHiddenUntilRenderer_ = !headless_ && !explicitStartupScene;
    glfwWindowHint(GLFW_VISIBLE, mainWindowHiddenUntilRenderer_ ? GLFW_FALSE : GLFW_TRUE);
    window_ = glfwCreateWindow(initialWidth, initialHeight, "Vibode Engine", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("glfwCreateWindow failed");
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
#if defined(_WIN32)
    enableDarkWindowFrame(window_);
#endif
    glfwSetWindowUserPointer(window_, this);
    glfwSetWindowFocusCallback(window_, windowFocusCallback);
    glfwSetDropCallback(window_, fileDropCallback);
    glfwGetWindowPos(window_, &windowedX_, &windowedY_);
    glfwGetWindowSize(window_, &windowedWidth_, &windowedHeight_);

    std::cout << "Controls: hold right mouse in the viewport to look/move, Ctrl+L drag rotates sun, WASD move, QE/Space/Ctrl vertical, Shift fast, F11 borderless fullscreen.\n"
              << "Settings: F1 debug view, F2 denoiser, F3 denoise while moving, F4 sun, F5 env, F6 direct light, R reset.\n"
              << "Adjust: +/- exposure, 1-5 tone mapper, 6 auto exposure, </> env intensity, [/ ] env rotation, PageUp/PageDown bounces, Home/End a-trous.\n"
              << "Files: drop .hdr for environment maps or .gltf/.glb for scene reload.\n";
}

void Application::initVulkan() {
    if (headless_) {
        context_ = VulkanContext::createHeadless();
    } else {
        context_ = std::make_unique<VulkanContext>(window_);
    }
    allocator_ = std::make_unique<ResourceAllocator>(*context_);
    uploadContext_ = std::make_unique<UploadContext>(context_->device(), context_->graphicsQueue(), context_->queueFamilies().graphics.value());
    uploader_ = std::make_unique<BufferUploader>(*allocator_, *uploadContext_);
    if (headless_) {
        constexpr VkExtent2D defaultExtent{1280, 720};
        swapchain_ = std::make_unique<Swapchain>(*context_, defaultExtent);
    } else {
        swapchain_ = std::make_unique<Swapchain>(*context_, window_);
    }
    commandSystem_ = std::make_unique<CommandSystem>(*context_, *swapchain_, disableAsyncCompute_, singleQueueFallback_);
    commandSystem_->setHeadless(headless_);

    if (!headless_) {
        uiOverlay_ = std::make_unique<UiOverlay>(window_, *context_, *swapchain_, *allocator_, *uploader_);
        notifications_.setLogSink(&uiOverlay_->editor().log());
        commandSystem_->setUiOverlay(uiOverlay_.get());
        uiOverlay_->editor().editorPrefs().load(EditorPreferences::defaultPath());
        uiOverlay_->editor().setEditorPreferencesPath(EditorPreferences::defaultPath());
    }

    const EditorPreferences* startupPrefs = uiOverlay_ != nullptr ? &uiOverlay_->editor().editorPrefs() : nullptr;
    if (startupPrefs != nullptr) {
        cameraController_.setMoveSpeed(std::clamp(startupPrefs->cameraMoveSpeed, 0.05f, 100.0f));
        cameraController_.setFastMoveSpeed(std::clamp(startupPrefs->cameraFastMoveSpeed, 0.05f, 250.0f));
        cameraController_.setMouseSensitivity(std::clamp(startupPrefs->cameraMouseSensitivity, 0.0001f, 0.02f));
        cameraController_.setInvertLookX(startupPrefs->cameraInvertLookX);
        cameraController_.setInvertLookY(startupPrefs->cameraInvertLookY);
    }
    const bool explicitStartupScene = scenePath_.has_value() || gltfPath_.has_value();
    if (explicitStartupScene && uiOverlay_ != nullptr) {
        uiOverlay_->editor().dismissProjectManager();
    }
    const std::optional<std::filesystem::path> startupProjectOverride = !headless_ ? startupProjectOverridePath() : std::nullopt;
    const bool hasStartupProjectOverride = startupProjectOverride.has_value() && std::filesystem::exists(*startupProjectOverride);
    const std::filesystem::path startupProjectPath = hasStartupProjectOverride
        ? *startupProjectOverride
        : (startupPrefs != nullptr ? std::filesystem::path(startupPrefs->lastOpenedProject) : std::filesystem::path{});
    const bool openLastProjectOnStartup = hasStartupProjectOverride ||
        (startupPrefs != nullptr && startupPrefs->openLastProject &&
            !startupPrefs->lastOpenedProject.empty() && std::filesystem::exists(startupPrefs->lastOpenedProject));
    const bool deferRendererForProjectManager = !headless_ && !explicitStartupScene && !openLastProjectOnStartup;
    if (deferRendererForProjectManager) {
        sceneUnsavedDirty_ = false;
        initializeProjectManagerStartupSceneDocument();
        sceneDocument_.clearDirty();
        std::cout << "Project Manager launcher active; renderer startup deferred until a project or scene is selected.\n";
        return;
    }

    bool loadedSceneDocument = false;
    if (scenePath_.has_value()) {
        if (!sceneDocument_.loadJson(*scenePath_)) {
            throw std::runtime_error("Scene JSON load failed: " + scenePath_->string());
        }
        loadedSceneDocument = true;
        gltfPath_ = sceneDocument_.sourceGltfPath();
        if (!hdrPath_.has_value()) {
            hdrPath_ = sceneDocument_.sourceHdrPath();
        }
        if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
            GltfLoader loader(assets_);
            importedScene_ = loader.loadWithCache(*gltfPath_);
        }
        undoStack_.clear();
        std::cout << "Loaded scene JSON: " << scenePath_->string() << '\n';
    } else if (gltfPath_.has_value()) {
        GltfLoader loader(assets_);
        importedScene_ = loader.loadWithCache(*gltfPath_);
        sceneDocument_.importSceneAsset(*importedScene_);
        sceneDocument_.setSourceGltfPath(gltfPath_);
        undoStack_.clear();
        std::cout << "Loaded glTF: " << gltfPath_->string()
                  << " meshes=" << importedScene_->meshes.size()
                  << " materials=" << importedScene_->materials.size()
                  << " textures=" << importedScene_->textures.size()
                  << " nodes=" << importedScene_->nodes.size() << '\n';
    } else if (!loadedSceneDocument) {
        if (headless_) {
            initializeFallbackSceneDocument();
        } else {
            initializeProjectManagerStartupSceneDocument();
        }
    }
    sceneUnsavedDirty_ = false;
    sceneDocument_.setSourceHdrPath(hdrPath_);
    rebuildGpuSceneAsset();
    RendererSettings startupSettings{};
    startupSettings.debugView = debugView_;
    startupSettings = rendererSettingsFromDocument(sceneDocument_, startupSettings);
    if (debugViewOverride_) {
        startupSettings.debugView = debugView_;
    } else if (loadedSceneDocument) {
        debugView_ = startupSettings.debugView;
    }
    bool largeSceneSettingsChanged = false;
    if (importedScene_.has_value()) {
        const bool importSafeRuntime = !headless_ && gltfPath_.has_value() && !scenePath_.has_value();
        startupSettings = interactiveSettingsForScene(startupSettings, *importedScene_, assets_, importSafeRuntime, largeSceneSettingsChanged);
        if (largeSceneSettingsChanged) {
            syncDocumentRenderSettings(sceneDocument_, startupSettings);
        }
    }
    if (denoiserOverride_.has_value()) {
        startupSettings.denoiserEnabled = *denoiserOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
    }
    if (renderPresetOverride_.has_value()) {
        applyRenderPreset(startupSettings, *renderPresetOverride_);
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (restirModeOverride_.has_value()) {
        startupSettings.restirMode = *restirModeOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (restirGiOverride_.has_value()) {
        startupSettings.restirGiEnabled = *restirGiOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (opacityMicromapOverride_.has_value()) {
        startupSettings.opacityMicromapsEnabled = *opacityMicromapOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (opacityMicromapSubdivisionOverride_.has_value()) {
        startupSettings.opacityMicromapSubdivisionLevel = *opacityMicromapSubdivisionOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
    }
    createPathTracer(&startupSettings);
    syncDocumentRenderSettings(sceneDocument_, pathTracer_->settings());
    applyActiveSceneCamera();
    sceneDocument_.clearDirty();
    if (!headless_) {
        if (loadedSceneDocument) {
            deserializeEditorSceneData();
        }
        if (openLastProjectOnStartup && !startupProjectPath.empty()) {
            if (openProjectFromFile(startupProjectPath, false)) {
                (void)applyPendingSceneUpdate(true);
            } else if (hasStartupProjectOverride) {
                std::cerr << "Startup project override failed: " << startupProjectPath.string() << '\n';
            }
        }
    }
    commandSystem_->setPathTracer(pathTracer_.get());
    showMainWindowIfHidden();
}

void Application::mainLoop(uint32_t maxFrames) {
    if (headless_) {
        runHeadless(0, maxFrames > 0 ? maxFrames : 120);
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    uint32_t frameCount = 0;
    const uint32_t totalMaxFrames = (headless_ && maxFrames == 0) ? 120u : maxFrames;

    if (headless_) {
        while (totalMaxFrames == 0 || frameCount < totalMaxFrames) {
            const auto now = std::chrono::steady_clock::now();
            const float seconds = std::chrono::duration<float>(now - start).count();
            const float rawDeltaSeconds = std::max(0.0f, seconds - lastFrameSeconds_);
            const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
            lastFrameSeconds_ = seconds;

            updateAnimationPlayers(deltaSeconds);
            commandSystem_->drawFrame(seconds, deltaSeconds);
            ++frameSerial_;
            releaseRetiredPathTracers();
            ++frameCount;
        }
        commandSystem_->waitIdle();
        return;
    }

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - start).count();
        const float rawDeltaSeconds = std::max(0.0f, seconds - lastFrameSeconds_);
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;

        if (uiOverlay_) {
            uiOverlay_->beginFrame();
        }
        processRuntimeControls(deltaSeconds);
        applyValidationObjectMotion(frameCount);
        applyValidationCameraMotion(frameCount);
        notifications_.update(deltaSeconds);
        updateAutosave(deltaSeconds);
        EditorRequests editorRequests;
        if (pendingOpenLevel_) {
            pendingOpenLevel_ = false;
            if (auto path = openSceneJsonFileDialog()) {
                editorRequests.loadSceneJson = *path;
                editorRequests.resetAccumulation = AccumulationResetReason::SceneChanged;
            }
        }
        if (pendingSaveLevel_) {
            pendingSaveLevel_ = false;
            if (scenePath_.has_value()) {
                editorRequests.saveScene = *scenePath_;
            } else if (auto path = saveSceneJsonFileDialog()) {
                editorRequests.saveSceneAs = *path;
            }
        }
        if (pendingReloadShaders_) {
            pendingReloadShaders_ = false;
            editorRequests.reloadShaders = true;
            editorRequests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (pathTracer_ && pathTracer_->shadersNeedReload()) {
            editorRequests.reloadShaders = true;
            editorRequests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        EditorJobCenterState jobCenter;
        jobCenter.sceneLoadRunning = asyncSceneLoader_.isRunning();
        jobCenter.sceneLoadProgress = asyncSceneLoader_.progress();
        jobCenter.sceneLoadStatus = sceneLoadingStatus_;
        jobCenter.sceneLoadStage = asyncSceneLoader_.stage();
        jobCenter.queuedSceneMerges = pendingMergeScenes_.size();
        if (activeSceneLoadRequest_.has_value()) {
            jobCenter.sceneLoadJobSerial = activeSceneLoadRequest_->serial;
            jobCenter.sceneLoadTitle = sceneLoadModeLabel(activeSceneLoadRequest_->mode);
            jobCenter.sceneLoadSourcePath = activeSceneLoadRequest_->sourcePath;
        }
        if (completedSceneLoadJob_.completedSceneLoadSerial != 0) {
            jobCenter.completedSceneLoadSerial = completedSceneLoadJob_.completedSceneLoadSerial;
            jobCenter.completedSceneLoadSuccess = completedSceneLoadJob_.completedSceneLoadSuccess;
            jobCenter.completedSceneLoadCancelled = completedSceneLoadJob_.completedSceneLoadCancelled;
            jobCenter.completedSceneLoadTitle = completedSceneLoadJob_.completedSceneLoadTitle;
            jobCenter.completedSceneLoadStatus = completedSceneLoadJob_.completedSceneLoadStatus;
            jobCenter.completedSceneLoadSourcePath = completedSceneLoadJob_.completedSceneLoadSourcePath;
            jobCenter.completedSceneLoadError = completedSceneLoadJob_.completedSceneLoadError;
            jobCenter.completedSceneLoadWarning = completedSceneLoadJob_.completedSceneLoadWarning;
            jobCenter.completedSceneLoadWorkerTotalMs = completedSceneLoadJob_.completedSceneLoadWorkerTotalMs;
            jobCenter.completedSceneLoadWorkerSceneParseMs = completedSceneLoadJob_.completedSceneLoadWorkerSceneParseMs;
            jobCenter.completedSceneLoadWorkerGltfLoadMs = completedSceneLoadJob_.completedSceneLoadWorkerGltfLoadMs;
            jobCenter.completedSceneLoadWorkerDocumentBuildMs = completedSceneLoadJob_.completedSceneLoadWorkerDocumentBuildMs;
        }
        jobCenter.queuedAssetImports = pendingAssetImportJobs_.size();
        if (completedAssetImportJob_.completedAssetImportSerial != 0) {
            jobCenter.completedAssetImportSerial = completedAssetImportJob_.completedAssetImportSerial;
            jobCenter.completedAssetImportSuccess = completedAssetImportJob_.completedAssetImportSuccess;
            jobCenter.completedAssetImportTitle = completedAssetImportJob_.completedAssetImportTitle;
            jobCenter.completedAssetImportStatus = completedAssetImportJob_.completedAssetImportStatus;
            jobCenter.completedAssetImportSourcePath = completedAssetImportJob_.completedAssetImportSourcePath;
            jobCenter.completedAssetImportReportPath = completedAssetImportJob_.completedAssetImportReportPath;
            jobCenter.completedAssetImportErrors = completedAssetImportJob_.completedAssetImportErrors;
            jobCenter.completedAssetImportWarnings = completedAssetImportJob_.completedAssetImportWarnings;
            jobCenter.completedAssetImportCanRetry = completedAssetImportJob_.completedAssetImportCanRetry;
            jobCenter.completedAssetImportPlaceAfterImport = completedAssetImportJob_.completedAssetImportPlaceAfterImport;
            jobCenter.completedAssetImportDestinationFolder = completedAssetImportJob_.completedAssetImportDestinationFolder;
            jobCenter.completedAssetImportMode = completedAssetImportJob_.completedAssetImportMode;
            jobCenter.completedAssetImportSettings = completedAssetImportJob_.completedAssetImportSettings;
            jobCenter.completedAssetReimportGuid = completedAssetImportJob_.completedAssetReimportGuid;
            jobCenter.completedAssetImportWorkerTotalMs = completedAssetImportJob_.completedAssetImportWorkerTotalMs;
            jobCenter.completedAssetImportWorkerValidateMs = completedAssetImportJob_.completedAssetImportWorkerValidateMs;
            jobCenter.completedAssetImportWorkerDirectoryMs = completedAssetImportJob_.completedAssetImportWorkerDirectoryMs;
            jobCenter.completedAssetImportWorkerInspectMs = completedAssetImportJob_.completedAssetImportWorkerInspectMs;
            jobCenter.completedAssetImportWorkerWriteMs = completedAssetImportJob_.completedAssetImportWorkerWriteMs;
        }
        if (activeAssetImportJob_.has_value()) {
            const AsyncAssetImportJob& job = activeAssetImportJob_->job;
            const bool reimport = job.kind == AsyncAssetImportKind::Reimport;
            float importProgress = 0.05f;
            std::string importStage = "Queued";
            double importWorkerElapsedMs = 0.0;
            double importStageElapsedMs = 0.0;
            if (activeAssetImportJob_->progress != nullptr) {
                std::lock_guard<std::mutex> lock(activeAssetImportJob_->progress->mutex);
                importProgress = activeAssetImportJob_->progress->progress;
                importStage = activeAssetImportJob_->progress->stage;
                const auto progressNow = std::chrono::steady_clock::now();
                if (activeAssetImportJob_->progress->workerStartedAt.time_since_epoch().count() != 0) {
                    importWorkerElapsedMs = std::chrono::duration<double, std::milli>(progressNow - activeAssetImportJob_->progress->workerStartedAt).count();
                }
                if (activeAssetImportJob_->progress->stageStartedAt.time_since_epoch().count() != 0) {
                    importStageElapsedMs = std::chrono::duration<double, std::milli>(progressNow - activeAssetImportJob_->progress->stageStartedAt).count();
                }
            }
            jobCenter.assetImportJobSerial = job.serial;
            jobCenter.assetImportRunning = true;
            jobCenter.assetImportProgress = importProgress;
            jobCenter.assetImportTitle = reimport ? "Reimport Asset" : (job.placeAfterImport ? "Import and Place" : "Import Asset");
            jobCenter.assetImportStatus = job.request.sourcePath.empty()
                ? importStage
                : importStage + ": " + job.request.sourcePath.filename().string();
            jobCenter.assetImportCanRetry = !job.request.sourcePath.empty();
            jobCenter.assetImportPlaceAfterImport = job.placeAfterImport;
            jobCenter.assetImportSourcePath = job.request.sourcePath;
            jobCenter.assetImportDestinationFolder = job.request.destinationFolder;
            jobCenter.assetImportMode = job.request.mode;
            jobCenter.assetImportSettings = job.request.settings;
            jobCenter.assetImportWorkerElapsedMs = importWorkerElapsedMs;
            jobCenter.assetImportStageElapsedMs = importStageElapsedMs;
            if (reimport) {
                jobCenter.assetReimportGuid = job.assetGuid;
            }
        }
        if (activeCookProjectJob_.has_value()) {
            const CookManifestProgress manifestProgress = readCookManifestProgress(activeCookProjectJob_->manifestPath);
            float cookProgress = 0.35f;
            std::string cookStatus = "Cooking transparent project assets";
            if (manifestProgress.available) {
                if (manifestProgress.plannedFileCount > 0) {
                    const float copyRatio = static_cast<float>(manifestProgress.copiedFileCount) / static_cast<float>(manifestProgress.plannedFileCount);
                    cookProgress = 0.10f + 0.85f * std::clamp(copyRatio, 0.0f, 1.0f);
                }
                if (!manifestProgress.status.empty()) {
                    cookStatus = "Cook " + manifestProgress.status;
                }
                if (manifestProgress.plannedFileCount > 0) {
                    cookStatus += ": " + std::to_string(manifestProgress.copiedFileCount) + "/" + std::to_string(manifestProgress.plannedFileCount) + " files";
                }
            }
            jobCenter.cookProjectJobSerial = activeCookProjectJob_->serial;
            jobCenter.cookProjectRunning = true;
            jobCenter.cookProjectProgress = cookProgress;
            jobCenter.cookProjectStatus = cookStatus;
            jobCenter.cookProjectFile = activeCookProjectJob_->projectFile;
            jobCenter.cookProjectOutputDir = activeCookProjectJob_->outputDir;
            jobCenter.cookProjectManifestPath = activeCookProjectJob_->manifestPath;
            jobCenter.cookProjectValidationReportPath = activeCookProjectJob_->validationReportPath;
            jobCenter.cookProjectLogPath = activeCookProjectJob_->logPath;
            jobCenter.cookProjectManifestStatus = manifestProgress.status;
            jobCenter.cookProjectPlannedFileCount = manifestProgress.plannedFileCount;
            jobCenter.cookProjectCopiedFileCount = manifestProgress.copiedFileCount;
        }
        if (completedCookProjectJob_.completedCookProjectSerial != 0) {
            jobCenter.completedCookProjectSerial = completedCookProjectJob_.completedCookProjectSerial;
            jobCenter.completedCookProjectSuccess = completedCookProjectJob_.completedCookProjectSuccess;
            jobCenter.completedCookProjectStatus = completedCookProjectJob_.completedCookProjectStatus;
            jobCenter.completedCookProjectFile = completedCookProjectJob_.completedCookProjectFile;
            jobCenter.completedCookProjectOutputDir = completedCookProjectJob_.completedCookProjectOutputDir;
            jobCenter.completedCookProjectManifestPath = completedCookProjectJob_.completedCookProjectManifestPath;
            jobCenter.completedCookProjectValidationReportPath = completedCookProjectJob_.completedCookProjectValidationReportPath;
            jobCenter.completedCookProjectLogPath = completedCookProjectJob_.completedCookProjectLogPath;
            jobCenter.completedCookProjectExitCode = completedCookProjectJob_.completedCookProjectExitCode;
            jobCenter.completedCookProjectWorkerTotalMs = completedCookProjectJob_.completedCookProjectWorkerTotalMs;
        }
        if (uiOverlay_ && pathTracer_) {
            editorRequests = uiOverlay_->build(
                *pathTracer_,
                swapchain_->extent(),
                importedScene_ ? &*importedScene_ : nullptr,
                &sceneDocument_,
                importedScene_ ? &assets_ : nullptr,
                gltfPath_,
                hdrPath_,
                scenePath_,
                project_ ? &*project_ : nullptr,
                (project_ || !assetRegistry_.state().path.empty()) ? &assetRegistry_ : nullptr,
                &dirtyMaterialAssets_,
                &materialAssetAutosavePaths_,
                sceneUnsavedDirty_ || sceneDocument_.dirty(),
                projectSettingsDirty_,
                &gpuInstanceEntities_,
                sceneLoadingStatus_,
                asyncSceneLoader_.isRunning(),
                asyncSceneLoader_.progress(),
                &cameraController_,
                &undoStack_,
                &editorRenderJob_,
                &editorPlacement_,
                &jobCenter,
                &pendingDroppedFiles_,
                rawDeltaSeconds * 1000.0f,
                &notifications_,
                sunDrag_.phase != SunDragPhase::Idle);
        } else if (uiOverlay_ != nullptr) {
            editorRequests = uiOverlay_->buildProjectManager(
                project_ ? &*project_ : nullptr,
                (project_ || !assetRegistry_.state().path.empty()) ? &assetRegistry_ : nullptr,
                scenePath_,
                sceneUnsavedDirty_ || sceneDocument_.dirty(),
                projectSettingsDirty_,
                dirtyMaterialAssets_.size(),
                sceneLoadingStatus_,
                asyncSceneLoader_.isRunning(),
                asyncSceneLoader_.progress(),
                &jobCenter,
                &notifications_);
        }
        if (pendingUndo_) {
            editorRequests.undo = true;
            pendingUndo_ = false;
        }
        if (pendingRedo_) {
            editorRequests.redo = true;
            pendingRedo_ = false;
        }
        if (pendingSaveAll_) {
            editorRequests.saveAll = true;
            pendingSaveAll_ = false;
        }
        applyEditorRequests(editorRequests, false);
        updateAnimationPlayers(deltaSeconds);
        prepareEditorRenderJobFrame();
        if (beginFrameCapture_) {
            beginFrameCapture_(frameCount + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        if (uiOverlay_) {
            uiOverlay_->renderPlatformWindows();
        }
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(frameCount + 1u);
        }
        updateEditorRenderJob(deltaSeconds);
        applyEditorRequests(editorRequests, true);
        pollAsyncSceneLoad();
        pollAssetImportWorker();
        pollCookProjectJob();
        captureProjectThumbnailIfReady();
        updateWindowTitle(seconds);

        ++frameCount;
        if (maxFrames > 0 && frameCount >= maxFrames) {
            break;
        }
    }
}

void Application::onWindowFocusChanged(bool focused) {
    if (!focused && window_ != nullptr) {
        cameraController_.releaseMouse(window_);
        finishSunDrag(true);
    }
}

void Application::startEditorRenderJob(EditorRenderJobKind kind, const std::filesystem::path& renderOutputRoot, const EditorRenderRequest* request) {
    if (kind == EditorRenderJobKind::None || pathTracer_ == nullptr) {
        return;
    }

    if (editorRenderJob_.active) {
        editorRenderJob_.active = false;
        editorRenderJob_.cancelled = true;
        editorRenderJob_.status = "Cancelled by new render request";
        restoreEditorRenderJobSceneState();
        writeEditorRenderJobManifest("cancelled");
    }

    std::error_code ec;
    std::filesystem::create_directories(renderOutputRoot, ec);
    const uint64_t serial = nextEditorRenderJobSerial_++;
    const std::string action = editorRenderJobAction(kind);
    const std::filesystem::path jobRoot = renderOutputRoot / (editorRenderTimestamp() + "_" + action + "_" + std::to_string(serial));
    std::filesystem::create_directories(jobRoot, ec);

    editorRenderJob_ = EditorRenderJobStatus{};
    editorRenderJob_.kind = kind;
    editorRenderJob_.active = true;
    editorRenderJob_.progress = 0.02f;
    editorRenderJob_.serial = serial;
    editorRenderJob_.title = editorRenderJobTitle(kind);
    editorRenderJob_.status = "Preparing output";
    editorRenderJob_.outputRoot = jobRoot;
    editorRenderJob_.manifestPath = jobRoot / "render_manifest.json";
    editorRenderJobFramesRendered_ = 0;
    editorRenderJobFramePrepared_ = false;
    editorRenderJobOutputFiles_.clear();
    editorRenderJobSceneSnapshot_.reset();
    editorRenderJobSettingsSnapshot_.reset();
    editorRenderJobAppliedSettings_.reset();
    editorRenderJobRequest_.reset();
    editorRenderJobSequenceFramesPerTimelineFrame_ = 1;
    editorRenderJobSequenceOutputFramesWritten_ = 0;
    editorRenderJobSequenceAccumulationFrame_ = 0;
    editorRenderJob_.currentFrame = 0;
    editorRenderJob_.totalFrames = 1;

    auto disableDlssRayReconstructionForOfflineRender = [&](RendererSettings& jobSettings) {
        if (!jobSettings.dlssRayReconstructionEnabled) {
            return;
        }
        jobSettings.dlssRayReconstructionEnabled = false;
    };

    if (request != nullptr && kind != EditorRenderJobKind::CurrentViewport) {
        editorRenderJobRequest_ = *request;
        editorRenderJobSettingsSnapshot_ = pathTracer_->settings();
        RendererSettings jobSettings = *editorRenderJobSettingsSnapshot_;
        jobSettings.renderPreset = RenderPreset::Custom;
        jobSettings.samplesPerPixel = std::clamp(request->samplesPerPixel, 1u, kMaxSamplesPerPixel);
        jobSettings.limitSamplesPerPixel = request->limitSamplesPerPixel;
        jobSettings.renderResolutionScale = std::clamp(request->renderResolutionScale, 0.25f, 1.0f);
        disableDlssRayReconstructionForOfflineRender(jobSettings);
        if (kind == EditorRenderJobKind::Image) {
            const uint32_t effectiveSpp = request->limitSamplesPerPixel ? 1u : jobSettings.samplesPerPixel;
            jobSettings.accumulationLimit = std::max(1u, request->imageAccumulationFrames) * std::max(1u, effectiveSpp);
        }
        (void)pathTracer_->applySettings(jobSettings);
        editorRenderJobAppliedSettings_ = jobSettings;
        if (request->requestedWidth > 0u && request->requestedHeight > 0u && uiOverlay_ != nullptr) {
            uiOverlay_->setRenderExtentOverride(VkExtent2D{request->requestedWidth, request->requestedHeight});
        }
    } else if (kind != EditorRenderJobKind::CurrentViewport && pathTracer_->settings().dlssRayReconstructionEnabled) {
        editorRenderJobSettingsSnapshot_ = pathTracer_->settings();
        RendererSettings jobSettings = *editorRenderJobSettingsSnapshot_;
        jobSettings.renderPreset = RenderPreset::Custom;
        disableDlssRayReconstructionForOfflineRender(jobSettings);
        (void)pathTracer_->applySettings(jobSettings);
        editorRenderJobAppliedSettings_ = jobSettings;
    }

    if (kind == EditorRenderJobKind::Image) {
        editorRenderJob_.totalFrames = request != nullptr
            ? static_cast<int>(std::max(1u, request->imageAccumulationFrames))
            : static_cast<int>(editorRenderImageTargetFrames(*pathTracer_));
        editorRenderJob_.status = "Rendering image accumulation";
        pathTracer_->resetAccumulation(AccumulationResetReason::Manual);
    }
    if (kind == EditorRenderJobKind::Sequence && uiOverlay_ != nullptr) {
        const EditorTimeline& timeline = uiOverlay_->editor().timeline();
        editorRenderJobSequenceStartFrame_ = request != nullptr ? request->sequenceStartFrame : timeline.startFrame;
        editorRenderJobSequenceEndFrame_ = request != nullptr ? request->sequenceEndFrame : timeline.endFrame;
        if (editorRenderJobSequenceEndFrame_ < editorRenderJobSequenceStartFrame_) {
            editorRenderJobSequenceEndFrame_ = editorRenderJobSequenceStartFrame_;
        }
        editorRenderJobSequenceFramesPerTimelineFrame_ = request != nullptr
            ? std::clamp(request->sequenceFramesPerTimelineFrame, 1u, kMaxEditorRenderSequenceFramesPerTimelineFrame)
            : static_cast<uint32_t>(std::clamp(
                uiOverlay_->editor().editorPrefs().renderSequenceFramesPerTimelineFrame,
                1,
                512));
        if (request != nullptr && request->saveSequenceFramesAsDefault) {
            EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
            prefs.renderSequenceFramesPerTimelineFrame = std::clamp(static_cast<int>(editorRenderJobSequenceFramesPerTimelineFrame_), 1, 512);
            (void)saveActiveEditorPreferences();
        }
        const int timelineFrameCount = std::max(1, editorRenderJobSequenceEndFrame_ - editorRenderJobSequenceStartFrame_ + 1);
        const uint64_t totalFrames = static_cast<uint64_t>(timelineFrameCount) * editorRenderJobSequenceFramesPerTimelineFrame_;
        editorRenderJob_.totalFrames = static_cast<int>(std::min<uint64_t>(totalFrames, static_cast<uint64_t>(std::numeric_limits<int>::max())));
        editorRenderJobTimelineWasPlaying_ = timeline.playing;
        editorRenderJobPreviousTimelineFrame_ = timeline.currentFrame;
        editorRenderJobSceneSnapshot_ = sceneDocument_;
        editorRenderJob_.status = "Rendering sequence frame 1 of " + std::to_string(timelineFrameCount);
    }
    editorRenderJobElapsedSeconds_ = 0.0f;

    appendRenderHistoryEvent(renderOutputRoot, action.c_str(), sceneDocument_, pathTracer_->settings());
    writeEditorRenderJobManifest("queued");
    if (kind == EditorRenderJobKind::CurrentViewport) {
        const std::filesystem::path outputPath = jobRoot / "viewport.png";
        editorRenderJob_.status = "Writing viewport.png";
        if (exportEditorRenderJobImage(outputPath)) {
            editorRenderJobOutputFiles_.push_back(outputPath);
            editorRenderJobFramesRendered_ = 1;
            editorRenderJob_.currentFrame = 1;
            editorRenderJob_.progress = 1.0f;
            editorRenderJob_.active = false;
            editorRenderJob_.completed = true;
            editorRenderJob_.status = "Viewport PNG ready";
            writeEditorRenderJobManifest("completed");
        } else {
            editorRenderJob_.progress = 0.0f;
            editorRenderJob_.active = false;
            editorRenderJob_.failed = true;
            editorRenderJob_.status = "Unable to write viewport PNG";
            writeEditorRenderJobManifest("failed");
        }
    }
    if (kind == EditorRenderJobKind::CurrentViewport) {
        const NotificationType notificationType = editorRenderJob_.completed ? NotificationType::Success : NotificationType::Error;
        const std::string message = editorRenderJob_.completed ? "Render Current Viewport complete" : "Render Current Viewport failed";
        notifications_.notify(message, notificationType, NotificationAction::OpenOutputFolder, "Open Output", 6.0f);
    } else {
        notifications_.notify(std::string(editorRenderJob_.title) + " queued", NotificationType::Info, NotificationAction::OpenOutputFolder, "Open Output", 5.0f);
    }
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().log().add(EditorLogCategory::Command, editorRenderJob_.title + ": " + editorRenderJob_.outputRoot.string());
    }
}

void Application::prepareEditorRenderJobFrame() {
    if (!editorRenderJob_.active || editorRenderJobFramePrepared_ || pathTracer_ == nullptr) {
        return;
    }

    if (editorRenderJob_.kind == EditorRenderJobKind::Image) {
        const int nextFrame = static_cast<int>(editorRenderJobFramesRendered_) + 1;
        editorRenderJob_.status = "Rendering image frame " + std::to_string(nextFrame) + " of " + std::to_string(editorRenderJob_.totalFrames);
        editorRenderJobFramePrepared_ = true;
        return;
    }

    if (editorRenderJob_.kind != EditorRenderJobKind::Sequence || uiOverlay_ == nullptr) {
        editorRenderJobFramePrepared_ = true;
        return;
    }

    EditorTimeline& timeline = uiOverlay_->editor().timeline();
    timeline.playing = false;
    const int timelineFrame = editorRenderJobSequenceStartFrame_ + static_cast<int>(editorRenderJobSequenceOutputFramesWritten_);
    timeline.currentFrame = std::clamp(timelineFrame, editorRenderJobSequenceStartFrame_, editorRenderJobSequenceEndFrame_);

    bool changed = false;
    if (editorRenderJobSequenceAccumulationFrame_ == 0u) {
        for (uint64_t uuid : timeline.animatedEntityUuids()) {
            Transform sampled;
            if (!timeline.sampleTransform(uuid, timeline.currentFrame, sampled)) {
                continue;
            }
            for (const Entity* existing : sceneDocument_.registry().entities()) {
                if (existing == nullptr || existing->uuid != uuid) {
                    continue;
                }
                if (Entity* entity = sceneDocument_.registry().entity(existing->id)) {
                    entity->transform = sampled;
                    entity->transform.dirty = true;
                    changed = true;
                }
                break;
            }
        }
    }
    if (changed) {
        sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
        if (!applyPendingSceneUpdate(false)) {
            editorRenderJob_.active = false;
            editorRenderJob_.failed = true;
            editorRenderJob_.status = "Unable to apply timeline frame for render sequence";
            restoreEditorRenderJobSceneState();
            writeEditorRenderJobManifest("failed");
            notifications_.notify("Render sequence failed", NotificationType::Error, NotificationAction::OpenOutputFolder, "Open Output", 6.0f);
            return;
        }
    }
    if (editorRenderJobSequenceAccumulationFrame_ == 0u) {
        pathTracer_->resetAccumulation(AccumulationResetReason::Manual);
    }
    const int sequenceFrameNumber = static_cast<int>(editorRenderJobSequenceOutputFramesWritten_) + 1;
    const int sequenceFrameCount = std::max(1, editorRenderJobSequenceEndFrame_ - editorRenderJobSequenceStartFrame_ + 1);
    const uint32_t accumulationFrame = editorRenderJobSequenceAccumulationFrame_ + 1u;
    editorRenderJob_.status = "Rendering timeline frame " + std::to_string(timeline.currentFrame) +
        " (" + std::to_string(sequenceFrameNumber) + " of " + std::to_string(sequenceFrameCount) +
        ", frame " + std::to_string(accumulationFrame) + " of " + std::to_string(editorRenderJobSequenceFramesPerTimelineFrame_) + ")";
    editorRenderJobFramePrepared_ = true;
}

void Application::updateEditorRenderJob(float deltaSeconds) {
    if (editorRenderJob_.kind == EditorRenderJobKind::None) {
        return;
    }

    editorRenderJobElapsedSeconds_ += deltaSeconds;
    if (editorRenderJob_.active) {
        if (!editorRenderJobFramePrepared_) {
            return;
        }

        bool frameExported = true;
        bool sequenceFrameReadyForExport = false;
        if (editorRenderJob_.kind == EditorRenderJobKind::Sequence) {
            sequenceFrameReadyForExport = editorRenderJobSequenceAccumulationFrame_ + 1u >= editorRenderJobSequenceFramesPerTimelineFrame_;
            if (sequenceFrameReadyForExport) {
                const uint32_t outputIndex = editorRenderJobSequenceOutputFramesWritten_ + 1u;
                const std::filesystem::path outputPath = editorSequenceFramePath(editorRenderJob_.outputRoot, outputIndex);
                frameExported = exportEditorRenderJobImage(outputPath);
                if (frameExported) {
                    editorRenderJobOutputFiles_.push_back(outputPath);
                }
            }
        }

        if (!frameExported) {
            editorRenderJob_.active = false;
            editorRenderJob_.failed = true;
            editorRenderJob_.status = "Unable to write render PNG";
            restoreEditorRenderJobSceneState();
            writeEditorRenderJobManifest("failed");
            notifications_.notify(editorRenderJob_.title + " failed", NotificationType::Error, NotificationAction::OpenOutputFolder, "Open Output", 6.0f);
            return;
        }

        ++editorRenderJobFramesRendered_;
        if (editorRenderJob_.kind == EditorRenderJobKind::Sequence) {
            ++editorRenderJobSequenceAccumulationFrame_;
            if (sequenceFrameReadyForExport) {
                ++editorRenderJobSequenceOutputFramesWritten_;
                editorRenderJobSequenceAccumulationFrame_ = 0u;
            }
        }
        editorRenderJob_.currentFrame = static_cast<int>(editorRenderJobFramesRendered_);
        editorRenderJob_.progress = std::clamp(
            static_cast<float>(editorRenderJobFramesRendered_) / static_cast<float>(std::max(1, editorRenderJob_.totalFrames)),
            0.02f,
            1.0f);

        const int sequenceFrameCount = std::max(1, editorRenderJobSequenceEndFrame_ - editorRenderJobSequenceStartFrame_ + 1);
        const bool finished = editorRenderJob_.kind == EditorRenderJobKind::Sequence
            ? editorRenderJobSequenceOutputFramesWritten_ >= static_cast<uint32_t>(sequenceFrameCount)
            : editorRenderJobFramesRendered_ >= static_cast<uint32_t>(std::max(1, editorRenderJob_.totalFrames));
        if (!finished) {
            editorRenderJobFramePrepared_ = false;
            writeEditorRenderJobManifest("progress");
            return;
        }

        if (editorRenderJob_.kind == EditorRenderJobKind::Image) {
            const std::filesystem::path outputPath = editorRenderJob_.outputRoot / "image.png";
            editorRenderJob_.status = "Writing image.png";
            if (!exportEditorRenderJobImage(outputPath)) {
                editorRenderJob_.active = false;
                editorRenderJob_.failed = true;
                editorRenderJob_.status = "Unable to write image.png";
                restoreEditorRenderJobSceneState();
                writeEditorRenderJobManifest("failed");
                notifications_.notify("Render image failed", NotificationType::Error, NotificationAction::OpenOutputFolder, "Open Output", 6.0f);
                return;
            }
            editorRenderJobOutputFiles_.push_back(outputPath);
        }

        restoreEditorRenderJobSceneState();
        editorRenderJob_.active = false;
        editorRenderJob_.completed = true;
        editorRenderJob_.progress = 1.0f;
        editorRenderJob_.status = "Render output ready";
        writeEditorRenderJobManifest("completed");
        notifications_.notify(editorRenderJob_.title + " complete", NotificationType::Success, NotificationAction::OpenOutputFolder, "Open Output", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, editorRenderJob_.title + " complete: " + editorRenderJob_.outputRoot.string());
        }
        editorRenderJobFramePrepared_ = false;
        return;
    }

    if ((editorRenderJob_.completed || editorRenderJob_.cancelled || editorRenderJob_.failed) && editorRenderJobElapsedSeconds_ > 10.0f) {
        editorRenderJob_ = EditorRenderJobStatus{};
        editorRenderJobElapsedSeconds_ = 0.0f;
        editorRenderJobFramesRendered_ = 0;
        editorRenderJobFramePrepared_ = false;
        editorRenderJobSequenceFramesPerTimelineFrame_ = 1;
        editorRenderJobSequenceOutputFramesWritten_ = 0;
        editorRenderJobSequenceAccumulationFrame_ = 0;
        editorRenderJobOutputFiles_.clear();
        editorRenderJobSceneSnapshot_.reset();
        editorRenderJobSettingsSnapshot_.reset();
        editorRenderJobAppliedSettings_.reset();
        editorRenderJobRequest_.reset();
    }
}

bool Application::exportEditorRenderJobImage(const std::filesystem::path& outputPath) {
    if (pathTracer_ == nullptr || context_ == nullptr || allocator_ == nullptr || swapchain_ == nullptr) {
        return false;
    }
    const VkExtent2D extent = pathTracer_->displayExtent();
    if (extent.width == 0u || extent.height == 0u) {
        return false;
    }

    try {
        DiagnosticImageExport exporter(*context_, *allocator_);
        return exporter.initialize(swapchain_->format(), extent) &&
            exporter.exportView(*pathTracer_, pathTracer_->settings().debugView, outputPath, 0);
    } catch (const std::exception& error) {
        std::cerr << "Editor render output export failed: " << error.what() << '\n';
        return false;
    }
}

void Application::restoreEditorRenderJobSceneState() {
    const bool restoredScene = editorRenderJobSceneSnapshot_.has_value();
    if (restoredScene) {
        sceneDocument_ = std::move(*editorRenderJobSceneSnapshot_);
        editorRenderJobSceneSnapshot_.reset();
        if (uiOverlay_ != nullptr) {
            EditorTimeline& timeline = uiOverlay_->editor().timeline();
            timeline.playing = editorRenderJobTimelineWasPlaying_;
            timeline.currentFrame = editorRenderJobPreviousTimelineFrame_;
        }
    }
    if (uiOverlay_ != nullptr) {
        uiOverlay_->setRenderExtentOverride(std::nullopt);
    }
    if (pathTracer_ != nullptr) {
        if (editorRenderJobSettingsSnapshot_.has_value()) {
            (void)pathTracer_->applySettings(*editorRenderJobSettingsSnapshot_);
            editorRenderJobSettingsSnapshot_.reset();
        }
        if (restoredScene) {
            sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
            (void)applyPendingSceneUpdate(false);
        }
        pathTracer_->resetAccumulation(AccumulationResetReason::Manual);
    } else {
        editorRenderJobSettingsSnapshot_.reset();
    }
}

void Application::writeEditorRenderJobManifest(const char* eventLabel) {
    if (editorRenderJob_.manifestPath.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(editorRenderJob_.manifestPath.parent_path(), ec);
    if (ec) {
        editorRenderJob_.failed = true;
        editorRenderJob_.active = false;
        editorRenderJob_.status = "Unable to create render output folder";
        return;
    }

    const RendererSettings settings = editorRenderJobAppliedSettings_.value_or(pathTracer_->settings());
    nlohmann::json manifest;
    manifest["event"] = eventLabel;
    manifest["action"] = editorRenderJobAction(editorRenderJob_.kind);
    manifest["title"] = editorRenderJob_.title;
    manifest["status"] = editorRenderJob_.status;
    manifest["active"] = editorRenderJob_.active;
    manifest["completed"] = editorRenderJob_.completed;
    manifest["cancelled"] = editorRenderJob_.cancelled;
    manifest["failed"] = editorRenderJob_.failed;
    manifest["progress"] = editorRenderJob_.progress;
    manifest["current_frame"] = editorRenderJob_.currentFrame;
    manifest["total_frames"] = editorRenderJob_.totalFrames;
    manifest["rendered_frames"] = editorRenderJobFramesRendered_;
    if (editorRenderJob_.kind == EditorRenderJobKind::Sequence) {
        manifest["sequence"] = {
            {"timeline_start_frame", editorRenderJobSequenceStartFrame_},
            {"timeline_end_frame", editorRenderJobSequenceEndFrame_},
            {"timeline_frame_count", std::max(1, editorRenderJobSequenceEndFrame_ - editorRenderJobSequenceStartFrame_ + 1)},
            {"frames_per_timeline_frame", editorRenderJobSequenceFramesPerTimelineFrame_},
            {"output_frames_written", editorRenderJobSequenceOutputFramesWritten_},
            {"current_accumulation_frame", editorRenderJobSequenceAccumulationFrame_},
        };
    }
    manifest["output_root"] = editorRenderJob_.outputRoot.string();
    manifest["output_files"] = nlohmann::json::array();
    for (const std::filesystem::path& outputFile : editorRenderJobOutputFiles_) {
        manifest["output_files"].push_back({
            {"path", outputFile.string()},
            {"file", outputFile.filename().string()},
        });
    }
    manifest["scene_dirty"] = sceneDocument_.dirty();
    if (scenePath_.has_value()) {
        manifest["scene_path"] = scenePath_->string();
    }
    manifest["resolution"] = {
        {"width", pathTracer_->displayExtent().width},
        {"height", pathTracer_->displayExtent().height},
    };
    manifest["render_extent"] = {
        {"width", pathTracer_->renderExtent().width},
        {"height", pathTracer_->renderExtent().height},
    };
    if (editorRenderJobRequest_.has_value()) {
        const EditorRenderRequest& request = *editorRenderJobRequest_;
        manifest["requested"] = {
            {"output_root", request.outputRoot.string()},
            {"output_preset", request.outputPresetName},
            {"width", request.requestedWidth},
            {"height", request.requestedHeight},
            {"render_resolution_scale", request.renderResolutionScale},
            {"samples_per_pixel", request.samplesPerPixel},
            {"limit_samples_per_pixel", request.limitSamplesPerPixel},
            {"target_samples_per_pixel", request.targetSamplesPerPixel},
            {"image_accumulation_frames", request.imageAccumulationFrames},
            {"sequence_start_frame", request.sequenceStartFrame},
            {"sequence_end_frame", request.sequenceEndFrame},
            {"sequence_frames_per_timeline_frame", request.sequenceFramesPerTimelineFrame},
        };
    }
    manifest["renderer"] = {
        {"debug_view", rendererDebugViewName(settings.debugView)},
        {"samples_per_pixel", settings.samplesPerPixel},
        {"limit_samples_per_pixel", settings.limitSamplesPerPixel},
        {"render_resolution_scale", settings.renderResolutionScale},
        {"accumulation_limit", settings.accumulationLimit},
    };

    std::ofstream out(editorRenderJob_.manifestPath);
    if (out.is_open()) {
        out << manifest.dump(2);
    } else {
        editorRenderJob_.failed = true;
        editorRenderJob_.active = false;
        editorRenderJob_.status = "Unable to write render manifest";
    }
}

void Application::cancelEditorRenderJob(const std::filesystem::path& renderOutputRoot) {
    appendRenderHistoryEvent(renderOutputRoot, "StopRender", sceneDocument_, pathTracer_->settings());
    if (!editorRenderJob_.active) {
        notifications_.notify("No active render job to stop", NotificationType::Warning, NotificationAction::OpenOutputFolder, "Open Output", 5.0f);
        return;
    }

    editorRenderJob_.active = false;
    editorRenderJob_.cancelled = true;
    editorRenderJob_.status = "Render stopped";
    editorRenderJobFramePrepared_ = false;
    restoreEditorRenderJobSceneState();
    writeEditorRenderJobManifest("cancelled");
    notifications_.notify("Render stopped", NotificationType::Warning, NotificationAction::OpenOutputFolder, "Open Output", 5.0f);
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().log().add(EditorLogCategory::Command, "Render stopped: " + editorRenderJob_.outputRoot.string());
    }
}

void Application::onFilesDropped(int count, const char** paths) {
    if (pathTracer_ == nullptr || paths == nullptr || count <= 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        if (paths[i] == nullptr) {
            continue;
        }
        pendingDroppedFiles_.push_back(std::filesystem::path{paths[i]});
    }
    notifications_.notify("File drop queued", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
}

void Application::serializeEditorSceneData() {
    if (uiOverlay_ == nullptr) {
        return;
    }
    uiOverlay_->editor().cameraBookmarks().serialize(sceneDocument_);
    sceneDocument_.setTimelineJson(uiOverlay_->editor().timeline().serialize());
}

void Application::deserializeEditorSceneData() {
    if (uiOverlay_ == nullptr) {
        return;
    }
    uiOverlay_->editor().cameraBookmarks().deserialize(sceneDocument_);
    if (sceneDocument_.timelineJson().has_value()) {
        uiOverlay_->editor().timeline().deserialize(*sceneDocument_.timelineJson());
    } else {
        uiOverlay_->editor().timeline().clear();
    }
}

void Application::queueProjectThumbnailCapture() {
    if (headless_ || !project_.has_value()) {
        return;
    }
    pendingProjectThumbnailPath_ = project_->savedRoot / "Thumbnail.png";
    pendingProjectThumbnailFrame_ = frameSerial_ + CommandSystem::framesInFlight + 1u;
    pendingProjectThumbnailAttempts_ = 0;
}

void Application::captureProjectThumbnailIfReady() {
    if (!pendingProjectThumbnailPath_.has_value() || headless_ || frameSerial_ < pendingProjectThumbnailFrame_) {
        return;
    }
    if (pathTracer_ == nullptr || context_ == nullptr || allocator_ == nullptr || swapchain_ == nullptr) {
        return;
    }

    const VkExtent2D extent = pathTracer_->displayExtent();
    if (extent.width == 0 || extent.height == 0) {
        pendingProjectThumbnailFrame_ = frameSerial_ + 1u;
        if (++pendingProjectThumbnailAttempts_ > 8u) {
            pendingProjectThumbnailPath_.reset();
        }
        return;
    }

    try {
        DiagnosticImageExport exporter(*context_, *allocator_);
        if (!exporter.initialize(swapchain_->format(), extent) ||
            !exporter.exportView(*pathTracer_, RendererDebugView::Beauty, *pendingProjectThumbnailPath_, 0)) {
            pendingProjectThumbnailFrame_ = frameSerial_ + 1u;
            if (++pendingProjectThumbnailAttempts_ > 8u) {
                notifications_.notify("Project thumbnail capture failed", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
                pendingProjectThumbnailPath_.reset();
            }
            return;
        }
    } catch (const std::exception& error) {
        std::cerr << "Project thumbnail capture failed: " << error.what() << '\n';
        notifications_.notify("Project thumbnail capture failed", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        pendingProjectThumbnailPath_.reset();
        return;
    }

    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().invalidateAssetThumbnails();
        uiOverlay_->editor().log().add(EditorLogCategory::Project, "Captured project thumbnail: " + pendingProjectThumbnailPath_->string());
    }
    notifications_.notify("Project thumbnail captured", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
    pendingProjectThumbnailPath_.reset();
}

void Application::writeCrashMarker(bool running) {
    if (!project_.has_value()) {
        return;
    }
    const std::filesystem::path marker = project_->savedRoot / "editor_session.json";
    std::error_code ec;
    std::filesystem::create_directories(marker.parent_path(), ec);
    if (ec) {
        return;
    }
    if (!running) {
        std::filesystem::remove(marker, ec);
        return;
    }
    nlohmann::json json;
    json["running"] = true;
    json["scene"] = scenePath_.has_value() ? scenePath_->generic_string() : std::string{};
    json["project"] = project_->projectFile.generic_string();
    json["sceneAutosave"] = editorSceneAutosavePath(*project_, scenePath_, gltfPath_).generic_string();
    json["projectAutosave"] = editorProjectAutosavePath(*project_).generic_string();
    json["assetRegistryAutosave"] = editorAssetRegistryAutosavePath(*project_).generic_string();
    json["materialAssetAutosaves"] = nlohmann::json::array();
    for (const auto& [guid, path] : materialAssetAutosavePaths_) {
        json["materialAssetAutosaves"].push_back({
            {"guid", guid},
            {"path", path.generic_string()},
        });
    }
    std::ofstream out(marker, std::ios::trunc);
    if (out.is_open()) {
        out << json.dump(2);
    }
}

bool Application::writeAutosave() {
    const bool sceneDirty = sceneUnsavedDirty_ || sceneDocument_.dirty();
    if (!project_.has_value() || (!sceneDirty && !projectSettingsDirty_ && !assetRegistry_.dirty() && dirtyMaterialAssets_.empty())) {
        return false;
    }
    const std::filesystem::path autosaveDir = project_->savedRoot / "Autosaves";
    std::error_code ec;
    std::filesystem::create_directories(autosaveDir, ec);
    if (ec) {
        notifications_.notify("Autosave folder creation failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }

    bool wroteAny = false;
    bool failed = false;
    if (sceneDirty) {
        serializeEditorSceneData();
        const std::filesystem::path autosavePath = editorSceneAutosavePath(*project_, scenePath_, gltfPath_);
        if (sceneDocument_.saveJson(autosavePath)) {
            wroteAny = true;
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().log().add(EditorLogCategory::Scene, "Autosaved scene to " + autosavePath.string());
            }
        } else {
            failed = true;
        }
    }
    if (projectSettingsDirty_) {
        ProjectContext autosaveProject = *project_;
        autosaveProject.projectFile = editorProjectAutosavePath(*project_);
        if (saveProjectFile(autosaveProject)) {
            wroteAny = true;
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().log().add(EditorLogCategory::Project, "Autosaved project settings to " + autosaveProject.projectFile.string());
            }
        } else {
            failed = true;
        }
    }
    if (assetRegistry_.dirty()) {
        const std::filesystem::path autosavePath = editorAssetRegistryAutosavePath(*project_);
        if (assetRegistry_.save(autosavePath)) {
            wroteAny = true;
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().log().add(EditorLogCategory::Project, "Autosaved asset registry to " + autosavePath.string());
            }
        } else {
            failed = true;
        }
    }
    if (!dirtyMaterialAssets_.empty()) {
        if (autosaveDirtyMaterialAssets()) {
            wroteAny = true;
        } else {
            failed = true;
        }
    }
    if (failed) {
        notifications_.notify("Autosave failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    if (wroteAny) {
        notifications_.notify("Autosave updated", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
    }
    return wroteAny;
}

void Application::updateAutosave(float deltaSeconds) {
    const bool sceneDirty = sceneUnsavedDirty_ || sceneDocument_.dirty();
    if (!project_.has_value() || !project_->autosaveEnabled || (!sceneDirty && !projectSettingsDirty_ && !assetRegistry_.dirty() && dirtyMaterialAssets_.empty())) {
        autosaveElapsedSeconds_ = 0.0f;
        return;
    }
    autosaveElapsedSeconds_ += std::max(deltaSeconds, 0.0f);
    const float interval = static_cast<float>(std::max(project_->autosaveIntervalMinutes, 1) * 60);
    if (autosaveElapsedSeconds_ >= interval) {
        autosaveElapsedSeconds_ = 0.0f;
        (void)writeAutosave();
    }
}

void Application::reloadGltfScene(const std::filesystem::path& path) {
    SceneLoadResult result;
    result.mode = SceneLoadMode::ImportSceneAsNewScene;
    result.sourcePath = path;
    try {
        GltfLoader loader(result.assets);
        loader.setCacheWritesEnabled(false);
        result.importedScene = loader.loadWithCache(path);
        auto document = std::make_unique<SceneDocument>();
        document->importSceneAsset(*result.importedScene);
        document->setSourceGltfPath(path);
        result.stagedScene = std::move(document);
        result.success = true;
    } catch (const std::exception& error) {
        result.errorMessage = error.what();
    }
    (void)applySceneLoadResult(std::move(result));
}

bool Application::requestSceneLoad(SceneLoadRequest request) {
    if (request.serial == 0) {
        request.serial = nextSceneLoadJobSerial_++;
    }
    if (asyncSceneLoader_.isRunning()) {
        sceneLoadingStatus_ = "Scene load already running";
        if (activeSceneLoadRequest_.has_value()) {
            sceneLoadingStatus_ += ": " + activeSceneLoadRequest_->sourcePath.string();
        }
        notifications_.notify("Scene load already running", NotificationType::Warning);
        return false;
    }

    if (asyncSceneLoader_.hasCompletedResult()) {
        SceneLoadResult completed = asyncSceneLoader_.takeCompletedResult();
        activeSceneLoadRequest_.reset();
        if (completed.cancelled) {
            sceneLoadingStatus_ = "Scene load cancelled: " + completed.sourcePath.string();
            recordCompletedSceneLoadJob(completed, false, true, sceneLoadingStatus_);
            notifications_.notify("Scene load cancelled", NotificationType::Warning);
        } else if (!completed.success) {
            sceneLoadingStatus_ = std::string(sceneLoadModeLabel(completed.mode)) + " failed: " + completed.errorMessage;
            recordCompletedSceneLoadJob(completed, false, false, sceneLoadingStatus_, completed.errorMessage, completed.warningMessage);
            notifications_.notify(std::string(sceneLoadModeLabel(completed.mode)) + " failed", NotificationType::Error);
        } else {
            (void)applySceneLoadResult(std::move(completed));
        }
    }

    const std::string operation = sceneLoadModeLabel(request.mode);
    sceneLoadingStatus_ = operation + " queued: " + request.sourcePath.string();
    notifications_.notify(operation + " queued", NotificationType::Info);
    std::cout << sceneLoadingStatus_ << '\n';
    activeSceneLoadRequest_ = request;
    if (!asyncSceneLoader_.start(std::move(request))) {
        activeSceneLoadRequest_.reset();
        sceneLoadingStatus_ = "Scene load already running";
        notifications_.notify("Scene load already running", NotificationType::Warning);
        return false;
    }
    return true;
}

void Application::recordCompletedSceneLoadJob(
    const SceneLoadResult& result,
    bool success,
    bool cancelled,
    const std::string& status,
    const std::string& error,
    const std::string& warning) {
    completedSceneLoadJob_ = EditorJobCenterState{};
    completedSceneLoadJob_.completedSceneLoadSerial = result.serial;
    completedSceneLoadJob_.completedSceneLoadSuccess = success;
    completedSceneLoadJob_.completedSceneLoadCancelled = cancelled;
    completedSceneLoadJob_.completedSceneLoadTitle = sceneLoadModeLabel(result.mode);
    completedSceneLoadJob_.completedSceneLoadStatus = status;
    completedSceneLoadJob_.completedSceneLoadSourcePath = result.sourcePath;
    completedSceneLoadJob_.completedSceneLoadError = error;
    completedSceneLoadJob_.completedSceneLoadWarning = warning.empty() ? result.warningMessage : warning;
    completedSceneLoadJob_.completedSceneLoadWorkerTotalMs = result.workerTotalMs;
    completedSceneLoadJob_.completedSceneLoadWorkerSceneParseMs = result.workerSceneParseMs;
    completedSceneLoadJob_.completedSceneLoadWorkerGltfLoadMs = result.workerGltfLoadMs;
    completedSceneLoadJob_.completedSceneLoadWorkerDocumentBuildMs = result.workerDocumentBuildMs;
}

void Application::pollAsyncSceneLoad() {
    if (!asyncSceneLoader_.hasCompletedResult()) {
        if (asyncSceneLoader_.isRunning() && activeSceneLoadRequest_.has_value()) {
            const int progress = static_cast<int>(std::clamp(asyncSceneLoader_.progress(), 0.0f, 1.0f) * 100.0f);
            sceneLoadingStatus_ = std::string(sceneLoadModeLabel(activeSceneLoadRequest_->mode)) + " " +
                sceneLoadStatusLabel(asyncSceneLoader_.status()) + " (" + std::to_string(progress) + "%): " +
                asyncSceneLoader_.stage() + " - " + activeSceneLoadRequest_->sourcePath.string();
        }
        return;
    }

    SceneLoadResult result = asyncSceneLoader_.takeCompletedResult();
    activeSceneLoadRequest_.reset();
    if (result.cancelled) {
        sceneLoadingStatus_ = "Scene load cancelled: " + result.sourcePath.string();
        recordCompletedSceneLoadJob(result, false, true, sceneLoadingStatus_);
        notifications_.notify("Scene load cancelled", NotificationType::Warning);
        std::cout << sceneLoadingStatus_ << '\n';
        startNextPendingMergeScene();
        return;
    }
    if (!result.success) {
        sceneLoadingStatus_ = std::string(sceneLoadModeLabel(result.mode)) + " failed: " + result.errorMessage;
        recordCompletedSceneLoadJob(result, false, false, sceneLoadingStatus_, result.errorMessage, result.warningMessage);
        notifications_.notify(std::string(sceneLoadModeLabel(result.mode)) + " failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        startNextPendingMergeScene();
        return;
    }

    sceneLoadingStatus_ = std::string(sceneLoadModeLabel(result.mode)) + " applying: " + result.sourcePath.string();
    (void)applySceneLoadResult(std::move(result));
    startNextPendingMergeScene();
}

void Application::queueMergeScenes(std::vector<std::filesystem::path> paths) {
    for (std::filesystem::path& path : paths) {
        if (!path.empty()) {
            pendingMergeScenes_.push_back(std::move(path));
        }
    }
    if (pendingMergeScenes_.empty()) {
        return;
    }
    notifications_.notify("Merge levels queued", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
    startNextPendingMergeScene();
}

void Application::startNextPendingMergeScene() {
    if (asyncSceneLoader_.isRunning() || pendingMergeScenes_.empty()) {
        return;
    }
    SceneLoadRequest request;
    request.mode = SceneLoadMode::MergeSceneIntoCurrent;
    request.sourcePath = std::move(pendingMergeScenes_.front());
    pendingMergeScenes_.pop_front();
    if (project_.has_value()) {
        request.projectSnapshot = *project_;
    }
    if (!requestSceneLoad(std::move(request))) {
        if (!pendingMergeScenes_.empty()) {
            notifications_.notify("Some queued level merges are still pending", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
    }
}

bool Application::applySceneLoadResult(SceneLoadResult&& result) {
    switch (result.mode) {
    case SceneLoadMode::OpenRtLevel:
    case SceneLoadMode::LoadProjectStartupScene:
        return applyReplacementSceneResult(std::move(result), false);
    case SceneLoadMode::ImportSceneAsNewScene:
        return applyReplacementSceneResult(std::move(result), true);
    case SceneLoadMode::MergeSceneIntoCurrent:
        return applyMergeSceneResult(std::move(result));
    }
    return false;
}

bool Application::applyReplacementSceneResult(SceneLoadResult&& result, bool sceneDirtyAfterApply) {
    if (!context_ || !allocator_ || !uploader_ || !swapchain_ || !commandSystem_) {
        recordCompletedSceneLoadJob(result, false, false, "Scene apply failed: renderer context is not ready", "Renderer context is not ready", result.warningMessage);
        return false;
    }
    if (!result.success || result.stagedScene == nullptr) {
        sceneLoadingStatus_ = std::string(sceneLoadModeLabel(result.mode)) + " failed: " + result.errorMessage;
        recordCompletedSceneLoadJob(result, false, false, sceneLoadingStatus_, result.errorMessage, result.warningMessage);
        notifications_.notify(std::string(sceneLoadModeLabel(result.mode)) + " failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return false;
    }

    const auto applyStart = std::chrono::steady_clock::now();
    auto elapsedMs = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    double prepareDocumentMs = 0.0;
    double sceneBuildMs = 0.0;
    double rendererCreateMs = 0.0;
    double stateSwapMs = 0.0;

    const bool restoreAutosaveAsUnsaved = result.restoreAsUnsaved && result.mode == SceneLoadMode::OpenRtLevel;
    const bool markSceneDirtyAfterApply = sceneDirtyAfterApply || restoreAutosaveAsUnsaved;
    const RendererSettings previousSettings = pathTracer_ != nullptr ? pathTracer_->settings() : RendererSettings{};
    const std::optional<std::filesystem::path> nextGltfPath = sceneDirtyAfterApply
        ? std::optional<std::filesystem::path>{result.sourcePath}
        : result.stagedScene->sourceGltfPath();
    const std::optional<std::filesystem::path> nextHdrPath = sceneDirtyAfterApply
        ? hdrPath_
        : result.stagedScene->sourceHdrPath();
    RendererSettings reloadSettings = sceneDirtyAfterApply
        ? previousSettings
        : rendererSettingsFromDocument(*result.stagedScene, previousSettings);

    try {
        const auto prepareDocumentStart = std::chrono::steady_clock::now();
        SceneDocument nextDocument = std::move(*result.stagedScene);
        if (sceneDirtyAfterApply) {
            nextDocument.setSourceHdrPath(hdrPath_);
            syncDocumentRenderSettings(nextDocument, reloadSettings);
        }
        (void)SunController::migrateLegacyDirectionalSun(nextDocument);
        (void)SunController::repairPrimarySunTransform(nextDocument);
        if (!nextDocument.prefabInstances().empty()) {
            std::filesystem::path registryRoot = project_.has_value() ? project_->projectRoot : result.sourcePath.parent_path();
            if (!project_.has_value()) {
                const std::filesystem::path sceneRegistryPath = result.sourcePath.parent_path() / (result.sourcePath.stem().string() + ".assets.json");
                if (std::filesystem::exists(sceneRegistryPath) && assetRegistry_.state().path != sceneRegistryPath) {
                    std::string registryError;
                    if (!assetRegistry_.load(sceneRegistryPath, &registryError)) {
                        std::cerr << "Scene asset registry load failed: " << registryError << '\n';
                    } else {
                        (void)assetRegistry_.refreshRecordHealth(registryRoot, false);
                    }
                }
            }
            PrefabRuntimeBindings prefabBindings;
            for (const PrefabInstance& instance : nextDocument.prefabInstances()) {
                const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
                    return record.guid == instance.prefabGuid && record.type == AssetType::Prefab;
                });
                if (recordIt == assetRegistry_.records().end()) {
                    continue;
                }
                PrefabAsset prefab;
                std::string prefabError;
                (void)loadPrefabAsset(resolveAssetRecordPath(*recordIt, registryRoot), prefab, &prefabError);
                if (std::string bindError; !appendPrefabRuntimeAssets(*recordIt, prefab, registryRoot, &assetRegistry_, result.assets, prefabBindings, &bindError)) {
                    std::cerr << "Prefab runtime binding failed during scene load: " << bindError << '\n';
                }
            }
            const uint32_t rebound = rebindGuidBackedRenderers(nextDocument, prefabBindings);
            if (rebound > 0) {
                nextDocument.markDirty(SceneUpdateKind::TopologyChanged);
            }
        }
        reloadSettings = rendererSettingsFromDocument(nextDocument, reloadSettings);
        if (result.importedScene.has_value()) {
            bool finalInteractiveSettingsChanged = false;
            const bool importSafeRuntime = sceneDirtyAfterApply && !headless_ && result.mode == SceneLoadMode::ImportSceneAsNewScene;
            reloadSettings = interactiveSettingsForScene(reloadSettings, *result.importedScene, result.assets, importSafeRuntime, finalInteractiveSettingsChanged);
            if (finalInteractiveSettingsChanged && sceneDirtyAfterApply) {
                syncDocumentRenderSettings(nextDocument, reloadSettings);
            }
        }
        applyDocumentMaterialAssignments(nextDocument, result.assets);
        prepareDocumentMs = elapsedMs(prepareDocumentStart);

        std::cout << sceneLoadModeLabel(result.mode) << " apply stage: scene_builder path="
                  << result.sourcePath.string() << '\n' << std::flush;
        const auto sceneBuildStart = std::chrono::steady_clock::now();
        const SceneGpuBuildResult build = sceneBuilder_.build(nextDocument, &result.assets, reloadSettings);
        sceneBuildMs = elapsedMs(sceneBuildStart);
        const std::optional<std::filesystem::path> cachePath = sceneDirtyAfterApply
            ? SceneCache::cachePathFor(result.sourcePath)
            : (nextGltfPath.has_value() ? SceneCache::cachePathFor(*nextGltfPath) : std::optional<std::filesystem::path>{});
        std::optional<std::filesystem::path> rendererCachePath;
        if (result.importedScene.has_value() && cachePath.has_value() && nextDocument.prefabInstances().empty() &&
            build.sceneAsset.meshes.size() == result.importedScene->meshes.size() &&
            build.sceneAsset.materials.size() == result.importedScene->materials.size() &&
            build.sceneAsset.textures.size() == result.importedScene->textures.size() &&
            build.sceneAsset.nodes.size() == result.importedScene->nodes.size()) {
            rendererCachePath = cachePath;
        }

        std::cout << sceneLoadModeLabel(result.mode) << " apply stage: renderer_create meshes="
                  << build.sceneAsset.meshes.size()
                  << " materials=" << build.sceneAsset.materials.size()
                  << " textures=" << build.sceneAsset.textures.size()
                  << " path=" << result.sourcePath.string() << '\n' << std::flush;
        const auto rendererCreateStart = std::chrono::steady_clock::now();
        RendererSettings replacementSettings = build.rendererSettings;
        if (disableDlssForRendererReplacement(replacementSettings)) {
            syncDocumentRenderSettings(nextDocument, replacementSettings);
            const std::string message = "DLSS disabled for scene rebuild; re-enable it after the scene is loaded.";
            std::cerr << message << '\n';
            notifications_.notify(message, NotificationType::Warning, NotificationAction::OpenRenderSettings, "Render Settings", 6.0f);
        }
        preparePathTracerForRendererReplacement(pathTracer_ != nullptr ? pathTracer_->settings() : replacementSettings);
        std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
            build.sceneAsset.meshes.empty() ? nullptr : &build.sceneAsset,
            build.sceneAsset.meshes.empty() ? nullptr : &result.assets,
            rendererCachePath,
            &replacementSettings);
        rendererCreateMs = elapsedMs(rendererCreateStart);

        const auto stateSwapStart = std::chrono::steady_clock::now();
        if (uiOverlay_) {
            uiOverlay_->invalidateRendererTextures();
            uiOverlay_->editor().invalidateAssetThumbnails();
            uiOverlay_->editor().clearSelection();
        }
        cameraController_.releaseMouse(window_);
        pendingPostFrameSettings_.reset();
        editorPlacement_ = {};

        assets_ = std::move(result.assets);
        importedScene_ = std::move(result.importedScene);
        gltfPath_ = nextGltfPath;
        hdrPath_ = nextHdrPath;
        if (sceneDirtyAfterApply) {
            scenePath_.reset();
        } else if (restoreAutosaveAsUnsaved && !result.restoredScenePath.empty()) {
            scenePath_ = result.restoredScenePath;
        } else if (restoreAutosaveAsUnsaved) {
            scenePath_.reset();
        } else {
            scenePath_ = result.sourcePath;
        }
        sceneDocument_ = std::move(nextDocument);
        sceneDocument_.clearDirty();
        sceneUnsavedDirty_ = markSceneDirtyAfterApply;
        undoStack_.clear();
        gpuSceneAsset_ = std::move(build.sceneAsset);
        gpuInstanceEntities_ = std::move(build.instanceEntities);
        retirePathTracer(std::move(pathTracer_));
        pathTracer_ = std::move(nextPathTracer);
        applyActiveSceneCamera();
        commandSystem_->setPathTracer(pathTracer_.get());
        showMainWindowIfHidden();
        stateSwapMs = elapsedMs(stateSwapStart);
    } catch (const std::exception& error) {
        sceneLoadingStatus_ = std::string(sceneLoadModeLabel(result.mode)) + " apply failed: " + error.what();
        recordCompletedSceneLoadJob(result, false, false, sceneLoadingStatus_, error.what(), result.warningMessage);
        notifications_.notify(std::string(sceneLoadModeLabel(result.mode)) + " apply failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        std::cerr << sceneLoadModeLabel(result.mode)
                  << " apply failed after_ms=" << elapsedMs(applyStart)
                  << " prepare_ms=" << prepareDocumentMs
                  << " scene_build_ms=" << sceneBuildMs
                  << " renderer_create_ms=" << rendererCreateMs
                  << " state_swap_ms=" << stateSwapMs
                  << " worker_total_ms=" << result.workerTotalMs
                  << " path=" << result.sourcePath.string() << '\n';
        return false;
    }

    if (uiOverlay_) {
        if (sceneDirtyAfterApply) {
            uiOverlay_->editor().timeline().clear();
            sceneDocument_.clearTimelineJson();
        } else {
            deserializeEditorSceneData();
        }
    }
    sceneLoadingStatus_ = std::string(sceneLoadModeLabel(result.mode)) + " completed: " + result.sourcePath.string();
    recordCompletedSceneLoadJob(result, true, false, sceneLoadingStatus_, {}, result.warningMessage);
    if (uiOverlay_) {
        EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
        prefs.addRecentFile(result.sourcePath);
        (void)saveActiveEditorPreferences();
    }
    if (!result.warningMessage.empty()) {
        notifications_.notify(result.warningMessage, NotificationType::Warning);
        std::cerr << result.warningMessage << '\n';
    }
    notifications_.notify(std::string(sceneLoadModeLabel(result.mode)) + " completed", NotificationType::Success);
    if (project_.has_value() && result.mode == SceneLoadMode::LoadProjectStartupScene) {
        queueProjectThumbnailCapture();
    }
    const double applyTotalMs = elapsedMs(applyStart);
    std::cout << sceneLoadModeLabel(result.mode)
              << " apply timings: total_ms=" << applyTotalMs
              << " prepare_ms=" << prepareDocumentMs
              << " scene_build_ms=" << sceneBuildMs
              << " renderer_create_ms=" << rendererCreateMs
              << " state_swap_ms=" << stateSwapMs
              << " worker_total_ms=" << result.workerTotalMs
              << " worker_gltf_cache_ms=" << result.workerGltfLoadMs
              << " path=" << result.sourcePath.string() << '\n';
    std::cout << sceneLoadingStatus_;
    if (importedScene_.has_value()) {
        std::cout << " meshes=" << importedScene_->meshes.size()
                  << " materials=" << importedScene_->materials.size()
                  << " textures=" << importedScene_->textures.size()
                  << " nodes=" << importedScene_->nodes.size();
    }
    std::cout << '\n';
    return true;
}

bool Application::applyMergeSceneResult(SceneLoadResult&& result) {
    if (!result.success || !result.importedScene.has_value()) {
        sceneLoadingStatus_ = "Merge Scene failed: " + result.errorMessage;
        recordCompletedSceneLoadJob(result, false, false, sceneLoadingStatus_, result.errorMessage, result.warningMessage);
        notifications_.notify("Merge Scene failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return false;
    }
    if (result.importedScene->nodes.empty() && result.importedScene->lights.empty()) {
        sceneLoadingStatus_ = "Merge Scene produced no entities: " + result.sourcePath.string();
        recordCompletedSceneLoadJob(result, false, false, sceneLoadingStatus_, "Merge Scene produced no entities", result.warningMessage);
        notifications_.notify("Merge Scene produced no entities", NotificationType::Warning);
        std::cerr << sceneLoadingStatus_ << '\n';
        return false;
    }

    AssetManager previousAssets = assets_;
    AssetManager nextAssets = assets_;
    SceneAsset sceneToMerge = *result.importedScene;
    const ImportedAssetHandleRemap remap = appendImportedAssets(nextAssets, result.assets);
    remapSceneAssetHandles(sceneToMerge, remap);

    assets_ = std::move(nextAssets);
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    const std::string rootName = "Merged " + result.sourcePath.stem().string();
    const EntityId root = sceneOps.mergeSceneAsset(sceneToMerge, rootName);
    if (!root.valid()) {
        assets_ = std::move(previousAssets);
        sceneLoadingStatus_ = "Merge Scene failed during apply: " + result.sourcePath.string();
        recordCompletedSceneLoadJob(result, false, false, sceneLoadingStatus_, "Merge Scene failed during apply", result.warningMessage);
        notifications_.notify("Merge Scene failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return false;
    }

    importedScene_ = sceneDocument_.toSceneAsset();
    sceneUnsavedDirty_ = true;
    const bool rebuilt = applyPendingSceneUpdate(true);
    sceneLoadingStatus_ = "Merge Scene completed: " + result.sourcePath.string();
    recordCompletedSceneLoadJob(result, true, false, sceneLoadingStatus_, {}, result.warningMessage);
    notifications_.notify(rebuilt ? "Scene merged" : "Scene merged; renderer rebuild pending", NotificationType::Success);
    std::cout << "Merged scene into current: " << result.sourcePath.string()
              << " nodes=" << result.importedScene->nodes.size()
              << " lights=" << result.importedScene->lights.size()
              << " root=" << root.index << ':' << root.generation << '\n';
    return true;
}

Application::DirtyScenePromptResult Application::promptDirtySceneBefore(std::string_view action) const {
    const bool sceneDirty = sceneUnsavedDirty_ || sceneDocument_.dirty();
    const bool registryDirty = assetRegistry_.dirty();
    const bool materialAssetsDirty = !dirtyMaterialAssets_.empty();
    if (!sceneDirty && !projectSettingsDirty_ && !registryDirty && !materialAssetsDirty) {
        return DirtyScenePromptResult::Discard;
    }

    const std::string sceneName = scenePath_.has_value()
        ? scenePath_->filename().string()
        : (gltfPath_.has_value() ? gltfPath_->filename().string() : std::string("Untitled Scene"));
    std::vector<std::string> dirtyBuckets;
    if (sceneDirty) {
        dirtyBuckets.push_back("Level: " + sceneName);
    }
    if (projectSettingsDirty_) {
        dirtyBuckets.push_back("Project settings");
    }
    if (registryDirty) {
        dirtyBuckets.push_back("Asset registry metadata");
    }
    if (materialAssetsDirty) {
        dirtyBuckets.push_back("Linked material asset metadata: " + std::to_string(dirtyMaterialAssets_.size()));
    }

    std::string dirtySummary;
    for (const std::string& bucket : dirtyBuckets) {
        dirtySummary += "- " + bucket + "\n";
    }
    const std::string message = "Save editor changes before " + std::string(action) + "?\n\n" + dirtySummary + "\n"
        "Unsaved changes will be lost if you choose Do Not Save.";

#if defined(_WIN32)
    const std::wstring wideMessage = widenAscii(message);
    const int result = MessageBoxW(
        nullptr,
        wideMessage.c_str(),
        L"Unsaved Scene Changes",
        MB_ICONWARNING | MB_YESNOCANCEL | MB_DEFBUTTON1 | MB_APPLMODAL);
    if (result == IDYES) {
        return DirtyScenePromptResult::Save;
    }
    if (result == IDNO) {
        return DirtyScenePromptResult::Discard;
    }
#else
    std::cerr << message << "\nAction cancelled because modal dirty-scene prompts are only implemented on this platform.\n";
#endif
    return DirtyScenePromptResult::Cancel;
}

bool Application::saveCurrentSceneForDirtyPrompt() {
    std::filesystem::path savePath;
    if (scenePath_.has_value()) {
        savePath = *scenePath_;
    } else if (auto selected = saveSceneJsonFileDialog()) {
        savePath = *selected;
    }

    if (savePath.empty()) {
        notifications_.notify("Scene save cancelled", NotificationType::Warning);
        return false;
    }

    serializeEditorSceneData();
    if (!sceneDocument_.saveJson(savePath)) {
        notifications_.notify("Scene save failed", NotificationType::Error);
        std::cerr << "Scene save failed: " << savePath.string() << '\n';
        return false;
    }

    scenePath_ = savePath;
    sceneDocument_.clearDirty();
    sceneUnsavedDirty_ = false;
    notifications_.notify("Scene saved", NotificationType::Success);
    std::cout << "Saved scene: " << savePath.string() << '\n';
    return true;
}

std::optional<AssetRecord> Application::materialAssetRecordForMaterial(uint32_t materialId) const {
    if (!importedScene_.has_value() || assetRegistry_.records().empty()) {
        return std::nullopt;
    }
    const auto& sceneMaterials = importedScene_->materials;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.type != AssetType::Material || record.sourceHash.empty() || record.importSettingsHash.empty()) {
            continue;
        }
        for (size_t i = 0; i < sceneMaterials.size(); ++i) {
            const MaterialAssetHandle handle = sceneMaterials[i];
            if (!handle.valid() || handle.index != materialId) {
                continue;
            }
            if (importedAssetGuidFor(record.sourceHash, record.importSettingsHash, "Material", i) == record.guid) {
                return record;
            }
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Application::loadedMaterialIndexForRecord(const AssetRecord& record) const {
    if (record.type != AssetType::Material) {
        return std::nullopt;
    }
    if (importedScene_.has_value() && !record.sourceHash.empty() && !record.importSettingsHash.empty()) {
        const auto& sceneMaterials = importedScene_->materials;
        for (size_t i = 0; i < sceneMaterials.size(); ++i) {
            const MaterialAssetHandle handle = sceneMaterials[i];
            if (handle.valid() && importedAssetGuidFor(record.sourceHash, record.importSettingsHash, "Material", i) == record.guid) {
                return handle.index;
            }
        }
    }
    for (const Entity* entity : sceneDocument_.registry().entities()) {
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            continue;
        }
        for (const MaterialSlot& slot : entity->meshRenderer->materialSlots) {
            if (slot.materialGuid == record.guid && slot.material.valid()) {
                return slot.material.index;
            }
            if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == record.guid && slot.overrideMaterial.has_value()) {
                return slot.overrideMaterial->index;
            }
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Application::loadedMeshIndexForRecord(const AssetRecord& record) const {
    if (record.type != AssetType::Mesh) {
        return std::nullopt;
    }
    if (importedScene_.has_value() && !record.sourceHash.empty() && !record.importSettingsHash.empty()) {
        const auto& sceneMeshes = importedScene_->meshes;
        for (size_t i = 0; i < sceneMeshes.size(); ++i) {
            const MeshAssetHandle handle = sceneMeshes[i];
            if (handle.valid() && importedAssetGuidFor(record.sourceHash, record.importSettingsHash, "Mesh", i) == record.guid) {
                return handle.index;
            }
        }
    }
    for (const Entity* entity : sceneDocument_.registry().entities()) {
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            continue;
        }
        const MeshRenderer& renderer = *entity->meshRenderer;
        if (renderer.meshGuid == record.guid && renderer.mesh.valid()) {
            return renderer.mesh.index;
        }
    }
    return std::nullopt;
}

bool Application::writeMaterialAssetFile(const AssetRecord& record, const MaterialAsset& material, const std::filesystem::path& path, bool autosave) {
    if (path.empty()) {
        return false;
    }
    nlohmann::json root = nlohmann::json::object();
    if (!autosave) {
        try {
            std::ifstream existing(path);
            if (existing.is_open()) {
                existing >> root;
            }
        } catch (...) {
            root = nlohmann::json::object();
        }
    }
    if (!root.is_object()) {
        root = nlohmann::json::object();
    }

    const nlohmann::json materialOverride = materialEditorOverrideJson(material);
    root["version"] = root.value("version", 1);
    root["kind"] = autosave ? "MaterialAssetAutosave" : root.value("kind", std::string("EditedMaterialAsset"));
    root["guid"] = record.guid;
    root["displayName"] = record.displayName.empty() ? material.name : record.displayName;
    root["sourcePath"] = record.sourcePath;
    root["sourceHash"] = record.sourceHash;
    root["importSettingsHash"] = record.importSettingsHash;
    root["alphaMode"] = materialAlphaModeName(material.alphaMode);
    root["doubleSided"] = material.doubleSided != 0u;
    root["pbr"] = materialOverride;
    root["editorMaterialOverride"] = materialOverride;
    root["lastEditedTimestamp"] = editorTimestampString();
    if (autosave) {
        root["targetImportedPath"] = record.importedPath;
        root["autosavePolicy"] = "Restore by reopening the project, reviewing this transparent metadata file, and saving the material asset through Save All after applying the intended values.";
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path tempPath = path.string() + ".tmp";
    std::ofstream out(tempPath, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << root.dump(2) << '\n';
    out.close();
    if (!out) {
        return false;
    }
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tempPath, path, ec);
    }
    return !ec;
}

bool Application::saveDirtyMaterialAsset(const AssetGuid& guid, std::string& saved, std::string& failure) {
    if (guid.empty()) {
        failure = "Material Asset: no material asset selected";
        return false;
    }
    if (!project_.has_value()) {
        failure = "Material Assets: no project is open";
        return false;
    }
    const auto dirtyIt = dirtyMaterialAssets_.find(guid);
    if (dirtyIt == dirtyMaterialAssets_.end()) {
        failure = "Material Asset: no unsaved edits for " + guid;
        return false;
    }
    auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
        return record.guid == guid && record.type == AssetType::Material;
    });
    if (recordIt == assetRegistry_.records().end()) {
        failure = "Material Asset: missing registry record " + guid;
        return false;
    }

    const std::filesystem::path materialPath = resolveAssetRecordPath(*recordIt, project_->projectRoot);
    if (!writeMaterialAssetFile(*recordIt, dirtyIt->second, materialPath, false)) {
        failure = "Material Asset: " + materialPath.string();
        return false;
    }

    AssetRecord updated = *recordIt;
    updated.importedHash = assetSourceHashForPath(materialPath);
    updated.lastModifiedTimestamp = editorTimestampString();
    updated.importedMetadataMissing = false;
    updated.missing = false;
    updated.status = AssetImportStatus::Imported;
    assetRegistry_.addOrReplaceRecord(std::move(updated), AssetRegistryDirtyReason::AssetEdited);

    dirtyMaterialAssets_.erase(guid);
    materialAssetAutosavePaths_.erase(guid);
    saved = "Material Asset: " + materialPath.string();
    return true;
}

bool Application::saveDirtyMaterialAssets(std::vector<std::string>& saved, std::vector<std::string>& failures) {
    if (dirtyMaterialAssets_.empty()) {
        return true;
    }

    bool allSaved = true;
    std::vector<AssetGuid> pendingGuids;
    pendingGuids.reserve(dirtyMaterialAssets_.size());
    for (const auto& [guid, material] : dirtyMaterialAssets_) {
        (void)material;
        pendingGuids.push_back(guid);
    }
    for (const AssetGuid& guid : pendingGuids) {
        std::string savedItem;
        std::string failureItem;
        if (saveDirtyMaterialAsset(guid, savedItem, failureItem)) {
            saved.push_back(std::move(savedItem));
        } else {
            failures.push_back(std::move(failureItem));
            allSaved = false;
        }
    }
    return allSaved;
}

bool Application::autosaveDirtyMaterialAssets() {
    if (!project_.has_value() || dirtyMaterialAssets_.empty()) {
        return false;
    }
    bool wroteAny = false;
    for (const auto& [guid, material] : dirtyMaterialAssets_) {
        auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
            return record.guid == guid && record.type == AssetType::Material;
        });
        if (recordIt == assetRegistry_.records().end()) {
            continue;
        }
        const std::filesystem::path autosavePath = editorMaterialAssetAutosavePath(*project_, *recordIt);
        if (writeMaterialAssetFile(*recordIt, material, autosavePath, true)) {
            materialAssetAutosavePaths_[guid] = autosavePath;
            wroteAny = true;
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().log().add(EditorLogCategory::Project, "Autosaved material asset to " + autosavePath.string());
            }
        }
    }
    return wroteAny;
}

bool Application::restoreMaterialAssetAutosaves() {
    if (!project_.has_value() || pendingRecoveryMaterialAssetAutosaves_.empty()) {
        return false;
    }

    bool restoredAny = false;
    bool runtimeUpdated = false;
    for (const auto& [pendingGuid, autosavePath] : pendingRecoveryMaterialAssetAutosaves_) {
        if (autosavePath.empty() || !std::filesystem::exists(autosavePath)) {
            continue;
        }
        try {
            std::ifstream in(autosavePath);
            nlohmann::json root;
            in >> root;
            const AssetGuid guid = root.value("guid", pendingGuid);
            if (guid.empty()) {
                continue;
            }
            std::optional<MaterialAsset> restoredMaterial = materialAssetFromEditorOverrideJson(root);
            if (!restoredMaterial.has_value()) {
                std::cerr << "Material autosave restore skipped, missing editor override: " << autosavePath.string() << '\n';
                continue;
            }
            auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
                return record.guid == guid && record.type == AssetType::Material;
            });
            if (recordIt == assetRegistry_.records().end()) {
                std::cerr << "Material autosave restore skipped, missing registry record: " << guid << '\n';
                continue;
            }

            dirtyMaterialAssets_[guid] = *restoredMaterial;
            materialAssetAutosavePaths_[guid] = autosavePath;
            assetRegistry_.markDirty(AssetRegistryDirtyReason::AssetAutosaveRestored);
            restoredAny = true;

            if (std::optional<uint32_t> materialIndex = loadedMaterialIndexForRecord(*recordIt)) {
                if (MaterialAsset* material = assets_.material(MaterialAssetHandle{*materialIndex})) {
                    *material = *restoredMaterial;
                    for (uint32_t meshIndex = 0; meshIndex < assets_.meshes().size(); ++meshIndex) {
                        MeshAsset* mesh = assets_.mesh(MeshAssetHandle{meshIndex});
                        if (mesh == nullptr) {
                            continue;
                        }
                        for (MeshPrimitiveAsset& primitive : mesh->primitives) {
                            if (primitive.material.index == *materialIndex) {
                                updatePrimitiveAlphaClassification(primitive, material);
                            }
                        }
                    }
                    runtimeUpdated = true;
                }
            }
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().log().add(EditorLogCategory::Project, "Restored material asset autosave as unsaved metadata: " + autosavePath.string());
            }
        } catch (const std::exception& error) {
            std::cerr << "Material autosave restore failed: " << autosavePath.string() << " " << error.what() << '\n';
        }
    }

    if (runtimeUpdated) {
        bool gpuUpdated = false;
        if (gpuSceneAsset_.has_value() && pathTracer_ != nullptr) {
            gpuUpdated = pathTracer_->updateMaterials(*gpuSceneAsset_, assets_);
        }
        if (!gpuUpdated && pathTracer_ != nullptr) {
            pathTracer_->resetAccumulation(AccumulationResetReason::MaterialChanged);
        }
    }
    return restoredAny;
}

bool Application::saveAllEditorState() {
    std::vector<std::string> failures;
    std::vector<std::string> saved;

    auto logFailure = [&](std::string message) {
        failures.push_back(std::move(message));
    };
    auto logSaved = [&](std::string message) {
        saved.push_back(std::move(message));
    };

    const bool sceneNeedsSave = sceneUnsavedDirty_ || sceneDocument_.dirty();
    if (sceneNeedsSave) {
        std::filesystem::path savePath;
        if (scenePath_.has_value()) {
            savePath = *scenePath_;
        } else if (auto selected = saveSceneJsonFileDialog()) {
            savePath = *selected;
        }

        if (savePath.empty()) {
            logFailure("Scene has unsaved changes but no save path was selected");
        } else {
            serializeEditorSceneData();
            if (sceneDocument_.saveJson(savePath)) {
                scenePath_ = savePath;
                sceneDocument_.clearDirty();
                sceneUnsavedDirty_ = false;
                logSaved("Scene: " + savePath.string());
            } else {
                logFailure("Scene: " + savePath.string());
            }
        }
    }

    if (project_.has_value()) {
        if (saveProjectFile(*project_)) {
            projectSettingsDirty_ = false;
            logSaved("Project: " + project_->projectFile.string());
        } else {
            logFailure("Project: " + project_->projectFile.string());
        }
    }

    (void)saveDirtyMaterialAssets(saved, failures);

    if (assetRegistry_.dirty() || !assetRegistry_.state().path.empty()) {
        const std::filesystem::path registryPath = assetRegistry_.state().path;
        if (assetRegistry_.save()) {
            assetRegistry_.clearDirty();
            logSaved("Asset Registry: " + registryPath.string());
        } else {
            logFailure(registryPath.empty()
                ? std::string("Asset Registry: no registry path")
                : std::string("Asset Registry: ") + registryPath.string());
        }
    }

    if (uiOverlay_ != nullptr) {
        const std::filesystem::path prefsPath = activeEditorPreferencesPath();
        if (saveActiveEditorPreferences()) {
            logSaved("Editor Preferences: " + prefsPath.string());
        } else {
            logFailure("Editor Preferences: " + prefsPath.string());
        }
    }

    if (failures.empty() && project_.has_value() && !assetRegistry_.state().path.empty()) {
        std::filesystem::path referenceIndexPath;
        std::string referenceIndexError;
        if (refreshPersistentAssetReferenceIndex(
                *project_,
                assetRegistry_,
                referenceIndexPath,
                referenceIndexError,
                "SaveAll",
                "This index was refreshed after Save All completed scene, project, material, and asset registry persistence.")) {
            logSaved("Asset Reference Index: " + referenceIndexPath.string());
            std::cout << "Persistent project reference index refreshed after Save All: " << referenceIndexPath.string() << '\n';
        } else {
            logFailure("Asset Reference Index: " + referenceIndexError);
        }
    }

    if (uiOverlay_ != nullptr) {
        EditorLog& log = uiOverlay_->editor().log();
        for (const std::string& item : saved) {
            log.add(EditorLogCategory::Project, "Save All saved " + item);
        }
        for (const std::string& item : failures) {
            log.add(EditorLogCategory::Warning, "Save All failed " + item);
        }
    }

    if (!failures.empty()) {
        notifications_.notify("Save All failed for " + std::to_string(failures.size()) + " item(s)", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        for (const std::string& item : failures) {
            std::cerr << "Save All failed: " << item << '\n';
        }
        return false;
    }

    if (saved.empty()) {
        notifications_.notify("Nothing to save", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
    } else {
        notifications_.notify("Save All complete", NotificationType::Success, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
    }
    return true;
}

bool Application::confirmDestructiveSceneAction(std::string_view action) {
    switch (promptDirtySceneBefore(action)) {
    case DirtyScenePromptResult::Discard:
        return true;
    case DirtyScenePromptResult::Save:
        return saveAllEditorState();
    case DirtyScenePromptResult::Cancel:
        notifications_.notify("Scene action cancelled", NotificationType::Warning);
        return false;
    }
    return false;
}

bool Application::writeDefaultProjectScene(const ProjectContext& project, std::string_view templateName) {
    SceneDocument document;
    const std::string name(templateName);

    auto createCamera = [&]() -> EntityId {
        EntityId camera = document.registry().createEntity("Camera", SceneUpdateKind::CameraOnly);
        Camera cameraComponent;
        cameraComponent.active = true;
        document.registry().addCamera(camera, cameraComponent);
        document.setActiveCamera(camera);
        if (Entity* entity = document.registry().entity(camera)) {
            entity->defaultTransform = entity->transform;
        }
        return camera;
    };

    auto createSun = [&]() -> EntityId {
        EntityId sun = document.registry().createEntity("Sun Light", SceneUpdateKind::LightOnly);
        if (Entity* entity = document.registry().entity(sun)) {
            entity->sun = Sun{};
            entity->sun->elevation = 0.85f;
            entity->sun->azimuth = glm::pi<float>();
            entity->defaultTransform = entity->transform;
        }
        document.setPrimarySun(sun);
        return sun;
    };

    auto createEnvironmentLight = [&]() -> EntityId {
        EntityId environment = document.registry().createEntity("Environment Light", SceneUpdateKind::RendererSettingsOnly);
        if (Entity* entity = document.registry().entity(environment)) {
            entity->environmentLight = EnvironmentLight{};
            entity->defaultTransform = entity->transform;
            document.worldSettings().activeEnvironment = environment;
        }
        return environment;
    };

    auto createSkyAtmosphere = [&]() -> EntityId {
        EntityId sky = document.registry().createEntity("Sky Atmosphere", SceneUpdateKind::RendererSettingsOnly);
        if (Entity* entity = document.registry().entity(sky)) {
            entity->skyAtmosphere = SkyAtmosphere{};
            entity->defaultTransform = entity->transform;
            document.worldSettings().skyAtmosphere = sky;
        }
        return sky;
    };

    auto createAreaLight = [&]() -> EntityId {
        EntityId light = document.registry().createEntity("Area Light", SceneUpdateKind::LightOnly);
        if (Entity* entity = document.registry().entity(light)) {
            Light area;
            area.type = LightType::Area;
            area.sizeOrRadius = 1.0f;
            area.intensity = 8.0f;
            entity->light = area;
            entity->transform.position = glm::vec3(0.0f, 2.2f, 0.0f);
            entity->defaultTransform = entity->transform;
        }
        return light;
    };

    auto createPostProcessVolume = [&]() -> EntityId {
        EntityId post = document.registry().createEntity("Post Process Volume", SceneUpdateKind::RendererSettingsOnly);
        if (Entity* entity = document.registry().entity(post)) {
            entity->postProcessVolume = PostProcessVolume{};
            entity->defaultTransform = entity->transform;
            document.worldSettings().postProcessVolume = post;
        }
        return post;
    };

    (void)createCamera();
    if (name == "Basic Lit") {
        (void)createSun();
        (void)createEnvironmentLight();
    } else if (name == "Outdoor / Atmosphere") {
        (void)createSun();
        (void)createEnvironmentLight();
        (void)createSkyAtmosphere();
    } else if (name == "Interior" || name == "Path Tracing Validation" || name == "Lighting Test Scene") {
        (void)createAreaLight();
        (void)createPostProcessVolume();
    } else if (name == "Cinematic") {
        (void)createPostProcessVolume();
    }

    applySceneWorldComponentsToDocumentSettings(document);
    document.clearDirty();
    return document.saveJson(project.startupScene);
}

bool Application::loadProjectStartupScene(const ProjectContext& project) {
    if (!std::filesystem::exists(project.startupScene)) {
        initializeFallbackSceneDocument();
        scenePath_.reset();
        gltfPath_.reset();
        importedScene_.reset();
        assets_.clear();
        undoStack_.clear();
        sceneUnsavedDirty_ = false;
        initializeRendererFromCurrentScene();
        queueProjectThumbnailCapture();
        notifications_.notify("Project startup scene missing", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        return true;
    }

    SceneLoadRequest request;
    request.mode = SceneLoadMode::LoadProjectStartupScene;
    request.sourcePath = project.startupScene;
    request.projectSnapshot = project;
    return requestSceneLoad(std::move(request));
}

bool Application::openProjectFromFile(const std::filesystem::path& projectFile, bool promptForDirtyScene) {
    if (promptForDirtyScene && !confirmDestructiveSceneAction("opening a project")) {
        return false;
    }
    dirtyMaterialAssets_.clear();
    materialAssetAutosavePaths_.clear();

    ProjectContext project;
    std::string error;
    if (!loadProjectFile(projectFile, project, &error)) {
        notifications_.notify("Project open failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        std::cerr << "Project open failed: " << projectFile.string() << " " << error << '\n';
        return false;
    }
    if (!createProjectFolders(project, true, &error)) {
        notifications_.notify("Project folder validation failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        std::cerr << "Project folder validation failed: " << error << '\n';
        return false;
    }
    if (!assetRegistry_.load(project.assetRegistryPath, &error)) {
        notifications_.notify("Asset registry load failed", NotificationType::Error);
        std::cerr << "Asset registry load failed: " << error << '\n';
        return false;
    }
    (void)assetRegistry_.refreshRecordHealth(project.projectRoot, false);

    project_ = project;
    projectSettingsDirty_ = false;
    const std::filesystem::path crashMarker = project.savedRoot / "editor_session.json";
    if (uiOverlay_ != nullptr) {
        EditorPreferences globalPrefs;
        globalPrefs.load(EditorPreferences::defaultPath());
        globalPrefs.addRecentProject(project.projectFile);
        globalPrefs.save(EditorPreferences::defaultPath());
        reloadEditorPreferencesForActiveProject();
        if (project.preferredWorkspacePreset >= 0) {
            uiOverlay_->editor().setProjectWorkspacePreset(project.preferredWorkspacePreset);
        } else {
            uiOverlay_->editor().clearProjectWorkspacePreset();
        }
        if (!editorRecoveryPromptSuppressed() && std::filesystem::exists(crashMarker)) {
            std::filesystem::path recoveredScene = project.startupScene;
            std::filesystem::path recoveredSceneAutosave;
            std::filesystem::path recoveredProjectAutosave;
            std::filesystem::path recoveredAssetRegistryAutosave;
            std::vector<std::pair<AssetGuid, std::filesystem::path>> recoveredMaterialAssetAutosaves;
            try {
                std::ifstream markerIn(crashMarker);
                nlohmann::json markerJson;
                markerIn >> markerJson;
                const std::string scene = markerJson.value("scene", std::string{});
                if (!scene.empty()) {
                    recoveredScene = scene;
                }
                const std::string sceneAutosave = markerJson.value("sceneAutosave", std::string{});
                const std::string projectAutosave = markerJson.value("projectAutosave", std::string{});
                const std::string assetRegistryAutosave = markerJson.value("assetRegistryAutosave", std::string{});
                if (!sceneAutosave.empty()) {
                    recoveredSceneAutosave = sceneAutosave;
                }
                if (!projectAutosave.empty()) {
                    recoveredProjectAutosave = projectAutosave;
                }
                if (!assetRegistryAutosave.empty()) {
                    recoveredAssetRegistryAutosave = assetRegistryAutosave;
                }
                if (markerJson.contains("materialAssetAutosaves") && markerJson["materialAssetAutosaves"].is_array()) {
                    for (const nlohmann::json& item : markerJson["materialAssetAutosaves"]) {
                        if (!item.is_object()) {
                            continue;
                        }
                        const AssetGuid guid = item.value("guid", std::string{});
                        const std::string path = item.value("path", std::string{});
                        if (!path.empty()) {
                            recoveredMaterialAssetAutosaves.emplace_back(guid, std::filesystem::path(path));
                        }
                    }
                }
            } catch (...) {
            }
            pendingRecoveryAutosavePath_ = recoveredSceneAutosave.empty()
                ? editorSceneAutosavePath(project, recoveredScene, std::nullopt)
                : recoveredSceneAutosave;
            pendingRecoveryScenePath_ = recoveredScene.empty()
                ? std::optional<std::filesystem::path>{}
                : std::optional<std::filesystem::path>{recoveredScene};
            pendingRecoveryProjectAutosavePath_ = recoveredProjectAutosave.empty()
                ? editorProjectAutosavePath(project)
                : recoveredProjectAutosave;
            pendingRecoveryAssetRegistryAutosavePath_ = recoveredAssetRegistryAutosave.empty()
                ? editorAssetRegistryAutosavePath(project)
                : recoveredAssetRegistryAutosave;
            pendingRecoveryMaterialAssetAutosaves_ = std::move(recoveredMaterialAssetAutosaves);
            uiOverlay_->editor().showRecoveryPrompt(
                crashMarker,
                *pendingRecoveryAutosavePath_,
                *pendingRecoveryProjectAutosavePath_,
                *pendingRecoveryAssetRegistryAutosavePath_,
                pendingRecoveryMaterialAssetAutosaves_);
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Previous editor session marker found: " + crashMarker.string());
            notifications_.notify("Previous editor session marker found", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        }
    }
    writeCrashMarker(true);

    if (!loadProjectStartupScene(*project_)) {
        writeCrashMarker(false);
        project_.reset();
        projectSettingsDirty_ = false;
        pendingRecoveryScenePath_.reset();
        pendingRecoveryAutosavePath_.reset();
        pendingRecoveryProjectAutosavePath_.reset();
        pendingRecoveryMaterialAssetAutosaves_.clear();
        pendingRecoveryAssetRegistryAutosavePath_.reset();
        return false;
    }

    if (!asyncSceneLoader_.isRunning()) {
        sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
    }
    notifications_.notify("Project opened", NotificationType::Success, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().dismissProjectManager();
    }
    std::cout << "Opened project: " << project.projectFile.string() << '\n';
    return true;
}

bool Application::createProjectFromRequest(const CreateProjectRequest& request) {
    if (request.name.empty()) {
        notifications_.notify("Project name is required", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    const std::string invalidChars = "\\/:*?\"<>|";
    if (request.name.find_first_of(invalidChars) != std::string::npos) {
        notifications_.notify("Project name contains invalid path characters", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    if (request.location.empty()) {
        notifications_.notify("Project location is required", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    std::error_code locationEc;
    if (std::filesystem::exists(request.location, locationEc) && !std::filesystem::is_directory(request.location, locationEc)) {
        notifications_.notify("Project location is not a directory", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    const std::filesystem::path writableProbe = std::filesystem::exists(request.location, locationEc)
        ? request.location
        : nearestExistingParentForProject(request.location);
    if (writableProbe.empty() || !pathLooksWritableForProject(writableProbe)) {
        notifications_.notify("Project location is not writable", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    if (!confirmDestructiveSceneAction("creating a project")) {
        return false;
    }

    const std::filesystem::path projectRoot = request.location / request.name;
    const std::filesystem::path projectFile = projectRoot / (request.name + ".vproject");
    if (std::filesystem::exists(projectFile) || std::filesystem::exists(projectRoot / (request.name + ".rtproject"))) {
        notifications_.notify("Project already exists at that location", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    ProjectContext project = makeProjectContext(request.name, projectRoot, projectFile);
    if (request.templateName == "Runtime Viewer") {
        project.preferredWorkspacePreset = 3;
    }
    std::string error;
    if (!createProjectFolders(project, request.createDefaultContentFolders, &error)) {
        notifications_.notify("Project folder creation failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        std::cerr << "Project folder creation failed: " << error << '\n';
        return false;
    }
    if (request.createDefaultScene && !writeDefaultProjectScene(project, request.templateName)) {
        notifications_.notify("Default scene creation failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    if (!saveProjectFile(project)) {
        notifications_.notify("Project file save failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }
    return openProjectFromFile(project.projectFile, false);
}

bool Application::deleteProjectFromRequest(const DeleteProjectRequest& request) {
    if (request.projectFile.empty()) {
        notifications_.notify("No project selected", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        return false;
    }

    const std::filesystem::path projectFile = normalizedPathForCompare(request.projectFile);
    ProjectContext projectToDelete;
    std::string loadError;
    if (!loadProjectFile(projectFile, projectToDelete, &loadError)) {
        projectToDelete.projectFile = projectFile;
        projectToDelete.projectRoot = projectFile.parent_path();
        projectToDelete.name = projectFile.stem().string();
    }

    if (uiOverlay_ != nullptr) {
        EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
        prefs.removeRecentProject(projectFile.string());
        prefs.removeRecentProject(request.projectFile.string());
        if (!prefs.lastOpenedProject.empty() && normalizedPathForCompare(prefs.lastOpenedProject) == projectFile) {
            prefs.lastOpenedProject.clear();
        }
        EditorPreferences globalPrefs;
        globalPrefs.load(EditorPreferences::defaultPath());
        globalPrefs.removeRecentProject(projectFile.string());
        globalPrefs.removeRecentProject(request.projectFile.string());
        if (!globalPrefs.lastOpenedProject.empty() && normalizedPathForCompare(globalPrefs.lastOpenedProject) == projectFile) {
            globalPrefs.lastOpenedProject.clear();
        }
        globalPrefs.save(EditorPreferences::defaultPath());
    }

    if (!request.deleteFiles) {
        notifications_.notify("Project removed from recent list", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
        return true;
    }

    if (!projectRootLooksSafeToDelete(projectToDelete.projectRoot, projectFile)) {
        notifications_.notify("Project delete refused", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        std::cerr << "Project delete refused: unsafe project root for " << projectFile.string()
                  << " root=" << projectToDelete.projectRoot.string() << '\n';
        return false;
    }

    if (project_.has_value()) {
        const bool deletingCurrentProject = normalizedPathForCompare(project_->projectFile) == projectFile ||
            normalizedPathForCompare(project_->projectRoot) == normalizedPathForCompare(projectToDelete.projectRoot);
        if (deletingCurrentProject && !closeCurrentProject()) {
            notifications_.notify("Project delete cancelled", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
            return false;
        }
    }

    std::error_code ec;
    const uintmax_t removed = std::filesystem::remove_all(projectToDelete.projectRoot, ec);
    if (ec) {
        notifications_.notify("Project delete failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        std::cerr << "Project delete failed: " << projectToDelete.projectRoot.string() << " " << ec.message() << '\n';
        return false;
    }

    notifications_.notify("Project deleted", NotificationType::Success, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
    std::cout << "Deleted project: " << projectToDelete.projectRoot.string() << " files=" << removed << '\n';
    return true;
}

std::filesystem::path Application::activeEditorPreferencesPath() const {
    if (project_.has_value() && !project_->editorPreferencesPath.empty()) {
        return project_->editorPreferencesPath;
    }
    return EditorPreferences::defaultPath();
}

void Application::reloadEditorPreferencesForActiveProject() {
    if (uiOverlay_ == nullptr) {
        return;
    }
    EditorLayer& editor = uiOverlay_->editor();
    EditorPreferences& prefs = editor.editorPrefs();
    prefs = EditorPreferences{};
    EditorPreferences globalPrefs;
    globalPrefs.load(EditorPreferences::defaultPath());
    prefs = globalPrefs;
    if (project_.has_value() && !project_->editorPreferencesPath.empty() && std::filesystem::exists(project_->editorPreferencesPath)) {
        prefs.load(project_->editorPreferencesPath);
    }
    prefs.recentProjects = globalPrefs.recentProjects;
    prefs.lastOpenedProject = globalPrefs.lastOpenedProject;
    prefs.openLastProject = globalPrefs.openLastProject;
    editor.setEditorPreferencesPath(activeEditorPreferencesPath());
    editor.reloadViewportPreferences();
    cameraController_.setMoveSpeed(std::clamp(prefs.cameraMoveSpeed, 0.05f, 100.0f));
    cameraController_.setFastMoveSpeed(std::clamp(prefs.cameraFastMoveSpeed, 0.05f, 250.0f));
    cameraController_.setMouseSensitivity(std::clamp(prefs.cameraMouseSensitivity, 0.0001f, 0.02f));
    cameraController_.setInvertLookX(prefs.cameraInvertLookX);
    cameraController_.setInvertLookY(prefs.cameraInvertLookY);
}

bool Application::saveActiveEditorPreferences() {
    if (uiOverlay_ == nullptr) {
        return false;
    }
    uiOverlay_->editor().setEditorPreferencesPath(activeEditorPreferencesPath());
    return uiOverlay_->editor().saveEditorPreferences();
}

bool Application::closeCurrentProject() {
    if (!project_.has_value()) {
        return true;
    }
    if (!confirmDestructiveSceneAction("closing the project")) {
        return false;
    }
    if (assetRegistry_.dirty() && !assetRegistry_.save()) {
        notifications_.notify("Asset registry save failed", NotificationType::Error);
        return false;
    }
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().clearProjectWorkspacePreset();
        (void)saveActiveEditorPreferences();
        uiOverlay_->editor().timeline().clear();
    }
    writeCrashMarker(false);
    project_.reset();
    projectSettingsDirty_ = false;
    pendingRecoveryScenePath_.reset();
    pendingRecoveryAutosavePath_.reset();
    pendingRecoveryProjectAutosavePath_.reset();
    pendingRecoveryMaterialAssetAutosaves_.clear();
    pendingRecoveryAssetRegistryAutosavePath_.reset();
    dirtyMaterialAssets_.clear();
    materialAssetAutosavePaths_.clear();
    assetRegistry_.clear();
    reloadEditorPreferencesForActiveProject();
    initializeFallbackSceneDocument();
    scenePath_.reset();
    gltfPath_.reset();
    importedScene_.reset();
    assets_.clear();
    undoStack_.clear();
    sceneUnsavedDirty_ = false;
    notifications_.notify("Project closed", NotificationType::Info);
    return true;
}

std::optional<AssetImportWorkspace> Application::prepareAssetImportWorkspace(const std::filesystem::path& sourcePath) {
    AssetImportWorkspace workspace;
    if (project_.has_value()) {
        workspace.root = project_->projectRoot;
        workspace.contentRoot = project_->contentRoot;
        workspace.sourceAssetsRoot = project_->projectRoot / "SourceAssets";
        workspace.cacheRoot = project_->cacheRoot;
        workspace.registryPath = project_->assetRegistryPath;
    } else {
        if (!scenePath_.has_value()) {
            notifications_.notify("Open or create a project before importing assets", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().showProjectManager();
            }
            std::cout << "Import Asset deferred until a project or saved scene exists: " << sourcePath.string() << '\n';
            return std::nullopt;
        }
        workspace.compatibilityMode = true;
        workspace.root = scenePath_->parent_path();
        if (workspace.root.empty()) {
            workspace.root = std::filesystem::current_path();
        }
        workspace.contentRoot = workspace.root / "Content";
        workspace.sourceAssetsRoot = workspace.root / "SourceAssets";
        workspace.cacheRoot = workspace.root / "Cache";
        workspace.registryPath = workspace.root / (scenePath_->stem().string() + ".assets.json");
    }

    if (assetRegistry_.state().path != workspace.registryPath) {
        std::string error;
        if (!assetRegistry_.load(workspace.registryPath, &error)) {
            notifications_.notify("Asset registry load failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            std::cerr << "Asset registry load failed: " << error << '\n';
            return std::nullopt;
        }
    }
    (void)assetRegistry_.refreshRecordHealth(workspace.root, false);

    return workspace;
}

bool Application::queueAssetImportNonMutating(const EditorImportAssetRequest& editorRequest, bool placeAfterImport) {
    std::optional<AssetImportWorkspace> workspace = prepareAssetImportWorkspace(editorRequest.sourcePath);
    if (!workspace.has_value()) {
        return false;
    }

    AssetImportRequest request;
    request.sourcePath = editorRequest.sourcePath;
    request.destinationFolder = editorRequest.destinationFolder;
    request.mode = editorRequest.mode;
    request.settings = editorRequest.settings;
    AsyncAssetImportJob job;
    job.kind = AsyncAssetImportKind::Import;
    job.serial = nextAssetImportJobSerial_++;
    job.request = std::move(request);
    job.workspace = std::move(*workspace);
    job.placeAfterImport = placeAfterImport;
    pendingAssetImportJobs_.push_back(std::move(job));
    notifications_.notify(placeAfterImport ? "Import and place queued" : "Import Asset queued", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
    startNextAssetImportWorker();
    return true;
}

bool Application::placePrefabAsset(const AssetGuid& prefabGuid, const std::optional<Transform>& placementTransform) {
    const AssetRecord* prefabRecord = nullptr;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.guid == prefabGuid) {
            prefabRecord = &record;
            break;
        }
    }
    if (prefabRecord == nullptr || prefabRecord->type != AssetType::Prefab) {
        notifications_.notify("Prefab asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    if (assetPlacementBlocked(*prefabRecord)) {
        notifications_.notify("Prefab placement blocked", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        std::cerr << "Prefab placement blocked for asset " << prefabGuid << ": " << assetPlacementBlockReason(*prefabRecord) << '\n';
        return false;
    }

    std::filesystem::path root = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
    if (!project_.has_value() && assetRegistry_.state().path.has_parent_path()) {
        root = assetRegistry_.state().path.parent_path();
    }
    std::filesystem::path prefabPath = resolveAssetRecordPath(*prefabRecord, root);

    PrefabAsset prefab;
    std::string error;
    if (!loadPrefabAsset(prefabPath, prefab, &error)) {
        notifications_.notify("Prefab load failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        std::cerr << "Prefab load failed: " << prefabPath.string() << " " << error << '\n';
        return false;
    }

    AssetManager nextAssets = assets_;
    PrefabRuntimeBindings bindings;
    if (std::string bindError; !appendPrefabRuntimeAssets(*prefabRecord, prefab, root, &assetRegistry_, nextAssets, bindings, &bindError)) {
        notifications_.notify("Prefab runtime binding failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        std::cerr << "Prefab runtime binding failed: " << bindError << '\n';
        return false;
    }

    const SceneDocument beforeDocument = sceneDocument_;
    const AssetManager beforeAssets = assets_;
    assets_ = std::move(nextAssets);

    SceneOperations ops(sceneDocument_, &sceneEventBus_);
    PrefabInstance instance = ops.placePrefab(prefab, &bindings);
    if (!instance.instanceRoot.valid()) {
        assets_ = beforeAssets;
        sceneDocument_ = beforeDocument;
        notifications_.notify("Prefab placement failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    if (placementTransform.has_value()) {
        if (Entity* rootEntity = sceneDocument_.registry().entity(instance.instanceRoot)) {
            rootEntity->transform = *placementTransform;
            rootEntity->defaultTransform = *placementTransform;
        }
    }
    undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
        sceneDocument_,
        assets_,
        beforeDocument,
        beforeAssets,
        sceneDocument_,
        assets_,
        SceneUpdateKind::TopologyChanged,
        "Place Prefab Asset"));
    sceneUnsavedDirty_ = true;
    (void)applyPendingSceneUpdate(true);
    editorPlacement_.entity = instance.instanceRoot;
    editorPlacement_.serial = nextEditorPlacementSerial_++;
    editorPlacement_.label = prefab.name.empty() ? "Prefab asset" : prefab.name;
    notifications_.notify("Prefab placed and selected", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Placed prefab asset: " << prefabGuid << " root=" << instance.instanceRoot.index << '\n';
    return true;
}

bool Application::placeMeshAsset(const EditorMeshAssetPlacement& request) {
    const AssetRecord* meshRecord = nullptr;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.guid == request.meshGuid) {
            meshRecord = &record;
            break;
        }
    }
    if (meshRecord == nullptr || meshRecord->type != AssetType::Mesh) {
        notifications_.notify("Mesh asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    if (assetPlacementBlocked(*meshRecord)) {
        notifications_.notify("Mesh placement blocked", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        std::cerr << "Mesh placement blocked for asset " << request.meshGuid << ": " << assetPlacementBlockReason(*meshRecord) << '\n';
        return false;
    }

    const AssetManager beforeAssets = assets_;
    bool restoredRuntimeAssets = false;
    std::optional<uint32_t> meshIndex = loadedMeshIndexForRecord(*meshRecord);
    if (!meshIndex.has_value()) {
        std::filesystem::path root = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
        if (!project_.has_value() && assetRegistry_.state().path.has_parent_path()) {
            root = assetRegistry_.state().path.parent_path();
        }

        AssetManager nextAssets = assets_;
        PrefabRuntimeBindings bindings;
        std::string bindError;
        if (!appendCachedPrefabRuntimeAssets(
                *meshRecord,
                root,
                resolveAssetSourcePath(*meshRecord, root),
                resolveAssetCachePath(*meshRecord, root),
                &assetRegistry_,
                nextAssets,
                bindings,
                &bindError)) {
            notifications_.notify("Mesh cooked payload is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
            if (!bindError.empty()) {
                std::cerr << "Mesh runtime binding failed: " << bindError << '\n';
            }
            return false;
        }
        const auto restoredMesh = bindings.meshes.find(request.meshGuid);
        if (restoredMesh == bindings.meshes.end() || !restoredMesh->second.valid()) {
            notifications_.notify("Mesh cooked payload does not contain this asset", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            std::cerr << "Mesh runtime binding did not expose GUID: " << request.meshGuid << '\n';
            return false;
        }
        assets_ = std::move(nextAssets);
        meshIndex = restoredMesh->second.index;
        restoredRuntimeAssets = true;
    }
    const MeshAssetHandle meshHandle{*meshIndex};
    if (assets_.mesh(meshHandle) == nullptr) {
        notifications_.notify("Mesh runtime data is unavailable", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (restoredRuntimeAssets) {
            assets_ = beforeAssets;
        }
        return false;
    }

    auto makeRenderer = [&]() {
        MeshRenderer renderer;
        renderer.mesh = meshHandle;
        renderer.meshGuid = request.meshGuid;
        ensureMaterialSlotsForRenderer(renderer, assets_);
        for (MaterialSlot& slot : renderer.materialSlots) {
            if (!slot.material.valid()) {
                continue;
            }
            if (std::optional<AssetRecord> materialRecord = materialAssetRecordForMaterial(slot.material.index)) {
                slot.materialGuid = materialRecord->guid;
            }
        }
        return renderer;
    };

    const SceneDocument beforeDocument = sceneDocument_;
    if (request.replaceEntity.valid()) {
        Entity* target = sceneDocument_.registry().entity(request.replaceEntity);
        if (target == nullptr || !target->meshRenderer.has_value()) {
            if (restoredRuntimeAssets) {
                assets_ = beforeAssets;
            }
            notifications_.notify("Mesh replacement target is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
            return false;
        }
        target->meshRenderer = makeRenderer();
        sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
        sceneUnsavedDirty_ = true;
        if (restoredRuntimeAssets) {
            undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
                sceneDocument_,
                assets_,
                beforeDocument,
                beforeAssets,
                sceneDocument_,
                assets_,
                SceneUpdateKind::TopologyChanged,
                "Replace Mesh Asset"));
        } else {
            undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
                sceneDocument_,
                beforeDocument,
                sceneDocument_,
                SceneUpdateKind::TopologyChanged,
                "Replace Mesh Asset"));
        }
        (void)applyPendingSceneUpdate(true);
        editorPlacement_.entity = request.replaceEntity;
        editorPlacement_.serial = nextEditorPlacementSerial_++;
        editorPlacement_.label = target->name.empty() ? (meshRecord->displayName.empty() ? "Mesh Asset" : meshRecord->displayName) : target->name;
        notifications_.notify("Mesh asset replaced", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        return true;
    }

    SceneOperations ops(sceneDocument_, &sceneEventBus_);
    ops.setUndoStack(nullptr);
    const std::string entityName = meshRecord->displayName.empty() ? "Mesh Asset" : meshRecord->displayName;
    const EntityId created = ops.createEntity(entityName, {}, SceneUpdateKind::TopologyChanged);
    Entity* entity = sceneDocument_.registry().entity(created);
    if (entity == nullptr) {
        sceneDocument_ = beforeDocument;
        if (restoredRuntimeAssets) {
            assets_ = beforeAssets;
        }
        notifications_.notify("Mesh placement failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }

    entity->meshRenderer = makeRenderer();
    if (request.placementTransform.has_value()) {
        entity->transform = *request.placementTransform;
    }
    entity->defaultTransform = entity->transform;
    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
    sceneUnsavedDirty_ = true;
    if (restoredRuntimeAssets) {
        undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
            sceneDocument_,
            assets_,
            beforeDocument,
            beforeAssets,
            sceneDocument_,
            assets_,
            SceneUpdateKind::TopologyChanged,
            "Place Mesh Asset"));
    } else {
        undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
            sceneDocument_,
            beforeDocument,
            sceneDocument_,
            SceneUpdateKind::TopologyChanged,
            "Place Mesh Asset"));
    }
    (void)applyPendingSceneUpdate(true);
    editorPlacement_.entity = created;
    editorPlacement_.serial = nextEditorPlacementSerial_++;
    editorPlacement_.label = entityName;
    notifications_.notify("Mesh asset placed and selected", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    return true;
}

bool Application::assignMaterialAssetToEntity(const EditorMaterialAssetAssignment& request) {
    const AssetRecord* materialRecord = nullptr;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.guid == request.materialGuid) {
            materialRecord = &record;
            break;
        }
    }
    if (materialRecord == nullptr || materialRecord->type != AssetType::Material) {
        notifications_.notify("Material asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    const std::optional<uint32_t> materialIndex = loadedMaterialIndexForRecord(*materialRecord);
    if (!materialIndex.has_value()) {
        notifications_.notify("Material is not loaded in the current scene", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    Entity* entity = sceneDocument_.registry().entity(request.entity);
    if (entity == nullptr || !entity->meshRenderer.has_value()) {
        notifications_.notify("Drop material on a selected mesh entity", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    const SceneDocument beforeDocument = sceneDocument_;
    const AssetManager beforeAssets = assets_;
    MeshRenderer& renderer = *entity->meshRenderer;
    ensureMaterialSlotsForRenderer(renderer, assets_);
    MeshAsset* mesh = assets_.mesh(renderer.mesh);
    if (mesh == nullptr) {
        notifications_.notify("Material target mesh is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }
    const MaterialAssetHandle material{*materialIndex};
    bool assigned = false;
    auto assignSlot = [&](MaterialSlot& slot, uint32_t slotIndex) {
        (void)slotIndex;
        if (material.index == slot.material.index || slot.materialGuid == request.materialGuid) {
            slot.overrideMaterial.reset();
            slot.overrideMaterialGuid.reset();
        } else {
            slot.overrideMaterial = material;
            slot.overrideMaterialGuid = request.materialGuid;
        }
        assigned = true;
    };
    if (request.primitiveIndex == UINT32_MAX) {
        for (uint32_t i = 0; i < renderer.materialSlots.size(); ++i) {
            assignSlot(renderer.materialSlots[i], i);
        }
    } else if (request.primitiveIndex < renderer.materialSlots.size()) {
        assignSlot(renderer.materialSlots[request.primitiveIndex], request.primitiveIndex);
    }

    if (!assigned) {
        notifications_.notify("Material slot is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    sceneDocument_.markDirty(SceneUpdateKind::MaterialOnly);
    sceneUnsavedDirty_ = true;
    undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
        sceneDocument_,
        assets_,
        beforeDocument,
        beforeAssets,
        sceneDocument_,
        assets_,
        SceneUpdateKind::MaterialOnly,
        "Assign Material Asset"));
    notifications_.notify("Material assigned to mesh", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 4.0f);
    return true;
}

bool Application::assignEnvironmentPath(
    const std::filesystem::path& environmentPath,
    bool allowResourceRebuild,
    std::string_view undoLabel,
    std::string_view notificationLabel) {
    std::error_code environmentPathError;
    if (environmentPath.empty() || !std::filesystem::is_regular_file(environmentPath, environmentPathError)) {
        notifications_.notify("Environment file is missing", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }

    const SceneDocument beforeDocument = sceneDocument_;
    EntityId environmentId = sceneDocument_.worldSettings().activeEnvironment;
    Entity* environment = sceneDocument_.registry().entity(environmentId);
    if (environment == nullptr) {
        SceneOperations ops(sceneDocument_, &sceneEventBus_);
        ops.setUndoStack(nullptr);
        environmentId = ops.createEntity("Environment Light", {}, SceneUpdateKind::EnvironmentOnly);
        environment = sceneDocument_.registry().entity(environmentId);
    }
    if (environment == nullptr) {
        sceneDocument_ = beforeDocument;
        notifications_.notify("Environment entity creation failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    if (!environment->environmentLight.has_value()) {
        environment->environmentLight = EnvironmentLight{};
    }
    environment->environmentLight->hdrPath = environmentPath;
    environment->environmentLight->enabled = true;
    sceneDocument_.worldSettings().activeEnvironment = environmentId;
    sceneDocument_.setSourceHdrPath(environmentPath);
    applySceneWorldComponentsToDocumentSettings(sceneDocument_);
    sceneDocument_.markDirty(SceneUpdateKind::EnvironmentOnly);
    sceneUnsavedDirty_ = true;

    undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
        sceneDocument_,
        beforeDocument,
        sceneDocument_,
        SceneUpdateKind::EnvironmentOnly,
        std::string(undoLabel)));
    (void)applyPendingSceneUpdate(allowResourceRebuild);
    notifications_.notify(std::string(notificationLabel), NotificationType::Success, NotificationAction::OpenContent, "Open Content", 4.0f);
    return true;
}

bool Application::assignEnvironmentAsset(const AssetGuid& environmentGuid, bool allowResourceRebuild) {
    const AssetRecord* environmentRecord = nullptr;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.guid == environmentGuid) {
            environmentRecord = &record;
            break;
        }
    }
    if (environmentRecord == nullptr || environmentRecord->type != AssetType::HDRI) {
        notifications_.notify("Environment asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }

    std::filesystem::path root = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
    if (!project_.has_value() && assetRegistry_.state().path.has_parent_path()) {
        root = assetRegistry_.state().path.parent_path();
    }
    auto existingPath = [](const std::filesystem::path& path) {
        std::error_code ec;
        return !path.empty() && std::filesystem::is_regular_file(path, ec);
    };
    std::filesystem::path environmentPath = resolveAssetSourcePath(*environmentRecord, root);
    if (!existingPath(environmentPath)) {
        environmentPath = resolveAssetCachePath(*environmentRecord, root);
    }
    if (!existingPath(environmentPath)) {
        environmentPath = resolveAssetRecordPath(*environmentRecord, root);
    }
    if (!existingPath(environmentPath)) {
        notifications_.notify("Environment source or payload is missing", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    return assignEnvironmentPath(environmentPath, allowResourceRebuild, "Assign Environment Asset", "Environment asset assigned");
}

bool Application::relinkAssetSource(const EditorAssetRelinkSourceRequest& request) {
    if (request.guid.empty()) {
        notifications_.notify("Relink source failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    std::error_code ec;
    if (request.sourcePath.empty() || !std::filesystem::is_regular_file(request.sourcePath, ec)) {
        notifications_.notify("Relink source file missing", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }

    const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
        return record.guid == request.guid;
    });
    if (recordIt == assetRegistry_.records().end()) {
        notifications_.notify("Relink asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    if (recordIt->type != assetTypeForSourcePath(request.sourcePath)) {
        notifications_.notify("Relink source type mismatch", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        std::cerr << "Relink source type mismatch for asset " << request.guid
                  << ": existing=" << assetTypeName(recordIt->type)
                  << " source=" << assetTypeName(assetTypeForSourcePath(request.sourcePath))
                  << " path=" << request.sourcePath.string() << '\n';
        return false;
    }

    std::filesystem::path root = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
    if (!project_.has_value() && assetRegistry_.state().path.has_parent_path()) {
        root = assetRegistry_.state().path.parent_path();
    }
    AssetRecord record = *recordIt;
    record.sourcePath = assetRegistryPathValue(request.sourcePath, root);
    record.sourceHash = assetSourceHashForPath(request.sourcePath);
    record.sourceMissing = false;

    auto regularFileExists = [](const std::filesystem::path& path) {
        std::error_code error;
        return !path.empty() && std::filesystem::is_regular_file(path, error);
    };
    record.importedMetadataMissing = !record.importedPath.empty() && !regularFileExists(resolveAssetRecordPath(record, root));
    std::filesystem::path cachePath = record.cachePath;
    if (!cachePath.is_absolute()) {
        cachePath = root / cachePath;
    }
    record.cookedPayloadMissing = !record.cachePath.empty() && !regularFileExists(cachePath);
    record.missing = record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing;
    record.stale = !record.missing;
    record.status = record.missing ? AssetImportStatus::Missing : AssetImportStatus::Stale;
    record.lastModifiedTimestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetMoved);
    notifications_.notify("Asset source relinked", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Relinked asset source: " << request.guid << " -> " << request.sourcePath.string() << '\n';
    return true;
}

bool Application::replaceAssetReferences(const EditorReplaceAssetReferencesRequest& request, bool allowResourceRebuild) {
    if (request.oldGuid.empty() || request.newGuid.empty() || request.oldGuid == request.newGuid) {
        notifications_.notify("Replace references needs two different GUIDs", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    const AssetRecord* oldRecord = nullptr;
    const AssetRecord* newRecord = nullptr;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.guid == request.oldGuid) {
            oldRecord = &record;
        }
        if (record.guid == request.newGuid) {
            newRecord = &record;
        }
    }
    if (oldRecord == nullptr || newRecord == nullptr) {
        notifications_.notify("Replace references asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    if (oldRecord->type != newRecord->type) {
        notifications_.notify("Replace references type mismatch", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        std::cerr << "Replace references type mismatch: old=" << request.oldGuid << " type=" << assetTypeName(oldRecord->type)
                  << " new=" << request.newGuid << " type=" << assetTypeName(newRecord->type) << '\n';
        return false;
    }

    const SceneDocument beforeDocument = sceneDocument_;
    const size_t sceneReplacementCount = sceneDocument_.replaceAssetGuidReferences(request.oldGuid, request.newGuid);
    if (sceneReplacementCount > 0) {
        undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
            sceneDocument_, beforeDocument, sceneDocument_, SceneUpdateKind::TopologyChanged, "Replace Asset References"));
        sceneUnsavedDirty_ = true;
        (void)applyPendingSceneUpdate(allowResourceRebuild);
    }

    std::vector<AssetRecord> changedRecords;
    size_t registryReplacementCount = 0;
    for (const AssetRecord& sourceRecord : assetRegistry_.records()) {
        AssetRecord record = sourceRecord;
        bool changed = false;
        for (AssetDependency& dependency : record.dependencies) {
            if (dependency.guid == request.oldGuid) {
                dependency.guid = request.newGuid;
                changed = true;
                ++registryReplacementCount;
            }
        }
        for (AssetGuid& reference : record.references) {
            if (reference == request.oldGuid) {
                reference = request.newGuid;
                changed = true;
                ++registryReplacementCount;
            }
        }
        if (changed) {
            changedRecords.push_back(std::move(record));
        }
    }
    for (AssetRecord& record : changedRecords) {
        assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetDependencyChanged);
    }

    bool registrySaved = false;
    if (request.includeSavedProjectFiles && registryReplacementCount > 0 && !assetRegistry_.state().path.empty()) {
        registrySaved = assetRegistry_.save(assetRegistry_.state().path);
        if (registrySaved) {
            assetRegistry_.clearDirty();
        } else {
            notifications_.notify("Asset registry reference rewrite save failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        }
    }

    ProjectReferenceRewriteResult savedRewrite;
    bool savedRewriteAttempted = false;
    if (request.includeSavedProjectFiles) {
        if (!project_.has_value()) {
            notifications_.notify("Open a project before rewriting saved references", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        } else {
            savedRewriteAttempted = true;
            savedRewrite = rewriteSavedProjectAssetReferences(*project_, assetRegistry_, request.oldGuid, request.newGuid, scenePath_, sceneUnsavedDirty_);
            if (!savedRewrite.reportPath.empty()) {
                std::cout << "Saved project reference rewrite report: " << savedRewrite.reportPath.string() << '\n';
            }
            if (savedRewrite.refreshedReferenceIndex) {
                std::cout << "Persistent project reference index refreshed: " << savedRewrite.refreshedReferenceIndexPath.string() << '\n';
            } else if (!savedRewrite.refreshedReferenceIndexError.empty()) {
                std::cerr << "Persistent project reference index refresh failed: " << savedRewrite.refreshedReferenceIndexError << '\n';
            }
            if (savedRewrite.writeErrors.size() > 0 || savedRewrite.parseErrors.size() > 0 || !savedRewrite.refreshedReferenceIndex) {
                notifications_.notify("Saved reference rewrite completed with warnings", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
            }
        }
    }

    const size_t totalReplacements = sceneReplacementCount + registryReplacementCount + savedRewrite.occurrenceCount;
    if (totalReplacements == 0) {
        notifications_.notify("No references found", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }
    notifications_.notify(request.includeSavedProjectFiles ? "Project asset references replaced" : "Asset references replaced", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Replaced asset references: old=" << request.oldGuid << " new=" << request.newGuid
              << " scene=" << sceneReplacementCount
              << " registry=" << registryReplacementCount
              << " registry_saved=" << (registrySaved ? "true" : "false")
              << " saved_files=" << (savedRewriteAttempted ? savedRewrite.changedFileCount : 0u)
              << " saved_occurrences=" << (savedRewriteAttempted ? savedRewrite.occurrenceCount : 0u)
              << " refreshed_index=" << (savedRewriteAttempted && savedRewrite.refreshedReferenceIndex ? "true" : "false")
              << '\n';
    return true;
}

bool Application::repairMissingAssetDependencies(const EditorRepairMissingAssetDependenciesRequest& request) {
    if (request.ownerGuid.empty()) {
        notifications_.notify("Dependency repair needs a selected asset", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    const AssetRecord* ownerRecord = findAssetRecordByGuid(assetRegistry_, request.ownerGuid);
    if (ownerRecord == nullptr) {
        notifications_.notify("Dependency repair target missing", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    AssetRecord repaired = *ownerRecord;
    size_t repairedCount = 0;
    size_t ambiguousCount = 0;
    size_t missingCount = 0;
    for (AssetDependency& dependency : repaired.dependencies) {
        if (dependency.guid.empty() || findAssetRecordByGuid(assetRegistry_, dependency.guid) != nullptr) {
            continue;
        }
        ++missingCount;
        const std::vector<std::pair<int, const AssetRecord*>> candidates = rankedMissingDependencyRepairCandidates(assetRegistry_, repaired, dependency);
        if (candidates.size() != 1 || candidates.front().second == nullptr) {
            ++ambiguousCount;
            continue;
        }
        const AssetGuid oldGuid = dependency.guid;
        dependency.guid = candidates.front().second->guid;
        ++repairedCount;
        std::cout << "Repaired missing asset dependency: owner=" << request.ownerGuid
                  << " role=" << dependency.kind
                  << " old=" << oldGuid
                  << " new=" << dependency.guid << '\n';
    }

    if (missingCount == 0) {
        notifications_.notify("No missing dependencies found", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }
    if (repairedCount == 0) {
        notifications_.notify(ambiguousCount > 0 ? "Missing dependencies are ambiguous" : "No dependency repair candidates found", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    repaired.lastModifiedTimestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    assetRegistry_.addOrReplaceRecord(std::move(repaired), AssetRegistryDirtyReason::AssetDependencyChanged);
    const std::filesystem::path registryRoot = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
    (void)assetRegistry_.refreshRecordHealth(registryRoot, true);

    bool registrySaved = false;
    if (request.saveRegistry && !assetRegistry_.state().path.empty()) {
        registrySaved = assetRegistry_.save(assetRegistry_.state().path);
        if (registrySaved) {
            assetRegistry_.clearDirty();
        } else {
            notifications_.notify("Dependency repair registry save failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
    }

    notifications_.notify("Missing dependencies repaired", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Repaired missing dependencies: owner=" << request.ownerGuid
              << " repaired=" << repairedCount
              << " missing=" << missingCount
              << " ambiguous=" << ambiguousCount
              << " registry_saved=" << (registrySaved ? "true" : "false") << '\n';
    return true;
}

bool Application::renameAssetRecord(const EditorRenameAssetRequest& request) {
    auto trim = [](std::string value) {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
        return value;
    };

    if (request.guid.empty()) {
        notifications_.notify("Asset rename needs a selected asset", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    const std::string displayName = trim(request.displayName);
    if (displayName.empty()) {
        notifications_.notify("Asset name cannot be empty", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
        return record.guid == request.guid;
    });
    if (recordIt == assetRegistry_.records().end()) {
        notifications_.notify("Asset rename target missing", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    if (trim(recordIt->displayName) == displayName) {
        notifications_.notify("Asset name unchanged", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 3.0f);
        return false;
    }

    AssetRecord record = *recordIt;
    const std::string oldName = record.displayName.empty() ? record.guid : record.displayName;
    record.displayName = displayName;
    record.lastModifiedTimestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetRenamed);
    notifications_.notify("Asset renamed", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 4.0f);
    std::cout << "Renamed asset record: " << request.guid << " '" << oldName << "' -> '" << displayName << "'\n";
    return true;
}

bool Application::updateAssetTags(const EditorAssetTagsRequest& request) {
    if (request.guid.empty()) {
        notifications_.notify("Asset tags need a selected asset", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    auto normalizeTags = [](std::vector<std::string> tags) {
        for (std::string& tag : tags) {
            tag.erase(tag.begin(), std::find_if(tag.begin(), tag.end(), [](unsigned char ch) { return !std::isspace(ch); }));
            tag.erase(std::find_if(tag.rbegin(), tag.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), tag.end());
        }
        tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& tag) { return tag.empty(); }), tags.end());
        std::sort(tags.begin(), tags.end());
        tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
        return tags;
    };

    const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
        return record.guid == request.guid;
    });
    if (recordIt == assetRegistry_.records().end()) {
        notifications_.notify("Asset tag update target missing", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    AssetRecord record = *recordIt;
    std::vector<std::string> tags = normalizeTags(request.tags);
    if (normalizeTags(record.tags) == tags) {
        notifications_.notify("Asset tags unchanged", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 3.0f);
        return false;
    }

    record.tags = std::move(tags);
    record.lastModifiedTimestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetTagsChanged);
    notifications_.notify("Asset tags updated", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 4.0f);
    std::cout << "Updated asset tags: " << request.guid << '\n';
    return true;
}

bool Application::bulkAddAssetTag(const EditorBulkAssetTagRequest& request) {
    std::string tag = request.tag;
    tag.erase(tag.begin(), std::find_if(tag.begin(), tag.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    tag.erase(std::find_if(tag.rbegin(), tag.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), tag.end());
    if (tag.empty() || request.guids.empty()) {
        notifications_.notify("Bulk tag needs a tag and visible assets", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    auto toLower = [](std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    };
    auto tagExists = [&](const std::vector<std::string>& tags) {
        const std::string lowerTag = toLower(tag);
        return std::any_of(tags.begin(), tags.end(), [&](const std::string& existing) {
            return toLower(existing) == lowerTag;
        });
    };

    size_t changedCount = 0;
    for (const AssetGuid& guid : request.guids) {
        const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
            return record.guid == guid;
        });
        if (recordIt == assetRegistry_.records().end() || tagExists(recordIt->tags)) {
            continue;
        }
        AssetRecord record = *recordIt;
        record.tags.push_back(tag);
        std::sort(record.tags.begin(), record.tags.end());
        record.tags.erase(std::unique(record.tags.begin(), record.tags.end()), record.tags.end());
        record.lastModifiedTimestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetTagsChanged);
        ++changedCount;
    }

    if (changedCount == 0) {
        notifications_.notify("No visible assets needed that tag", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }
    notifications_.notify("Bulk asset tag applied", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 4.0f);
    std::cout << "Bulk asset tag applied: tag=" << tag << " count=" << changedCount << '\n';
    return true;
}

bool Application::bulkRemoveAssetTag(const EditorBulkAssetTagRequest& request) {
    std::string tag = request.tag;
    tag.erase(tag.begin(), std::find_if(tag.begin(), tag.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    tag.erase(std::find_if(tag.rbegin(), tag.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), tag.end());
    if (tag.empty() || request.guids.empty()) {
        notifications_.notify("Bulk untag needs a tag and visible assets", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    auto toLower = [](std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    };
    const std::string lowerTag = toLower(tag);

    size_t changedCount = 0;
    for (const AssetGuid& guid : request.guids) {
        const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
            return record.guid == guid;
        });
        if (recordIt == assetRegistry_.records().end()) {
            continue;
        }
        AssetRecord record = *recordIt;
        const size_t beforeCount = record.tags.size();
        record.tags.erase(std::remove_if(record.tags.begin(), record.tags.end(), [&](const std::string& existing) {
            return toLower(existing) == lowerTag;
        }), record.tags.end());
        if (record.tags.size() == beforeCount) {
            continue;
        }
        record.lastModifiedTimestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetTagsChanged);
        ++changedCount;
    }

    if (changedCount == 0) {
        notifications_.notify("No visible assets had that tag", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }
    notifications_.notify("Bulk asset tag removed", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 4.0f);
    std::cout << "Bulk asset tag removed: tag=" << tag << " count=" << changedCount << '\n';
    return true;
}

bool Application::moveAssetsToFolder(const EditorMoveAssetsToFolderRequest& request) {
    if (request.guids.empty()) {
        notifications_.notify("Asset folder move needs visible assets", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    auto trim = [](std::string value) {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
        return value;
    };
    auto normalizeFolder = [&](std::string value) {
        value = trim(std::move(value));
        std::replace(value.begin(), value.end(), '\\', '/');
        while (!value.empty() && value.front() == '/') {
            value.erase(value.begin());
        }
        while (!value.empty() && value.back() == '/') {
            value.pop_back();
        }
        std::string compact;
        compact.reserve(value.size());
        bool previousSlash = false;
        for (char c : value) {
            if (c == '/') {
                if (!previousSlash) {
                    compact.push_back('/');
                }
                previousSlash = true;
                continue;
            }
            compact.push_back(c);
            previousSlash = false;
        }
        return trim(std::move(compact));
    };
    auto lower = [](std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    };

    const std::string folderName = normalizeFolder(request.folderName);
    const std::string groupId = folderName.empty() ? std::string{} : std::string("folder:") + lower(folderName);
    const auto now = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    size_t changedCount = 0;
    for (const AssetGuid& guid : request.guids) {
        const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
            return record.guid == guid;
        });
        if (recordIt == assetRegistry_.records().end()) {
            continue;
        }
        if (recordIt->importGroupId == groupId && recordIt->importGroupName == folderName) {
            continue;
        }
        AssetRecord record = *recordIt;
        record.importGroupId = groupId;
        record.importGroupName = folderName;
        record.importRootGuid.clear();
        record.lastModifiedTimestamp = now;
        assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetMoved);
        ++changedCount;
    }

    if (changedCount == 0) {
        notifications_.notify("Asset folders unchanged", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }
    notifications_.notify(folderName.empty() ? "Asset folder metadata cleared" : "Assets moved to folder", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 4.0f);
    std::cout << "Moved asset records to virtual folder: folder='" << folderName << "' count=" << changedCount << '\n';
    return true;
}

bool Application::deleteAssetsFromRegistry(const EditorDeleteAssetRequest& request) {
    if (request.guids.empty()) {
        notifications_.notify("Asset delete needs selected assets", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    std::unordered_set<AssetGuid> targetGuids;
    targetGuids.reserve(request.guids.size());
    for (const AssetGuid& guid : request.guids) {
        if (!guid.empty()) {
            targetGuids.insert(guid);
        }
    }
    if (targetGuids.empty()) {
        notifications_.notify("Asset delete needs selected assets", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    std::vector<AssetRecord> targetRecords;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (targetGuids.find(record.guid) != targetGuids.end()) {
            targetRecords.push_back(record);
        }
    }
    if (targetRecords.empty()) {
        notifications_.notify("Asset delete target missing", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    std::filesystem::path root = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
    if (!project_.has_value() && assetRegistry_.state().path.has_parent_path()) {
        root = assetRegistry_.state().path.parent_path();
    }

    size_t deletedFiles = 0;
    size_t skippedFiles = 0;
    if (request.deleteGeneratedFiles) {
        std::vector<std::filesystem::path> candidates;
        for (const AssetRecord& record : targetRecords) {
            const std::array<std::string, 2> generatedPaths = {record.importedPath, record.cachePath};
            for (const std::string& value : generatedPaths) {
                if (value.empty()) {
                    continue;
                }
                std::filesystem::path path = value;
                if (!path.is_absolute()) {
                    path = root / path;
                }
                candidates.push_back(normalizedPathForCompare(path));
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        auto remainingRecordUsesPath = [&](const std::filesystem::path& path) {
            for (const AssetRecord& record : assetRegistry_.records()) {
                if (targetGuids.find(record.guid) != targetGuids.end()) {
                    continue;
                }
                const std::array<std::string, 2> values = {record.importedPath, record.cachePath};
                for (const std::string& value : values) {
                    if (value.empty()) {
                        continue;
                    }
                    std::filesystem::path recordPath = value;
                    if (!recordPath.is_absolute()) {
                        recordPath = root / recordPath;
                    }
                    if (normalizedPathForCompare(recordPath) == path) {
                        return true;
                    }
                }
            }
            return false;
        };

        for (const std::filesystem::path& path : candidates) {
            if (path.empty() || remainingRecordUsesPath(path) || !pathIsInsideDirectory(path, root)) {
                ++skippedFiles;
                continue;
            }
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec) && std::filesystem::remove(path, ec) && !ec) {
                ++deletedFiles;
            } else if (ec || std::filesystem::exists(path)) {
                ++skippedFiles;
            }
        }
    }

    const size_t removedRecords = assetRegistry_.removeRecords(request.guids, AssetRegistryDirtyReason::AssetDeleted);
    if (removedRecords == 0) {
        notifications_.notify("Asset delete removed no records", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    std::vector<AssetRecord> changedRecords;
    for (const AssetRecord& sourceRecord : assetRegistry_.records()) {
        AssetRecord record = sourceRecord;
        const size_t dependencyCount = record.dependencies.size();
        const size_t referenceCount = record.references.size();
        record.dependencies.erase(std::remove_if(record.dependencies.begin(), record.dependencies.end(), [&](const AssetDependency& dependency) {
            return targetGuids.find(dependency.guid) != targetGuids.end();
        }), record.dependencies.end());
        record.references.erase(std::remove_if(record.references.begin(), record.references.end(), [&](const AssetGuid& guid) {
            return targetGuids.find(guid) != targetGuids.end();
        }), record.references.end());
        if (record.dependencies.size() != dependencyCount || record.references.size() != referenceCount) {
            record.lastModifiedTimestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            changedRecords.push_back(std::move(record));
        }
    }
    for (AssetRecord& record : changedRecords) {
        assetRegistry_.addOrReplaceRecord(std::move(record), AssetRegistryDirtyReason::AssetDependencyChanged);
    }

    if (uiOverlay_ != nullptr) {
        EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
        const std::vector<AssetGuid> removedGuids(targetGuids.begin(), targetGuids.end());
        for (const AssetGuid& guid : removedGuids) {
            prefs.removeFavoriteAsset(guid);
        }
        std::vector<std::string> collectionNames;
        collectionNames.reserve(prefs.assetCollections.size());
        for (const EditorAssetCollection& collection : prefs.assetCollections) {
            collectionNames.push_back(collection.name);
        }
        for (const std::string& collectionName : collectionNames) {
            prefs.removeAssetsFromCollection(collectionName, removedGuids);
        }
        (void)saveActiveEditorPreferences();
    }

    const bool saved = assetRegistry_.state().path.empty() ? false : assetRegistry_.save(assetRegistry_.state().path);
    if (saved) {
        assetRegistry_.clearDirty();
    }
    notifications_.notify(saved ? "Asset registry item deleted" : "Asset deleted; registry save failed", saved ? NotificationType::Success : NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Deleted asset registry records: count=" << removedRecords
              << " files_deleted=" << deletedFiles
              << " files_skipped=" << skippedFiles
              << " saved=" << (saved ? "true" : "false") << '\n';
    return saved;
}

bool Application::startCookProject(const EditorCookProjectRequest& request) {
    if (!project_.has_value()) {
        notifications_.notify("Open a project before cooking", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        return false;
    }
    if (activeCookProjectJob_.has_value()) {
        notifications_.notify("Project cook already running", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
        return false;
    }

    const std::filesystem::path projectFile = request.projectFile.empty() ? project_->projectFile : request.projectFile;
    const std::filesystem::path outputDir = request.outputDir.empty() ? (project_->buildRoot / "Cooked") : request.outputDir;
    const std::filesystem::path manifestPath = outputDir / "cook_manifest.json";
    const std::filesystem::path validationReportPath = outputDir / "asset_validation_report.json";
    const std::filesystem::path logPath = outputDir / "cook_log.txt";

    auto recordBlockedCook = [&](std::string status, int exitCode) {
        const std::string failureStatus = status;
        completedCookProjectJob_ = EditorJobCenterState{};
        completedCookProjectJob_.completedCookProjectSerial = nextCookProjectJobSerial_++;
        completedCookProjectJob_.completedCookProjectSuccess = false;
        completedCookProjectJob_.completedCookProjectStatus = std::move(status);
        completedCookProjectJob_.completedCookProjectFile = projectFile;
        completedCookProjectJob_.completedCookProjectOutputDir = outputDir;
        completedCookProjectJob_.completedCookProjectManifestPath = manifestPath;
        completedCookProjectJob_.completedCookProjectValidationReportPath = validationReportPath;
        completedCookProjectJob_.completedCookProjectLogPath = logPath;
        completedCookProjectJob_.completedCookProjectExitCode = exitCode;
        ensureCookFailureArtifacts(projectFile, outputDir, manifestPath, validationReportPath, logPath, exitCode, failureStatus);
    };

    if (!saveAllEditorState()) {
        recordBlockedCook("Cook blocked: Save All failed", -1);
        notifications_.notify("Cook blocked by save failure", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        recordBlockedCook("Cook blocked: output folder failed", -2);
        notifications_.notify("Cook output folder failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
        std::cerr << "Cook output folder creation failed: " << outputDir.string() << " " << ec.message() << '\n';
        return false;
    }

    CookProjectResult seed;
    seed.serial = nextCookProjectJobSerial_++;
    seed.projectFile = projectFile;
    seed.outputDir = outputDir;
    seed.manifestPath = manifestPath;
    seed.validationReportPath = validationReportPath;
    seed.logPath = logPath;

    activeCookProjectJob_.emplace(ActiveCookProjectJob{
        seed.serial,
        seed.projectFile,
        seed.outputDir,
        seed.manifestPath,
        seed.validationReportPath,
        seed.logPath,
        std::async(std::launch::async, [seed]() mutable {
            const auto start = std::chrono::steady_clock::now();
            const std::filesystem::path exe = currentExecutablePath();
            seed.exitCode = runCookProjectProcess(
                exe,
                seed.projectFile,
                seed.outputDir,
                seed.manifestPath,
                seed.logPath,
                &seed.commandLine);
            seed.workerTotalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            return seed;
        })});

    completedCookProjectJob_ = EditorJobCenterState{};
    notifications_.notify("Project cook started", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
    std::cout << "Project cook started: " << projectFile.string() << " -> " << outputDir.string() << '\n';
    return true;
}

void Application::pollCookProjectJob() {
    using namespace std::chrono_literals;
    if (!activeCookProjectJob_.has_value()) {
        return;
    }
    if (activeCookProjectJob_->future.wait_for(0s) != std::future_status::ready) {
        return;
    }

    CookProjectResult result = activeCookProjectJob_->future.get();
    activeCookProjectJob_.reset();
    const bool success = result.exitCode == 0;

    completedCookProjectJob_ = EditorJobCenterState{};
    completedCookProjectJob_.completedCookProjectSerial = result.serial;
    completedCookProjectJob_.completedCookProjectSuccess = success;
    completedCookProjectJob_.completedCookProjectStatus = success ? "Cook completed" : "Cook failed";
    completedCookProjectJob_.completedCookProjectFile = result.projectFile;
    completedCookProjectJob_.completedCookProjectOutputDir = result.outputDir;
    completedCookProjectJob_.completedCookProjectManifestPath = result.manifestPath;
    completedCookProjectJob_.completedCookProjectValidationReportPath = result.validationReportPath;
    completedCookProjectJob_.completedCookProjectLogPath = result.logPath;
    completedCookProjectJob_.completedCookProjectExitCode = result.exitCode;
    completedCookProjectJob_.completedCookProjectWorkerTotalMs = result.workerTotalMs;

    if (success) {
        notifications_.notify("Project cook complete", NotificationType::Success, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
    } else {
        ensureCookFailureArtifacts(
            result.projectFile,
            result.outputDir,
            result.manifestPath,
            result.validationReportPath,
            result.logPath,
            result.exitCode,
            completedCookProjectJob_.completedCookProjectStatus,
            result.commandLine);
        notifications_.notify("Project cook failed", NotificationType::Error, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
    }
    std::cout << "Project cook " << (success ? "completed" : "failed")
              << ": exit=" << result.exitCode
              << " manifest=" << result.manifestPath.string()
              << " report=" << result.validationReportPath.string()
              << " log=" << result.logPath.string() << '\n';
}

bool Application::queueAssetReimport(const AssetGuid& assetGuid) {
    const AssetRecord* sourceRecord = nullptr;
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.guid == assetGuid) {
            sourceRecord = &record;
            break;
        }
    }
    if (sourceRecord == nullptr) {
        notifications_.notify("Reimport asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    const AssetType originalType = sourceRecord->type;

    std::filesystem::path root = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
    if (!project_.has_value() && assetRegistry_.state().path.has_parent_path()) {
        root = assetRegistry_.state().path.parent_path();
    }
    AssetImportWorkspace workspace;
    workspace.root = root;
    workspace.contentRoot = project_.has_value() ? project_->contentRoot : root / "Content";
    workspace.sourceAssetsRoot = root / "SourceAssets";
    workspace.cacheRoot = project_.has_value() ? project_->cacheRoot : root / "Cache";
    workspace.registryPath = project_.has_value() ? project_->assetRegistryPath : assetRegistry_.state().path;
    workspace.compatibilityMode = !project_.has_value();

    AssetImportRequest request;
    request.sourcePath = resolveAssetSourcePath(*sourceRecord, root);
    request.destinationFolder = originalType == AssetType::Prefab ? "Models" : "";
    request.mode = importModeForRecord(*sourceRecord, root);
    request.settings = sourceRecord->importSettings;

    AsyncAssetImportJob job;
    job.kind = AsyncAssetImportKind::Reimport;
    job.serial = nextAssetImportJobSerial_++;
    job.request = std::move(request);
    job.workspace = std::move(workspace);
    job.assetGuid = assetGuid;
    job.originalType = originalType;
    pendingAssetImportJobs_.push_back(std::move(job));
    notifications_.notify("Reimport Asset queued", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
    startNextAssetImportWorker();
    return true;
}

void Application::startNextAssetImportWorker() {
    if (activeAssetImportJob_.has_value() || pendingAssetImportJobs_.empty()) {
        return;
    }

    AsyncAssetImportJob job = std::move(pendingAssetImportJobs_.front());
    pendingAssetImportJobs_.pop_front();
    const AssetImportRequest request = job.request;
    const AssetImportWorkspace workspace = job.workspace;
    auto progress = std::make_shared<AsyncAssetImportProgress>();
    progress->progress = 0.02f;
    progress->stage = "Queued";
    progress->workerStartedAt = std::chrono::steady_clock::now();
    progress->stageStartedAt = progress->workerStartedAt;
    activeAssetImportJob_.emplace(ActiveAsyncAssetImportJob{
        std::move(job),
        progress,
        std::async(std::launch::async, [request, workspace, progress]() {
            auto updateProgress = [progress](float value, std::string stage) {
                if (progress == nullptr) {
                    return;
                }
                std::lock_guard<std::mutex> lock(progress->mutex);
                progress->progress = std::clamp(value, 0.0f, 1.0f);
                if (progress->stage != stage) {
                    progress->stageStartedAt = std::chrono::steady_clock::now();
                }
                progress->stage = std::move(stage);
            };
            try {
                return stagePlaceholderAssetImport(request, workspace, std::move(updateProgress));
            } catch (const std::exception& error) {
                StagedAssetImportResult result;
                result.errors.push_back(error.what());
                return result;
            } catch (...) {
                StagedAssetImportResult result;
                result.errors.push_back("Unknown asset import worker failure");
                return result;
            }
        })});
}

void Application::pollAssetImportWorker() {
    if (!activeAssetImportJob_.has_value()) {
        startNextAssetImportWorker();
        return;
    }

    using namespace std::chrono_literals;
    if (activeAssetImportJob_->future.wait_for(0s) != std::future_status::ready) {
        return;
    }

    ActiveAsyncAssetImportJob completed = std::move(*activeAssetImportJob_);
    activeAssetImportJob_.reset();
    StagedAssetImportResult result = completed.future.get();
    (void)applyCompletedAssetImport(std::move(completed.job), std::move(result));
    startNextAssetImportWorker();
}

void Application::waitForAssetImportWorker() {
    pendingAssetImportJobs_.clear();
    if (activeAssetImportJob_.has_value()) {
        activeAssetImportJob_->future.wait();
        activeAssetImportJob_.reset();
    }
}

bool Application::applyCompletedAssetImport(AsyncAssetImportJob&& job, StagedAssetImportResult&& result) {
    const bool reimport = job.kind == AsyncAssetImportKind::Reimport;
    const auto recordCompletedImportJob = [&](bool success, const std::string& status, std::vector<std::string> extraErrors = {}) {
        completedAssetImportJob_ = EditorJobCenterState{};
        completedAssetImportJob_.completedAssetImportSerial = job.serial;
        completedAssetImportJob_.completedAssetImportSuccess = success;
        completedAssetImportJob_.completedAssetImportTitle = reimport ? "Reimport Asset" : (job.placeAfterImport ? "Import and Place" : "Import Asset");
        completedAssetImportJob_.completedAssetImportStatus = status;
        completedAssetImportJob_.completedAssetImportSourcePath = job.request.sourcePath;
        completedAssetImportJob_.completedAssetImportReportPath = result.importReportPath;
        completedAssetImportJob_.completedAssetImportErrors = result.errors;
        completedAssetImportJob_.completedAssetImportWarnings = result.warnings;
        completedAssetImportJob_.completedAssetImportErrors.insert(
            completedAssetImportJob_.completedAssetImportErrors.end(),
            extraErrors.begin(),
            extraErrors.end());
        completedAssetImportJob_.completedAssetImportCanRetry = !job.request.sourcePath.empty();
        completedAssetImportJob_.completedAssetImportPlaceAfterImport = job.placeAfterImport;
        completedAssetImportJob_.completedAssetImportDestinationFolder = job.request.destinationFolder;
        completedAssetImportJob_.completedAssetImportMode = job.request.mode;
        completedAssetImportJob_.completedAssetImportSettings = job.request.settings;
        completedAssetImportJob_.completedAssetImportWorkerTotalMs = result.workerTotalMs;
        completedAssetImportJob_.completedAssetImportWorkerValidateMs = result.workerValidateMs;
        completedAssetImportJob_.completedAssetImportWorkerDirectoryMs = result.workerDirectoryMs;
        completedAssetImportJob_.completedAssetImportWorkerInspectMs = result.workerInspectMs;
        completedAssetImportJob_.completedAssetImportWorkerWriteMs = result.workerWriteMs;
        if (reimport) {
            completedAssetImportJob_.completedAssetReimportGuid = job.assetGuid;
        }
    };
    if (!result.success) {
        const std::string status = result.errors.empty() ? std::string("Failed") : result.errors.front();
        recordCompletedImportJob(false, status);
        notifications_.notify(reimport ? "Reimport Asset failed" : "Import Asset failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        for (const std::string& error : result.errors) {
            std::cerr << (reimport ? "Reimport Asset failed: " : "Import Asset failed: ") << error << '\n';
        }
        return false;
    }

    const AssetGuid importedGuid = result.record.guid;
    const AssetRegistryDirtyReason dirtyReason = reimport ? AssetRegistryDirtyReason::AssetReimported : AssetRegistryDirtyReason::AssetImported;
    const auto applyRecords = [&](AssetRegistry& registry) {
        for (const AssetRecord& record : result.records) {
            registry.addOrReplaceRecord(record, dirtyReason);
        }
    };

    if (!sameRegistryPath(assetRegistry_.state().path, job.workspace.registryPath)) {
        AssetRegistry completedRegistry;
        std::string error;
        if (!completedRegistry.load(job.workspace.registryPath, &error)) {
            recordCompletedImportJob(false, "Registry load failed", {error});
            notifications_.notify(reimport ? "Reimport registry load failed" : "Import registry load failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            std::cerr << (reimport ? "Reimport" : "Import") << " completed for inactive registry, but registry load failed: "
                      << job.workspace.registryPath.string() << " " << error << '\n';
            return false;
        }
        applyRecords(completedRegistry);
        (void)completedRegistry.refreshRecordHealth(job.workspace.root, true);
        if (!completedRegistry.save(job.workspace.registryPath)) {
            recordCompletedImportJob(false, "Registry save failed", {"Could not save registry: " + job.workspace.registryPath.string()});
            notifications_.notify(reimport ? "Reimport registry save failed" : "Import registry save failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            return false;
        }
        recordCompletedImportJob(true, job.placeAfterImport ? "Completed for inactive project; placement skipped" : "Completed for inactive project");
        notifications_.notify(
            reimport ? "Reimport completed for inactive project" : "Import completed for inactive project",
            job.placeAfterImport ? NotificationType::Warning : NotificationType::Info,
            NotificationAction::OpenContent,
            "Open Content",
            6.0f);
        if (job.placeAfterImport) {
            std::cerr << "Import and Place completed after the active project changed; placement skipped for "
                      << job.request.sourcePath.string() << '\n';
        }
        return true;
    }

    applyRecords(assetRegistry_);
    (void)assetRegistry_.refreshRecordHealth(job.workspace.root, true);
    if (!assetRegistry_.save(job.workspace.registryPath)) {
        recordCompletedImportJob(false, "Asset registry save failed", {"Could not save registry: " + job.workspace.registryPath.string()});
        notifications_.notify("Asset registry save failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    assetRegistry_.clearDirty();

    if (!reimport) {
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().editorPrefs().addRecentFile(job.request.sourcePath);
            (void)saveActiveEditorPreferences();
        }
        notifications_.notify("Import Asset staged", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        std::cout << "Import Asset staged without scene mutation: " << job.request.sourcePath.string()
                  << " report=" << result.importReportPath.string() << '\n';
        if (job.placeAfterImport) {
            if (!placePrefabAsset(importedGuid)) {
                result.warnings.push_back("Import staged, but prefab placement failed. See editor log for placement error details.");
                recordCompletedImportJob(false, "Import staged; placement failed", result.warnings);
                return false;
            }
        }
        recordCompletedImportJob(true, job.placeAfterImport ? "Import and Place completed" : "Import Asset staged");
        return true;
    }

    if (job.originalType == AssetType::Prefab) {
        const AssetRecord* refreshedRecord = nullptr;
        for (const AssetRecord& record : assetRegistry_.records()) {
            if (record.guid == job.assetGuid) {
                refreshedRecord = &record;
                break;
            }
        }
        if (refreshedRecord != nullptr) {
            AssetManager nextAssets = assets_;
            PrefabRuntimeBindings bindings;
            PrefabAsset prefab;
            std::string prefabError;
            (void)loadPrefabAsset(resolveAssetRecordPath(*refreshedRecord, job.workspace.root), prefab, &prefabError);
            if (std::string bindError; appendPrefabRuntimeAssets(*refreshedRecord, prefab, job.workspace.root, &assetRegistry_, nextAssets, bindings, &bindError)) {
                const SceneDocument beforeDocument = sceneDocument_;
                const AssetManager beforeAssets = assets_;
                assets_ = std::move(nextAssets);
                const uint32_t rebound = rebindGuidBackedRenderers(sceneDocument_, bindings);
                if (rebound > 0) {
                    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
                    undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
                        sceneDocument_,
                        assets_,
                        beforeDocument,
                        beforeAssets,
                        sceneDocument_,
                        assets_,
                        SceneUpdateKind::TopologyChanged,
                        "Reimport Asset"));
                    sceneUnsavedDirty_ = true;
                    (void)applyPendingSceneUpdate(true);
                }
            } else {
                notifications_.notify("Reimported metadata; runtime refresh failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
                std::cerr << "Reimport runtime refresh failed: " << bindError << '\n';
                result.warnings.push_back("Runtime refresh failed: " + bindError);
            }
        }
    }

    recordCompletedImportJob(true, "Asset reimported");
    notifications_.notify("Asset reimported", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Reimported asset: " << job.assetGuid << " source=" << job.request.sourcePath.string() << '\n';
    return true;
}

bool Application::mergeSceneIntoCurrent(const std::filesystem::path& path, bool allowResourceRebuild) {
    (void)allowResourceRebuild;
    SceneLoadResult result;
    result.mode = SceneLoadMode::MergeSceneIntoCurrent;
    result.sourcePath = path;
    try {
        GltfLoader loader(result.assets);
        loader.setCacheWritesEnabled(false);
        result.importedScene = loader.loadWithCache(path);
        result.success = true;
    } catch (const std::exception& error) {
        result.errorMessage = error.what();
    }
    return applyMergeSceneResult(std::move(result));
}

void Application::applyEditorRequests(const EditorRequests& requests, bool allowResourceRebuild) {
    if (allowResourceRebuild && requests.toggleFullscreen && window_ != nullptr) {
        toggleBorderlessFullscreen();
    }
    if (!pathTracer_) {
        if (allowResourceRebuild) {
            if (requests.createProject.has_value()) {
                (void)createProjectFromRequest(*requests.createProject);
            }
            if (requests.openProject.has_value()) {
                (void)openProjectFromFile(requests.openProject->projectFile, true);
            }
            if (requests.deleteProject.has_value()) {
                (void)deleteProjectFromRequest(*requests.deleteProject);
            }
            if (requests.openProjectDirectory) {
                if (project_.has_value() && openDirectoryInShell(project_->projectRoot)) {
                    notifications_.notify("Opening project directory", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
                } else {
                    notifications_.notify("No project directory to open", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
                }
            }
            if (requests.saveAll) {
                (void)saveAllEditorState();
            }
            const std::optional<std::filesystem::path>& openScenePath = requests.openScene.has_value()
                ? requests.openScene
                : requests.loadSceneJson;
            if (openScenePath.has_value()) {
                SceneLoadRequest request;
                request.mode = SceneLoadMode::OpenRtLevel;
                request.sourcePath = *openScenePath;
                if (project_.has_value()) {
                    request.projectSnapshot = *project_;
                }
                (void)requestSceneLoad(std::move(request));
            }
            const std::optional<std::filesystem::path>& importScenePath = requests.importSceneAsNewScene.has_value()
                ? requests.importSceneAsNewScene
                : requests.loadGltf;
            if (importScenePath.has_value()) {
                SceneLoadRequest request;
                request.mode = SceneLoadMode::ImportSceneAsNewScene;
                request.sourcePath = *importScenePath;
                if (project_.has_value()) {
                    request.projectSnapshot = *project_;
                }
                (void)requestSceneLoad(std::move(request));
            }
            if (requests.continueWithoutProject || requests.newScene) {
                initializeFallbackSceneDocument();
                scenePath_.reset();
                gltfPath_.reset();
                importedScene_.reset();
                assets_.clear();
                undoStack_.clear();
                sceneUnsavedDirty_ = false;
                initializeRendererFromCurrentScene();
                notifications_.notify("Editor opened without a project", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
            }
            if (requests.exit && window_ != nullptr) {
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
        }
        return;
    }

    if (requests.cancelSceneLoad && asyncSceneLoader_.isRunning()) {
        asyncSceneLoader_.requestCancel();
        sceneLoadingStatus_ = "Scene load cancellation requested";
        notifications_.notify("Scene load cancellation requested", NotificationType::Warning);
    }

    if (!allowResourceRebuild) {
        if (requests.undo) {
            if (undoStack_.undo()) {
                sceneUnsavedDirty_ = true;
                notifications_.notify("Undo", NotificationType::Info);
            }
        }
        if (requests.redo) {
            if (undoStack_.redo()) {
                sceneUnsavedDirty_ = true;
                notifications_.notify("Redo", NotificationType::Info);
            }
        }
        if (requests.settings.has_value()) {
            sceneUnsavedDirty_ = true;
            RendererSettings settings = *requests.settings;
            applySceneWorldComponentsToRendererSettings(sceneDocument_, settings);
            applyRendererSettingsSafely(settings, false);
        }
        if (requests.previewEntityTransform.has_value()) {
            sceneUnsavedDirty_ = true;
            if (Entity* entity = sceneDocument_.registry().entity(requests.previewEntityTransform->entity)) {
                entity->transform = requests.previewEntityTransform->transform;
                entity->transform.dirty = true;
                sceneDocument_.markDirty(requests.previewEntityTransform->updateKind);
            }
        }
        for (const EditorTimelineTransformSample& sample : requests.timelinePlaybackTransforms) {
            if (Entity* entity = sceneDocument_.registry().entity(sample.entity)) {
                entity->transform = sample.transform;
                entity->transform.dirty = true;
                sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
            }
        }
        (void)applyPendingSceneUpdate(false);
        if (requests.toggleDenoiser) {
            RendererSettings settings = pathTracer_->settings();
            settings.denoiserEnabled = !settings.denoiserEnabled;
            applyRendererSettingsSafely(settings, false);
        }
        if (requests.togglePrimarySun) {
            const bool hadPrimarySun = SunController::primarySunEntity(sceneDocument_).valid();
            EntityId sunId = SunController::ensurePrimarySun(sceneDocument_);
            if (Entity* sun = sceneDocument_.registry().entity(sunId); sun != nullptr && sun->sun.has_value()) {
                sun->sun->enabled = hadPrimarySun ? !sun->sun->enabled : true;
                sceneUnsavedDirty_ = true;
                sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
                (void)applyPendingSceneUpdate(false);
            }
        }
        if (requests.toggleDebugView) {
            RendererSettings settings = pathTracer_->settings();
            settings.debugView = nextDebugView(settings.debugView);
            applyRendererSettingsSafely(settings, false);
        }
        if (requests.cycleIntermediateView) {
            RendererSettings settings = pathTracer_->settings();
            constexpr int count = sizeof(intermediateViews) / sizeof(intermediateViews[0]);
            int idx = 0;
            for (int i = 0; i < count; ++i) {
                if (intermediateViews[i] == settings.debugView) { idx = (i + 1) % count; break; }
            }
            settings.debugView = intermediateViews[idx];
            applyRendererSettingsSafely(settings, false);
        }
        if (requests.resetAccumulation.has_value()) {
            pathTracer_->resetAccumulation(*requests.resetAccumulation);
        }
        if (requests.cameraMoveSpeed.has_value()) {
            const float moveSpeed = std::clamp(*requests.cameraMoveSpeed, 0.05f, 100.0f);
            cameraController_.setMoveSpeed(moveSpeed);
            if (uiOverlay_ != nullptr) {
                EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
                prefs.cameraMoveSpeed = moveSpeed;
                (void)saveActiveEditorPreferences();
            }
        }
        if (requests.cameraFastMoveSpeed.has_value()) {
            const float fastMoveSpeed = std::clamp(*requests.cameraFastMoveSpeed, 0.05f, 250.0f);
            cameraController_.setFastMoveSpeed(fastMoveSpeed);
            if (uiOverlay_ != nullptr) {
                EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
                prefs.cameraFastMoveSpeed = fastMoveSpeed;
                (void)saveActiveEditorPreferences();
            }
        }
        if (requests.cameraMouseSensitivity.has_value()) {
            const float mouseSensitivity = std::clamp(*requests.cameraMouseSensitivity, 0.0001f, 0.02f);
            cameraController_.setMouseSensitivity(mouseSensitivity);
            if (uiOverlay_ != nullptr) {
                EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
                prefs.cameraMouseSensitivity = mouseSensitivity;
                (void)saveActiveEditorPreferences();
            }
        }
        if (requests.cameraInvertLookX.has_value()) {
            cameraController_.setInvertLookX(*requests.cameraInvertLookX);
            if (uiOverlay_ != nullptr) {
                EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
                prefs.cameraInvertLookX = *requests.cameraInvertLookX;
                (void)saveActiveEditorPreferences();
            }
        }
        if (requests.cameraInvertLookY.has_value()) {
            cameraController_.setInvertLookY(*requests.cameraInvertLookY);
            if (uiOverlay_ != nullptr) {
                EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
                prefs.cameraInvertLookY = *requests.cameraInvertLookY;
                (void)saveActiveEditorPreferences();
            }
        }
        if (requests.resetCamera) {
            cameraController_.reset(*pathTracer_);
        }
        if (requests.sceneUpdate.has_value()) {
            sceneDocument_.markDirty(*requests.sceneUpdate);
        }
        if (requests.timelineChanged.has_value()) {
            const SceneDocument before = sceneDocument_;
            sceneDocument_.setTimelineJson(*requests.timelineChanged);
            SceneDocument after = sceneDocument_;
            after.markDirty(SceneUpdateKind::None);
            undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
                sceneDocument_, before, std::move(after), SceneUpdateKind::None, "Edit Timeline"));
            sceneUnsavedDirty_ = true;
            notifications_.notify("Timeline updated", NotificationType::Info);
        }
        return;
    }

    if (pendingPostFrameSettings_.has_value()) {
        RendererSettings pending = *pendingPostFrameSettings_;
        pendingPostFrameSettings_.reset();
        applyRendererSettingsSafely(pending, true);
    }

    const std::filesystem::path renderOutputRoot = editorRenderOutputRoot(project_);
    if (requests.renderCurrentViewport) {
        startEditorRenderJob(EditorRenderJobKind::CurrentViewport, renderOutputRoot);
    }
    if (requests.renderRequest.has_value()) {
        EditorRenderRequest request = *requests.renderRequest;
        if (request.outputRoot.empty()) {
            request.outputRoot = renderOutputRoot;
        }
        startEditorRenderJob(request.kind, request.outputRoot, &request);
    }
    if (requests.renderImage) {
        startEditorRenderJob(EditorRenderJobKind::Image, renderOutputRoot);
    }
    if (requests.renderSequence) {
        startEditorRenderJob(EditorRenderJobKind::Sequence, renderOutputRoot);
    }
    if (requests.stopRender) {
        cancelEditorRenderJob(renderOutputRoot);
    }
    if (requests.openOutputFolder || requests.openOutputFolderPath.has_value()) {
        std::error_code ec;
        std::filesystem::path outputFolder;
        if (requests.openOutputFolderPath.has_value() && !requests.openOutputFolderPath->empty()) {
            outputFolder = *requests.openOutputFolderPath;
        } else if (editorRenderJob_.kind != EditorRenderJobKind::None && !editorRenderJob_.outputRoot.empty()) {
            outputFolder = editorRenderJob_.outputRoot;
        } else {
            outputFolder = renderOutputRoot;
        }
        std::filesystem::create_directories(outputFolder, ec);
        if (!ec && openDirectoryInShell(outputFolder)) {
            notifications_.notify("Opening render output folder", NotificationType::Info, NotificationAction::OpenOutputFolder, "Open Output", 4.0f);
        } else {
            notifications_.notify("Render output folder could not be opened", NotificationType::Warning, NotificationAction::OpenOutputFolder, "Open Output", 5.0f);
        }
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, "Open render output folder: " + outputFolder.string());
        }
    }
    if (requests.openFilePath.has_value()) {
        const std::filesystem::path filePath = *requests.openFilePath;
        if (openFileInShell(filePath)) {
            notifications_.notify("Opening file", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        } else {
            notifications_.notify("File could not be opened", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, "Open file: " + filePath.string());
        }
    }
    if (requests.openDirectoryPath.has_value()) {
        const std::filesystem::path directoryPath = *requests.openDirectoryPath;
        if (openDirectoryInShell(directoryPath)) {
            notifications_.notify("Opening folder", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        } else {
            notifications_.notify("Folder could not be opened", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, "Open folder: " + directoryPath.string());
        }
    }
    if (requests.dismissAllDroppedFiles) {
        pendingDroppedFiles_.clear();
    } else if (requests.dismissDroppedFile.has_value()) {
        const std::filesystem::path dismissed = *requests.dismissDroppedFile;
        pendingDroppedFiles_.erase(
            std::remove_if(
                pendingDroppedFiles_.begin(),
                pendingDroppedFiles_.end(),
                [&](const std::filesystem::path& path) { return path == dismissed; }),
            pendingDroppedFiles_.end());
    }
    if (requests.openLogFolder) {
        std::filesystem::path logFolder = project_.has_value()
            ? project_->savedRoot / "Logs"
            : std::filesystem::current_path() / "out" / "editor_tools";
        std::error_code ec;
        std::filesystem::create_directories(logFolder, ec);
        if (!ec && openDirectoryInShell(logFolder)) {
            notifications_.notify("Opening log folder", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
        } else {
            notifications_.notify("Log folder could not be opened", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, "Open log folder: " + logFolder.string());
        }
    }
    if (requests.openProjectDirectory) {
        if (project_.has_value() && openDirectoryInShell(project_->projectRoot)) {
            notifications_.notify("Opening project directory", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 4.0f);
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().log().add(EditorLogCategory::Project, "Open project directory: " + project_->projectRoot.string());
            }
        } else {
            notifications_.notify("No project directory to open", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        }
    }
    if (requests.saveAll) {
        (void)saveAllEditorState();
    } else if (requests.saveMaterialAsset.has_value()) {
        std::string savedItem;
        std::string failureItem;
        std::vector<std::string> savedItems;
        std::vector<std::string> failures;
        if (saveDirtyMaterialAsset(*requests.saveMaterialAsset, savedItem, failureItem)) {
            savedItems.push_back(std::move(savedItem));
            const std::filesystem::path registryPath = assetRegistry_.state().path;
            if (assetRegistry_.dirty()) {
                if (assetRegistry_.save()) {
                    assetRegistry_.clearDirty();
                    savedItems.push_back("Asset Registry: " + registryPath.string());
                } else {
                    failures.push_back(registryPath.empty()
                        ? std::string("Asset Registry: no registry path")
                        : std::string("Asset Registry: ") + registryPath.string());
                }
            }
        } else {
            failures.push_back(std::move(failureItem));
        }

        if (uiOverlay_ != nullptr) {
            EditorLog& log = uiOverlay_->editor().log();
            for (const std::string& item : savedItems) {
                log.add(EditorLogCategory::Project, "Save Material saved " + item);
            }
            for (const std::string& item : failures) {
                log.add(EditorLogCategory::Warning, "Save Material failed " + item);
            }
        }
        if (!failures.empty()) {
            notifications_.notify("Save Material failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            for (const std::string& item : failures) {
                std::cerr << "Save Material failed: " << item << '\n';
            }
        } else {
            notifications_.notify("Save Material complete", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
    }

    if (requests.createProject.has_value()) {
        (void)createProjectFromRequest(*requests.createProject);
    }
    if (requests.openProject.has_value()) {
        (void)openProjectFromFile(requests.openProject->projectFile, true);
    }
    if (requests.deleteProject.has_value()) {
        (void)deleteProjectFromRequest(*requests.deleteProject);
    }
    if (requests.restoreAutosave) {
        bool restored = false;
        if (pendingRecoveryProjectAutosavePath_.has_value() && std::filesystem::exists(*pendingRecoveryProjectAutosavePath_) && project_.has_value()) {
            try {
                std::ifstream in(*pendingRecoveryProjectAutosavePath_);
                nlohmann::json json;
                in >> json;
                project_->autosaveEnabled = json.value("autosaveEnabled", project_->autosaveEnabled);
                project_->autosaveIntervalMinutes = std::clamp(json.value("autosaveIntervalMinutes", project_->autosaveIntervalMinutes), 1, 120);
                projectSettingsDirty_ = true;
                restored = true;
                if (uiOverlay_ != nullptr) {
                    uiOverlay_->editor().log().add(EditorLogCategory::Project, "Restored project settings autosave: " + pendingRecoveryProjectAutosavePath_->string());
                }
            } catch (const std::exception& error) {
                notifications_.notify("Project autosave restore failed", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
                std::cerr << "Project autosave restore failed: " << pendingRecoveryProjectAutosavePath_->string() << " " << error.what() << '\n';
            }
        }
        if (pendingRecoveryAssetRegistryAutosavePath_.has_value() && std::filesystem::exists(*pendingRecoveryAssetRegistryAutosavePath_) && project_.has_value()) {
            AssetRegistry restoredRegistry;
            std::string error;
            if (restoredRegistry.load(*pendingRecoveryAssetRegistryAutosavePath_, &error)) {
                assetRegistry_ = std::move(restoredRegistry);
                assetRegistry_.setPath(project_->assetRegistryPath);
                (void)assetRegistry_.refreshRecordHealth(project_->projectRoot, false);
                assetRegistry_.markDirty(AssetRegistryDirtyReason::AssetAutosaveRestored);
                restored = true;
                if (uiOverlay_ != nullptr) {
                    uiOverlay_->editor().log().add(EditorLogCategory::Project, "Restored asset registry autosave: " + pendingRecoveryAssetRegistryAutosavePath_->string());
                }
            } else {
                notifications_.notify("Asset registry autosave restore failed", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
                std::cerr << "Asset registry autosave restore failed: " << pendingRecoveryAssetRegistryAutosavePath_->string() << " " << error << '\n';
            }
        }
        if (restoreMaterialAssetAutosaves()) {
            restored = true;
        }
        if (pendingRecoveryAutosavePath_.has_value() && std::filesystem::exists(*pendingRecoveryAutosavePath_)) {
            SceneLoadRequest request;
            request.mode = SceneLoadMode::OpenRtLevel;
            request.sourcePath = *pendingRecoveryAutosavePath_;
            request.restoreAsUnsaved = true;
            if (pendingRecoveryScenePath_.has_value()) {
                request.restoredScenePath = *pendingRecoveryScenePath_;
            }
            if (project_.has_value()) {
                request.projectSnapshot = *project_;
            }
            (void)requestSceneLoad(std::move(request));
            restored = true;
        }
        if (restored) {
            notifications_.notify("Restoring autosaves", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        } else {
            notifications_.notify("No autosaves available to restore", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        }
        pendingRecoveryScenePath_.reset();
        pendingRecoveryAutosavePath_.reset();
        pendingRecoveryProjectAutosavePath_.reset();
        pendingRecoveryMaterialAssetAutosaves_.clear();
        pendingRecoveryAssetRegistryAutosavePath_.reset();
    }
    if (requests.discardRecovery) {
        if (project_.has_value()) {
            std::error_code ec;
            std::filesystem::remove(project_->savedRoot / "editor_session.json", ec);
        }
        pendingRecoveryScenePath_.reset();
        pendingRecoveryAutosavePath_.reset();
        pendingRecoveryProjectAutosavePath_.reset();
        pendingRecoveryMaterialAssetAutosaves_.clear();
        pendingRecoveryAssetRegistryAutosavePath_.reset();
        notifications_.notify("Recovery marker discarded", NotificationType::Info);
    }
    if (requests.projectSettingsUpdate.has_value() && project_.has_value()) {
        const std::filesystem::path requestedStartupScene = requests.projectSettingsUpdate->startupScene;
        const bool startupSceneChanged = normalizedPathForCompare(project_->startupScene) != normalizedPathForCompare(requestedStartupScene);
        bool startupSceneAccepted = true;
        if (startupSceneChanged) {
            std::error_code ec;
            const std::string extension = lowerPathExtension(requestedStartupScene);
            startupSceneAccepted = !requestedStartupScene.empty() &&
                extension == ".rtlevel" &&
                std::filesystem::is_regular_file(requestedStartupScene, ec);
            if (!startupSceneAccepted) {
                notifications_.notify("Default level must be an existing .rtlevel", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
            }
        }
        const bool changed =
            (startupSceneChanged && startupSceneAccepted) ||
            project_->preferredWorkspacePreset != requests.projectSettingsUpdate->preferredWorkspacePreset ||
            project_->autosaveEnabled != requests.projectSettingsUpdate->autosaveEnabled ||
            project_->autosaveIntervalMinutes != std::clamp(requests.projectSettingsUpdate->autosaveIntervalMinutes, 1, 120);
        if (startupSceneChanged && startupSceneAccepted) {
            project_->startupScene = requestedStartupScene;
        }
        project_->preferredWorkspacePreset = requests.projectSettingsUpdate->preferredWorkspacePreset;
        if (uiOverlay_ != nullptr) {
            if (project_->preferredWorkspacePreset >= 0) {
                uiOverlay_->editor().setProjectWorkspacePreset(project_->preferredWorkspacePreset);
            } else {
                uiOverlay_->editor().clearProjectWorkspacePreset();
            }
        }
        project_->autosaveEnabled = requests.projectSettingsUpdate->autosaveEnabled;
        project_->autosaveIntervalMinutes = std::clamp(requests.projectSettingsUpdate->autosaveIntervalMinutes, 1, 120);
        if (changed) {
            projectSettingsDirty_ = true;
            notifications_.notify("Project settings updated", NotificationType::Info);
        }
    }
    if (requests.saveProjectSettings && project_.has_value()) {
        const bool projectSaved = saveProjectFile(*project_);
        const bool registrySaved = !assetRegistry_.dirty() || assetRegistry_.save();
        if (projectSaved && registrySaved) {
            projectSettingsDirty_ = false;
            assetRegistry_.clearDirty();
            notifications_.notify("Project settings saved", NotificationType::Success);
        } else {
            notifications_.notify("Project settings save failed", NotificationType::Error);
        }
    }
    if (requests.closeProject) {
        (void)closeCurrentProject();
    }

    if (requests.loadHdr.has_value()) {
        if (assignEnvironmentPath(*requests.loadHdr, allowResourceRebuild, "Assign Environment", "Environment assigned")) {
            if (uiOverlay_ != nullptr) {
                EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
                prefs.addRecentFile(*requests.loadHdr);
                (void)saveActiveEditorPreferences();
            }
            std::cout << "Assigned HDR environment from editor: " << requests.loadHdr->string() << '\n';
        }
    }

    const std::optional<std::filesystem::path>& saveScenePath = requests.saveSceneAs.has_value()
        ? requests.saveSceneAs
        : (requests.saveScene.has_value() ? requests.saveScene : requests.saveSceneJson);
    if (saveScenePath.has_value()) {
        serializeEditorSceneData();
        if (sceneDocument_.saveJson(*saveScenePath)) {
            scenePath_ = *saveScenePath;
            sceneDocument_.clearDirty();
            sceneUnsavedDirty_ = false;
            notifications_.notify("Scene saved", NotificationType::Success);
            std::cout << "Saved scene: " << saveScenePath->string() << '\n';
        } else {
            notifications_.notify("Scene save failed", NotificationType::Error);
            std::cerr << "Scene save failed: " << saveScenePath->string() << '\n';
        }
    }

    const std::optional<std::filesystem::path>& openScenePath = requests.openScene.has_value()
        ? requests.openScene
        : requests.loadSceneJson;
    if (openScenePath.has_value()) {
        if (!confirmDestructiveSceneAction("opening another scene")) {
            std::cout << "Open scene cancelled: " << openScenePath->string() << '\n';
        } else {
            SceneLoadRequest request;
            request.mode = SceneLoadMode::OpenRtLevel;
            request.sourcePath = *openScenePath;
            if (project_.has_value()) {
                request.projectSnapshot = *project_;
            }
            (void)requestSceneLoad(std::move(request));
        }
    }

    if (requests.newScene) {
        if (!confirmDestructiveSceneAction("creating a new scene")) {
            std::cout << "New scene cancelled\n";
        } else {
            initializeFallbackSceneDocument();
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().timeline().clear();
            }
            scenePath_.reset();
            gltfPath_.reset();
            importedScene_.reset();
            assets_.clear();
            undoStack_.clear();
            sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
            sceneUnsavedDirty_ = true;
            notifications_.notify("New scene created", NotificationType::Info);
        }
    }

    if (requests.closeScene) {
        if (!confirmDestructiveSceneAction("closing the scene tab")) {
            std::cout << "Close scene tab cancelled\n";
        } else {
            initializeFallbackSceneDocument();
            if (uiOverlay_ != nullptr) {
                uiOverlay_->editor().timeline().clear();
            }
            scenePath_.reset();
            gltfPath_.reset();
            importedScene_.reset();
            assets_.clear();
            undoStack_.clear();
            sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
            (void)applyPendingSceneUpdate(true);
            sceneUnsavedDirty_ = false;
            notifications_.notify("Scene closed", NotificationType::Info);
        }
    }

    if (requests.materialUpdate.has_value()) {
        MaterialAsset* material = assets_.material(MaterialAssetHandle{requests.materialUpdate->materialId});
        if (material != nullptr) {
            const SceneDocument beforeDocument = sceneDocument_;
            const AssetManager beforeAssets = assets_;
            const std::optional<AssetRecord> materialRecord = materialAssetRecordForMaterial(requests.materialUpdate->materialId);
            *material = requests.materialUpdate->material;
            for (uint32_t meshIndex = 0; meshIndex < assets_.meshes().size(); ++meshIndex) {
                MeshAsset* mesh = assets_.mesh(MeshAssetHandle{meshIndex});
                if (mesh == nullptr) {
                    continue;
                }
                for (MeshPrimitiveAsset& primitive : mesh->primitives) {
                    if (primitive.material.index == requests.materialUpdate->materialId) {
                        updatePrimitiveAlphaClassification(primitive, material);
                    }
                }
            }
            bool gpuUpdated = false;
            if (gpuSceneAsset_.has_value()) {
                gpuUpdated = pathTracer_->updateMaterials(*gpuSceneAsset_, assets_);
            }
            if (!gpuUpdated) {
                pathTracer_->resetAccumulation(AccumulationResetReason::MaterialChanged);
            }
            if (materialRecord.has_value()) {
                dirtyMaterialAssets_[materialRecord->guid] = *material;
                if (project_.has_value()) {
                    materialAssetAutosavePaths_.try_emplace(materialRecord->guid, editorMaterialAssetAutosavePath(*project_, *materialRecord));
                }
                assetRegistry_.markDirty(AssetRegistryDirtyReason::AssetEdited);
            } else {
                sceneDocument_.markDirty(SceneUpdateKind::MaterialOnly);
                sceneUnsavedDirty_ = true;
            }
            if (!materialRecord.has_value()) {
                undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
                    sceneDocument_,
                    assets_,
                    beforeDocument,
                    beforeAssets,
                    sceneDocument_,
                    assets_,
                    SceneUpdateKind::MaterialOnly,
                    "Edit Material"));
            }
        }
    }

    if (requests.materialAssignment.has_value()) {
        const SceneDocument beforeDocument = sceneDocument_;
        const AssetManager beforeAssets = assets_;
        bool assigned = false;
        if (requests.materialAssignment->entity.valid()) {
            if (Entity* entity = sceneDocument_.registry().entity(requests.materialAssignment->entity);
                entity != nullptr && entity->meshRenderer.has_value()) {
                MeshRenderer& renderer = *entity->meshRenderer;
                ensureMaterialSlotsForRenderer(renderer, assets_);
                const MaterialAssetHandle material = requests.materialAssignment->material;
                if (requests.materialAssignment->primitiveIndex == UINT32_MAX) {
                    for (MaterialSlot& slot : renderer.materialSlots) {
                        slot.overrideMaterial = material.index == slot.material.index
                            ? std::optional<MaterialAssetHandle>{}
                            : std::optional<MaterialAssetHandle>{material};
                    }
                    assigned = true;
                } else if (requests.materialAssignment->primitiveIndex < renderer.materialSlots.size()) {
                    MaterialSlot& slot = renderer.materialSlots[requests.materialAssignment->primitiveIndex];
                    slot.overrideMaterial = material.index == slot.material.index
                        ? std::optional<MaterialAssetHandle>{}
                        : std::optional<MaterialAssetHandle>{material};
                    assigned = true;
                }
            }
        }
        MeshAsset* mesh = assigned ? nullptr : assets_.mesh(requests.materialAssignment->mesh);
        if (mesh != nullptr && requests.materialAssignment->primitiveIndex == UINT32_MAX) {
            for (MeshPrimitiveAsset& primitive : mesh->primitives) {
                primitive.material = requests.materialAssignment->material;
                updatePrimitiveAlphaClassification(primitive, assets_.material(requests.materialAssignment->material));
            }
            assigned = true;
        } else if (mesh != nullptr && requests.materialAssignment->primitiveIndex < mesh->primitives.size()) {
            MeshPrimitiveAsset& primitive = mesh->primitives[requests.materialAssignment->primitiveIndex];
            primitive.material = requests.materialAssignment->material;
            updatePrimitiveAlphaClassification(primitive, assets_.material(requests.materialAssignment->material));
            assigned = true;
        }
        if (assigned) {
            sceneDocument_.markDirty(SceneUpdateKind::MaterialOnly);
            sceneUnsavedDirty_ = true;
            undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
                sceneDocument_,
                assets_,
                beforeDocument,
                beforeAssets,
                sceneDocument_,
                assets_,
                SceneUpdateKind::MaterialOnly,
                "Assign Material"));
        }
    }

    if (requests.materialAssetAssignment.has_value()) {
        (void)assignMaterialAssetToEntity(*requests.materialAssetAssignment);
    }

    if (requests.meshAssetPlacement.has_value()) {
        (void)placeMeshAsset(*requests.meshAssetPlacement);
    }

    if (requests.environmentAssetAssignment.has_value()) {
        (void)assignEnvironmentAsset(*requests.environmentAssetAssignment, allowResourceRebuild);
    }

    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (requests.createEntity.has_value()) {
        const EditorEntityCreateRequest& create = *requests.createEntity;
        const SceneUpdateKind createUpdateKind = createEntityUpdateKind(create.kind);
        const SceneDocument beforeDocument = sceneDocument_;
        sceneOps.setUndoStack(nullptr);
        EntityId created{};
        switch (create.kind) {
        case EditorEntityCreateKind::Empty:
            created = sceneOps.createEntity("Entity", create.parent, createUpdateKind);
            break;
        case EditorEntityCreateKind::Camera:
            created = sceneOps.createEntity("Camera", create.parent, createUpdateKind);
            if (created.valid()) {
                Camera camera;
                camera.active = true;
                (void)sceneOps.addCameraComponent(created, camera);
            }
            break;
        case EditorEntityCreateKind::Light:
            created = sceneOps.createEntity("Point Light", create.parent, createUpdateKind);
            if (created.valid()) {
                Light light;
                light.intensity = 100.0f;
                (void)sceneOps.addLightComponent(created, light);
            }
            break;
        case EditorEntityCreateKind::Sun:
            created = sceneOps.createEntity("Sun", create.parent, createUpdateKind);
            if (created.valid()) {
                Sun sun;
                sun.elevation = sceneDocument_.renderSettings().sunElevation;
                sun.azimuth = sceneDocument_.renderSettings().sunAzimuth;
                (void)sceneOps.addSunComponent(created, sun);
            }
            break;
        case EditorEntityCreateKind::SpotLight:
            created = sceneOps.createEntity("Spot Light", create.parent, createUpdateKind);
            if (created.valid()) {
                Light light;
                light.type = LightType::Spot;
                light.intensity = 100.0f;
                (void)sceneOps.addLightComponent(created, light);
            }
            break;
        case EditorEntityCreateKind::AreaLight:
            created = sceneOps.createEntity("Area Light", create.parent, createUpdateKind);
            if (created.valid()) {
                Light light;
                light.type = LightType::Area;
                light.sizeOrRadius = 1.0f;
                light.intensity = 8.0f;
                (void)sceneOps.addLightComponent(created, light);
            }
            break;
        case EditorEntityCreateKind::EnvironmentLight:
            created = sceneOps.createEntity("Environment Light", create.parent, createUpdateKind);
            if (Entity* entity = sceneDocument_.registry().entity(created)) {
                entity->environmentLight = EnvironmentLight{};
                sceneDocument_.worldSettings().activeEnvironment = created;
            }
            break;
        case EditorEntityCreateKind::SkyAtmosphere:
            created = sceneOps.createEntity("Sky Atmosphere", create.parent, createUpdateKind);
            if (Entity* entity = sceneDocument_.registry().entity(created)) {
                entity->skyAtmosphere = SkyAtmosphere{};
                sceneDocument_.worldSettings().skyAtmosphere = created;
            }
            break;
        case EditorEntityCreateKind::HeightFog:
            created = sceneOps.createEntity("Height Fog", create.parent, createUpdateKind);
            if (Entity* entity = sceneDocument_.registry().entity(created)) {
                entity->heightFog = HeightFog{};
                sceneDocument_.worldSettings().heightFog = created;
            }
            break;
        case EditorEntityCreateKind::VolumetricCloud:
            created = sceneOps.createEntity("Volumetric Cloud", create.parent, createUpdateKind);
            if (Entity* entity = sceneDocument_.registry().entity(created)) {
                entity->volumetricCloud = VolumetricCloud{};
            }
            break;
        case EditorEntityCreateKind::PostProcessVolume:
            created = sceneOps.createEntity("Post Process Volume", create.parent, createUpdateKind);
            if (Entity* entity = sceneDocument_.registry().entity(created)) {
                entity->postProcessVolume = PostProcessVolume{};
                sceneDocument_.worldSettings().postProcessVolume = created;
            }
            break;
        }
        if (created.valid()) {
            sceneUnsavedDirty_ = true;
            if (Entity* entity = sceneDocument_.registry().entity(created)) {
                glm::vec3 forward = cameraController_.direction();
                if (glm::dot(forward, forward) <= 0.0f) {
                    forward = glm::vec3(0.0f, 0.0f, -1.0f);
                } else {
                    forward = glm::normalize(forward);
                }
                entity->transform.position = cameraController_.position() + forward * 2.5f;
                entity->transform.dirty = true;
                entity->defaultTransform = entity->transform;
                sceneDocument_.markDirty(createUpdateKind);
            }
            sceneOps.setUndoStack(&undoStack_);
            sceneOps.pushDocumentSnapshot(beforeDocument, createUpdateKind, "Create Entity");
            editorPlacement_.entity = created;
            editorPlacement_.serial = nextEditorPlacementSerial_++;
            editorPlacement_.label = "Created entity";
        } else {
            sceneOps.setUndoStack(&undoStack_);
        }
    }

    if (requests.ensurePrimarySun) {
        if (sceneOps.ensurePrimarySun()) {
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.duplicateEntity.has_value()) {
        const EntityId duplicate = sceneOps.duplicateEntity(*requests.duplicateEntity);
        if (duplicate.valid()) {
            editorPlacement_.entity = duplicate;
            editorPlacement_.serial = nextEditorPlacementSerial_++;
            editorPlacement_.label = "Duplicated entity";
            sceneUnsavedDirty_ = true;
        }
    }

    auto clearDeletedEditorEntityState = [&](EntityId id) {
        if (editorPlacement_.entity == id) {
            editorPlacement_ = EditorPlacementStatus{};
        }
        if (validationObjectMotionEntity_ == id) {
            validationObjectMotionEntity_ = {};
            validationObjectMotionBaseTransform_ = {};
        }
        if (sunDrag_.entity == id) {
            if (sunDrag_.phase == SunDragPhase::Dragging && window_ != nullptr) {
                glfwSetInputMode(window_, GLFW_CURSOR, sunDrag_.previousCursorMode);
            }
            sunDrag_ = SunDragState{};
        }
    };

    if (requests.deleteEntity.has_value()) {
        const EntityId deleted = *requests.deleteEntity;
        if (sceneOps.deleteEntity(deleted)) {
            clearDeletedEditorEntityState(deleted);
            sceneUnsavedDirty_ = true;
        }
    }

    if (!requests.deleteEntities.empty()) {
        if (sceneOps.deleteEntities(requests.deleteEntities)) {
            for (EntityId deleted : requests.deleteEntities) {
                clearDeletedEditorEntityState(deleted);
            }
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.renameEntity.has_value()) {
        if (sceneOps.renameEntity(requests.renameEntity->entity, requests.renameEntity->name)) {
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.reparentEntity.has_value()) {
        const auto [child, newParent] = *requests.reparentEntity;
        (void)sceneOps.reparentEntity(child, newParent);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setEntityVisibility.has_value()) {
        if (sceneOps.setVisibility(requests.setEntityVisibility->entity, requests.setEntityVisibility->value)) {
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.setEntityLocked.has_value()) {
        if (sceneOps.setLocked(requests.setEntityLocked->entity, requests.setEntityLocked->value)) {
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.setEntityTransform.has_value()) {
        sceneOps.setTransformGizmoDrag(
            requests.setEntityTransform->entity,
            requests.setEntityTransform->oldTransform,
            requests.setEntityTransform->newTransform);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setMeshRenderer.has_value()) {
        if (sceneOps.setMeshRenderer(
                requests.setMeshRenderer->entity,
                requests.setMeshRenderer->oldRenderer,
                requests.setMeshRenderer->newRenderer,
                requests.setMeshRenderer->updateKind)) {
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.addComponent.has_value()) {
        const SceneDocument beforeDocument = sceneDocument_;
        bool directComponentAdded = false;
        SceneUpdateKind directUpdateKind = SceneUpdateKind::TopologyChanged;
        switch (requests.addComponent->kind) {
        case EditorComponentKind::Light:
            {
                Light light;
                light.intensity = 100.0f;
                (void)sceneOps.addLightComponent(requests.addComponent->entity, light);
            }
            break;
        case EditorComponentKind::Sun:
            (void)sceneOps.addSunComponent(requests.addComponent->entity, Sun{});
            break;
        case EditorComponentKind::Camera:
            (void)sceneOps.addCameraComponent(requests.addComponent->entity, Camera{});
            break;
        case EditorComponentKind::MeshRenderer:
            (void)sceneOps.addMeshRendererComponent(requests.addComponent->entity, MeshRenderer{});
            break;
        case EditorComponentKind::EnvironmentLight:
            if (Entity* entity = sceneDocument_.registry().entity(requests.addComponent->entity); entity != nullptr && !entity->environmentLight.has_value()) {
                entity->environmentLight = EnvironmentLight{};
                sceneDocument_.worldSettings().activeEnvironment = entity->id;
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                directComponentAdded = true;
            }
            break;
        case EditorComponentKind::SkyAtmosphere:
            if (Entity* entity = sceneDocument_.registry().entity(requests.addComponent->entity); entity != nullptr && !entity->skyAtmosphere.has_value()) {
                entity->skyAtmosphere = SkyAtmosphere{};
                sceneDocument_.worldSettings().skyAtmosphere = entity->id;
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                directComponentAdded = true;
            }
            break;
        case EditorComponentKind::HeightFog:
            if (Entity* entity = sceneDocument_.registry().entity(requests.addComponent->entity); entity != nullptr && !entity->heightFog.has_value()) {
                entity->heightFog = HeightFog{};
                sceneDocument_.worldSettings().heightFog = entity->id;
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                directComponentAdded = true;
            }
            break;
        case EditorComponentKind::VolumetricCloud:
            if (Entity* entity = sceneDocument_.registry().entity(requests.addComponent->entity); entity != nullptr && !entity->volumetricCloud.has_value()) {
                entity->volumetricCloud = VolumetricCloud{};
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                directComponentAdded = true;
            }
            break;
        case EditorComponentKind::PostProcessVolume:
            if (Entity* entity = sceneDocument_.registry().entity(requests.addComponent->entity); entity != nullptr && !entity->postProcessVolume.has_value()) {
                entity->postProcessVolume = PostProcessVolume{};
                sceneDocument_.worldSettings().postProcessVolume = entity->id;
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                directComponentAdded = true;
            }
            break;
        case EditorComponentKind::CameraPostProcess:
            if (Entity* entity = sceneDocument_.registry().entity(requests.addComponent->entity); entity != nullptr && entity->camera.has_value() && !entity->cameraPostProcess.has_value()) {
                entity->cameraPostProcess = CameraPostProcess{};
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                directComponentAdded = true;
            }
            break;
        case EditorComponentKind::AnimationPlayer:
            if (Entity* entity = sceneDocument_.registry().entity(requests.addComponent->entity); entity != nullptr && !entity->animationPlayer.has_value()) {
                entity->animationPlayer = AnimationPlayer{};
                sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
                directUpdateKind = SceneUpdateKind::TransformOnly;
                directComponentAdded = true;
            }
            break;
        }
        if (directComponentAdded) {
            undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
                sceneDocument_, beforeDocument, sceneDocument_, directUpdateKind, "Add Component"));
        }
        sceneUnsavedDirty_ = true;
    }

    if (requests.removeComponent.has_value()) {
        const SceneDocument beforeDocument = sceneDocument_;
        bool removed = false;
        SceneUpdateKind directUpdateKind = SceneUpdateKind::TopologyChanged;
        switch (requests.removeComponent->kind) {
        case EditorComponentKind::Light:
            removed = sceneOps.removeLightComponent(requests.removeComponent->entity);
            break;
        case EditorComponentKind::Sun:
            removed = sceneOps.removeSunComponent(requests.removeComponent->entity);
            break;
        case EditorComponentKind::Camera:
            removed = sceneOps.removeCameraComponent(requests.removeComponent->entity);
            break;
        case EditorComponentKind::MeshRenderer:
            removed = sceneOps.removeMeshRendererComponent(requests.removeComponent->entity);
            break;
        case EditorComponentKind::EnvironmentLight:
            if (Entity* entity = sceneDocument_.registry().entity(requests.removeComponent->entity); entity != nullptr && entity->environmentLight.has_value()) {
                entity->environmentLight.reset();
                if (sceneDocument_.worldSettings().activeEnvironment == entity->id) sceneDocument_.worldSettings().activeEnvironment = {};
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                removed = true;
            }
            break;
        case EditorComponentKind::SkyAtmosphere:
            if (Entity* entity = sceneDocument_.registry().entity(requests.removeComponent->entity); entity != nullptr && entity->skyAtmosphere.has_value()) {
                entity->skyAtmosphere.reset();
                if (sceneDocument_.worldSettings().skyAtmosphere == entity->id) sceneDocument_.worldSettings().skyAtmosphere = {};
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                removed = true;
            }
            break;
        case EditorComponentKind::HeightFog:
            if (Entity* entity = sceneDocument_.registry().entity(requests.removeComponent->entity); entity != nullptr && entity->heightFog.has_value()) {
                entity->heightFog.reset();
                if (sceneDocument_.worldSettings().heightFog == entity->id) sceneDocument_.worldSettings().heightFog = {};
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                removed = true;
            }
            break;
        case EditorComponentKind::VolumetricCloud:
            if (Entity* entity = sceneDocument_.registry().entity(requests.removeComponent->entity); entity != nullptr && entity->volumetricCloud.has_value()) {
                entity->volumetricCloud.reset();
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                removed = true;
            }
            break;
        case EditorComponentKind::PostProcessVolume:
            if (Entity* entity = sceneDocument_.registry().entity(requests.removeComponent->entity); entity != nullptr && entity->postProcessVolume.has_value()) {
                entity->postProcessVolume.reset();
                if (sceneDocument_.worldSettings().postProcessVolume == entity->id) sceneDocument_.worldSettings().postProcessVolume = {};
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                removed = true;
            }
            break;
        case EditorComponentKind::CameraPostProcess:
            if (Entity* entity = sceneDocument_.registry().entity(requests.removeComponent->entity); entity != nullptr && entity->cameraPostProcess.has_value()) {
                entity->cameraPostProcess.reset();
                applySceneWorldComponentsToDocumentSettings(sceneDocument_);
                sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
                directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
                removed = true;
            }
            break;
        case EditorComponentKind::AnimationPlayer:
            if (Entity* entity = sceneDocument_.registry().entity(requests.removeComponent->entity); entity != nullptr && entity->animationPlayer.has_value()) {
                entity->animationPlayer.reset();
                sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
                directUpdateKind = SceneUpdateKind::TransformOnly;
                removed = true;
            }
            break;
        }
        if (removed) {
            if (requests.removeComponent->kind >= EditorComponentKind::EnvironmentLight) {
                undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
                    sceneDocument_, beforeDocument, sceneDocument_, directUpdateKind, "Remove Component"));
            }
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.sceneSnapshot.has_value()) {
        sceneDocument_.markDirty(requests.sceneSnapshot->updateKind);
        undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
            sceneDocument_,
            requests.sceneSnapshot->before,
            sceneDocument_,
            requests.sceneSnapshot->updateKind,
            requests.sceneSnapshot->label.empty() ? "Edit Scene" : requests.sceneSnapshot->label));
        sceneUnsavedDirty_ = true;
    }

    if (requests.setLight.has_value()) {
        (void)sceneOps.setLight(
            requests.setLight->entity,
            requests.setLight->oldLight,
            requests.setLight->newLight);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setSun.has_value()) {
        (void)sceneOps.setSun(
            requests.setSun->entity,
            requests.setSun->oldSun,
            requests.setSun->newSun);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setCamera.has_value()) {
        (void)sceneOps.setCamera(
            requests.setCamera->entity,
            requests.setCamera->oldCamera,
            requests.setCamera->newCamera,
            requests.setCamera->oldActiveCamera,
            requests.setCamera->newActiveCamera);
        sceneUnsavedDirty_ = true;
    }

    if (requests.focusOnEntity.has_value()) {
        const Entity* entity = sceneDocument_.registry().entity(*requests.focusOnEntity);
        if (entity != nullptr) {
            glm::vec3 minBounds(std::numeric_limits<float>::max());
            glm::vec3 maxBounds(-std::numeric_limits<float>::max());
            bool hasBounds = false;
            expandEntityBounds(sceneDocument_.registry(), assets_, *entity, minBounds, maxBounds, hasBounds);
            if (hasBounds) {
                const glm::vec3 target = (minBounds + maxBounds) * 0.5f;
                const float radius = std::max(glm::length(maxBounds - minBounds) * 0.5f, 0.35f);
                glm::vec3 forward = cameraController_.direction();
                if (glm::dot(forward, forward) <= 0.0001f) {
                    forward = glm::vec3(0.0f, 0.0f, -1.0f);
                }
                forward = glm::normalize(forward);
                const float fovY = std::max(activeCameraFovRadians(sceneDocument_), glm::radians(5.0f));
                const float distance = std::max(radius / std::tan(fovY * 0.5f) * 1.35f, radius + 0.5f);
                const glm::vec3 position = target - forward * distance;
                cameraController_.setPose(position, forward, *pathTracer_);
                notifications_.notify("Framed selected entity", NotificationType::Info);
            }
        }
    }

    if (requests.saveCameraBookmark.has_value()) {
        uiOverlay_->editor().cameraBookmarks().saveBookmark(
            cameraController_, *requests.saveCameraBookmark, &pathTracer_->settings());
    }
    if (requests.loadCameraBookmarkIndex.has_value()) {
        const auto& bookmarks = uiOverlay_->editor().cameraBookmarks().bookmarks();
        const size_t index = *requests.loadCameraBookmarkIndex;
        if (index < bookmarks.size()) {
            RendererSettings settings = pathTracer_->settings();
            uiOverlay_->editor().cameraBookmarks().loadBookmark(
                bookmarks[index], cameraController_, *pathTracer_, &settings);
            applyRendererSettingsSafely(settings, true);
        }
    }
    if (requests.deleteCameraBookmarkIndex.has_value()) {
        uiOverlay_->editor().cameraBookmarks().deleteBookmark(*requests.deleteCameraBookmarkIndex);
    }
    if (requests.removeFavorite.has_value()) {
        EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
        prefs.removeFavorite(*requests.removeFavorite);
        (void)saveActiveEditorPreferences();
    }

    (void)applyPendingSceneUpdate(allowResourceRebuild);

    if (requests.reloadShaders) {
        reloadShadersFromEditor();
    }

    const std::optional<std::filesystem::path>& importScenePath = requests.importSceneAsNewScene.has_value()
        ? requests.importSceneAsNewScene
        : requests.loadGltf;
    if (importScenePath.has_value()) {
        if (!confirmDestructiveSceneAction("importing a scene as a new scene")) {
            std::cout << "Import Scene as New Scene cancelled: " << importScenePath->string() << '\n';
        } else {
            SceneLoadRequest request;
            request.mode = SceneLoadMode::ImportSceneAsNewScene;
            request.sourcePath = *importScenePath;
            if (project_.has_value()) {
                request.projectSnapshot = *project_;
            }
            (void)requestSceneLoad(std::move(request));
        }
    }

    for (const EditorImportAssetRequest& importRequest : requests.importAssets) {
        (void)queueAssetImportNonMutating(importRequest, false);
    }

    if (requests.importAsset.has_value()) {
        (void)queueAssetImportNonMutating(*requests.importAsset, false);
    }

    for (EditorImportAssetRequest importRequest : requests.importAndPlaceAssets) {
        if (importRequest.mode.empty()) {
            importRequest.mode = "ImportAndPlace";
        }
        (void)queueAssetImportNonMutating(importRequest, true);
    }

    if (requests.importAndPlace.has_value()) {
        EditorImportAssetRequest importRequest = *requests.importAndPlace;
        if (importRequest.mode.empty()) {
            importRequest.mode = "ImportAndPlace";
        }
        (void)queueAssetImportNonMutating(importRequest, true);
    }

    if (requests.reimportAsset.has_value()) {
        (void)queueAssetReimport(*requests.reimportAsset);
    }

    if (requests.relinkAssetSource.has_value()) {
        (void)relinkAssetSource(*requests.relinkAssetSource);
    }

    if (requests.replaceAssetReferences.has_value()) {
        (void)replaceAssetReferences(*requests.replaceAssetReferences, allowResourceRebuild);
    }

    if (requests.repairMissingAssetDependencies.has_value()) {
        (void)repairMissingAssetDependencies(*requests.repairMissingAssetDependencies);
    }

    if (requests.updateAssetTags.has_value()) {
        (void)updateAssetTags(*requests.updateAssetTags);
    }

    if (requests.renameAsset.has_value()) {
        (void)renameAssetRecord(*requests.renameAsset);
    }

    if (requests.bulkAddAssetTag.has_value()) {
        (void)bulkAddAssetTag(*requests.bulkAddAssetTag);
    }

    if (requests.bulkRemoveAssetTag.has_value()) {
        (void)bulkRemoveAssetTag(*requests.bulkRemoveAssetTag);
    }

    if (requests.moveAssetsToFolder.has_value()) {
        (void)moveAssetsToFolder(*requests.moveAssetsToFolder);
    }

    if (requests.deleteAssets.has_value()) {
        (void)deleteAssetsFromRegistry(*requests.deleteAssets);
    }

    if (requests.cookProject.has_value()) {
        (void)startCookProject(*requests.cookProject);
    }

    if (requests.placeAsset.has_value()) {
        (void)placePrefabAsset(*requests.placeAsset, requests.placeAssetTransform);
    }

    if (requests.mergeScene.has_value()) {
        SceneLoadRequest request;
        request.mode = SceneLoadMode::MergeSceneIntoCurrent;
        request.sourcePath = *requests.mergeScene;
        if (project_.has_value()) {
            request.projectSnapshot = *project_;
        }
        (void)requestSceneLoad(std::move(request));
    }

    if (!requests.mergeScenes.empty()) {
        queueMergeScenes(requests.mergeScenes);
    }

    if (requests.exit && window_ != nullptr && confirmDestructiveSceneAction("exiting")) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

void Application::applyValidationCameraMotion(uint32_t frameIndex) {
    if (!validationCameraMotion_ || pathTracer_ == nullptr) {
        return;
    }
    const float angle = static_cast<float>(frameIndex) * 0.035f;
    const float radius = 4.2f;
    const glm::vec3 target{0.0f, 0.55f, 0.0f};
    const glm::vec3 position{
        std::sin(angle) * radius,
        0.75f + std::sin(angle * 0.37f) * 0.25f,
        std::cos(angle) * radius};
    cameraController_.setPose(position, glm::normalize(target - position), *pathTracer_);
}

void Application::applyValidationObjectMotion(uint32_t frameIndex) {
    if (!validationObjectMotion_ || pathTracer_ == nullptr) {
        return;
    }

    Entity* entity = validationObjectMotionEntity_.valid()
        ? sceneDocument_.registry().entity(validationObjectMotionEntity_)
        : nullptr;
    if (entity == nullptr || !entity->meshRenderer.has_value()) {
        validationObjectMotionEntity_ = {};
        for (Entity* candidate : sceneDocument_.registry().entities()) {
            if (candidate != nullptr && candidate->meshRenderer.has_value()) {
                validationObjectMotionEntity_ = candidate->id;
                validationObjectMotionBaseTransform_ = candidate->transform;
                entity = candidate;
                break;
            }
        }
    }
    if (entity == nullptr || !entity->meshRenderer.has_value()) {
        return;
    }

    constexpr float kAmplitude = 0.65f;
    constexpr float kAngularStep = 0.42f;
    const float phase = static_cast<float>(frameIndex) * kAngularStep;
    entity->transform = validationObjectMotionBaseTransform_;
    entity->transform.position.x += std::sin(phase) * kAmplitude;
    entity->transform.rotationEuler.y += phase * 0.35f;
    entity->transform.dirty = true;
    const RendererSettings currentSettings = pathTracer_->settings();
    RenderSettings& documentSettings = sceneDocument_.renderSettings();
    documentSettings.motionBlurEnabled = currentSettings.motionBlurEnabled;
    documentSettings.motionBlurShutterOpen = currentSettings.motionBlurShutterOpen;
    documentSettings.motionBlurShutterClose = currentSettings.motionBlurShutterClose;
    sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
    (void)applyPendingSceneUpdate(false);
}

void Application::beginSunDragArm(bool dragEligible) {
    if (window_ == nullptr || sunDrag_.phase != SunDragPhase::Idle) {
        return;
    }
    glfwGetCursorPos(window_, &sunDrag_.startMouseX, &sunDrag_.startMouseY);
    sunDrag_.lastMouseX = sunDrag_.startMouseX;
    sunDrag_.lastMouseY = sunDrag_.startMouseY;
    sunDrag_.armedTimeSeconds = glfwGetTime();
    sunDrag_.dragEligible = dragEligible;
    sunDrag_.suppressOpenLevel = false;
    if (pathTracer_ != nullptr) {
        const RendererSettings settings = pathTracer_->settings();
        sunDrag_.elevation = settings.sunElevation;
        sunDrag_.azimuth = settings.sunAzimuth;
    }
    sunDrag_.phase = SunDragPhase::Armed;
}

void Application::startSunDrag(double mouseX, double mouseY) {
    if (window_ == nullptr || pathTracer_ == nullptr || sunDrag_.phase != SunDragPhase::Armed) {
        return;
    }

    cameraController_.releaseMouse(window_);
    sunDrag_.beforeDocument = sceneDocument_;
    sunDrag_.entity = SunController::ensurePrimarySun(sceneDocument_);

    Entity* sun = sceneDocument_.registry().entity(sunDrag_.entity);
    if (sun == nullptr || !sun->sun.has_value()) {
        finishSunDrag(true);
        return;
    }

    sunDrag_.originalTransform = sun->transform;
    sunDrag_.elevation = sun->sun->elevation;
    sunDrag_.azimuth = sun->sun->azimuth;
    sunDrag_.lastMouseX = mouseX;
    sunDrag_.lastMouseY = mouseY;
    sunDrag_.previousCursorMode = glfwGetInputMode(window_, GLFW_CURSOR);
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    sunDrag_.phase = SunDragPhase::Dragging;
}

void Application::updateSunDrag(double mouseX, double mouseY) {
    if (sunDrag_.phase != SunDragPhase::Dragging) {
        return;
    }
    Entity* sun = sceneDocument_.registry().entity(sunDrag_.entity);
    if (sun == nullptr || !sun->sun.has_value()) {
        finishSunDrag(true);
        return;
    }

    constexpr float sensitivity = 0.0035f;
    const double dx = mouseX - sunDrag_.lastMouseX;
    const double dy = mouseY - sunDrag_.lastMouseY;
    sunDrag_.lastMouseX = mouseX;
    sunDrag_.lastMouseY = mouseY;
    if (dx == 0.0 && dy == 0.0) {
        return;
    }

    sunDrag_.azimuth -= static_cast<float>(dx) * sensitivity;
    sunDrag_.elevation = std::clamp(
        sunDrag_.elevation - static_cast<float>(dy) * sensitivity,
        -0.20f,
        1.45f);
    sun->sun->azimuth = sunDrag_.azimuth;
    sun->sun->elevation = sunDrag_.elevation;
    sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
    (void)applyPendingSceneUpdate(false);
}

void Application::finishSunDrag(bool cancel) {
    if (sunDrag_.phase == SunDragPhase::Idle) {
        return;
    }
    const SunDragPhase phase = sunDrag_.phase;
    if (phase == SunDragPhase::Dragging && window_ != nullptr) {
        glfwSetInputMode(window_, GLFW_CURSOR, sunDrag_.previousCursorMode);
    }

    if (phase == SunDragPhase::Dragging) {
        if (cancel && sunDrag_.beforeDocument.has_value()) {
            sceneDocument_ = *sunDrag_.beforeDocument;
            sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
            (void)applyPendingSceneUpdate(false);
        } else if (sunDrag_.beforeDocument.has_value()) {
            SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
            sceneOps.setUndoStack(&undoStack_);
            sceneOps.commitSunDrag(std::move(*sunDrag_.beforeDocument), SceneUpdateKind::LightOnly);
        }
    }

    sunDrag_ = SunDragState{};
}

void Application::processSunDragControls(bool shortcutsBlocked, bool viewportHovered, bool viewportInteraction, bool ctrlDown) {
    if (window_ == nullptr || pathTracer_ == nullptr) {
        return;
    }
    const bool lDown = glfwGetKey(window_, GLFW_KEY_L) == GLFW_PRESS;
    const bool escapeDown = glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    const bool focused = glfwGetWindowAttrib(window_, GLFW_FOCUSED) == GLFW_TRUE;

    if (sunDrag_.phase == SunDragPhase::Idle) {
        if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_L)) {
            beginSunDragArm(viewportHovered || viewportInteraction);
        }
        return;
    }

    if (sunDrag_.phase == SunDragPhase::Armed) {
        if (escapeDown || !focused) {
            finishSunDrag(true);
            return;
        }
        double mouseX = 0.0;
        double mouseY = 0.0;
        glfwGetCursorPos(window_, &mouseX, &mouseY);
        constexpr double dragThresholdPixels = 4.0;
        const double dx = mouseX - sunDrag_.startMouseX;
        const double dy = mouseY - sunDrag_.startMouseY;
        const double dragDistanceSq = dx * dx + dy * dy;
        if (!ctrlDown || !lDown) {
            constexpr double openLevelTapSeconds = 0.25;
            const bool quickTap = (glfwGetTime() - sunDrag_.armedTimeSeconds) <= openLevelTapSeconds &&
                                  dragDistanceSq < dragThresholdPixels * dragThresholdPixels;
            if (!sunDrag_.suppressOpenLevel && quickTap) {
                pendingOpenLevel_ = true;
            }
            finishSunDrag(false);
            return;
        }
        if (dragDistanceSq >= dragThresholdPixels * dragThresholdPixels) {
            if (sunDrag_.dragEligible) {
                startSunDrag(mouseX, mouseY);
            } else {
                sunDrag_.suppressOpenLevel = true;
            }
        }
        return;
    }

    if (sunDrag_.phase == SunDragPhase::Dragging) {
        if (escapeDown || !focused) {
            finishSunDrag(true);
            return;
        }
        if (!ctrlDown || !lDown) {
            finishSunDrag(false);
            return;
        }
        double mouseX = 0.0;
        double mouseY = 0.0;
        glfwGetCursorPos(window_, &mouseX, &mouseY);
        updateSunDrag(mouseX, mouseY);
    }
}

bool Application::applyPendingSceneUpdate(bool allowResourceRebuild) {
    if (!pathTracer_ || !sceneDocument_.dirty()) {
        return false;
    }

    const auto applyStart = std::chrono::steady_clock::now();
    const SceneUpdateMask pendingMask = sceneDocument_.pendingUpdateMask();
    SceneUpdateRoute route = SceneUpdateRouter::route(pendingMask);
    const std::string routeKindName = sceneUpdateMaskName(pendingMask);
    const std::string routeActionName = sceneUpdateGpuActionMaskName(route.actionMask);
    auto recordRouteAndReturn = [&](bool result, std::string_view suffix = {}) {
        const auto applyEnd = std::chrono::steady_clock::now();
        const double cpuMs = std::chrono::duration<double, std::milli>(applyEnd - applyStart).count();
        std::string actionName = routeActionName;
        if (!suffix.empty()) {
            actionName += suffix;
        }
        if (pathTracer_ != nullptr) {
            pathTracer_->validationLog().recordSceneUpdateRoute(routeKindName, std::move(actionName), cpuMs);
        }
        return result;
    };
    if (route.actionMask == 0u) {
        sceneDocument_.clearDirty();
        return recordRouteAndReturn(true);
    }
    if (!allowResourceRebuild &&
        route.requiresRendererRebuild) {
        return recordRouteAndReturn(false, "+Deferred");
    }

    std::optional<SceneGpuBuildResult> build;
    auto ensureBuild = [&]() -> SceneGpuBuildResult& {
        if (!build.has_value()) {
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            build = sceneBuilder_.build(sceneDocument_, &assets_, pathTracer_->settings());
        }
        return *build;
    };
    if (route.requiresGpuSceneBuild && !route.requiresRendererRebuild) {
        applyRendererSettingsSafely(ensureBuild().rendererSettings, allowResourceRebuild);
    }

    auto rebuildRenderer = [&]() {
        SceneGpuBuildResult& sceneBuild = ensureBuild();
        const RendererSettings previousSettings = pathTracer_->settings();
        RendererSettings replacementSettings = sceneBuild.rendererSettings;
        if (disableDlssForRendererReplacement(replacementSettings)) {
            syncDocumentRenderSettings(sceneDocument_, replacementSettings);
            const std::string message = "DLSS disabled for topology rebuild; re-enable it after placement completes.";
            std::cerr << message << '\n';
            notifications_.notify(message, NotificationType::Warning, NotificationAction::OpenRenderSettings, "Render Settings", 6.0f);
        }
        if (uiOverlay_) {
            uiOverlay_->invalidateRendererTextures();
        }
        gpuSceneAsset_ = sceneBuild.sceneAsset;
        gpuInstanceEntities_ = sceneBuild.instanceEntities;
        rebuildGpuSceneAsset();
        preparePathTracerForRendererReplacement(previousSettings);
        std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
            currentSceneCachePathForRenderer(),
            &replacementSettings);
        retirePathTracer(std::move(pathTracer_));
        pathTracer_ = std::move(nextPathTracer);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(route.resetReason);
        commandSystem_->setPathTracer(pathTracer_.get());
    };

    auto syncDerivedSceneSettings = [&]() {
        applySceneWorldComponentsToDocumentSettings(sceneDocument_);
    };

    auto syncBuiltScene = [&]() {
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
    };

    auto completeAfterRebuild = [&]() {
        sceneDocument_.clearDirty();
        notifications_.notify("Scene topology rebuilt", NotificationType::Info);
        return true;
    };

    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::RebuildTopology)) {
        if (!allowResourceRebuild) {
            return recordRouteAndReturn(false, "+Deferred");
        }
        rebuildRenderer();
        return recordRouteAndReturn(completeAfterRebuild());
    }

    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateCamera)) {
        applyActiveSceneCamera();
    }
    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateLights)) {
        syncBuiltScene();
        applyRendererSettingsSafely(ensureBuild().rendererSettings, allowResourceRebuild);
        if (!pathTracer_->updateSceneLights(*gpuSceneAsset_)) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
    }
    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateMaterials)) {
        syncBuiltScene();
        if (!pathTracer_->updateMaterials(*gpuSceneAsset_, assets_)) {
            if (!allowResourceRebuild) {
                return recordRouteAndReturn(false, "+Deferred");
            }
            rebuildRenderer();
            return recordRouteAndReturn(completeAfterRebuild(), "+FallbackRebuild");
        }
    }
    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateEnvironment)) {
        syncBuiltScene();
        std::filesystem::path environmentPath = sceneDocument_.environment().hdrPath;
        if (environmentPath.empty() && sceneDocument_.sourceHdrPath().has_value()) {
            environmentPath = *sceneDocument_.sourceHdrPath();
        }
        if (!environmentPath.empty() && !environmentPath.is_absolute()) {
            environmentPath = project_.has_value() ? project_->projectRoot / environmentPath : std::filesystem::current_path() / environmentPath;
        }
        std::error_code environmentPathError;
        if (!environmentPath.empty() &&
            std::filesystem::is_regular_file(environmentPath, environmentPathError) &&
            (!hdrPath_.has_value() || *hdrPath_ != environmentPath)) {
            try {
                pathTracer_->loadEnvironment(environmentPath);
                hdrPath_ = environmentPath;
            } catch (const std::exception& error) {
                std::cerr << "Environment update load failed: " << environmentPath.string() << " " << error.what() << '\n';
            }
        }
        applyRendererSettingsSafely(rendererSettingsFromDocument(sceneDocument_, pathTracer_->settings()), allowResourceRebuild);
        if (route.resetsAccumulation) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
    }
    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::ApplyRendererSettings)) {
        syncDerivedSceneSettings();
        applyRendererSettingsSafely(rendererSettingsFromDocument(sceneDocument_, pathTracer_->settings()), allowResourceRebuild);
        if (route.resetsAccumulation) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
    }
    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateVisibility)) {
        syncBuiltScene();
        if (!pathTracer_->updateSceneVisibility(*gpuSceneAsset_, assets_)) {
            if (!allowResourceRebuild) {
                return recordRouteAndReturn(false, "+Deferred");
            }
            rebuildRenderer();
            return recordRouteAndReturn(completeAfterRebuild(), "+FallbackRebuild");
        }
    }
    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateTransforms)) {
        syncBuiltScene();
        if (!pathTracer_->updateSceneTransforms(*gpuSceneAsset_, assets_)) {
            if (!allowResourceRebuild) {
                return recordRouteAndReturn(false, "+Deferred");
            }
            rebuildRenderer();
            return recordRouteAndReturn(completeAfterRebuild(), "+FallbackRebuild");
        }
    }

    sceneDocument_.clearDirty();
    return recordRouteAndReturn(true);
}

void Application::applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return;
    }

    const RendererSettings current = pathTracer_->settings();
    const bool renderResolutionChanged =
        std::abs(settings.renderResolutionScale - current.renderResolutionScale) > 0.0001f;
    if (!renderResolutionChanged || allowRenderResolutionChange) {
        if (pathTracer_->applySettings(settings)) {
            syncDocumentRenderSettings(sceneDocument_, settings);
        }
        return;
    }

    RendererSettings immediate = settings;
    immediate.renderResolutionScale = current.renderResolutionScale;
    if (pathTracer_->applySettings(immediate)) {
        syncDocumentRenderSettings(sceneDocument_, settings);
    }
    pendingPostFrameSettings_ = settings;
}

void Application::reloadShadersFromEditor() {
    if (!pathTracer_ || !commandSystem_) {
        return;
    }
    const RendererSettings previousSettings = pathTracer_->settings();
    preparePathTracerForRendererReplacement(previousSettings);
    std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
        gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
        gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
        currentSceneCachePathForRenderer(),
        &previousSettings);
    if (uiOverlay_) {
        uiOverlay_->invalidateRendererTextures();
        uiOverlay_->editor().invalidateAssetThumbnails();
    }
    retirePathTracer(std::move(pathTracer_));
    pathTracer_ = std::move(nextPathTracer);
    applyActiveSceneCamera();
    pathTracer_->resetAccumulation(AccumulationResetReason::ShaderReloaded);
    commandSystem_->setPathTracer(pathTracer_.get());
    notifications_.notify("Shaders reloaded", NotificationType::Success);
    std::cout << "Reloaded shaders from editor.\n";
}

void Application::preparePathTracerForRendererReplacement(const RendererSettings& previousSettings) {
    if (pathTracer_ == nullptr || !rendererSettingsRequestDlss(previousSettings)) {
        return;
    }
    if (commandSystem_ != nullptr) {
        commandSystem_->waitIdle();
    }
    pathTracer_->releaseExclusiveRuntimeForRendererReplacement();
}

void Application::retirePathTracer(std::unique_ptr<PathTracerRenderer> renderer) {
    if (renderer == nullptr) {
        return;
    }
    retiredPathTracers_.push_back(RetiredPathTracer{
        .renderer = std::move(renderer),
        .releaseFrame = frameSerial_ + CommandSystem::framesInFlight + 1u,
    });
}

void Application::releaseRetiredPathTracers() {
    retiredPathTracers_.erase(
        std::remove_if(
            retiredPathTracers_.begin(),
            retiredPathTracers_.end(),
            [this](const RetiredPathTracer& retired) {
                return frameSerial_ >= retired.releaseFrame;
            }),
        retiredPathTracers_.end());
}

std::optional<std::filesystem::path> Application::currentSceneCachePathForRenderer() const {
    if (!gltfPath_.has_value() || !gpuSceneAsset_.has_value() || gpuSceneAsset_->meshes.empty()) {
        return std::nullopt;
    }
    if (sceneUnsavedDirty_ || !sceneDocument_.prefabInstances().empty()) {
        return std::nullopt;
    }
    if (!importedScene_.has_value() ||
        gpuSceneAsset_->meshes.size() != importedScene_->meshes.size() ||
        gpuSceneAsset_->materials.size() != importedScene_->materials.size() ||
        gpuSceneAsset_->textures.size() != importedScene_->textures.size() ||
        gpuSceneAsset_->nodes.size() != importedScene_->nodes.size()) {
        return std::nullopt;
    }
    return SceneCache::cachePathFor(*gltfPath_);
}

std::unique_ptr<PathTracerRenderer> Application::makePathTracer(
    const SceneAsset* sceneAsset,
    const AssetManager* assets,
    std::optional<std::filesystem::path> sceneCachePath,
    const RendererSettings* settingsToRestore) {
    const auto projectRoot = resolveProjectRoot();
    const auto shaderDir = projectRoot / "native" / "vulkan" / "shaders";
    const auto shaderOutDir = projectRoot / "native" / "vulkan" / "build" / "shaders";
    auto renderer = std::make_unique<PathTracerRenderer>(
        *context_,
        *allocator_,
        *uploader_,
        swapchain_->format(),
        shaderDir,
        shaderOutDir,
        debugView_,
        sceneAsset,
        sceneAsset != nullptr ? assets : nullptr,
        hdrPath_,
        std::move(sceneCachePath),
        !disableResourceAliasing_,
        settingsToRestore);
    if (settingsToRestore != nullptr) {
        renderer->applySettings(*settingsToRestore);
    }
    return renderer;
}

void Application::createPathTracer(const RendererSettings* settingsToRestore) {
    const SceneAsset* sceneAsset = gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr;
    pathTracer_ = makePathTracer(sceneAsset, sceneAsset != nullptr ? &assets_ : nullptr, currentSceneCachePathForRenderer(), settingsToRestore);
}

void Application::initializeRendererFromCurrentScene(const RendererSettings* settingsToRestore) {
    if (pathTracer_ != nullptr) {
        return;
    }

    rebuildGpuSceneAsset();
    RendererSettings startupSettings = settingsToRestore != nullptr ? *settingsToRestore : RendererSettings{};
    if (settingsToRestore == nullptr) {
        startupSettings.debugView = debugView_;
        startupSettings = rendererSettingsFromDocument(sceneDocument_, startupSettings);
    }
    createPathTracer(&startupSettings);
    syncDocumentRenderSettings(sceneDocument_, pathTracer_->settings());
    applyActiveSceneCamera();
    sceneDocument_.clearDirty();
    if (commandSystem_ != nullptr) {
        commandSystem_->setPathTracer(pathTracer_.get());
    }
    showMainWindowIfHidden();
    if (uiOverlay_ != nullptr) {
        uiOverlay_->invalidateViewportTexture();
    }
}

void Application::applyActiveSceneCamera() {
    if (pathTracer_ == nullptr || !sceneDocument_.activeCamera().valid()) {
        return;
    }

    const Entity* cameraEntity = sceneDocument_.registry().entity(sceneDocument_.activeCamera());
    if (cameraEntity == nullptr || !cameraEntity->camera.has_value()) {
        return;
    }

    const glm::mat4 transform = entityWorldMatrix(sceneDocument_.registry(), *cameraEntity);
    const glm::vec3 position = glm::vec3(transform[3]);
    glm::vec3 forward = glm::mat3(transform) * glm::vec3(0.0f, 0.0f, -1.0f);
    if (glm::dot(forward, forward) <= 0.0f) {
        forward = glm::vec3(0.0f, 0.0f, -1.0f);
    }
    pathTracer_->setCameraProjection(
        cameraEntity->camera->projection,
        cameraEntity->camera->verticalFovRadians,
        cameraEntity->camera->aspectRatio,
        cameraEntity->camera->orthographicXmag,
        cameraEntity->camera->orthographicYmag,
        cameraEntity->camera->nearPlane,
        cameraEntity->camera->farPlane);
    cameraController_.setPose(position, forward, *pathTracer_);
}

void Application::syncActiveSceneCameraFromController() {
    const EntityId active = sceneDocument_.activeCamera();
    if (!active.valid()) {
        return;
    }
    Entity* cameraEntity = sceneDocument_.registry().entity(active);
    if (cameraEntity == nullptr || !cameraEntity->camera.has_value()) {
        return;
    }

    glm::vec3 localPosition = cameraController_.position();
    glm::vec3 localForward = cameraController_.direction();
    if (cameraEntity->parent.valid()) {
        if (const Entity* parent = sceneDocument_.registry().entity(cameraEntity->parent)) {
            const glm::mat4 invParent = glm::inverse(entityWorldMatrix(sceneDocument_.registry(), *parent));
            localPosition = glm::vec3(invParent * glm::vec4(localPosition, 1.0f));
            localForward = glm::normalize(glm::mat3(invParent) * localForward);
        }
    }

    const glm::vec3 zAxis = -glm::normalize(localForward);
    glm::vec3 xAxis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), zAxis);
    if (glm::dot(xAxis, xAxis) <= 1.0e-6f) {
        xAxis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), zAxis);
    }
    xAxis = glm::normalize(xAxis);
    const glm::vec3 yAxis = glm::normalize(glm::cross(zAxis, xAxis));
    const glm::mat3 rotation(xAxis, yAxis, zAxis);
    cameraEntity->transform.position = localPosition;
    cameraEntity->transform.rotationEuler = glm::eulerAngles(glm::normalize(glm::quat_cast(rotation)));
    cameraEntity->transform.dirty = true;
    sceneDocument_.markDirty(SceneUpdateKind::CameraOnly);
}

void Application::rebuildGpuSceneAsset() {
    const RendererSettings settings = pathTracer_ != nullptr ? pathTracer_->settings() : RendererSettings{};
    (void)SunController::migrateLegacyDirectionalSun(sceneDocument_);
    (void)SunController::repairPrimarySunTransform(sceneDocument_);
    applyDocumentMaterialAssignments(sceneDocument_, assets_);
    SceneGpuBuildResult build = sceneBuilder_.build(sceneDocument_, &assets_, settings);
    gpuSceneAsset_ = std::move(build.sceneAsset);
    gpuInstanceEntities_ = std::move(build.instanceEntities);
}

void Application::initializeFallbackSceneDocument() {
    sceneDocument_ = SceneDocument{};
    EntityId camera = sceneDocument_.registry().createEntity("Camera");
    Camera cameraComponent;
    cameraComponent.active = true;
    sceneDocument_.registry().addCamera(camera, cameraComponent);
    sceneDocument_.setActiveCamera(camera);
    (void)sceneDocument_.registry().createEntity("Cornell Fallback");
    EntityId sun = sceneDocument_.registry().createEntity("Sun");
    if (Entity* sunEntity = sceneDocument_.registry().entity(sun)) {
        sunEntity->sun = Sun{};
        sunEntity->sun->elevation = 0.97f;
        sunEntity->sun->azimuth = glm::pi<float>();
        sunEntity->defaultTransform = sunEntity->transform;
    }
    sceneDocument_.setPrimarySun(sun);
    sceneDocument_.clearDirty();
    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
}

void Application::initializeProjectManagerStartupSceneDocument() {
    sceneDocument_ = SceneDocument{};
    EntityId camera = sceneDocument_.registry().createEntity("Project Manager Camera");
    Camera cameraComponent;
    cameraComponent.active = true;
    sceneDocument_.registry().addCamera(camera, cameraComponent);
    sceneDocument_.setActiveCamera(camera);
    sceneDocument_.clearDirty();
    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
}

void Application::processRuntimeControls(float deltaSeconds) {
    if (!pathTracer_) {
        return;
    }

    const bool uiWantsTextInput = uiOverlay_ != nullptr && uiOverlay_->wantsTextInput();
    const bool shortcutsBlocked = uiWantsTextInput;
    const bool viewportInteraction = uiOverlay_ != nullptr && uiOverlay_->viewportInteractionActive();
    const bool viewportHovered = uiOverlay_ != nullptr && uiOverlay_->viewportHovered();
    const bool cameraCaptured = cameraController_.mouseCaptured();
    const bool ctrlDown =
        glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    const bool shiftDown =
        glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    const bool altDown =
        glfwGetKey(window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    processSunDragControls(shortcutsBlocked, viewportHovered, viewportInteraction, ctrlDown);
    const bool sunDragCapturing = sunDrag_.phase != SunDragPhase::Idle;
    const bool cameraMoved = cameraController_.update(
        window_,
        deltaSeconds,
        *pathTracer_,
        !sunDragCapturing && (viewportHovered || cameraCaptured),
        !sunDragCapturing && (viewportInteraction || cameraCaptured) && !uiWantsTextInput);
    if (cameraMoved) {
        syncActiveSceneCameraFromController();
    }

    RendererSettings settings = pathTracer_->settings();
    bool changed = false;
    const EditorPreferences* editorPrefs = uiOverlay_ != nullptr ? &uiOverlay_->editor().editorPrefs() : nullptr;
    auto commandPressed = [&](EditorCommandId id) {
        const EditorKeybinding binding = editorCommandKeybinding(id, editorPrefs);
        if (shortcutsBlocked || binding.glfwKey < 0) {
            return false;
        }
        if (binding.ctrl != ctrlDown || binding.shift != shiftDown || binding.alt != altDown) {
            return false;
        }
        return pressedOnce(binding.glfwKey);
    };

    if (commandPressed(EditorCommandId::Undo)) {
        pendingUndo_ = true;
    }
    if (commandPressed(EditorCommandId::Redo)) {
        pendingRedo_ = true;
    }
    if (commandPressed(EditorCommandId::CycleDebugView)) {
        settings.debugView = nextDebugView(settings.debugView);
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugBeauty)) {
        settings.debugView = RendererDebugView::Beauty;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugDirectLighting)) {
        settings.debugView = RendererDebugView::DirectLighting;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugIndirectLighting)) {
        settings.debugView = RendererDebugView::IndirectLighting;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugNormals)) {
        settings.debugView = RendererDebugView::Normals;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugDepth)) {
        settings.debugView = RendererDebugView::Depth;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugMotionVectors)) {
        settings.debugView = RendererDebugView::MotionVectors;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugVariance)) {
        settings.debugView = RendererDebugView::Variance;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetDebugAlbedo)) {
        settings.debugView = RendererDebugView::Albedo;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetToneMapperLinear)) {
        settings.toneMapper = ToneMapper::Linear;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetToneMapperReinhard)) {
        settings.toneMapper = ToneMapper::Reinhard;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetToneMapperAces)) {
        settings.toneMapper = ToneMapper::ACES;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetToneMapperPbrNeutral)) {
        settings.toneMapper = ToneMapper::PBRNeutral;
        changed = true;
    }
    if (commandPressed(EditorCommandId::SetToneMapperAgx)) {
        settings.toneMapper = ToneMapper::AgX;
        changed = true;
    }
    if (commandPressed(EditorCommandId::ToggleAutoExposure)) {
        settings.autoExposureEnabled = !settings.autoExposureEnabled;
        changed = true;
    }
    if (commandPressed(EditorCommandId::ToggleFullscreen)) {
        toggleBorderlessFullscreen();
    }
    if (commandPressed(EditorCommandId::ToggleDenoiser)) {
        settings.denoiserEnabled = !settings.denoiserEnabled;
        changed = true;
    }
    if (commandPressed(EditorCommandId::ToggleMovingDenoiser)) {
        settings.denoiseWhileMoving = !settings.denoiseWhileMoving;
        changed = true;
    }
    if (commandPressed(EditorCommandId::ToggleSun)) {
        const bool hadPrimarySun = SunController::primarySunEntity(sceneDocument_).valid();
        EntityId sunId = SunController::ensurePrimarySun(sceneDocument_);
        if (Entity* sun = sceneDocument_.registry().entity(sunId); sun != nullptr && sun->sun.has_value()) {
            sun->sun->enabled = hadPrimarySun ? !sun->sun->enabled : true;
            sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
            (void)applyPendingSceneUpdate(false);
            settings = pathTracer_->settings();
        }
    }
    if (commandPressed(EditorCommandId::ToggleEnvironment)) {
        settings.environmentEnabled = !settings.environmentEnabled;
        changed = true;
    }
    if (commandPressed(EditorCommandId::ToggleDirectLighting)) {
        settings.directLightingEnabled = !settings.directLightingEnabled;
        changed = true;
    }
    if (commandPressed(EditorCommandId::CycleIntermediateView)) {
        constexpr int count = sizeof(intermediateViews) / sizeof(intermediateViews[0]);
        int idx = 0;
        for (int i = 0; i < count; ++i) {
            if (intermediateViews[i] == settings.debugView) { idx = (i + 1) % count; break; }
        }
        settings.debugView = intermediateViews[idx];
        changed = true;
    }
    if (commandPressed(EditorCommandId::ReloadShaders)) {
        pendingReloadShaders_ = true;
    }
    if (commandPressed(EditorCommandId::SaveScene)) {
        pendingSaveLevel_ = true;
    }
    if (commandPressed(EditorCommandId::SaveAll)) {
        pendingSaveAll_ = true;
    }
    if (commandPressed(EditorCommandId::OpenScene)) {
        pendingOpenLevel_ = true;
    }
    if (commandPressed(EditorCommandId::ResetAccumulation) && !viewportInteraction) {
        pathTracer_->resetAccumulation();
    }

    const float exposureRate = 0.9f * deltaSeconds;
    if (!shortcutsBlocked && (glfwGetKey(window_, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_ADD) == GLFW_PRESS)) {
        settings.exposure += exposureRate;
        changed = true;
    }
    if (!shortcutsBlocked && (glfwGetKey(window_, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS)) {
        settings.exposure = std::max(0.05f, settings.exposure - exposureRate);
        changed = true;
    }

    const float envRate = 1.2f * deltaSeconds;
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_PERIOD) == GLFW_PRESS) {
        settings.environmentIntensity += envRate;
        changed = true;
    }
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_COMMA) == GLFW_PRESS) {
        settings.environmentIntensity = std::max(0.0f, settings.environmentIntensity - envRate);
        changed = true;
    }
    const float rotationRate = 1.4f * deltaSeconds;
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation += rotationRate;
        changed = true;
    }
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation -= rotationRate;
        changed = true;
    }

    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_PAGE_UP)) {
        ++settings.maxBounces;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_PAGE_DOWN) && settings.maxBounces > 1) {
        --settings.maxBounces;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_HOME)) {
        ++settings.atrousIterations;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_END) && settings.atrousIterations > 1) {
        --settings.atrousIterations;
        changed = true;
    }

    if (changed) {
        applyRendererSettingsSafely(settings, false);
        settings = pathTracer_->settings();
        std::cout << "Settings changed: debug=" << rendererDebugViewName(settings.debugView)
                  << " tone=" << toneMapperName(settings.toneMapper)
                  << " autoExposure=" << (settings.autoExposureEnabled ? "on" : "off")
                  << " denoiser=" << (settings.denoiserEnabled ? "on" : "off")
                  << " sun=" << (settings.sunlightEnabled ? "on" : "off")
                  << " env=" << (settings.environmentEnabled ? "on" : "off")
                  << " bounces=" << settings.maxBounces << '\n';
    }
}

void Application::updateWindowTitle(float seconds) {
    if (!pathTracer_ || seconds - lastTitleUpdateSeconds_ < 0.25f) {
        return;
    }
    lastTitleUpdateSeconds_ = seconds;

    std::filesystem::path activeScenePath;
    if (scenePath_.has_value()) {
        activeScenePath = *scenePath_;
    } else if (gltfPath_.has_value()) {
        activeScenePath = *gltfPath_;
    }

    std::ostringstream title;
    title << (activeScenePath.empty() ? "Untitled Scene" : activeScenePath.stem().string());
    if (sceneUnsavedDirty_ || sceneDocument_.dirty()) {
        title << "*";
    }
    if (assetRegistry_.dirty()) {
        title << " [Registry*]";
    }
    if (projectSettingsDirty_) {
        title << " [Project*]";
    }
    title << " - Vibode Engine";
    glfwSetWindowTitle(window_, title.str().c_str());
}

void Application::showMainWindowIfHidden() {
    if (!mainWindowHiddenUntilRenderer_ || window_ == nullptr) {
        return;
    }
    glfwShowWindow(window_);
    mainWindowHiddenUntilRenderer_ = false;
}

void Application::toggleBorderlessFullscreen() {
    if (window_ == nullptr) {
        return;
    }

    cameraController_.releaseMouse(window_);
    if (!borderlessFullscreen_) {
        glfwGetWindowPos(window_, &windowedX_, &windowedY_);
        glfwGetWindowSize(window_, &windowedWidth_, &windowedHeight_);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
        if (monitor == nullptr || mode == nullptr) {
            return;
        }

        borderlessFullscreen_ = true;
        glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        borderlessFullscreen_ = false;
        glfwSetWindowMonitor(window_, nullptr, windowedX_, windowedY_, windowedWidth_, windowedHeight_, 0);
    }
}

bool Application::pressedOnce(int key) {
    if (key < 0 || static_cast<size_t>(key) >= keyState_.size()) {
        return false;
    }
    const bool down = glfwGetKey(window_, key) == GLFW_PRESS;
    const bool wasDown = keyState_[static_cast<size_t>(key)] != 0;
    keyState_[static_cast<size_t>(key)] = down ? 1u : 0u;
    return down && !wasDown;
}

} // namespace rtv
