#include "rtv/Application.h"

#include "rtv/AssetImport.h"
#include "rtv/Buffer.h"
#include "rtv/CommandSystem.h"
#include "rtv/NsightPerfMarkers.h"
#include "rtv/BufferUploader.h"
#include "rtv/DiagnosticImageExport.h"
#include "rtv/EditorCommands.h"
#include "rtv/EditorLog.h"
#include "rtv/FileDialog.h"
#include "rtv/GltfLoader.h"
#include "rtv/GpuSceneStreamingState.h"
#include "rtv/NsightGraphicsRuntime.h"
#include "rtv/NativeAssetFormat.h"
#include "rtv/NativeAssetRuntimeLoader.h"
#include "rtv/NativeAssetMigration.h"
#include "rtv/NativeTextureFormatPolicy.h"
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
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
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

bool mainLoopTraceEnabled() {
    static const bool enabled = [] {
#if defined(_WIN32)
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "RTV_MAIN_LOOP_TRACE") != 0 || value == nullptr) {
            return false;
        }
        const bool result = value[0] != '\0' && value[0] != '0';
        std::free(value);
        return result;
#else
        const char* value = std::getenv("RTV_MAIN_LOOP_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
#endif
    }();
    return enabled;
}

void traceMainLoopPhase(uint32_t frame, const char* phase) {
    if (!mainLoopTraceEnabled()) {
        return;
    }
    std::cout << "MAIN_LOOP frame=" << frame << " phase=" << phase << '\n' << std::flush;
}

void traceStartupPhase(const char* phase) {
    if (!mainLoopTraceEnabled()) {
        return;
    }
    std::cout << "STARTUP phase=" << phase << '\n' << std::flush;
}
constexpr uint64_t largeSceneTriangleThreshold = 1'000'000ull;
constexpr float defaultMaxFrameDeltaSeconds = 1.0f / 30.0f;
constexpr uint32_t streamingFinalRebuildMaterialTexturePreviewMaxDimension = 1024u;

constexpr RendererDebugView intermediateViews[] = {
    RendererDebugView::Beauty,
    RendererDebugView::DirectLighting,
    RendererDebugView::IndirectLighting,
    RendererDebugView::Variance,
    RendererDebugView::Normals,
    RendererDebugView::Depth,
    RendererDebugView::MotionVectors,
    RendererDebugView::SkinnedMotionVectors,
};

nlohmann::json nativeTextureFormatSupportJson(const NativeTextureFormatSupport& support);

std::vector<TopologyRebuildStageDesc> makeTopologyRebuildStagePlan(std::string_view prefix, double stageCostMs = 1.5) {
    std::vector<TopologyRebuildStageDesc> stages;
    stages.reserve(9);
    const std::string labelPrefix(prefix);
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::CpuSceneExtraction, stageCostMs, labelPrefix + " cpu scene extraction"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::GpuSceneBufferBuild, stageCostMs, labelPrefix + " gpu scene build"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::BufferUploads, stageCostMs, labelPrefix + " buffer uploads"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::TextureUploads, stageCostMs, labelPrefix + " texture uploads"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::BlasBuildBatch, stageCostMs, labelPrefix + " BLAS build"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::TlasBuildOrRefit, stageCostMs, labelPrefix + " TLAS build/refit"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::RendererDescriptorUpdate, 0.5, labelPrefix + " descriptor update"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::FinalRendererSwap, 0.25, labelPrefix + " final renderer swap"});
    stages.push_back(TopologyRebuildStageDesc{TopologyRebuildStage::RetireOldRenderer, 0.25, labelPrefix + " retire old renderer"});
    return stages;
}

std::string quoteShellArg(std::string_view value) {
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

std::string quoteShellPath(const std::filesystem::path& path) {
    return quoteShellArg(path.string());
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
    if (lowerRole.find("controller") != std::string::npos) {
        return {AssetType::AnimationController};
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
    const std::filesystem::path& manifestPath,
    const NativeTextureFormatSupport& textureFormatSupport,
    const std::string& packageTextureTargetSetJson = {}) {
    const std::string supportJson = nativeTextureFormatSupportJson(textureFormatSupport).dump();
    const std::wstring supportJsonWide(supportJson.begin(), supportJson.end());
    std::wstring command = quoteWindowsArg(exe.wstring()) +
        L" --cook-project " + quoteWindowsArg(projectFile.wstring()) +
        L" --cook-output " + quoteWindowsArg(outputDir.wstring()) +
        L" --cook-manifest " + quoteWindowsArg(manifestPath.wstring()) +
        L" --native-texture-format-support-json " + quoteWindowsArg(supportJsonWide);
    if (!packageTextureTargetSetJson.empty()) {
        const std::wstring targetSetJsonWide(packageTextureTargetSetJson.begin(), packageTextureTargetSetJson.end());
        command += L" --native-package-texture-target-set-json " + quoteWindowsArg(targetSetJsonWide);
    }
    return command;
}

int runCookProjectProcess(
    const std::filesystem::path& exe,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& logPath,
    const NativeTextureFormatSupport& textureFormatSupport,
    const std::string& packageTextureTargetSetJson,
    std::string* commandLineOut) {
    const std::wstring commandLine = cookProcessCommandLineWide(exe, projectFile, outputDir, manifestPath, textureFormatSupport, packageTextureTargetSetJson);
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

bool launchQuickNsightExperimentMatrix(const std::filesystem::path& scenePath) {
    const std::filesystem::path script =
        std::filesystem::current_path() / "scripts" / "nsight_raytracing_matrix.ps1";
    if (!std::filesystem::exists(script) || scenePath.empty()) {
        return false;
    }
    const std::wstring commandLine =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
        quoteWindowsArg(script.wstring()) +
        L" -Scene " + quoteWindowsArg(scenePath.wstring()) +
        L" -Quick";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = std::filesystem::current_path().wstring();
    const BOOL created = CreateProcessW(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDirectory.c_str(),
        &startup,
        &process);
    if (!created) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
std::string cookProcessCommandLine(
    const std::filesystem::path& exe,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath,
    const NativeTextureFormatSupport& textureFormatSupport,
    const std::string& packageTextureTargetSetJson,
    const std::filesystem::path& logPath) {
    const std::string supportJson = nativeTextureFormatSupportJson(textureFormatSupport).dump();
    std::string command = quoteShellPath(exe) +
        " --cook-project " + quoteShellPath(projectFile) +
        " --cook-output " + quoteShellPath(outputDir) +
        " --cook-manifest " + quoteShellPath(manifestPath) +
        " --native-texture-format-support-json " + quoteShellArg(supportJson);
    if (!packageTextureTargetSetJson.empty()) {
        command += " --native-package-texture-target-set-json " + quoteShellArg(packageTextureTargetSetJson);
    }
    command += " > " + quoteShellPath(logPath) + " 2>&1";
    return command;
}

int runCookProjectProcess(
    const std::filesystem::path& exe,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& logPath,
    const NativeTextureFormatSupport& textureFormatSupport,
    const std::string& packageTextureTargetSetJson,
    std::string* commandLineOut) {
    std::error_code logEc;
    std::filesystem::create_directories(logPath.parent_path(), logEc);
    const std::string command = cookProcessCommandLine(exe, projectFile, outputDir, manifestPath, textureFormatSupport, packageTextureTargetSetJson, logPath);
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

const char* createEntityKindLabel(EditorEntityCreateKind kind) {
    switch (kind) {
    case EditorEntityCreateKind::Empty: return "empty entity";
    case EditorEntityCreateKind::Camera: return "camera";
    case EditorEntityCreateKind::Light: return "point light";
    case EditorEntityCreateKind::Sun: return "sun";
    case EditorEntityCreateKind::SpotLight: return "spot light";
    case EditorEntityCreateKind::AreaLight: return "area light";
    case EditorEntityCreateKind::EnvironmentLight: return "environment light";
    case EditorEntityCreateKind::SkyAtmosphere: return "sky atmosphere";
    case EditorEntityCreateKind::HeightFog: return "height fog";
    case EditorEntityCreateKind::VolumetricCloud: return "volumetric cloud";
    case EditorEntityCreateKind::PostProcessVolume: return "post process volume";
    }
    return "entity";
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

NativeTextureFormatSupport nativeTextureFormatSupportForContext(const VulkanContext* context) {
    if (context == nullptr || context->physicalDevice() == VK_NULL_HANDLE) {
        return nativeTextureOfflineFallbackFormatSupport();
    }
    const VkPhysicalDeviceProperties properties = context->physicalDeviceProperties();
    std::string platformName = properties.deviceName;
    if (platformName.empty()) {
        platformName = "vulkan-physical-device";
    }
    return nativeTextureFormatSupportFromPhysicalDevice(context->physicalDevice(), std::move(platformName));
}

nlohmann::json nativeTextureFormatSupportJson(const NativeTextureFormatSupport& support) {
    return {
        {"platformName", support.platformName},
        {"queriedFromVulkan", support.queriedFromVulkan},
        {"bc1SrgbSampled", support.bc1SrgbSampled},
        {"bc1UnormSampled", support.bc1UnormSampled},
        {"bc3SrgbSampled", support.bc3SrgbSampled},
        {"bc3UnormSampled", support.bc3UnormSampled},
        {"bc7SrgbSampled", support.bc7SrgbSampled},
        {"bc7UnormSampled", support.bc7UnormSampled},
        {"bc5UnormSampled", support.bc5UnormSampled},
        {"bc4UnormSampled", support.bc4UnormSampled},
        {"bc6hUfloatSampled", support.bc6hUfloatSampled},
        {"bc6hSfloatSampled", support.bc6hSfloatSampled},
        {"rgba8SrgbSampled", support.rgba8SrgbSampled},
        {"rgba8UnormSampled", support.rgba8UnormSampled},
        {"rgba16fSampled", support.rgba16fSampled},
    };
}

std::string nativeTextureTargetSetProfileName(EditorNativeTextureTargetSetProfile profile) {
    switch (profile) {
    case EditorNativeTextureTargetSetProfile::ActiveAndAllBc: return "active-and-all-bc";
    case EditorNativeTextureTargetSetProfile::ActiveOnly: return "active-only";
    case EditorNativeTextureTargetSetProfile::AllBcAudit: return "all-bc-audit";
    case EditorNativeTextureTargetSetProfile::ActiveAllBcAndRgbaFallback: return "active-all-bc-rgba-fallback";
    case EditorNativeTextureTargetSetProfile::Custom: return "custom";
    case EditorNativeTextureTargetSetProfile::CustomLibrary: return "custom-library";
    }
    return "active-and-all-bc";
}

NativeTextureFormatSupport nativeTextureCustomTargetSetSupport(const EditorNativeTextureTargetSetCustomProfile& custom) {
    NativeTextureFormatSupport support;
    support.queriedFromVulkan = false;
    support.platformName = custom.platformName.empty() ? std::string("editor-custom-target-set") : custom.platformName;
    support.bc1SrgbSampled = true;
    support.bc1UnormSampled = true;
    support.bc3SrgbSampled = true;
    support.bc3UnormSampled = true;
    support.bc7SrgbSampled = custom.bc7SrgbSampled;
    support.bc7UnormSampled = custom.bc7UnormSampled;
    support.bc5UnormSampled = custom.bc5UnormSampled;
    support.bc4UnormSampled = custom.bc4UnormSampled;
    support.bc6hUfloatSampled = false;
    support.bc6hSfloatSampled = false;
    support.rgba8SrgbSampled = custom.rgba8SrgbSampled;
    support.rgba8UnormSampled = custom.rgba8UnormSampled;
    support.rgba16fSampled = custom.rgba16fSampled;
    return support;
}

NativeTextureFormatSupport nativeTextureLibraryTargetSetSupport(const EditorNativeTextureTargetSetLibraryProfile& custom) {
    NativeTextureFormatSupport support;
    support.queriedFromVulkan = false;
    support.platformName = custom.name.empty() ? std::string("editor-library-target-set") : custom.name;
    support.bc1SrgbSampled = true;
    support.bc1UnormSampled = true;
    support.bc3SrgbSampled = true;
    support.bc3UnormSampled = true;
    support.bc7SrgbSampled = custom.bc7SrgbSampled;
    support.bc7UnormSampled = custom.bc7UnormSampled;
    support.bc5UnormSampled = custom.bc5UnormSampled;
    support.bc4UnormSampled = custom.bc4UnormSampled;
    support.bc6hUfloatSampled = false;
    support.bc6hSfloatSampled = false;
    support.rgba8SrgbSampled = custom.rgba8SrgbSampled;
    support.rgba8UnormSampled = custom.rgba8UnormSampled;
    support.rgba16fSampled = custom.rgba16fSampled;
    return support;
}

std::string nativeTextureTargetSetProfileJson(
    EditorNativeTextureTargetSetProfile profile,
    const NativeTextureFormatSupport& activeSupport,
    const EditorNativeTextureTargetSetCustomProfile& customProfile,
    const std::vector<EditorNativeTextureTargetSetLibraryProfile>& customLibrary) {
    nlohmann::json targetSets = nlohmann::json::array();
    auto append = [&](NativeTextureFormatSupport support, std::string label) {
        nlohmann::json supportJson = nativeTextureFormatSupportJson(support);
        supportJson["targetSetLabel"] = std::move(label);
        targetSets.push_back(std::move(supportJson));
    };

    switch (profile) {
    case EditorNativeTextureTargetSetProfile::ActiveOnly:
        append(activeSupport, "active-vulkan");
        break;
    case EditorNativeTextureTargetSetProfile::AllBcAudit:
        append(nativeTextureAllBcFormatSupportForAudit(), "all-bc-audit");
        break;
    case EditorNativeTextureTargetSetProfile::ActiveAllBcAndRgbaFallback:
        append(activeSupport, "active-vulkan");
        append(nativeTextureAllBcFormatSupportForAudit(), "all-bc-audit");
        append(nativeTextureOfflineFallbackFormatSupport(), "rgba-fallback");
        break;
    case EditorNativeTextureTargetSetProfile::Custom:
        append(nativeTextureCustomTargetSetSupport(customProfile), "editor-custom");
        break;
    case EditorNativeTextureTargetSetProfile::CustomLibrary:
        if (customLibrary.empty()) {
            append(nativeTextureCustomTargetSetSupport(customProfile), "editor-custom-empty-library-fallback");
        } else {
            for (const EditorNativeTextureTargetSetLibraryProfile& libraryProfile : customLibrary) {
                append(nativeTextureLibraryTargetSetSupport(libraryProfile), "editor-custom-library");
            }
        }
        break;
    case EditorNativeTextureTargetSetProfile::ActiveAndAllBc:
    default:
        append(activeSupport, "active-vulkan");
        append(nativeTextureAllBcFormatSupportForAudit(), "all-bc-audit");
        break;
    }

    return nlohmann::json{
        {"profile", nativeTextureTargetSetProfileName(profile)},
        {"targetSets", std::move(targetSets)},
    }.dump();
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

std::string projectCookPackageBaseName(const ProjectContext& project) {
    std::string name = project.name.empty() ? project.projectFile.stem().string() : project.name;
    for (char& ch : name) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    return name.empty() ? std::string("Project") : name;
}

std::filesystem::path projectCookPackagePath(const ProjectContext& project) {
    return project.buildRoot / "Cooked" / (projectCookPackageBaseName(project) + ".rtpkg");
}

std::filesystem::path projectStartupNativePackageMountReportPath(const ProjectContext& project) {
    const std::filesystem::path root = project.savedRoot.empty() ? project.projectRoot / "Saved" : project.savedRoot;
    return root / "Reports" / "project_startup_native_package_mount.json";
}

std::filesystem::path editorNativePackageMountReportPath(const std::optional<ProjectContext>& project, const std::filesystem::path& packagePath) {
    const std::filesystem::path root = project.has_value()
        ? (project->savedRoot.empty() ? project->projectRoot / "Saved" : project->savedRoot)
        : (std::filesystem::current_path() / "out" / "editor_tools");
    const std::string name = packagePath.filename().empty() ? std::string("package") : packagePath.filename().string();
    return root / "Reports" / ("content_browser_native_package_mount_" + safeReportName(name) + ".json");
}

std::filesystem::path editorNativePackageUnloadReportPath(const std::optional<ProjectContext>& project, const std::filesystem::path& packagePath) {
    const std::filesystem::path root = project.has_value()
        ? (project->savedRoot.empty() ? project->projectRoot / "Saved" : project->savedRoot)
        : (std::filesystem::current_path() / "out" / "editor_tools");
    const std::string name = packagePath.filename().empty() ? std::string("package") : packagePath.filename().string();
    return root / "Reports" / ("content_browser_native_package_unload_" + safeReportName(name) + ".json");
}

std::filesystem::path editorNativePackageRefreshReportPath(const std::optional<ProjectContext>& project, const std::filesystem::path& packagePath) {
    const std::filesystem::path root = project.has_value()
        ? (project->savedRoot.empty() ? project->projectRoot / "Saved" : project->savedRoot)
        : (std::filesystem::current_path() / "out" / "editor_tools");
    const std::string name = packagePath.filename().empty() ? std::string("package") : packagePath.filename().string();
    return root / "Reports" / ("content_browser_native_package_refresh_" + safeReportName(name) + ".json");
}

std::filesystem::path editorNativePackageRefreshDetectionReportPath(const std::optional<ProjectContext>& project, const std::filesystem::path& packagePath) {
    const std::filesystem::path root = project.has_value()
        ? (project->savedRoot.empty() ? project->projectRoot / "Saved" : project->savedRoot)
        : (std::filesystem::current_path() / "out" / "editor_tools");
    const std::string name = packagePath.filename().empty() ? std::string("package") : packagePath.filename().string();
    return root / "Reports" / ("content_browser_native_package_refresh_detected_" + safeReportName(name) + ".json");
}

uint64_t regularFileSizeOrZero(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0ull : static_cast<uint64_t>(std::min<uintmax_t>(size, std::numeric_limits<uint64_t>::max()));
}

nlohmann::json legacyCpuPackageMountBlockedJson(
    std::string schema,
    std::string inspectionSource,
    const std::filesystem::path& packagePath,
    uint64_t packageBytes,
    const NativeRuntimeLoadOptions& loadOptions) {
    return {
        {"schema", std::move(schema)},
        {"inspectionSource", std::move(inspectionSource)},
        {"ok", false},
        {"package", {
            {"path", packagePath.generic_string()},
            {"exists", true},
            {"sizeBytes", packageBytes},
        }},
        {"mutationExecuted", false},
        {"legacy_cpu_load_policy", {
            {"available", true},
            {"asset_manager_backed", false},
            {"package_backed", true},
            {"estimated_eager_cpu_bytes", packageBytes},
            {"warning_threshold_bytes", loadOptions.eagerCpuLoadWarningBytes},
            {"hard_limit_bytes", loadOptions.eagerCpuLoadHardLimitBytes},
            {"allow_large_eager_cpu_load", loadOptions.allowLargeEagerCpuLoad},
            {"large_eager_load_warning", packageBytes >= loadOptions.eagerCpuLoadWarningBytes},
            {"hard_limit_exceeded", packageBytes >= loadOptions.eagerCpuLoadHardLimitBytes && !loadOptions.allowLargeEagerCpuLoad},
            {"streaming_recommended", true},
            {"policy", "legacy-eager-cpu-package-mount-blocked-before-decode"},
            {"recommended_action", "Use package inspection/validation and the progressive native streaming path instead of blocking CPU AssetManager package mount."},
        }},
        {"error", "Legacy eager CPU package mount is blocked for large packages before payload decode."},
    };
}

std::filesystem::path normalizedPackagePathKey(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return (ec ? path : canonical).lexically_normal();
}

bool nativeRuntimePathBelongsToPackage(const std::filesystem::path& nativePath, const std::filesystem::path& packagePath) {
    if (nativePath.empty() || packagePath.empty()) {
        return false;
    }
    const std::string packageKey = lowerAscii(normalizedPackagePathKey(packagePath).generic_string());
    const std::string nativeKey = lowerAscii(normalizedPackagePathKey(nativePath).generic_string());
    if (nativeKey == packageKey) {
        return true;
    }
    const std::string packagePrefix = packageKey + "/";
    return nativeKey.size() > packagePrefix.size() && nativeKey.rfind(packagePrefix, 0) == 0;
}

std::filesystem::file_time_type nativePackageLastWriteTimeOrZero(const std::filesystem::path& packagePath) {
    std::error_code ec;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(packagePath, ec);
    return ec ? std::filesystem::file_time_type{} : stamp;
}

TextureAssetHandle remapTextureHandle(TextureAssetHandle handle, const std::vector<uint32_t>& remap) {
    if (!handle.valid() || handle.index >= remap.size() || remap[handle.index] == UINT32_MAX) {
        return {};
    }
    return TextureAssetHandle{remap[handle.index]};
}

MaterialAssetHandle remapMaterialHandle(MaterialAssetHandle handle, const std::vector<uint32_t>& remap) {
    if (!handle.valid() || handle.index >= remap.size() || remap[handle.index] == UINT32_MAX) {
        return {};
    }
    return MaterialAssetHandle{remap[handle.index]};
}

MeshAssetHandle remapMeshHandle(MeshAssetHandle handle, const std::vector<uint32_t>& remap) {
    if (!handle.valid() || handle.index >= remap.size() || remap[handle.index] == UINT32_MAX) {
        return {};
    }
    return MeshAssetHandle{remap[handle.index]};
}

void remapMaterialTextureHandles(MaterialAsset& material, const std::vector<uint32_t>& textureRemap) {
    material.baseColorTexture = remapTextureHandle(material.baseColorTexture, textureRemap);
    material.normalTexture = remapTextureHandle(material.normalTexture, textureRemap);
    material.metallicRoughnessTexture = remapTextureHandle(material.metallicRoughnessTexture, textureRemap);
    material.emissiveTexture = remapTextureHandle(material.emissiveTexture, textureRemap);
    material.clearcoatTexture = remapTextureHandle(material.clearcoatTexture, textureRemap);
    material.clearcoatRoughnessTexture = remapTextureHandle(material.clearcoatRoughnessTexture, textureRemap);
    material.clearcoatNormalTexture = remapTextureHandle(material.clearcoatNormalTexture, textureRemap);
    material.transmissionTexture = remapTextureHandle(material.transmissionTexture, textureRemap);
    material.volumeThicknessTexture = remapTextureHandle(material.volumeThicknessTexture, textureRemap);
    material.specularTexture = remapTextureHandle(material.specularTexture, textureRemap);
    material.specularColorTexture = remapTextureHandle(material.specularColorTexture, textureRemap);
    material.sheenColorTexture = remapTextureHandle(material.sheenColorTexture, textureRemap);
    material.sheenRoughnessTexture = remapTextureHandle(material.sheenRoughnessTexture, textureRemap);
    material.iridescenceTexture = remapTextureHandle(material.iridescenceTexture, textureRemap);
    material.iridescenceThicknessTexture = remapTextureHandle(material.iridescenceThicknessTexture, textureRemap);
    material.anisotropyTexture = remapTextureHandle(material.anisotropyTexture, textureRemap);
    material.occlusionTexture = remapTextureHandle(material.occlusionTexture, textureRemap);
    material.opacityTexture = remapTextureHandle(material.opacityTexture, textureRemap);
    material.heightTexture = remapTextureHandle(material.heightTexture, textureRemap);
}

void remapMeshMaterialHandles(MeshAsset& mesh, const std::vector<uint32_t>& materialRemap) {
    for (MeshPrimitiveAsset& primitive : mesh.primitives) {
        primitive.material = remapMaterialHandle(primitive.material, materialRemap);
        for (MeshPrimitiveAsset::MaterialVariant& variant : primitive.materialVariants) {
            variant.material = remapMaterialHandle(variant.material, materialRemap);
        }
    }
}

void remapSceneAssetHandles(SceneAsset& scene, const std::vector<uint32_t>& textureRemap, const std::vector<uint32_t>& materialRemap, const std::vector<uint32_t>& meshRemap) {
    for (TextureAssetHandle& texture : scene.textures) {
        texture = remapTextureHandle(texture, textureRemap);
    }
    scene.textures.erase(std::remove_if(scene.textures.begin(), scene.textures.end(), [](TextureAssetHandle handle) { return !handle.valid(); }), scene.textures.end());
    for (MaterialAssetHandle& material : scene.materials) {
        material = remapMaterialHandle(material, materialRemap);
    }
    scene.materials.erase(std::remove_if(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle handle) { return !handle.valid(); }), scene.materials.end());
    for (MeshAssetHandle& mesh : scene.meshes) {
        mesh = remapMeshHandle(mesh, meshRemap);
    }
    scene.meshes.erase(std::remove_if(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle handle) { return !handle.valid(); }), scene.meshes.end());
    for (SceneNodeAsset& node : scene.nodes) {
        node.mesh = remapMeshHandle(node.mesh, meshRemap);
    }
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
        if (settings.restirGiMode != RestirGiMode::Off) {
            settings.restirGiMode = RestirGiMode::Off;
            settings.restirGiReservoirLayout = RestirGiReservoirLayout::LegacyCachePacked;
            largeSceneSettingsChanged = true;
        }
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
    render.secondaryDirectLightingEnabled = settings.secondaryDirectLightingEnabled;
    render.maxBounces = settings.maxBounces;
    render.pathTraceKernelMode = settings.pathTraceKernelMode;
    render.finalBounceFastPathEnabled = settings.finalBounceFastPathEnabled;
    render.native2BTerminalDirectSampleProbability = settings.native2BTerminalDirectSampleProbability;
    render.blendedDecalShadowMode = settings.blendedDecalShadowMode;
    render.native2BDirectReuseMode = settings.native2BDirectReuseMode;
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
    render.restirDiMode = settings.restirDiMode;
    render.restirDiTemporalEnabled = settings.restirDiTemporalEnabled;
    render.restirDiSpatialEnabled = settings.restirDiSpatialEnabled;
    render.restirDiFinalVisibilityEnabled = settings.restirDiFinalVisibilityEnabled;
    render.restirDiSpatialRounds = settings.restirDiSpatialRounds;
    render.restirDiSpatialRadius = settings.restirDiSpatialRadius;
    render.restirDiTemporalMaxAge = settings.restirDiTemporalMaxAge;
    render.restirDiMaxM = settings.restirDiMaxM;
    render.restirDiVisibilityRayBudget = settings.restirDiVisibilityRayBudget;
    render.restirDiProductionStabilizationEnabled = settings.restirDiProductionStabilizationEnabled;
    render.restirDiClampLuminance = settings.restirDiClampLuminance;
    render.restirDiIncludeSun = settings.restirDiIncludeSun;
    render.restirDiIncludeEnvironment = settings.restirDiIncludeEnvironment;
    render.restirDiReservoirLayout = settings.restirDiReservoirLayout;
    render.restirGiMode = settings.restirGiMode;
    render.restirGiReservoirLayout = settings.restirGiReservoirLayout;
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
    render.streamlineReflexEnabled = settings.streamlineReflexEnabled;
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
    render.compactImportedEmissiveTriangleSampling = settings.compactImportedEmissiveTriangleSampling;
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
    render.restirGiActiveTileMaskMode = settings.restirGiActiveTileMaskMode;
    render.restirHistoryCopyMode = settings.restirHistoryCopyMode;
    render.lightingReuseMode = settings.lightingReuseMode;
    render.regirGridDimensions = settings.regirGridDimensions;
    render.regirReservoirsPerCell = settings.regirReservoirsPerCell;
    render.regirCandidatesPerReservoir = settings.regirCandidatesPerReservoir;
    render.regirGridPadding = settings.regirGridPadding;
    render.regirCanonicalMix = settings.regirCanonicalMix;
    render.regirQueryMode = settings.regirQueryMode;
    render.regirGridMode = settings.regirGridMode;
    render.regirFiniteQueryFramePeriod = settings.regirFiniteQueryFramePeriod;
    render.regirSpatialReuse = settings.regirSpatialReuse;
    render.regirSpatialRounds = settings.regirSpatialRounds;
    render.regirTemporalReuse = settings.regirTemporalReuse;
    render.regirTemporalHistory = settings.regirTemporalHistory;
    render.regirTemporalMaxM = settings.regirTemporalMaxM;
    render.regirVisibilityReuse = settings.regirVisibilityReuse;
    render.regirEnvironment = settings.regirEnvironment;
    render.pathReservoirLayout = settings.pathReservoirLayout;
    render.adaptiveSamplingMode = settings.adaptiveSamplingMode;
    render.adaptiveSamplingBudget = settings.adaptiveSamplingBudget;
    render.adaptiveWeightVariance = settings.adaptiveWeightVariance;
    render.adaptiveWeightHistory = settings.adaptiveWeightHistory;
    render.adaptiveWeightMotion = settings.adaptiveWeightMotion;
    render.adaptiveWeightDisocclusion = settings.adaptiveWeightDisocclusion;
    render.adaptiveWeightReactive = settings.adaptiveWeightReactive;
    render.adaptiveWeightEdge = settings.adaptiveWeightEdge;
    render.adaptiveWeightSpecular = settings.adaptiveWeightSpecular;
    render.adaptiveWeightDI = settings.adaptiveWeightDI;
    render.adaptiveWeightGI = settings.adaptiveWeightGI;
    render.adaptiveWeightVolumetric = settings.adaptiveWeightVolumetric;
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
    settings.secondaryDirectLightingEnabled = render.secondaryDirectLightingEnabled;
    settings.maxBounces = render.maxBounces;
    settings.pathTraceKernelMode = render.pathTraceKernelMode;
    settings.finalBounceFastPathEnabled = render.finalBounceFastPathEnabled;
    settings.native2BTerminalDirectSampleProbability = render.native2BTerminalDirectSampleProbability;
    settings.blendedDecalShadowMode = render.blendedDecalShadowMode;
    settings.native2BDirectReuseMode = render.native2BDirectReuseMode;
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
    settings.restirDiMode = render.restirDiMode;
    settings.restirDiTemporalEnabled = render.restirDiTemporalEnabled;
    settings.restirDiSpatialEnabled = render.restirDiSpatialEnabled;
    settings.restirDiFinalVisibilityEnabled = render.restirDiFinalVisibilityEnabled;
    settings.restirDiSpatialRounds = render.restirDiSpatialRounds;
    settings.restirDiSpatialRadius = render.restirDiSpatialRadius;
    settings.restirDiTemporalMaxAge = render.restirDiTemporalMaxAge;
    settings.restirDiMaxM = render.restirDiMaxM;
    settings.restirDiVisibilityRayBudget = render.restirDiVisibilityRayBudget;
    settings.restirDiProductionStabilizationEnabled = render.restirDiProductionStabilizationEnabled;
    settings.restirDiClampLuminance = render.restirDiClampLuminance;
    settings.restirDiIncludeSun = render.restirDiIncludeSun;
    settings.restirDiIncludeEnvironment = render.restirDiIncludeEnvironment;
    settings.restirDiReservoirLayout = render.restirDiReservoirLayout;
    settings.restirGiMode = render.restirGiMode;
    settings.restirGiReservoirLayout = render.restirGiReservoirLayout;
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
    settings.streamlineReflexEnabled = render.streamlineReflexEnabled;
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
    settings.compactImportedEmissiveTriangleSampling = render.compactImportedEmissiveTriangleSampling;
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
    settings.restirGiActiveTileMaskMode = render.restirGiActiveTileMaskMode;
    settings.restirHistoryCopyMode = render.restirHistoryCopyMode;
    settings.lightingReuseMode = render.lightingReuseMode;
    settings.regirGridDimensions = render.regirGridDimensions;
    settings.regirReservoirsPerCell = render.regirReservoirsPerCell;
    settings.regirCandidatesPerReservoir = render.regirCandidatesPerReservoir;
    settings.regirGridPadding = render.regirGridPadding;
    settings.regirCanonicalMix = render.regirCanonicalMix;
    settings.regirQueryMode = render.regirQueryMode;
    settings.regirGridMode = render.regirGridMode;
    settings.regirFiniteQueryFramePeriod = render.regirFiniteQueryFramePeriod;
    settings.regirSpatialReuse = render.regirSpatialReuse;
    settings.regirSpatialRounds = render.regirSpatialRounds;
    settings.regirTemporalReuse = render.regirTemporalReuse;
    settings.regirTemporalHistory = render.regirTemporalHistory;
    settings.regirTemporalMaxM = render.regirTemporalMaxM;
    settings.regirVisibilityReuse = render.regirVisibilityReuse;
    settings.regirEnvironment = render.regirEnvironment;
    settings.pathReservoirLayout = render.pathReservoirLayout;
    settings.adaptiveSamplingMode = render.adaptiveSamplingMode;
    settings.adaptiveSamplingBudget = render.adaptiveSamplingBudget;
    settings.adaptiveWeightVariance = render.adaptiveWeightVariance;
    settings.adaptiveWeightHistory = render.adaptiveWeightHistory;
    settings.adaptiveWeightMotion = render.adaptiveWeightMotion;
    settings.adaptiveWeightDisocclusion = render.adaptiveWeightDisocclusion;
    settings.adaptiveWeightReactive = render.adaptiveWeightReactive;
    settings.adaptiveWeightEdge = render.adaptiveWeightEdge;
    settings.adaptiveWeightSpecular = render.adaptiveWeightSpecular;
    settings.adaptiveWeightDI = render.adaptiveWeightDI;
    settings.adaptiveWeightGI = render.adaptiveWeightGI;
    settings.adaptiveWeightVolumetric = render.adaptiveWeightVolumetric;
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

const char* materialWorkflowName(uint32_t workflow) {
    switch (workflow) {
    case kMaterialWorkflowPackedOcclusionRoughnessMetalness:
        return "PackedOcclusionRoughnessMetalness";
    case kMaterialWorkflowSpecularGlossiness:
        return "SpecularGlossiness";
    case kMaterialWorkflowMetallicRoughness:
    default:
        return "MetallicRoughness";
    }
}

const char* materialNormalMapConventionName(uint32_t convention) {
    switch (convention) {
    case kMaterialNormalMapDirectX:
        return "DirectX";
    case kMaterialNormalMapOpenGL:
    default:
        return "OpenGL";
    }
}

const char* materialSpecularTextureAlphaModeName(uint32_t mode) {
    switch (mode) {
    case kMaterialSpecularTextureAlphaGlossiness:
        return "Glossiness";
    case kMaterialSpecularTextureAlphaNone:
    default:
        return "None";
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
        {"workflow", materialWorkflowName(material.materialWorkflow)},
        {"normalMapConvention", materialNormalMapConventionName(material.normalMapConvention)},
        {"specularTextureAlphaMode", materialSpecularTextureAlphaModeName(material.specularTextureAlphaMode)},
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

uint32_t materialWorkflowFromName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "packedocclusionroughnessmetalness" || name == "packedorm" || name == "orm") {
        return kMaterialWorkflowPackedOcclusionRoughnessMetalness;
    }
    if (name == "specularglossiness" || name == "specgloss") {
        return kMaterialWorkflowSpecularGlossiness;
    }
    return kMaterialWorkflowMetallicRoughness;
}

uint32_t materialNormalMapConventionFromName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "directx" || name == "dx") {
        return kMaterialNormalMapDirectX;
    }
    return kMaterialNormalMapOpenGL;
}

uint32_t materialSpecularTextureAlphaModeFromName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "glossiness" || name == "gloss") {
        return kMaterialSpecularTextureAlphaGlossiness;
    }
    return kMaterialSpecularTextureAlphaNone;
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
    material.materialWorkflow = materialWorkflowFromName(source->value("workflow", std::string("MetallicRoughness")));
    material.normalMapConvention = materialNormalMapConventionFromName(source->value("normalMapConvention", std::string("OpenGL")));
    material.specularTextureAlphaMode = materialSpecularTextureAlphaModeFromName(source->value("specularTextureAlphaMode", std::string("None")));
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
    material.materialWorkflow = edited->materialWorkflow;
    material.normalMapConvention = edited->normalMapConvention;
    material.specularTextureAlphaMode = edited->specularTextureAlphaMode;
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
    if (material.name.empty() && !recordIt->displayName.empty()) {
        material.name = recordIt->displayName;
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

class SceneAndAssetAppendSnapshotCommand final : public ICommand {
public:
    SceneAndAssetAppendSnapshotCommand(
        SceneDocument& document,
        AssetManager& assets,
        SceneDocument beforeDocument,
        AssetLoadStats beforeAssetStats,
        SceneDocument afterDocument,
        SceneUpdateKind updateKind,
        std::string label)
        : document_(document),
          assets_(assets),
          beforeDocument_(std::move(beforeDocument)),
          beforeAssetStats_(beforeAssetStats),
          afterDocument_(std::move(afterDocument)),
          updateKind_(updateKind),
          label_(std::move(label)) {
        const auto appendRange = [](auto& out, const auto& source, uint32_t begin) {
            const size_t start = std::min<size_t>(begin, source.size());
            out.insert(out.end(), source.begin() + static_cast<std::ptrdiff_t>(start), source.end());
        };
        appendRange(appendedTextures_, assets_.textures(), beforeAssetStats_.textureCount);
        appendRange(appendedMaterials_, assets_.materials(), beforeAssetStats_.materialCount);
        appendRange(appendedMeshes_, assets_.meshes(), beforeAssetStats_.meshCount);
    }

    void undo() override {
        document_ = beforeDocument_;
        assets_.truncateTo(beforeAssetStats_);
        document_.markDirty(updateKind_);
    }

    void redo() override {
        assets_.truncateTo(beforeAssetStats_);
        for (const TextureAsset& texture : appendedTextures_) {
            (void)assets_.addTexture(texture);
        }
        for (const MaterialAsset& material : appendedMaterials_) {
            (void)assets_.addMaterial(material);
        }
        for (const MeshAsset& mesh : appendedMeshes_) {
            (void)assets_.addMesh(mesh);
        }
        document_ = afterDocument_;
        document_.markDirty(updateKind_);
    }

    [[nodiscard]] const std::string& label() const override { return label_; }

private:
    SceneDocument& document_;
    AssetManager& assets_;
    SceneDocument beforeDocument_;
    AssetLoadStats beforeAssetStats_{};
    SceneDocument afterDocument_;
    std::vector<TextureAsset> appendedTextures_;
    std::vector<MaterialAsset> appendedMaterials_;
    std::vector<MeshAsset> appendedMeshes_;
    SceneUpdateKind updateKind_ = SceneUpdateKind::TopologyChanged;
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
    remapTexture(material.opacityTexture);
    remapTexture(material.heightTexture);
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

bool shouldDeferInteractiveTopologyRebuild(const SceneDocument& document, const AssetManager& assets) {
    constexpr size_t kLargeSceneEntityThreshold = 512;
    constexpr size_t kLargeSceneMeshThreshold = 128;
    constexpr size_t kLargeSceneTextureThreshold = 128;
    constexpr uint64_t kLargeSceneTriangleThreshold = 500000ull;

    if (document.registry().entities().size() >= kLargeSceneEntityThreshold ||
        assets.meshes().size() >= kLargeSceneMeshThreshold ||
        assets.textures().size() >= kLargeSceneTextureThreshold) {
        return true;
    }
    uint64_t triangleCount = 0;
    for (const MeshAsset& mesh : assets.meshes()) {
        triangleCount += static_cast<uint64_t>(mesh.indices.size() / 3u);
        if (triangleCount >= kLargeSceneTriangleThreshold) {
            return true;
        }
    }
    return false;
}

bool populatePrefabBindingsFromLoadedAssets(
    const PrefabAsset& prefab,
    const AssetManager& assets,
    PrefabRuntimeBindings& bindings) {
    PrefabRuntimeBindings resolvedBindings;
    std::unordered_map<AssetGuid, MeshAssetHandle> loadedMeshes;
    std::unordered_map<AssetGuid, MaterialAssetHandle> loadedMaterials;
    const auto& meshes = assets.meshes();
    loadedMeshes.reserve(meshes.size());
    for (uint32_t i = 0; i < meshes.size(); ++i) {
        const MeshAsset& mesh = meshes[i];
        if (!mesh.nativeGuid.empty()) {
            loadedMeshes.emplace(mesh.nativeGuid, MeshAssetHandle{i});
        }
    }
    const auto& materials = assets.materials();
    loadedMaterials.reserve(materials.size());
    for (uint32_t i = 0; i < materials.size(); ++i) {
        const MaterialAsset& material = materials[i];
        if (!material.nativeGuid.empty()) {
            loadedMaterials.emplace(material.nativeGuid, MaterialAssetHandle{i});
        }
    }

    bool needsMesh = false;
    bool reboundMesh = false;
    for (const PrefabNodeAsset& node : prefab.nodes) {
        if (!node.meshGuid.empty()) {
            needsMesh = true;
            const auto meshIt = loadedMeshes.find(node.meshGuid);
            if (meshIt == loadedMeshes.end()) {
                return false;
            }
            resolvedBindings.meshes[node.meshGuid] = meshIt->second;
            reboundMesh = true;
        }
        for (const AssetGuid& materialGuid : node.materialGuids) {
            if (materialGuid.empty()) {
                continue;
            }
            const auto materialIt = loadedMaterials.find(materialGuid);
            if (materialIt == loadedMaterials.end()) {
                return false;
            }
            resolvedBindings.materials[materialGuid] = materialIt->second;
        }
    }
    if (needsMesh && !reboundMesh) {
        return false;
    }
    bindings.meshes.insert(resolvedBindings.meshes.begin(), resolvedBindings.meshes.end());
    bindings.materials.insert(resolvedBindings.materials.begin(), resolvedBindings.materials.end());
    return true;
}

bool isNativeRuntimeAssetRecordType(AssetType type) {
    return type == AssetType::Texture ||
        type == AssetType::Material ||
        type == AssetType::Mesh ||
        type == AssetType::Skeleton ||
        type == AssetType::Animation ||
        type == AssetType::AnimationController ||
        type == AssetType::SkeletalMesh;
}

std::vector<std::filesystem::path> nativeRuntimeCacheAllowListForRecord(
    const AssetRecord& rootRecord,
    const std::filesystem::path& root,
    const AssetRegistry* registry) {
    std::vector<std::filesystem::path> paths;
    if (registry == nullptr) {
        return paths;
    }

    std::unordered_map<AssetGuid, const AssetRecord*> recordsByGuid;
    recordsByGuid.reserve(registry->records().size());
    for (const AssetRecord& record : registry->records()) {
        if (!record.guid.empty()) {
            recordsByGuid.emplace(record.guid, &record);
        }
    }

    std::unordered_set<AssetGuid> visited;
    std::vector<AssetGuid> pending;
    pending.push_back(rootRecord.guid);
    while (!pending.empty()) {
        const AssetGuid guid = std::move(pending.back());
        pending.pop_back();
        if (guid.empty() || !visited.insert(guid).second) {
            continue;
        }
        const auto recordIt = recordsByGuid.find(guid);
        if (recordIt == recordsByGuid.end() || recordIt->second == nullptr) {
            continue;
        }
        const AssetRecord& record = *recordIt->second;
        if (isNativeRuntimeAssetRecordType(record.type) && !record.cachePath.empty()) {
            std::filesystem::path path = record.cachePath;
            if (!path.is_absolute()) {
                path = root / path;
            }
            paths.push_back(path.lexically_normal());
        }
        for (const AssetDependency& dependency : record.dependencies) {
            if (!dependency.guid.empty()) {
                pending.push_back(dependency.guid);
            }
        }
    }

    if (paths.empty() && (!rootRecord.importGroupId.empty() || !rootRecord.importRootGuid.empty())) {
        for (const AssetRecord& record : registry->records()) {
            const bool sameRoot = !rootRecord.importRootGuid.empty() && record.importRootGuid == rootRecord.importRootGuid;
            const bool sameGroup = !rootRecord.importGroupId.empty() && record.importGroupId == rootRecord.importGroupId;
            if ((!sameRoot && !sameGroup) || !isNativeRuntimeAssetRecordType(record.type) || record.cachePath.empty()) {
                continue;
            }
            std::filesystem::path path = record.cachePath;
            if (!path.is_absolute()) {
                path = root / path;
            }
            paths.push_back(path.lexically_normal());
        }
    }

    auto runtimePayloadSortRank = [](const std::filesystem::path& path) {
        const NativeAssetKind kind = nativeAssetKindFromExtension(path);
        if (kind == NativeAssetKind::Texture) {
            return 0;
        }
        if (kind == NativeAssetKind::Material) {
            return 1;
        }
        if (kind == NativeAssetKind::Mesh || kind == NativeAssetKind::SkeletalMesh) {
            return 2;
        }
        return 3;
    };
    std::sort(paths.begin(), paths.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
        const int ar = runtimePayloadSortRank(a);
        const int br = runtimePayloadSortRank(b);
        if (ar != br) {
            return ar < br;
        }
        return a.generic_string() < b.generic_string();
    });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

uint64_t existingFileBytes(const std::vector<std::filesystem::path>& paths) {
    uint64_t total = 0;
    for (const std::filesystem::path& path : paths) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            const uintmax_t size = std::filesystem::file_size(path, ec);
            if (!ec) {
                total += static_cast<uint64_t>(size);
            }
        }
    }
    return total;
}

bool appendCachedPrefabRuntimeAssets(
    const AssetRecord& prefabRecord,
    const std::filesystem::path& root,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& explicitCachePath,
    const AssetRegistry* registry,
    AssetManager& destination,
    PrefabRuntimeBindings& bindings,
    const NativeRuntimeLoadOptions& nativeLoadOptions,
    std::string* error) {
    std::filesystem::path cachePath = explicitCachePath.empty() ? SceneCache::cachePathFor(sourcePath) : explicitCachePath;
    if (!cachePath.is_absolute()) {
        cachePath = root / cachePath;
    }
    const bool sourceExists = std::filesystem::exists(sourcePath);
    if (sourceExists && !SceneCache::isCacheValid(sourcePath, cachePath)) {
        const NativeAssetKind nativeKind = nativeAssetKindFromExtension(cachePath);
        if (nativeKind == NativeAssetKind::Unknown || nativeKind == NativeAssetKind::Package) {
            return false;
        }
    }
    auto cached = SceneCache::load(cachePath);
    if (!cached.has_value()) {
        const NativeAssetKind nativeKind = nativeAssetKindFromExtension(cachePath);
        if (nativeKind == NativeAssetKind::Unknown || nativeKind == NativeAssetKind::Package) {
            return false;
        }

        std::filesystem::path nativeLoadRoot = cachePath;
        const std::filesystem::path parent = cachePath.parent_path();
        const std::filesystem::path groupRoot = parent.parent_path();
        const std::string parentName = lowerAscii(parent.filename().string());
        if (!groupRoot.empty() && (parentName == "meshes" || parentName == "materials" || parentName == "textures")) {
            std::error_code groupEc;
            if (std::filesystem::is_directory(groupRoot, groupEc)) {
                nativeLoadRoot = groupRoot;
            }
        }

        NativeAssetRuntimeLoader loader;
        NativeRuntimeLoadOptions loadOptions = nativeLoadOptions;
        loadOptions.retainLoadedPayloadsInReport = false;
        loadOptions.looseFileAllowList = nativeRuntimeCacheAllowListForRecord(prefabRecord, root, registry);
        NativeRuntimeLoadReport loadReport = loader.loadLooseRoot(nativeLoadRoot, &destination, loadOptions);
        std::unordered_map<std::string, AssetGuid> registryGuidByCachePath;
        if (registry != nullptr) {
            registryGuidByCachePath.reserve(registry->records().size());
            for (const AssetRecord& record : registry->records()) {
                if ((record.type != AssetType::Mesh && record.type != AssetType::Material) || record.cachePath.empty()) {
                    continue;
                }
                std::filesystem::path recordCachePath = record.cachePath;
                if (!recordCachePath.is_absolute()) {
                    recordCachePath = root / recordCachePath;
                }
                registryGuidByCachePath.emplace(normalizedPathForCompare(recordCachePath).generic_string(), record.guid);
            }
        }
        auto registryGuidForNativeAsset = [&](const NativeRuntimeLoadedAsset& asset) -> AssetGuid {
            if (registry == nullptr || asset.path.empty()) {
                return asset.guid;
            }
            const AssetType expectedType = asset.kind == NativeAssetKind::Mesh
                ? AssetType::Mesh
                : (asset.kind == NativeAssetKind::Material ? AssetType::Material : AssetType::Unknown);
            if (expectedType == AssetType::Unknown) {
                return asset.guid;
            }
            (void)expectedType;
            const auto guidIt = registryGuidByCachePath.find(normalizedPathForCompare(asset.path).generic_string());
            if (guidIt != registryGuidByCachePath.end()) {
                return guidIt->second;
            }
            return asset.guid;
        };
        for (const NativeRuntimeLoadedAsset& asset : loadReport.assets) {
            if (!asset.ok || asset.guid.empty()) {
                continue;
            }
            const AssetGuid bindingGuid = registryGuidForNativeAsset(asset);
            if (bindingGuid.empty()) {
                continue;
            }
            if (asset.kind == NativeAssetKind::Mesh && asset.meshHandle.valid()) {
                bindings.meshes[bindingGuid] = asset.meshHandle;
            } else if (asset.kind == NativeAssetKind::Material && asset.materialHandle.valid()) {
                bindings.materials[bindingGuid] = asset.materialHandle;
            }
        }
        if (loadReport.ok && (!bindings.meshes.empty() || !bindings.materials.empty())) {
            std::cout << "Native runtime assets restored for placement: " << nativeLoadRoot.string()
                      << " meshes=" << loadReport.meshCount
                      << " materials=" << loadReport.materialCount
                      << " textures=" << loadReport.textureCount;
            if (!loadOptions.looseFileAllowList.empty()) {
                std::cout << " filteredFiles=" << loadOptions.looseFileAllowList.size();
            }
            std::cout << '\n';
            return true;
        }
        if (error != nullptr) {
            *error = "Native runtime asset load failed: " + nativeLoadRoot.string();
            if (!loadReport.errors.empty()) {
                *error += " " + loadReport.errors.front().message;
            }
        }
        return false;
    }

    std::vector<TextureAssetHandle> textures;
    textures.reserve(cached->textures.size());
    for (CachedTextureData& cachedTex : cached->textures) {
        TextureAsset texture;
        texture.name = cachedTex.name;
        texture.sourcePath = cachedTex.sourcePath.empty() ? sourcePath : std::filesystem::path(cachedTex.sourcePath);
        texture.width = cachedTex.width;
        texture.height = cachedTex.height;
        texture.channels = cachedTex.channels;
        texture.sourceArrayLayers = cachedTex.sourceArrayLayers;
        texture.sourceDepth = cachedTex.sourceDepth;
        texture.sourceFaceCount = cachedTex.sourceFaceCount;
        texture.sourceIsCubemap = cachedTex.sourceIsCubemap;
        texture.mipLevels = cachedTex.mipLevels;
        texture.srgb = cachedTex.srgb;
        texture.fallback = cachedTex.fallback;
        texture.isCompressed = cachedTex.isCompressed;
        texture.linearColorSpace = cachedTex.linearColorSpace;
        texture.format = static_cast<VkFormat>(cachedTex.format);
        texture.compressedFormat = static_cast<VkFormat>(cachedTex.compressedFormat);
        texture.rgba8 = std::move(cachedTex.rgba8);
        texture.mipData = std::move(cachedTex.mipData);
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
        material.nestedPriority = cachedMat.nestedPriority;
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
        material.opacityTexture = textureHandleFor(cachedMat.opacityTextureIndex);
        material.heightTexture = textureHandleFor(cachedMat.heightTextureIndex);
        material.heightScale = cachedMat.heightScale;
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
        material.materialWorkflow = cachedMat.materialWorkflow;
        material.normalMapConvention = cachedMat.normalMapConvention;
        material.specularTextureAlphaMode = cachedMat.specularTextureAlphaMode;
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
    for (CachedMeshData& cachedMesh : cached->meshes) {
        MeshAsset mesh;
        mesh.name = cachedMesh.name;
        mesh.vertices = std::move(cachedMesh.vertices);
        mesh.indices = std::move(cachedMesh.indices);
        mesh.defaultMorphWeights = std::move(cachedMesh.defaultMorphWeights);
        for (CachedPrimitiveData& cachedPrim : cachedMesh.primitives) {
            MeshPrimitiveAsset primitive;
            primitive.firstVertex = cachedPrim.firstVertex;
            primitive.vertexCount = cachedPrim.vertexCount;
            primitive.firstIndex = cachedPrim.firstIndex;
            primitive.indexCount = cachedPrim.indexCount;
            primitive.morphTargets = std::move(cachedPrim.morphTargets);
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

std::optional<glm::vec3> vec3FromJsonArray(const nlohmann::json& value) {
    if (!value.is_array() || value.size() < 3u) {
        return std::nullopt;
    }
    auto component = [](const nlohmann::json& item) {
        return item.is_number() ? item.get<float>() : 0.0f;
    };
    return glm::vec3{
        component(value[0]),
        component(value[1]),
        component(value[2]),
    };
}

std::optional<Transform> transformFromJsonObject(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    Transform transform;
    if (const auto position = vec3FromJsonArray(value.value("position", nlohmann::json::array()))) {
        transform.position = *position;
    } else {
        return std::nullopt;
    }
    if (const auto rotation = vec3FromJsonArray(value.value("rotationEuler", nlohmann::json::array()))) {
        transform.rotationEuler = *rotation;
    }
    if (const auto scale = vec3FromJsonArray(value.value("scale", nlohmann::json::array()))) {
        transform.scale = *scale;
    }
    transform.dirty = true;
    return transform;
}

std::optional<Transform> usdMeshPlacementTransformForRecord(const AssetRecord& record, const std::filesystem::path& root) {
    const std::filesystem::path importedPath = resolveAssetRecordPath(record, root);
    if (importedPath.empty()) {
        return std::nullopt;
    }
    try {
        std::ifstream file(importedPath);
        if (!file.is_open()) {
            return std::nullopt;
        }
        nlohmann::json json;
        file >> json;
        const nlohmann::json runtimePayload = json.value("runtimePayload", nlohmann::json::object());
        const nlohmann::json sourcePrimTransform = runtimePayload.value("sourcePrimTransform", nlohmann::json::object());
        if (const auto transform = transformFromJsonObject(sourcePrimTransform.value("worldPlacementTransform", nlohmann::json::object()))) {
            return transform;
        }
        if (const auto transform = transformFromJsonObject(sourcePrimTransform.value("placementTransform", nlohmann::json::object()))) {
            return transform;
        }
        const nlohmann::json sidecarSourcePrimTransform = json.value("sourcePrimTransform", nlohmann::json::object());
        if (const auto transform = transformFromJsonObject(sidecarSourcePrimTransform.value("worldPlacementTransform", nlohmann::json::object()))) {
            return transform;
        }
        if (const auto transform = transformFromJsonObject(sidecarSourcePrimTransform.value("placementTransform", nlohmann::json::object()))) {
            return transform;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<nlohmann::json> runtimePayloadForImportedRecord(const AssetRecord& record, const std::filesystem::path& root) {
    const std::filesystem::path importedPath = resolveAssetRecordPath(record, root);
    if (importedPath.empty()) {
        return std::nullopt;
    }
    try {
        std::ifstream file(importedPath);
        if (!file.is_open()) {
            return std::nullopt;
        }
        nlohmann::json json;
        file >> json;
        nlohmann::json runtimePayload = json.value("runtimePayload", nlohmann::json::object());
        return runtimePayload.is_object() ? std::optional<nlohmann::json>{std::move(runtimePayload)} : std::nullopt;
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<Transform> usdRuntimeEntityTransform(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json transform = value.value("transform", nlohmann::json::object());
    if (const auto world = transformFromJsonObject(transform.value("worldPlacementTransform", nlohmann::json::object()))) {
        return world;
    }
    if (const auto local = transformFromJsonObject(transform.value("placementTransform", nlohmann::json::object()))) {
        return local;
    }
    return std::nullopt;
}

std::string usdRuntimeEntityName(const nlohmann::json& value, std::string_view fallbackPrefix, size_t index) {
    std::string name = value.value("name", std::string{});
    if (name.empty()) {
        name = value.value("primPath", std::string{});
        const size_t slash = name.find_last_of('/');
        if (slash != std::string::npos && slash + 1u < name.size()) {
            name = name.substr(slash + 1u);
        }
    }
    if (name.empty()) {
        name = std::string(fallbackPrefix) + " " + std::to_string(index + 1u);
    }
    return name;
}

std::string usdPrimNameFromPath(std::string path, std::string_view fallbackPrefix, size_t index) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash + 1u < path.size()) {
        path = path.substr(slash + 1u);
    }
    if (path.empty()) {
        path = std::string(fallbackPrefix) + " " + std::to_string(index + 1u);
    }
    return path;
}

std::optional<Transform> usdPrimLocalTransform(const nlohmann::json& primJson) {
    if (!primJson.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json transform = primJson.value("transform", nlohmann::json::object());
    return transformFromJsonObject(transform.value("placementTransform", nlohmann::json::object()));
}

std::unordered_map<std::string, nlohmann::json> usdPrimMetadataByPath(const nlohmann::json& runtimePayload) {
    std::unordered_map<std::string, nlohmann::json> result;
    const nlohmann::json usdStageImport = runtimePayload.value("usdStageImport", nlohmann::json::object());
    const nlohmann::json prims = usdStageImport.value("prims", nlohmann::json::array());
    if (!prims.is_array()) {
        return result;
    }
    for (const nlohmann::json& prim : prims) {
        if (!prim.is_object()) {
            continue;
        }
        const std::string path = prim.value("path", std::string{});
        if (!path.empty()) {
            result[path] = prim;
        }
    }
    return result;
}

bool usdPrimRuntimeRenderable(const nlohmann::json& primJson) {
    if (!primJson.is_object()) {
        return true;
    }
    if (!primJson.value("active", true) || !primJson.value("visible", true)) {
        return false;
    }
    const std::string purpose = lowerAscii(primJson.value("purpose", std::string{}));
    return purpose.empty() || purpose == "default" || purpose == "render";
}

void applyUsdPrimRuntimeCulling(Entity& entity, const nlohmann::json& primJson) {
    const bool renderable = usdPrimRuntimeRenderable(primJson);
    if (entity.meshRenderer.has_value()) {
        MeshRenderer& renderer = *entity.meshRenderer;
        renderer.visible = renderable;
        renderer.visibleToCamera = renderable;
        renderer.castShadow = renderable;
        renderer.receiveShadow = renderable;
    }
    if (entity.light.has_value()) {
        entity.light->enabled = entity.light->enabled && renderable;
        entity.light->visibleToCamera = entity.light->visibleToCamera && renderable;
    }
}

std::string usdPrimPathEntityTag(const std::string& path) {
    return "usdPrimPath:" + path;
}

bool entityHasTag(const Entity& entity, const std::string& tag) {
    return std::find(entity.tags.begin(), entity.tags.end(), tag) != entity.tags.end();
}

void addEntityTagIfMissing(Entity& entity, const std::string& tag) {
    if (!entityHasTag(entity, tag)) {
        entity.tags.push_back(tag);
    }
}

EntityId findEntityWithTag(SceneDocument& document, const std::string& tag) {
    for (Entity* entity : document.registry().entities()) {
        if (entity != nullptr && entityHasTag(*entity, tag)) {
            return entity->id;
        }
    }
    return {};
}

std::optional<nlohmann::json> importedRecordJson(const AssetRecord& record, const std::filesystem::path& root) {
    const std::filesystem::path importedPath = resolveAssetRecordPath(record, root);
    if (importedPath.empty()) {
        return std::nullopt;
    }
    try {
        std::ifstream file(importedPath);
        if (!file.is_open()) {
            return std::nullopt;
        }
        nlohmann::json json;
        file >> json;
        return json;
    } catch (...) {
    }
    return std::nullopt;
}

std::string usdMeshSourcePrimPathForRecord(const AssetRecord& record, const std::filesystem::path& root) {
    const std::optional<nlohmann::json> json = importedRecordJson(record, root);
    if (!json.has_value() || !json->is_object()) {
        return {};
    }
    const nlohmann::json runtimePayload = json->value("runtimePayload", nlohmann::json::object());
    std::string path = runtimePayload.value("sourcePrimPath", std::string{});
    if (path.empty()) {
        path = json->value("sourcePrimPath", std::string{});
    }
    return path;
}

std::optional<Transform> usdMeshLocalPlacementTransformForRecord(const AssetRecord& record, const std::filesystem::path& root) {
    const std::optional<nlohmann::json> json = importedRecordJson(record, root);
    if (!json.has_value() || !json->is_object()) {
        return std::nullopt;
    }
    const nlohmann::json runtimePayload = json->value("runtimePayload", nlohmann::json::object());
    const nlohmann::json runtimeTransform = runtimePayload.value("sourcePrimTransform", nlohmann::json::object());
    if (const auto transform = transformFromJsonObject(runtimeTransform.value("placementTransform", nlohmann::json::object()))) {
        return transform;
    }
    const nlohmann::json sidecarTransform = json->value("sourcePrimTransform", nlohmann::json::object());
    return transformFromJsonObject(sidecarTransform.value("placementTransform", nlohmann::json::object()));
}

Camera cameraFromUsdRuntimeJson(const nlohmann::json& value, bool active) {
    Camera camera;
    camera.projection = value.value("cameraProjection", 0u);
    camera.verticalFovRadians = value.value("cameraYfov", camera.verticalFovRadians);
    camera.aspectRatio = value.value("cameraAspectRatio", camera.aspectRatio);
    camera.orthographicXmag = value.value("cameraOrthoXmag", camera.orthographicXmag);
    camera.orthographicYmag = value.value("cameraOrthoYmag", camera.orthographicYmag);
    camera.nearPlane = value.value("cameraNear", camera.nearPlane);
    camera.farPlane = value.value("cameraFar", camera.farPlane);
    camera.active = active;
    return camera;
}

Light lightFromUsdRuntimeJson(const nlohmann::json& value) {
    Light light;
    const uint32_t type = std::min(value.value("lightType", 1u), 3u);
    light.type = static_cast<LightType>(type);
    if (const auto color = vec3FromJsonArray(value.value("color", nlohmann::json::array()))) {
        light.color = *color;
    }
    light.intensity = value.value("intensity", light.intensity);
    light.sizeOrRadius = value.value("sizeOrRadius", light.sizeOrRadius);
    light.innerConeRadians = value.value("innerConeRadians", light.innerConeRadians);
    light.outerConeRadians = value.value("outerConeRadians", light.outerConeRadians);
    light.enabled = value.value("enabled", light.enabled);
    return light;
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
    const NativeRuntimeLoadOptions& nativeLoadOptions,
    std::string* error) {
    if (populatePrefabBindingsFromLoadedAssets(prefab, destination, bindings)) {
        return true;
    }
    const std::filesystem::path sourcePath = resolveAssetSourcePath(prefabRecord, root);
    if (appendCachedPrefabRuntimeAssets(prefabRecord, root, sourcePath, prefab.runtimeCachePath, registry, destination, bindings, nativeLoadOptions, error)) {
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

class SceneAndNativeAssetAppendReloadCommand final : public ICommand {
public:
    SceneAndNativeAssetAppendReloadCommand(
        SceneDocument& document,
        AssetManager& assets,
        const AssetRegistry& registry,
        AssetRecord record,
        std::filesystem::path root,
        std::optional<PrefabAsset> prefab,
        SceneDocument beforeDocument,
        AssetLoadStats beforeAssetStats,
        SceneDocument afterDocument,
        NativeRuntimeLoadOptions nativeLoadOptions,
        SceneUpdateKind updateKind,
        std::string label)
        : document_(document),
          assets_(assets),
          registry_(registry),
          record_(std::move(record)),
          root_(std::move(root)),
          prefab_(std::move(prefab)),
          beforeDocument_(std::move(beforeDocument)),
          beforeAssetStats_(beforeAssetStats),
          afterDocument_(std::move(afterDocument)),
          nativeLoadOptions_(nativeLoadOptions),
          updateKind_(updateKind),
          label_(std::move(label)) {}

    void undo() override {
        document_ = beforeDocument_;
        assets_.truncateTo(beforeAssetStats_);
        document_.markDirty(updateKind_);
    }

    void redo() override {
        assets_.truncateTo(beforeAssetStats_);
        PrefabRuntimeBindings bindings;
        std::string error;
        NativeRuntimeLoadOptions loadOptions = nativeLoadOptions_;
        loadOptions.retainLoadedPayloadsInReport = false;
        const bool restored = prefab_.has_value()
            ? appendPrefabRuntimeAssets(record_, *prefab_, root_, &registry_, assets_, bindings, loadOptions, &error)
            : appendCachedPrefabRuntimeAssets(
                record_,
                root_,
                resolveAssetSourcePath(record_, root_),
                resolveAssetCachePath(record_, root_),
                &registry_,
                assets_,
                bindings,
                loadOptions,
                &error);
        if (!restored) {
            document_ = beforeDocument_;
            document_.markDirty(updateKind_);
            std::cerr << "Redo failed for " << label_ << ": " << error << '\n';
            return;
        }
        SceneDocument reboundDocument = afterDocument_;
        (void)rebindGuidBackedRenderers(reboundDocument, bindings);
        document_ = std::move(reboundDocument);
        document_.markDirty(updateKind_);
    }

    [[nodiscard]] const std::string& label() const override { return label_; }

private:
    SceneDocument& document_;
    AssetManager& assets_;
    const AssetRegistry& registry_;
    AssetRecord record_;
    std::filesystem::path root_;
    std::optional<PrefabAsset> prefab_;
    SceneDocument beforeDocument_;
    AssetLoadStats beforeAssetStats_{};
    SceneDocument afterDocument_;
    NativeRuntimeLoadOptions nativeLoadOptions_;
    SceneUpdateKind updateKind_ = SceneUpdateKind::TopologyChanged;
    std::string label_;
};

bool sceneHasUsdRuntimePlacement(
    SceneDocument& document,
    const std::vector<EditorMeshAssetPlacement>& meshPlacements,
    const std::optional<nlohmann::json>& runtimePayload) {
    std::unordered_set<AssetGuid> meshGuids;
    meshGuids.reserve(meshPlacements.size());
    for (const EditorMeshAssetPlacement& placement : meshPlacements) {
        if (!placement.meshGuid.empty()) {
            meshGuids.insert(placement.meshGuid);
        }
    }
    if (!meshGuids.empty()) {
        for (Entity* entity : document.registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            if (!renderer.meshGuid.empty() && meshGuids.find(renderer.meshGuid) != meshGuids.end()) {
                return true;
            }
        }
    }

    if (!runtimePayload.has_value() || !runtimePayload->is_object()) {
        return false;
    }
    const nlohmann::json usdStageImport = runtimePayload->value("usdStageImport", nlohmann::json::object());
    const nlohmann::json prims = usdStageImport.value("prims", nlohmann::json::array());
    if (!prims.is_array()) {
        return false;
    }
    for (const nlohmann::json& prim : prims) {
        if (!prim.is_object()) {
            continue;
        }
        const std::string path = prim.value("path", std::string{});
        if (!path.empty() && findEntityWithTag(document, usdPrimPathEntityTag(path)).valid()) {
            return true;
        }
    }
    return false;
}

bool attachUsdRuntimeAnimationPlayer(
    SceneDocument& document,
    const nlohmann::json& runtimePayload,
    const std::filesystem::path& root) {
    const nlohmann::json controllers = runtimePayload.value("animationControllerAssets", nlohmann::json::array());
    const nlohmann::json animations = runtimePayload.value("animationAssets", nlohmann::json::array());
    if (!controllers.is_array() || controllers.empty() || !animations.is_array() || animations.empty()) {
        return false;
    }
    const nlohmann::json controller = controllers.front();
    const nlohmann::json animation = animations.front();
    const AssetGuid controllerGuid = controller.value("guid", std::string{});
    const AssetGuid animationGuid = animation.value("guid", std::string{});
    if (controllerGuid.empty() || animationGuid.empty()) {
        return false;
    }
    EntityId target{};
    const nlohmann::json usdStageImport = runtimePayload.value("usdStageImport", nlohmann::json::object());
    const nlohmann::json rootPrims = usdStageImport.value("rootPrims", nlohmann::json::array());
    if (rootPrims.is_array()) {
        for (const nlohmann::json& item : rootPrims) {
            if (!item.is_string()) {
                continue;
            }
            target = findEntityWithTag(document, usdPrimPathEntityTag(item.get<std::string>()));
            if (target.valid()) {
                break;
            }
        }
    }
    if (!target.valid()) {
        for (Entity* entity : document.registry().entities()) {
            if (entity != nullptr && std::any_of(entity->tags.begin(), entity->tags.end(), [](const std::string& tag) {
                return tag.rfind("usdPrimPath:", 0) == 0;
            })) {
                target = entity->id;
                break;
            }
        }
    }
    Entity* entity = document.registry().entity(target);
    if (entity == nullptr) {
        return false;
    }
    AnimationPlayer player;
    player.animationGuid = animationGuid;
    player.controllerGuid = controllerGuid;
    const std::string animationPath = animation.value("path", std::string{});
    const std::string controllerPath = controller.value("path", std::string{});
    if (!animationPath.empty()) {
        player.animationPath = root / animationPath;
    }
    if (!controllerPath.empty()) {
        player.controllerPath = root / controllerPath;
    }
    player.enabled = true;
    player.playOnStart = true;
    player.playing = true;
    player.loop = true;
    player.applyRootMotion = false;
    player.applyMorphWeights = true;
    entity->animationPlayer = std::move(player);
    return true;
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
    std::optional<std::filesystem::path> nativePackageScenePath,
    NativePackageAnimationSelection nativePackageAnimationSelection,
    std::optional<bool> denoiserOverride,
    std::optional<RestirMode> restirModeOverride,
    std::optional<RenderPreset> renderPresetOverride,
    std::optional<bool> restirGiOverride,
    std::optional<bool> opacityMicromapOverride,
    std::optional<bool> opacityMicromapBlendOverride,
    std::optional<bool> hardwareBackfaceCullingOverride,
    std::optional<uint32_t> opacityMicromapSubdivisionOverride,
    bool debugViewOverride,
    bool validationCameraMotion,
    bool validationObjectMotion,
    bool headless,
    ApplicationMode mode,
    uint32_t headlessWidth,
    uint32_t headlessHeight,
    bool disableAsyncCompute,
    bool singleQueueFallback,
    bool disableResourceAliasing,
    StreamingRuntimeOptions streamingOptions)
    : debugView_(debugView),
      gltfPath_(std::move(gltfPath)),
      hdrPath_(std::move(hdrPath)),
      scenePath_(std::move(scenePath)),
      nativePackageScenePath_(std::move(nativePackageScenePath)),
      nativePackageAnimationSelection_(std::move(nativePackageAnimationSelection)),
      denoiserOverride_(denoiserOverride),
      restirModeOverride_(restirModeOverride),
      renderPresetOverride_(renderPresetOverride),
      restirGiOverride_(restirGiOverride),
      opacityMicromapOverride_(opacityMicromapOverride),
      opacityMicromapBlendOverride_(opacityMicromapBlendOverride),
      hardwareBackfaceCullingOverride_(hardwareBackfaceCullingOverride),
      opacityMicromapSubdivisionOverride_(opacityMicromapSubdivisionOverride),
      debugViewOverride_(debugViewOverride),
      validationCameraMotion_(validationCameraMotion),
      validationObjectMotion_(validationObjectMotion),
      disableAsyncCompute_(disableAsyncCompute),
      singleQueueFallback_(singleQueueFallback),
      disableResourceAliasing_(disableResourceAliasing),
      streamingOptions_(streamingOptions),
      mode_(headless ? ApplicationMode::Headless : mode),
      headless_(headless || mode == ApplicationMode::Headless),
      rendererOnly_(mode == ApplicationMode::RendererOnly && !headless),
      headlessExtent_{std::max(headlessWidth, 1u), std::max(headlessHeight, 1u)} {
    streamingRuntimeState_.setOptions(streamingOptions_);
    if (!headless_) {
        initWindow();
    }
    initVulkan();
    (void)initializeNsightGraphicsRuntime();
    frameWorkProbeJobId_ = frameWorkScheduler_.enqueue(FrameWorkJobDesc{
        .queue = FrameWorkQueue::MainThreadApply,
        .title = "Editor main loop scheduler probe",
        .status = "waiting for fence",
        .estimatedCostMs = 0.0,
        .estimatedUploadBytes = 0,
        .callback = [](FrameWorkJobContext&) {
            return FrameWorkJobStepResult{
                .complete = false,
                .waitingForFence = true,
                .progress = 0.25f,
            };
        },
    });
    frameWorkProbeCompletionPending_ = frameWorkProbeJobId_ != 0;
    initializeEditorTicketProbeQueues();
}

Application::~Application() {
    std::cerr << "[shutdown] Application destructor begin\n";

    // Phase 1: Streaming runtime shutdown
    shutdownStreamingRuntime();
    std::cerr << "[shutdown] streaming runtime drained\n";
    asyncSceneLoader_.requestCancel();
    asyncSceneLoader_.wait();
    waitForAssetImportWorker();

    // Phase 2: Stop GPU work producers
    if (streamingGpuTransferExecutorReady_) {
        streamingGpuTransferExecutor_.shutdown();
        streamingGpuTransferExecutorReady_ = false;
        std::cerr << "[shutdown] transfer executor stopped\n";
    }

    // Phase 3: Wait idle (non-throwing)
    if (commandSystem_) {
        try {
            commandSystem_->waitIdle();
            std::cerr << "[shutdown] command system idle OK\n";
        } catch (const std::exception& e) {
            std::cerr << "[shutdown] command system waitIdle threw: " << e.what() << " -- continuing teardown\n";
        } catch (...) {
            std::cerr << "[shutdown] command system waitIdle threw unknown -- continuing teardown\n";
        }
    }

    // Phase 4: Drain GPU resources (queues idle, safe to destroy)
    streamingGpuBufferUploadPayloads_.clear();
    streamingGpuImageMipUploadPayloads_.clear();
    streamingGpuBlasBuildPayloads_.clear();
    streamingGpuBlasCompactionPayloads_.clear();
    streamingGpuWorkTimelineMarkers_.clear();
    streamingGpuWorkQueue_ = StreamingGpuWorkQueue();
    streamingGpuSceneUpdateQueue_ = IncrementalGpuSceneUpdateQueue();
    nativeGpuAssetCache_.clear();
    std::cerr << "[shutdown] GPU payloads + cache drained\n";

    if (uiOverlay_) {
        (void)saveActiveEditorPreferences();
    }
    writeCrashMarker(false);
    if (commandSystem_) {
        commandSystem_->setPathTracer(nullptr);
    }
    retiredPathTracers_.clear();
    shutdownNsightPerfMarkers();
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

void Application::run(uint32_t maxFrames, uint32_t warmupFrames, bool collectProfile) {
    interactiveProfileCollectionEnabled_ = collectProfile;
    if (interactiveProfileCollectionEnabled_) {
        warmupFrameCount_ = warmupFrames;
        totalFrameCount_ = maxFrames;
        cpuFrameTimings_.clear();
        gpuFrameTimings_.clear();
        perFrameGpuTimings_.clear();
        if (maxFrames > 0u) {
            cpuFrameTimings_.reserve(maxFrames);
            gpuFrameTimings_.reserve(maxFrames);
            perFrameGpuTimings_.reserve(maxFrames);
        }
    }
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
        if (frameCount == warmupFrames && uploader_ != nullptr) {
            captureReadyImageUploadCount_ = uploader_->stats().imageUploadCount;
            captureReadyUploadSnapshotValid_ = true;
            captureReadyFrameSerial_ = frameSerial_;
        }

        const float rawDeltaSeconds = 1.0f / 60.0f;
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;
        stepEditorTicketProbeQueues();
        stepStreamingGpuWorkQueue();
        stepStreamingGpuSceneUpdateQueue();
        pollProgressiveRuntimeLoadJob();
        applyValidationObjectMotion(nextDiagnosticFrameIndex_);
        applyValidationCameraMotion(nextDiagnosticFrameIndex_++);
        updateAnimationPlayers(deltaSeconds);
        if (beginFrameCapture_) {
            beginFrameCapture_(frameCount + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        if (pathTracer_) {
            updateFrameWorkAccelerationStructureBudgetFeedback(pathTracer_->timings());
        }
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
    uint32_t nvPerfDrainFrames = 0;
    constexpr uint32_t kMaxNvPerfDrainFrames = 256;
    while (nsightPerfReportNeedsMoreFrames() && nvPerfDrainFrames < kMaxNvPerfDrainFrames) {
        const float deltaSeconds = 1.0f / 60.0f;
        commandSystem_->drawFrame(seconds, deltaSeconds);
        seconds += deltaSeconds;
        ++frameSerial_;
        ++nvPerfDrainFrames;
    }
    if (nsightPerfReportNeedsMoreFrames()) {
        std::cerr << "Warning: Nsight Perf report collection did not finish within "
                  << kMaxNvPerfDrainFrames << " drain frames.\n";
    }
    commandSystem_->waitIdle();
    if (savePresentFramePath_.has_value() && !initialPresentFrameSaveComplete_) {
        initialPresentFrameSaveComplete_ = savePresentFrame(*savePresentFramePath_);
    }
}

void Application::renderFrames(uint32_t count) {
    float seconds = lastFrameSeconds_ + 1.0f / 60.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const float rawDeltaSeconds = 1.0f / 60.0f;
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;
        stepEditorTicketProbeQueues();
        stepStreamingGpuWorkQueue();
        stepStreamingGpuSceneUpdateQueue();
        pollProgressiveRuntimeLoadJob();
        applyValidationObjectMotion(nextDiagnosticFrameIndex_);
        applyValidationCameraMotion(nextDiagnosticFrameIndex_++);
        updateAnimationPlayers(deltaSeconds);
        if (beginFrameCapture_) {
            beginFrameCapture_(i + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        if (pathTracer_) {
            updateFrameWorkAccelerationStructureBudgetFeedback(pathTracer_->timings());
        }
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(i + 1u);
        }
        seconds += deltaSeconds;
    }
    commandSystem_->waitIdle();
}

std::vector<GpuUploadTicketSnapshot> Application::editorGpuUploadTicketSnapshots(bool includeChunks) const {
    std::vector<GpuUploadTicketSnapshot> snapshots = editorGpuUploadTickets_.snapshots(includeChunks);
    if (uploader_ != nullptr) {
        std::vector<GpuUploadTicketSnapshot> liveUploadSnapshots = uploader_->uploadTicketSnapshots(includeChunks);
        snapshots.insert(
            snapshots.end(),
            std::make_move_iterator(liveUploadSnapshots.begin()),
            std::make_move_iterator(liveUploadSnapshots.end()));
    }
    return snapshots;
}

uint64_t Application::editorGpuUploadNextTimelineValue() const {
    uint64_t nextTimeline = editorGpuUploadTickets_.nextTimelineValue();
    if (uploader_ != nullptr) {
        nextTimeline = std::max(nextTimeline, uploader_->uploadTicketNextTimelineValue());
    }
    return nextTimeline;
}

std::vector<MainThreadApplyTicketSnapshot> Application::editorMainThreadApplyTicketSnapshots(bool includeOperations) const {
    return editorMainThreadApplyTickets_.snapshots(includeOperations);
}

std::vector<TopologyRebuildTicketSnapshot> Application::editorTopologyRebuildTicketSnapshots(bool includeStages) const {
    return editorTopologyRebuildTickets_.snapshots(includeStages);
}

namespace {

rtv::NativeGpuAssetKind nativeGpuAssetKindForCatalogEntry(const rtv::NativeAssetCatalogEntry& entry) {
    switch (entry.nativeKind) {
    case rtv::NativeAssetKind::Mesh:
    case rtv::NativeAssetKind::SkeletalMesh:
        return rtv::NativeGpuAssetKind::Mesh;
    case rtv::NativeAssetKind::Texture:
        return rtv::NativeGpuAssetKind::Texture;
    case rtv::NativeAssetKind::Material:
        return rtv::NativeGpuAssetKind::Material;
    default:
        break;
    }
    switch (entry.assetType) {
    case rtv::AssetType::Mesh:
        return rtv::NativeGpuAssetKind::Mesh;
    case rtv::AssetType::Texture:
        return rtv::NativeGpuAssetKind::Texture;
    case rtv::AssetType::Material:
        return rtv::NativeGpuAssetKind::Material;
    default:
        return rtv::NativeGpuAssetKind::Material;
    }
}

uint64_t estimatedMeshUploadBytes(const rtv::MeshAsset& mesh) {
    const uint64_t vertexBytes = static_cast<uint64_t>(mesh.vertices.size()) * static_cast<uint64_t>(sizeof(rtv::MeshVertex));
    const uint64_t indexBytes = static_cast<uint64_t>(mesh.indices.size()) * static_cast<uint64_t>(sizeof(uint32_t));
    return vertexBytes + indexBytes;
}

std::vector<uint8_t> meshUploadPayloadChunk(const rtv::MeshAsset& mesh, uint64_t byteOffset, uint64_t byteCount) {
    const uint64_t totalBytes = estimatedMeshUploadBytes(mesh);
    if (byteCount == 0 || byteOffset >= totalBytes) {
        return {};
    }
    byteCount = std::min(byteCount, totalBytes - byteOffset);
    std::vector<uint8_t> out(static_cast<size_t>(byteCount));

    const uint64_t vertexBytes = static_cast<uint64_t>(mesh.vertices.size()) * static_cast<uint64_t>(sizeof(rtv::MeshVertex));
    const auto copyRange = [&](uint64_t srcStart, uint64_t srcSize, const void* srcData) {
        const uint64_t srcEnd = srcStart + srcSize;
        const uint64_t dstEnd = byteOffset + byteCount;
        const uint64_t overlapBegin = std::max(byteOffset, srcStart);
        const uint64_t overlapEnd = std::min(dstEnd, srcEnd);
        if (overlapBegin >= overlapEnd) {
            return;
        }
        const uint64_t dstOffset = overlapBegin - byteOffset;
        const uint64_t srcOffset = overlapBegin - srcStart;
        std::memcpy(
            out.data() + static_cast<size_t>(dstOffset),
            static_cast<const uint8_t*>(srcData) + static_cast<size_t>(srcOffset),
            static_cast<size_t>(overlapEnd - overlapBegin));
    };

    if (!mesh.vertices.empty()) {
        copyRange(0, vertexBytes, mesh.vertices.data());
    }
    if (!mesh.indices.empty()) {
        copyRange(vertexBytes, totalBytes - vertexBytes, mesh.indices.data());
    }
    return out;
}

uint64_t estimatedTextureUploadBytes(const rtv::TextureAsset& texture) {
    uint64_t bytes = 0;
    for (const rtv::TextureMipLevel& mip : texture.mipData) {
        bytes += mip.size;
    }
    if (bytes != 0) {
        return bytes;
    }
    if (!texture.rgba8.empty()) {
        return static_cast<uint64_t>(texture.rgba8.size());
    }
    const uint64_t channelBytes = std::max<uint32_t>(1u, texture.channels);
    return static_cast<uint64_t>(std::max<uint32_t>(1u, texture.width)) *
        static_cast<uint64_t>(std::max<uint32_t>(1u, texture.height)) *
        channelBytes;
}

std::string textureFormatDiagnosticName(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return "R8_UNORM";
    case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case VK_FORMAT_R8G8B8_UNORM: return "R8G8B8_UNORM";
    case VK_FORMAT_R8G8B8_SRGB: return "R8G8B8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R16_UNORM: return "R16_UNORM";
    case VK_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_R16_SFLOAT: return "R16_SFLOAT";
    case VK_FORMAT_R16G16_SFLOAT: return "R16G16_SFLOAT";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R32_SFLOAT: return "R32_SFLOAT";
    case VK_FORMAT_R32G32_SFLOAT: return "R32G32_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1_RGB_UNORM";
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return "BC1_RGB_SRGB";
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM";
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB";
    case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2_UNORM";
    case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2_SRGB";
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB";
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4_UNORM";
    case VK_FORMAT_BC4_SNORM_BLOCK: return "BC4_SNORM";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5_UNORM";
    case VK_FORMAT_BC5_SNORM_BLOCK: return "BC5_SNORM";
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return "BC6H_UFLOAT";
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return "BC6H_SFLOAT";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7_UNORM";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7_SRGB";
    default: return "VkFormat_" + std::to_string(static_cast<uint32_t>(format));
    }
}

uint32_t compressedFormatBlockBytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return 8u;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return 16u;
    default:
        return 0u;
    }
}

uint32_t uncompressedFormatTexelBytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SRGB:
        return 1u;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SFLOAT:
        return 2u;
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_R8G8B8_SRGB:
        return 3u;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
        return 4u;
    case VK_FORMAT_R16G16B16_UNORM:
    case VK_FORMAT_R16G16B16_SFLOAT:
        return 6u;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
        return 8u;
    case VK_FORMAT_R32G32B32_SFLOAT:
        return 12u;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16u;
    default:
        return 4u;
    }
}

uint64_t estimateResidentImageBytes(VkFormat format, uint32_t width, uint32_t height, uint32_t mipLevels) {
    if (width == 0u || height == 0u) {
        return 0ull;
    }
    const uint32_t mipCount = std::max(1u, mipLevels);
    const uint32_t blockBytes = compressedFormatBlockBytes(format);
    const bool compressed = blockBytes != 0u;
    const uint32_t texelBytes = uncompressedFormatTexelBytes(format);
    uint64_t bytes = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint32_t mipWidth = std::max(1u, width >> mip);
        const uint32_t mipHeight = std::max(1u, height >> mip);
        if (compressed) {
            const uint64_t blocksWide = (static_cast<uint64_t>(mipWidth) + 3ull) / 4ull;
            const uint64_t blocksHigh = (static_cast<uint64_t>(mipHeight) + 3ull) / 4ull;
            bytes += blocksWide * blocksHigh * blockBytes;
        } else {
            bytes += static_cast<uint64_t>(mipWidth) * static_cast<uint64_t>(mipHeight) * texelBytes;
        }
    }
    return bytes;
}

std::vector<uint8_t> textureMipUploadPayload(const rtv::TextureAsset& texture, uint32_t mipLevel) {
    if (mipLevel < texture.mipData.size()) {
        const rtv::TextureMipLevel& mip = texture.mipData[mipLevel];
        if (mip.size != 0 && mip.offset <= texture.rgba8.size() && mip.size <= texture.rgba8.size() - mip.offset) {
            const auto begin = texture.rgba8.begin() + static_cast<std::ptrdiff_t>(mip.offset);
            const auto end = begin + static_cast<std::ptrdiff_t>(mip.size);
            return std::vector<uint8_t>(begin, end);
        }
    }
    if (mipLevel == 0 && !texture.rgba8.empty()) {
        return texture.rgba8;
    }
    return {};
}

uint32_t materialTextureDependencyCount(const rtv::MaterialAsset& material) {
    const rtv::TextureAssetHandle handles[] = {
        material.baseColorTexture,
        material.normalTexture,
        material.metallicRoughnessTexture,
        material.emissiveTexture,
        material.clearcoatTexture,
        material.clearcoatRoughnessTexture,
        material.clearcoatNormalTexture,
        material.transmissionTexture,
        material.volumeThicknessTexture,
        material.specularTexture,
        material.specularColorTexture,
        material.sheenColorTexture,
        material.sheenRoughnessTexture,
        material.iridescenceTexture,
        material.iridescenceThicknessTexture,
        material.anisotropyTexture,
        material.occlusionTexture,
        material.opacityTexture,
        material.heightTexture,
    };
    uint32_t count = 0;
    for (const rtv::TextureAssetHandle handle : handles) {
        if (handle.valid()) {
            ++count;
        }
    }
    return count;
}

void assignStreamingMaterialTextureSlot(rtv::MaterialAsset& material, uint32_t slot, rtv::TextureAssetHandle handle) {
    switch (static_cast<rtv::RtmaterialTextureSlot>(slot)) {
    case rtv::RtmaterialTextureSlot::BaseColor: material.baseColorTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Normal: material.normalTexture = handle; break;
    case rtv::RtmaterialTextureSlot::MetallicRoughness: material.metallicRoughnessTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Occlusion: material.occlusionTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Emissive: material.emissiveTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Transmission: material.transmissionTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Clearcoat: material.clearcoatTexture = handle; break;
    case rtv::RtmaterialTextureSlot::ClearcoatRoughness: material.clearcoatRoughnessTexture = handle; break;
    case rtv::RtmaterialTextureSlot::ClearcoatNormal: material.clearcoatNormalTexture = handle; break;
    case rtv::RtmaterialTextureSlot::VolumeThickness: material.volumeThicknessTexture = handle; break;
    case rtv::RtmaterialTextureSlot::SheenColor: material.sheenColorTexture = handle; break;
    case rtv::RtmaterialTextureSlot::SheenRoughness: material.sheenRoughnessTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Specular: material.specularTexture = handle; break;
    case rtv::RtmaterialTextureSlot::SpecularColor: material.specularColorTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Iridescence: material.iridescenceTexture = handle; break;
    case rtv::RtmaterialTextureSlot::IridescenceThickness: material.iridescenceThicknessTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Anisotropy: material.anisotropyTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Opacity: material.opacityTexture = handle; break;
    case rtv::RtmaterialTextureSlot::Height: material.heightTexture = handle; break;
    }
}

bool materialContributesRestirLightCandidate(const rtv::MaterialAsset& material) {
    const glm::vec3 emissive = material.emissiveFactor * std::max(material.emissiveStrength, 0.0f);
    return emissive.x > 0.0f ||
        emissive.y > 0.0f ||
        emissive.z > 0.0f ||
        material.emissiveTexture.valid();
}

double estimatedBlasBuildMs(const rtv::MeshAsset& mesh) {
    uint64_t primitiveCount = 0;
    for (const rtv::MeshPrimitiveAsset& primitive : mesh.primitives) {
        primitiveCount += static_cast<uint64_t>(primitive.indexCount / 3u);
    }
    return std::clamp(0.15 + static_cast<double>(primitiveCount) / 250000.0, 0.15, 4.0);
}

struct MeshBlasBuildSizing {
    uint64_t accelerationStructureBytes = 0;
    uint64_t scratchBytes = 0;
};

std::optional<MeshBlasBuildSizing> queryMeshBlasBuildSizing(VkDevice device, const rtv::MeshAsset& mesh) {
    if (device == VK_NULL_HANDLE || mesh.vertices.empty() || mesh.indices.size() < 3) {
        return std::nullopt;
    }
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexStride = sizeof(rtv::MeshVertex);
    triangles.maxVertex = static_cast<uint32_t>(mesh.vertices.size() - 1u);
    triangles.indexType = VK_INDEX_TYPE_UINT32;

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        &primitiveCount,
        &sizes);
    if (sizes.accelerationStructureSize == 0 || sizes.buildScratchSize == 0) {
        return std::nullopt;
    }
    return MeshBlasBuildSizing{
        .accelerationStructureBytes = sizes.accelerationStructureSize,
        .scratchBytes = sizes.buildScratchSize,
    };
}

} // namespace

nlohmann::json Application::textureDiagnosticsJson() const {
    const float requestedMaterialAnisotropy = pathTracer_ != nullptr
        ? pathTracer_->settings().materialTextureAnisotropy
        : 0.0f;
    const float effectiveMaterialAnisotropy = pathTracer_ != nullptr
        ? pathTracer_->scene().materialTextureAnisotropy()
        : 0.0f;
    const uint32_t materialTextureCount = pathTracer_ != nullptr
        ? pathTracer_->scene().materialTextureCount()
        : 0u;
    const uint32_t materialTextureSamplerCount = pathTracer_ != nullptr
        ? pathTracer_->scene().materialTextureSamplerCount()
        : 0u;
    const bool anisotropicSamplingEnabled =
        context_ != nullptr &&
        context_->supportsSamplerAnisotropy() &&
        effectiveMaterialAnisotropy > 1.0001f;
    const uint32_t anisotropicTextureCount = anisotropicSamplingEnabled ? materialTextureCount : 0u;
    const uint32_t highAnisotropyTextureCount =
        effectiveMaterialAnisotropy >= 8.0f ? anisotropicTextureCount : 0u;
    nlohmann::json report = {
        {"bound_texture_count", 0},
        {"total_texture_bytes", 0},
        {"total_mip_count", 0},
        {"missing_mip_chain_count", 0},
        {"fallback_missing_texture_count", 0},
        {"oversized_texture_count", 0},
        {"sampler_anisotropy", requestedMaterialAnisotropy},
        {"sampler_anisotropy_summary", {
            {"supported", context_ != nullptr && context_->supportsSamplerAnisotropy()},
            {"requested", requestedMaterialAnisotropy},
            {"effective", effectiveMaterialAnisotropy},
            {"device_max", context_ != nullptr ? context_->maxSamplerAnisotropy() : 1.0f},
            {"bound_texture_count", materialTextureCount},
            {"unique_sampler_count", materialTextureSamplerCount},
            {"anisotropic_texture_count", anisotropicTextureCount},
            {"high_anisotropy_texture_count", highAnisotropyTextureCount},
        }},
        {"streaming_upload_count", 0},
        {"streaming_image_upload_count", 0},
        {"streaming_image_uploads_after_capture_ready", 0},
        {"streaming_image_uploads_during_capture", 0},
        {"capture_upload_snapshot_valid", captureReadyUploadSnapshotValid_},
        {"capture_snapshot_frame", captureReadyFrameSerial_},
        {"runtime_residency", nlohmann::json::object()},
        {"streaming_textures", nlohmann::json::array()},
        {"eviction_history", nlohmann::json::array()},
        {"top_largest_textures", nlohmann::json::array()},
        {"warnings", nlohmann::json::array()},
    };

    struct TextureEntry {
        std::string name;
        std::filesystem::path sourcePath;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipCount = 0;
        uint64_t bytes = 0;
        uint32_t runtimeWidth = 0;
        uint32_t runtimeHeight = 0;
        uint32_t runtimeMipCount = 0;
        uint64_t runtimeBytes = 0;
        VkFormat runtimeFormat = VK_FORMAT_UNDEFINED;
        bool runtimeResident = false;
        bool fallback = false;
        bool compressed = false;
    };
    std::vector<TextureEntry> entries;
    uint64_t totalBytes = 0;
    uint64_t runtimeImportedBytes = 0;
    uint32_t totalMips = 0;
    uint32_t runtimeImportedMips = 0;
    uint32_t runtimeImportedCount = 0;
    uint32_t runtimeImportedResidentCount = 0;
    uint32_t missingMipChains = 0;
    uint32_t fallbackCount = 0;
    uint32_t oversizedCount = 0;
    const VkExtent2D displayExtent = pathTracer_ != nullptr ? pathTracer_->displayExtent() : VkExtent2D{};
    const BindlessTextureTable* residentTextures = pathTracer_ != nullptr
        ? &pathTracer_->scene().materialTextureTable()
        : nullptr;
    const uint32_t oversizedWidth = std::max(1u, displayExtent.width) * 2u;
    const uint32_t oversizedHeight = std::max(1u, displayExtent.height) * 2u;

    if (importedScene_.has_value()) {
        const std::vector<TextureAsset>& sceneTextures = assets_.textures();
        entries.reserve(sceneTextures.size());
        for (size_t textureIndex = 0; textureIndex < sceneTextures.size(); ++textureIndex) {
            const TextureAsset& texture = sceneTextures[textureIndex];
            const uint64_t bytes = estimatedTextureUploadBytes(texture);
            const uint32_t sourceMipCount = texture.mipLevels > 0
                ? static_cast<uint32_t>(texture.mipLevels)
                : static_cast<uint32_t>(texture.mipData.size());
            const BindlessTextureImageInfo resident = residentTextures != nullptr
                ? residentTextures->imageInfo(static_cast<uint32_t>(textureIndex))
                : BindlessTextureImageInfo{};
            const uint32_t mipCount = resident.mipLevels > 0 ? resident.mipLevels : sourceMipCount;
            const bool runtimeResident = resident.width > 0u && resident.height > 0u && resident.format != VK_FORMAT_UNDEFINED;
            const uint32_t runtimeMipCount = runtimeResident ? std::max(1u, resident.mipLevels) : 0u;
            const uint64_t runtimeBytes = runtimeResident
                ? estimateResidentImageBytes(resident.format, resident.width, resident.height, runtimeMipCount)
                : 0ull;
            totalBytes += bytes;
            totalMips += std::max(1u, mipCount);
            ++runtimeImportedCount;
            if (runtimeResident) {
                ++runtimeImportedResidentCount;
                runtimeImportedMips += runtimeMipCount;
                runtimeImportedBytes += runtimeBytes;
            }
            if (std::max(texture.width, texture.height) > 1u && std::max(1u, mipCount) <= 1u) {
                ++missingMipChains;
            }
            if (texture.fallback || (texture.rgba8.empty() && texture.mipData.empty())) {
                ++fallbackCount;
            }
            if (displayExtent.width > 0u && displayExtent.height > 0u &&
                (texture.width > oversizedWidth || texture.height > oversizedHeight)) {
                ++oversizedCount;
            }
            entries.push_back(TextureEntry{
                .name = texture.name.empty() ? texture.sourcePath.filename().string() : texture.name,
                .sourcePath = texture.sourcePath,
                .width = texture.width,
                .height = texture.height,
                .mipCount = std::max(1u, mipCount),
                .bytes = bytes,
                .runtimeWidth = resident.width,
                .runtimeHeight = resident.height,
                .runtimeMipCount = runtimeMipCount,
                .runtimeBytes = runtimeBytes,
                .runtimeFormat = resident.format,
                .runtimeResident = runtimeResident,
                .fallback = texture.fallback,
                .compressed = texture.isCompressed,
            });
        }
    }

    std::sort(entries.begin(), entries.end(), [](const TextureEntry& a, const TextureEntry& b) {
        return a.bytes > b.bytes;
    });
    nlohmann::json top = nlohmann::json::array();
    for (size_t i = 0; i < std::min<size_t>(entries.size(), 8u); ++i) {
        const TextureEntry& entry = entries[i];
        top.push_back({
            {"name", entry.name},
            {"source", entry.sourcePath.generic_string()},
            {"width", entry.width},
            {"height", entry.height},
            {"mip_count", entry.mipCount},
            {"estimated_bytes", entry.bytes},
            {"runtime_resident", entry.runtimeResident},
            {"runtime_width", entry.runtimeWidth},
            {"runtime_height", entry.runtimeHeight},
            {"runtime_mip_count", entry.runtimeMipCount},
            {"runtime_format", textureFormatDiagnosticName(entry.runtimeFormat)},
            {"runtime_estimated_bytes", entry.runtimeBytes},
            {"fallback", entry.fallback},
            {"compressed", entry.compressed},
        });
    }

    std::vector<const TextureEntry*> runtimeEntries;
    runtimeEntries.reserve(entries.size());
    for (const TextureEntry& entry : entries) {
        if (entry.runtimeResident) {
            runtimeEntries.push_back(&entry);
        }
    }
    std::sort(runtimeEntries.begin(), runtimeEntries.end(), [](const TextureEntry* a, const TextureEntry* b) {
        return a->runtimeBytes > b->runtimeBytes;
    });
    nlohmann::json topRuntime = nlohmann::json::array();
    for (size_t i = 0; i < std::min<size_t>(runtimeEntries.size(), 8u); ++i) {
        const TextureEntry& entry = *runtimeEntries[i];
        topRuntime.push_back({
            {"name", entry.name},
            {"source", entry.sourcePath.generic_string()},
            {"width", entry.runtimeWidth},
            {"height", entry.runtimeHeight},
            {"mip_count", entry.runtimeMipCount},
            {"format", textureFormatDiagnosticName(entry.runtimeFormat)},
            {"estimated_resident_bytes", entry.runtimeBytes},
            {"fallback", entry.fallback},
        });
    }

    if (uploader_ != nullptr) {
        const auto& uploadStats = uploader_->stats();
        report["streaming_upload_count"] = uploadStats.uploadCount;
        report["streaming_image_upload_count"] = uploadStats.imageUploadCount;
        const uint64_t uploadsDuringCapture =
            captureReadyUploadSnapshotValid_ && uploadStats.imageUploadCount >= captureReadyImageUploadCount_
            ? uploadStats.imageUploadCount - captureReadyImageUploadCount_
            : 0;
        report["streaming_image_uploads_after_capture_ready"] = uploadsDuringCapture;
        report["streaming_image_uploads_during_capture"] = uploadsDuringCapture;
    }
    report["bound_texture_count"] = static_cast<uint32_t>(entries.size());
    report["total_texture_bytes"] = totalBytes;
    report["total_mip_count"] = totalMips;
    report["missing_mip_chain_count"] = missingMipChains;
    report["fallback_missing_texture_count"] = fallbackCount;
    report["oversized_texture_count"] = oversizedCount;
    report["top_largest_textures"] = std::move(top);

    const NativeGpuAssetCacheStats nativeStats = nativeGpuAssetCache_.stats();
    report["runtime_residency"] = {
        {"texture_count", nativeStats.textureCount},
        {"fully_resident_texture_count", nativeStats.textureFullyResidentCount},
        {"partially_resident_texture_count", nativeStats.texturePartiallyResidentCount},
        {"fallback_texture_count", nativeStats.textureFallbackCount},
        {"resident_mip_count", nativeStats.residentTextureMipCount},
        {"total_mip_count", nativeStats.totalTextureMipCount},
        {"resident_gpu_bytes", nativeStats.residentGpuBytes},
        {"in_flight_upload_bytes", nativeStats.inFlightUploadBytes},
        {"pending_retired_gpu_bytes", nativeStats.pendingRetiredGpuBytes},
        {"pending_retired_resource_count", nativeStats.pendingRetiredResourceCount},
        {"bound_imported_texture_count", runtimeImportedCount},
        {"resident_imported_texture_count", runtimeImportedResidentCount},
        {"resident_imported_mip_count", runtimeImportedMips},
        {"estimated_resident_imported_bytes", runtimeImportedBytes},
        {"top_runtime_imported_textures", std::move(topRuntime)},
        {"last_eviction", nativeGpuAssetEvictionResultJson(lastStreamingEviction_)},
        {"native_cache_assets", nativeGpuAssetCacheSnapshotsJson(nativeGpuAssetCache_.snapshots())},
    };
    report["streaming_textures"] = textureStreamingToJson(textureStreamingManager_.textures());
    nlohmann::json evictionHistory = nlohmann::json::array();
    for (const auto& [frame, eviction] : streamingEvictionHistory_) {
        evictionHistory.push_back({
            {"frame", frame},
            {"result", nativeGpuAssetEvictionResultJson(eviction)},
        });
    }
    report["eviction_history"] = std::move(evictionHistory);

    nlohmann::json warnings = nlohmann::json::array();
    if (missingMipChains > 0u) {
        warnings.push_back("Missing mip chains on " + std::to_string(missingMipChains) + " texture(s)");
    }
    if (oversizedCount > 0u) {
        warnings.push_back("Oversized textures relative to display resolution: " + std::to_string(oversizedCount));
    }
    if (fallbackCount > 0u) {
        warnings.push_back("Fallback or missing texture payloads active: " + std::to_string(fallbackCount));
    }
    if (pathTracer_ != nullptr && pathTracer_->settings().materialTextureAnisotropy >= 8.0f) {
        warnings.push_back("High material texture anisotropy during capture");
    }
    if (uploader_ != nullptr && captureReadyUploadSnapshotValid_ &&
        uploader_->stats().imageUploadCount > captureReadyImageUploadCount_) {
        warnings.push_back("Image uploads continued after CAPTURE_READY; capture may include active texture streaming");
    }
    if (nativeStats.texturePartiallyResidentCount > 0u) {
        warnings.push_back(
            "Partially resident native textures: " +
            std::to_string(nativeStats.texturePartiallyResidentCount));
    }
    if (nativeStats.pendingRetiredGpuBytes > 0u) {
        warnings.push_back(
            "Evicted GPU resources are awaiting timeline retirement: " +
            std::to_string(nativeStats.pendingRetiredGpuBytes) + " bytes");
    }
    report["warnings"] = std::move(warnings);
    return report;
}

nlohmann::json Application::streamingRuntimeReport() const {
    nlohmann::json report = streamingRuntimeState_.toJson(&nativeAssetCatalog_);
    const NativeGpuAssetCacheStats nativeGpuStats = nativeGpuAssetCache_.stats();
    const uint64_t gpuBudget = streamingOptions_.gpuMemoryBudgetBytes;
    const uint64_t cpuBudget = streamingOptions_.cpuMemoryBudgetBytes;
    report["native_gpu_asset_cache"] = {
        {"stats", nativeGpuAssetCacheStatsJson(nativeGpuStats)},
        {"memory_pressure", {
            {"eviction_enabled", streamingOptions_.evictionEnabled},
            {"gpu_budget_bytes", gpuBudget},
            {"cpu_budget_bytes", cpuBudget},
            {"resident_gpu_bytes", nativeGpuStats.residentGpuBytes},
            {"resident_cpu_bytes", nativeGpuStats.residentCpuBytes},
            {"over_gpu_budget_bytes", nativeGpuStats.residentGpuBytes > gpuBudget ? nativeGpuStats.residentGpuBytes - gpuBudget : 0ull},
            {"over_cpu_budget_bytes", nativeGpuStats.residentCpuBytes > cpuBudget ? nativeGpuStats.residentCpuBytes - cpuBudget : 0ull},
            {"evictable_assets", nativeGpuStats.evictableCount},
            {"retired_assets", nativeGpuStats.retiredCount},
        }},
        {"last_eviction", nativeGpuAssetEvictionResultJson(lastStreamingEviction_)},
        {"assets", nativeGpuAssetCacheSnapshotsJson(nativeGpuAssetCache_.snapshots())},
    };
    nlohmann::json evictionHistory = nlohmann::json::array();
    for (const auto& [frame, eviction] : streamingEvictionHistory_) {
        evictionHistory.push_back({
            {"frame", frame},
            {"result", nativeGpuAssetEvictionResultJson(eviction)},
        });
    }
    report["native_gpu_asset_cache"]["eviction_history"] = std::move(evictionHistory);
    GpuSceneStreamingState gpuSceneStreaming;
    gpuSceneStreaming.rebuild(sceneDocument_, gpuInstanceEntities_, &nativeGpuAssetCache_);
    const GpuSceneStreamingUpdatePlan gpuSceneUpdatePlan = buildGpuSceneStreamingUpdatePlan(
        lastStreamingGpuSceneSnapshots_,
        gpuSceneStreaming.instances());
    report["gpu_scene_streaming"] = {
        {"stats", gpuSceneStreamingStatsJson(gpuSceneStreaming.stats())},
        {"update_plan", gpuSceneStreamingUpdatePlanJson(gpuSceneUpdatePlan)},
        {"instances", gpuSceneStreamingInstancesJson(gpuSceneStreaming.instances())},
    };
    report["incremental_gpu_scene_update_queue"] = {
        {"stats", incrementalGpuSceneUpdateStatsJson(streamingGpuSceneUpdateQueue_.stats())},
        {"last_frame", incrementalGpuSceneApplyFrameResultJson(lastStreamingGpuSceneApply_)},
        {"operations", incrementalGpuSceneUpdateSnapshotsJson(streamingGpuSceneUpdateQueue_.snapshots())},
    };
    if (pathTracer_ != nullptr) {
        const PathTracerRenderer::StreamingResetMaskReport& resetMasks = pathTracer_->streamingResetMaskReport();
        report["streaming_reset_masks"] = {
            {"generation", resetMasks.generation},
            {"last_frame", resetMasks.lastFrame},
            {"pending_temporal_entity_count", resetMasks.pendingTemporalEntityCount},
            {"pending_restir_entity_count", resetMasks.pendingRestirEntityCount},
            {"pending_denoiser_entity_count", resetMasks.pendingDenoiserEntityCount},
            {"total_temporal_entity_count", resetMasks.totalTemporalEntityCount},
            {"total_restir_entity_count", resetMasks.totalRestirEntityCount},
            {"total_denoiser_entity_count", resetMasks.totalDenoiserEntityCount},
            {"denoiser_history_invalidated", resetMasks.denoiserHistoryInvalidated},
            {"denoiser_history_invalidation_generation", resetMasks.denoiserHistoryInvalidationGeneration},
            {"gpu_record_count", resetMasks.gpuRecordCount},
            {"gpu_record_capacity", resetMasks.gpuRecordCapacity},
            {"gpu_buffer_bytes", resetMasks.gpuBufferBytes},
            {"gpu_buffer_allocated", resetMasks.gpuBufferAllocated},
            {"gpu_instance_mask_count", resetMasks.gpuInstanceMaskCount},
            {"gpu_instance_mask_capacity", resetMasks.gpuInstanceMaskCapacity},
            {"gpu_instance_mask_buffer_bytes", resetMasks.gpuInstanceMaskBufferBytes},
            {"gpu_instance_mask_buffer_allocated", resetMasks.gpuInstanceMaskBufferAllocated},
            {"temporal_entity_uuids", resetMasks.temporalEntityUuids},
            {"restir_entity_uuids", resetMasks.restirEntityUuids},
            {"temporal_instance_indices", resetMasks.temporalInstanceIndices},
            {"restir_instance_indices", resetMasks.restirInstanceIndices},
        };
    }
    nlohmann::json streamingGpuTimelineMarkers = nlohmann::json::array();
    for (const auto& [workQueueTimeline, executorTimeline] : streamingGpuWorkTimelineMarkers_) {
        streamingGpuTimelineMarkers.push_back({
            {"work_queue_timeline", workQueueTimeline},
            {"executor_timeline", executorTimeline},
        });
    }
    bool hasPayloadBackedWork = false;
    for (const StreamingGpuWorkSnapshot& ticket : streamingGpuWorkQueue_.snapshots()) {
        hasPayloadBackedWork = hasPayloadBackedWork || ticket.payloadBacked;
    }
    uint64_t pendingBufferPayloadBytes = 0;
    uint64_t pendingPayloadBytes = 0;
    for (const auto& [ticketId, payload] : streamingGpuBufferUploadPayloads_) {
        (void)ticketId;
        pendingBufferPayloadBytes += static_cast<uint64_t>(payload.bytes.size());
        pendingPayloadBytes += static_cast<uint64_t>(payload.bytes.size());
    }
    for (const auto& [ticketId, payload] : streamingGpuImageMipUploadPayloads_) {
        (void)ticketId;
        pendingPayloadBytes += static_cast<uint64_t>(payload.bytes.size());
    }
    const size_t pendingBlasBuildPayloads = streamingGpuBlasBuildPayloads_.size();
    const size_t pendingBlasCompactionPayloads = streamingGpuBlasCompactionPayloads_.size();
    report["streaming_gpu_work_queue"] = {
        {"stats", streamingGpuWorkQueueStatsJson(streamingGpuWorkQueue_.stats())},
        {"hitch_summary", streamingGpuWorkPressureStatsJson(streamingGpuWorkQueue_.pressureStats())},
        {"tickets", streamingGpuWorkQueueSnapshotsJson(streamingGpuWorkQueue_.snapshots())},
        {"next_timeline_value", streamingGpuWorkQueue_.nextTimelineValue()},
        {"completed_timeline", streamingGpuWorkCompletedTimeline_},
        {"payload_backed_executor_work", hasPayloadBackedWork},
        {"pending_payload_buffer_uploads", streamingGpuBufferUploadPayloads_.size()},
        {"pending_payload_image_mip_uploads", streamingGpuImageMipUploadPayloads_.size()},
        {"pending_payload_blas_builds", pendingBlasBuildPayloads},
        {"pending_payload_blas_compactions", pendingBlasCompactionPayloads},
        {"pending_payload_buffer_upload_bytes", pendingBufferPayloadBytes},
        {"pending_payload_upload_bytes", pendingPayloadBytes},
        {"marker_only_completion_event_emitted", streamingGpuMarkerOnlyCompletionEventEmitted_},
        {"pending_timeline_markers", streamingGpuTimelineMarkers},
        {"transfer_executor_ready", streamingGpuTransferExecutorReady_},
        {"transfer_executor_init_attempted", streamingGpuTransferExecutorInitAttempted_},
        {"transfer_executor", streamingGpuTransferExecutorStatsJson(streamingGpuTransferExecutor_.stats())},
    };
    report["streaming_io"] = {
        {"backend", streamingIoBackendAvailabilityJson(streamingOptions_)},
        {"metrics", streamingIoMetricsJson(streamingIoMetrics_)},
    };
    return report;
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
            const std::filesystem::path nativePath = resolveAssetCachePath(*recordIt, root);
            std::error_code nativeEc;
            if (!nativePath.empty() &&
                nativeAssetKindFromExtension(nativePath) == NativeAssetKind::Animation &&
                std::filesystem::is_regular_file(nativePath, nativeEc)) {
                return nativePath.lexically_normal();
            }
            const std::filesystem::path path = resolveAssetRecordPath(*recordIt, root);
            if (!path.empty()) {
                return path.lexically_normal();
            }
        }
        const auto nativeRuntimeIt = nativeRuntimeAnimationPathsByGuid_.find(player.animationGuid);
        if (nativeRuntimeIt != nativeRuntimeAnimationPathsByGuid_.end() && !nativeRuntimeIt->second.empty()) {
            return nativeRuntimeIt->second.lexically_normal();
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

std::optional<std::filesystem::path> Application::resolveAnimationControllerPath(const AnimationPlayer& player) const {
    const std::filesystem::path root = assetResolutionRoot();
    if (!player.controllerGuid.empty()) {
        const auto recordIt = std::find_if(assetRegistry_.records().begin(), assetRegistry_.records().end(), [&](const AssetRecord& record) {
            return record.guid == player.controllerGuid && record.type == AssetType::AnimationController;
        });
        if (recordIt != assetRegistry_.records().end()) {
            const std::filesystem::path nativePath = resolveAssetCachePath(*recordIt, root);
            std::error_code nativeEc;
            if (!nativePath.empty() &&
                nativeAssetKindFromExtension(nativePath) == NativeAssetKind::AnimationController &&
                std::filesystem::is_regular_file(nativePath, nativeEc)) {
                return nativePath.lexically_normal();
            }
            const std::filesystem::path path = resolveAssetRecordPath(*recordIt, root);
            if (!path.empty()) {
                return path.lexically_normal();
            }
        }
    }
    if (player.controllerPath.empty()) {
        return std::nullopt;
    }
    std::filesystem::path path = player.controllerPath;
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
    AnimationClip clip = AnimationClip::loadRtanim(*clipPath, &warnings);
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

const AnimationController* Application::animationControllerForPlayer(const AnimationPlayer& player) {
    const std::optional<std::filesystem::path> controllerPath = resolveAnimationControllerPath(player);
    if (!controllerPath.has_value() || controllerPath->empty()) {
        return nullptr;
    }
    const std::string key = controllerPath->string();
    if (const auto cached = animationControllerCache_.find(key); cached != animationControllerCache_.end()) {
        return &cached->second;
    }
    if (failedAnimationControllerLoads_.find(key) != failedAnimationControllerLoads_.end()) {
        return nullptr;
    }

    std::vector<std::string> warnings;
    AnimationController controller = AnimationController::load(*controllerPath, &warnings);
    for (const std::string& warning : warnings) {
        std::cerr << warning << '\n';
    }
    if (!controller.valid()) {
        failedAnimationControllerLoads_.insert(key);
        std::cerr << "Animation controller load failed or had no states: " << key << '\n';
        return nullptr;
    }
    auto [it, inserted] = animationControllerCache_.emplace(key, std::move(controller));
    return inserted ? &it->second : nullptr;
}

bool Application::attachNativePackageAnimationPlayer(const NativeRuntimeLoadReport& loadReport) {
    nativeRuntimeAnimationPathsByGuid_.clear();
    const NativeRuntimeLoadedAsset* fallbackAnimation = nullptr;
    const NativeRuntimeLoadedAsset* controller = nullptr;
    const NativeRuntimeAnimationClipBinding* controllerBinding = nullptr;
    const bool controllerSelectionRequested = !nativePackageAnimationSelection_.controllerGuid.empty() ||
        !nativePackageAnimationSelection_.controllerPath.empty();
    const bool entitySelectionRequested = !nativePackageAnimationSelection_.entityName.empty() ||
        nativePackageAnimationSelection_.entityUuid != 0;
    auto normalizedPathKey = [](const std::filesystem::path& path) {
        return path.lexically_normal().generic_string();
    };
    auto controllerPathMatchesSelection = [&](const NativeRuntimeLoadedAsset& asset) {
        if (nativePackageAnimationSelection_.controllerPath.empty()) {
            return false;
        }
        const std::string requested = normalizedPathKey(nativePackageAnimationSelection_.controllerPath);
        const std::string actual = normalizedPathKey(asset.path);
        return actual == requested || asset.path.filename() == nativePackageAnimationSelection_.controllerPath.filename();
    };
    auto controllerMatchesSelection = [&](const NativeRuntimeLoadedAsset& asset) {
        if (!controllerSelectionRequested) {
            return true;
        }
        if (!nativePackageAnimationSelection_.controllerGuid.empty() && asset.guid == nativePackageAnimationSelection_.controllerGuid) {
            return true;
        }
        return controllerPathMatchesSelection(asset);
    };

    for (const NativeRuntimeLoadedAsset& asset : loadReport.assets) {
        if (!asset.ok) {
            continue;
        }
        if (asset.kind == NativeAssetKind::Animation && asset.animationClip.valid() && !asset.path.empty()) {
            animationClipCache_[asset.path.lexically_normal().string()] = asset.animationClip;
            if (!asset.guid.empty()) {
                nativeRuntimeAnimationPathsByGuid_[asset.guid] = asset.path.lexically_normal();
            }
            if (fallbackAnimation == nullptr) {
                fallbackAnimation = &asset;
            }
        } else if (asset.kind == NativeAssetKind::AnimationController && asset.animationController.valid() && !asset.path.empty()) {
            animationControllerCache_[asset.path.lexically_normal().string()] = asset.animationController;
            if (controller == nullptr && controllerMatchesSelection(asset)) {
                controller = &asset;
                for (const NativeRuntimeAnimationClipBinding& binding : asset.animationClipBindings) {
                    if (binding.resolved && !binding.resolvedNativePath.empty()) {
                        controllerBinding = &binding;
                        break;
                    }
                }
            }
        }
    }

    if (controllerSelectionRequested && controller == nullptr) {
        std::cerr << "Native package animation controller selection did not match a loaded controller: guid="
                  << nativePackageAnimationSelection_.controllerGuid
                  << " path=" << nativePackageAnimationSelection_.controllerPath.generic_string() << '\n';
        return false;
    }
    if (controller == nullptr && fallbackAnimation == nullptr) {
        return false;
    }

    Entity* target = nullptr;
    if (entitySelectionRequested) {
        for (Entity* entity : sceneDocument_.registry().entities()) {
            if (entity == nullptr) {
                continue;
            }
            const bool uuidMatches = nativePackageAnimationSelection_.entityUuid != 0 && entity->uuid == nativePackageAnimationSelection_.entityUuid;
            const bool nameMatches = !nativePackageAnimationSelection_.entityName.empty() && entity->name == nativePackageAnimationSelection_.entityName;
            if (uuidMatches || nameMatches) {
                target = entity;
                break;
            }
        }
        if (target == nullptr) {
            std::cerr << "Native package animation target entity selection did not match: name="
                      << nativePackageAnimationSelection_.entityName
                      << " uuid=" << nativePackageAnimationSelection_.entityUuid << '\n';
            return false;
        }
    }
    for (Entity* entity : sceneDocument_.registry().entities()) {
        if (target == nullptr && entity != nullptr && entity->meshRenderer.has_value() && entity->meshRenderer->skinIndex >= 0) {
            target = entity;
            break;
        }
    }
    if (target == nullptr) {
        for (Entity* entity : sceneDocument_.registry().entities()) {
            if (entity != nullptr && entity->meshRenderer.has_value()) {
                target = entity;
                break;
            }
        }
    }
    if (target == nullptr) {
        return false;
    }

    AnimationPlayer player;
    player.enabled = true;
    player.playOnStart = true;
    player.playing = true;
    player.loop = true;
    player.applyMorphWeights = true;
    if (controller != nullptr) {
        player.controllerGuid = controller->guid;
        player.controllerPath = controller->path.lexically_normal();
        if (controllerBinding != nullptr) {
            player.animationGuid = controllerBinding->clipGuid;
            player.animationPath = controllerBinding->resolvedNativePath.lexically_normal();
        }
    } else if (fallbackAnimation != nullptr) {
        player.animationGuid = fallbackAnimation->guid;
        player.animationPath = fallbackAnimation->path.lexically_normal();
    }
    target->animationPlayer = std::move(player);
    std::cout << "Attached native package animation player: entity=" << target->name
              << " entityUuid=" << target->uuid
              << " controller=" << (controller != nullptr ? controller->path.generic_string() : std::string{})
              << " controllerGuid=" << (controller != nullptr ? controller->guid : std::string{})
              << " clip=" << (controllerBinding != nullptr ? controllerBinding->resolvedNativePath.generic_string()
                  : (fallbackAnimation != nullptr ? fallbackAnimation->path.generic_string() : std::string{}))
              << " selectionPolicy=" << (entitySelectionRequested || controllerSelectionRequested ? "explicit" : "first_compatible")
              << '\n';
    return true;
}

const AnimationClip* Application::controllerClipForPlayer(
    AnimationPlayer& player,
    const AnimationController& controller,
    std::vector<AnimationController::Event>* routedEvents,
    const AnimationClip** blendToClip,
    float* blendAlpha) {
    if (blendToClip != nullptr) {
        *blendToClip = nullptr;
    }
    if (blendAlpha != nullptr) {
        *blendAlpha = 0.0f;
    }
    std::unordered_map<std::string, AnimationControllerParameterValue> parameters = controller.defaultParameters();
    for (const AnimationControllerParameterOverride& overrideParameter : player.controllerParameters) {
        if (overrideParameter.name.empty()) {
            continue;
        }
        AnimationControllerParameterValue value;
        value.type = animationControllerParameterTypeFromName(overrideParameter.type);
        switch (value.type) {
        case AnimationControllerParameterType::Bool:
            value.boolValue = overrideParameter.boolValue;
            break;
        case AnimationControllerParameterType::Int:
            value.intValue = overrideParameter.intValue;
            break;
        case AnimationControllerParameterType::Float:
            value.floatValue = overrideParameter.floatValue;
            break;
        case AnimationControllerParameterType::Trigger:
            value.triggerValue = overrideParameter.triggerValue;
            break;
        case AnimationControllerParameterType::Unknown:
            continue;
        }
        parameters[overrideParameter.name] = value;
    }
    const AnimationControllerEvaluation evaluation = controller.evaluate(player.controllerState, static_cast<float>(player.currentTimeSeconds), parameters);
    if (!evaluation.state.empty()) {
        player.controllerState = evaluation.state;
    }
    if (routedEvents != nullptr) {
        const size_t eventCount = std::min(evaluation.routedEventNames.size(), evaluation.routedEventPayloads.size());
        routedEvents->reserve(routedEvents->size() + eventCount);
        for (size_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
            routedEvents->push_back(AnimationController::Event{
                evaluation.routedEventNames[eventIndex],
                evaluation.routedEventPayloads[eventIndex],
            });
        }
    }
    for (const std::string& consumedTrigger : evaluation.consumedTriggers) {
        for (AnimationControllerParameterOverride& overrideParameter : player.controllerParameters) {
            if (overrideParameter.name == consumedTrigger && animationControllerParameterTypeFromName(overrideParameter.type) == AnimationControllerParameterType::Trigger) {
                overrideParameter.triggerValue = false;
            }
        }
    }
    const AnimationController::State* state = controller.state(player.controllerState.empty() ? controller.initialState() : player.controllerState);
    if (state == nullptr) {
        return nullptr;
    }
    AnimationPlayer statePlayer = player;
    if (evaluation.blendTreeActive && (!evaluation.blendFromClipGuid.empty() || !evaluation.blendFromClipPath.empty())) {
        statePlayer.animationGuid = evaluation.blendFromClipGuid;
        statePlayer.animationPath = evaluation.blendFromClipPath;
        if (blendToClip != nullptr && (!evaluation.blendToClipGuid.empty() || !evaluation.blendToClipPath.empty()) &&
            (evaluation.blendToClipGuid != evaluation.blendFromClipGuid || evaluation.blendToClipPath != evaluation.blendFromClipPath)) {
            AnimationPlayer blendToPlayer = player;
            blendToPlayer.animationGuid = evaluation.blendToClipGuid;
            blendToPlayer.animationPath = evaluation.blendToClipPath;
            *blendToClip = animationClipForPlayer(blendToPlayer);
            if (blendAlpha != nullptr) {
                *blendAlpha = evaluation.blendAlpha;
            }
        }
    } else {
        statePlayer.animationGuid = !evaluation.selectedClipGuid.empty() ? evaluation.selectedClipGuid : state->clipGuid;
        statePlayer.animationPath = !evaluation.selectedClipPath.empty() ? evaluation.selectedClipPath : state->clipPath;
    }
    return animationClipForPlayer(statePlayer);
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
    auto differentVec3Vector = [](const std::vector<glm::vec3>& a, const MeshAsset& mesh) {
        constexpr float eps = 1.0e-5f;
        if (a.size() != mesh.vertices.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            const glm::vec3 current = mesh.vertices[i].position;
            const glm::vec3 next = a[i];
            if (std::abs(current.x - next.x) > eps || std::abs(current.y - next.y) > eps || std::abs(current.z - next.z) > eps) {
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
    auto blendNodeSamples = [](const AnimationNodeSample* a, const AnimationNodeSample* b, float alpha) {
        if (a == nullptr && b == nullptr) {
            return AnimationNodeSample{};
        }
        if (a == nullptr) {
            return *b;
        }
        if (b == nullptr) {
            return *a;
        }
        AnimationNodeSample result;
        if (a->hasTranslation && b->hasTranslation) {
            result.translation = glm::mix(a->translation, b->translation, alpha);
            result.hasTranslation = true;
        } else if (a->hasTranslation) {
            result.translation = a->translation;
            result.hasTranslation = true;
        } else if (b->hasTranslation) {
            result.translation = b->translation;
            result.hasTranslation = true;
        }
        if (a->hasRotation && b->hasRotation) {
            result.rotation = glm::slerp(a->rotation, b->rotation, alpha);
            result.hasRotation = true;
        } else if (a->hasRotation) {
            result.rotation = a->rotation;
            result.hasRotation = true;
        } else if (b->hasRotation) {
            result.rotation = b->rotation;
            result.hasRotation = true;
        }
        if (a->hasScale && b->hasScale) {
            result.scale = glm::mix(a->scale, b->scale, alpha);
            result.hasScale = true;
        } else if (a->hasScale) {
            result.scale = a->scale;
            result.hasScale = true;
        } else if (b->hasScale) {
            result.scale = b->scale;
            result.hasScale = true;
        }
        if (a->hasMorphWeights && b->hasMorphWeights) {
            const size_t count = std::max(a->morphWeights.size(), b->morphWeights.size());
            result.morphWeights.assign(count, 0.0f);
            for (size_t i = 0; i < count; ++i) {
                const float av = i < a->morphWeights.size() ? a->morphWeights[i] : 0.0f;
                const float bv = i < b->morphWeights.size() ? b->morphWeights[i] : 0.0f;
                result.morphWeights[i] = av + (bv - av) * alpha;
            }
            result.hasMorphWeights = true;
        } else if (a->hasMorphWeights) {
            result.morphWeights = a->morphWeights;
            result.hasMorphWeights = true;
        } else if (b->hasMorphWeights) {
            result.morphWeights = b->morphWeights;
            result.hasMorphWeights = true;
        }
        if (a->hasMeshVertexPositions && b->hasMeshVertexPositions) {
            const size_t count = std::min(a->meshVertexPositions.size(), b->meshVertexPositions.size());
            result.meshVertexPositions.resize(count);
            for (size_t i = 0; i < count; ++i) {
                result.meshVertexPositions[i] = glm::mix(a->meshVertexPositions[i], b->meshVertexPositions[i], alpha);
            }
            result.hasMeshVertexPositions = !result.meshVertexPositions.empty();
        } else if (a->hasMeshVertexPositions) {
            result.meshVertexPositions = a->meshVertexPositions;
            result.hasMeshVertexPositions = true;
        } else if (b->hasMeshVertexPositions) {
            result.meshVertexPositions = b->meshVertexPositions;
            result.hasMeshVertexPositions = true;
        }
        auto blendScalar = [alpha](bool hasA, float av, bool hasB, float bv, bool& hasOut, float& out) {
            if (hasA && hasB) {
                out = av + (bv - av) * alpha;
                hasOut = true;
            } else if (hasA) {
                out = av;
                hasOut = true;
            } else if (hasB) {
                out = bv;
                hasOut = true;
            }
        };
        blendScalar(a->hasCameraYfov, a->cameraYfov, b->hasCameraYfov, b->cameraYfov, result.hasCameraYfov, result.cameraYfov);
        blendScalar(a->hasCameraAspectRatio, a->cameraAspectRatio, b->hasCameraAspectRatio, b->cameraAspectRatio, result.hasCameraAspectRatio, result.cameraAspectRatio);
        blendScalar(a->hasCameraOrthoXmag, a->cameraOrthoXmag, b->hasCameraOrthoXmag, b->cameraOrthoXmag, result.hasCameraOrthoXmag, result.cameraOrthoXmag);
        blendScalar(a->hasCameraOrthoYmag, a->cameraOrthoYmag, b->hasCameraOrthoYmag, b->cameraOrthoYmag, result.hasCameraOrthoYmag, result.cameraOrthoYmag);
        blendScalar(a->hasLightIntensity, a->lightIntensity, b->hasLightIntensity, b->lightIntensity, result.hasLightIntensity, result.lightIntensity);
        blendScalar(a->hasLightRadius, a->lightRadius, b->hasLightRadius, b->lightRadius, result.hasLightRadius, result.lightRadius);
        if (a->hasCameraNearFar && b->hasCameraNearFar) {
            result.cameraNearFar = glm::mix(a->cameraNearFar, b->cameraNearFar, alpha);
            result.hasCameraNearFar = true;
        } else if (a->hasCameraNearFar) {
            result.cameraNearFar = a->cameraNearFar;
            result.hasCameraNearFar = true;
        } else if (b->hasCameraNearFar) {
            result.cameraNearFar = b->cameraNearFar;
            result.hasCameraNearFar = true;
        }
        if (a->hasLightColor && b->hasLightColor) {
            result.lightColor = glm::mix(a->lightColor, b->lightColor, alpha);
            result.hasLightColor = true;
        } else if (a->hasLightColor) {
            result.lightColor = a->lightColor;
            result.hasLightColor = true;
        } else if (b->hasLightColor) {
            result.lightColor = b->lightColor;
            result.hasLightColor = true;
        }
        if (a->hasLightConeAngles && b->hasLightConeAngles) {
            result.lightConeAngles = glm::mix(a->lightConeAngles, b->lightConeAngles, alpha);
            result.hasLightConeAngles = true;
        } else if (a->hasLightConeAngles) {
            result.lightConeAngles = a->lightConeAngles;
            result.hasLightConeAngles = true;
        } else if (b->hasLightConeAngles) {
            result.lightConeAngles = b->lightConeAngles;
            result.hasLightConeAngles = true;
        }
        return result;
    };
    auto blendAnimationSamples = [&](AnimationSample a, const AnimationSample& b, float alpha) {
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        for (const auto& [nodeIndex, nodeB] : b.nodes) {
            const auto itA = a.nodes.find(nodeIndex);
            const AnimationNodeSample* nodeA = itA != a.nodes.end() ? &itA->second : nullptr;
            a.nodes[nodeIndex] = blendNodeSamples(nodeA, &nodeB, alpha);
        }
        return a;
    };
    auto maskedJointNameMatches = [](std::string_view joint, std::string_view entityName, int32_t nodeIndex) {
        if (joint.empty()) {
            return false;
        }
        if (joint == entityName) {
            return true;
        }
        const std::string nodeText = std::to_string(nodeIndex);
        return joint == nodeText;
    };
    auto layerAffectsNode = [&](const AnimationController::AvatarMask* mask,
                                int32_t nodeIndex,
                                const std::unordered_map<int32_t, Entity*>& entityForSourceNode) {
        if (mask == nullptr) {
            return true;
        }
        const auto entityIt = entityForSourceNode.find(nodeIndex);
        const std::string_view entityName = entityIt != entityForSourceNode.end() && entityIt->second != nullptr
            ? std::string_view{entityIt->second->name}
            : std::string_view{};
        for (const std::string& excluded : mask->excludedJoints) {
            if (maskedJointNameMatches(excluded, entityName, nodeIndex)) {
                return false;
            }
        }
        if (mask->includedJoints.empty()) {
            return true;
        }
        return std::any_of(mask->includedJoints.begin(), mask->includedJoints.end(), [&](const std::string& included) {
            return maskedJointNameMatches(included, entityName, nodeIndex);
        });
    };
    auto overlayNodeSamples = [](const AnimationNodeSample* base, const AnimationNodeSample& overlay, float weight, bool additive) {
        weight = std::clamp(weight, 0.0f, 1.0f);
        AnimationNodeSample result = base != nullptr ? *base : AnimationNodeSample{};
        if (weight <= 0.0f) {
            return result;
        }

        if (additive) {
            if (overlay.hasTranslation) {
                const glm::vec3 baseTranslation = result.hasTranslation ? result.translation : glm::vec3{0.0f};
                result.translation = baseTranslation + overlay.translation * weight;
                result.hasTranslation = true;
            }
            if (overlay.hasRotation) {
                const glm::quat baseRotation = result.hasRotation ? result.rotation : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
                const glm::quat weightedDelta = glm::slerp(glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::normalize(overlay.rotation), weight);
                result.rotation = glm::normalize(weightedDelta * baseRotation);
                result.hasRotation = true;
            }
            if (overlay.hasScale) {
                const glm::vec3 baseScale = result.hasScale ? result.scale : glm::vec3{1.0f};
                result.scale = baseScale + (overlay.scale - glm::vec3{1.0f}) * weight;
                result.hasScale = true;
            }
            if (overlay.hasMorphWeights) {
                const size_t count = std::max(result.morphWeights.size(), overlay.morphWeights.size());
                if (result.morphWeights.size() < count) {
                    result.morphWeights.resize(count, 0.0f);
                }
                for (size_t i = 0; i < overlay.morphWeights.size(); ++i) {
                    result.morphWeights[i] += overlay.morphWeights[i] * weight;
                }
                result.hasMorphWeights = true;
            }
            return result;
        }

        if (overlay.hasTranslation) {
            const glm::vec3 baseTranslation = result.hasTranslation ? result.translation : overlay.translation;
            result.translation = glm::mix(baseTranslation, overlay.translation, weight);
            result.hasTranslation = true;
        }
        if (overlay.hasRotation) {
            const glm::quat baseRotation = result.hasRotation ? result.rotation : overlay.rotation;
            result.rotation = glm::normalize(glm::slerp(baseRotation, overlay.rotation, weight));
            result.hasRotation = true;
        }
        if (overlay.hasScale) {
            const glm::vec3 baseScale = result.hasScale ? result.scale : overlay.scale;
            result.scale = glm::mix(baseScale, overlay.scale, weight);
            result.hasScale = true;
        }
        if (overlay.hasMorphWeights) {
            const size_t count = std::max(result.morphWeights.size(), overlay.morphWeights.size());
            std::vector<float> blended(count, 0.0f);
            for (size_t i = 0; i < count; ++i) {
                const float baseValue = i < result.morphWeights.size() ? result.morphWeights[i] : 0.0f;
                const float overlayValue = i < overlay.morphWeights.size() ? overlay.morphWeights[i] : baseValue;
                blended[i] = baseValue + (overlayValue - baseValue) * weight;
            }
            result.morphWeights = std::move(blended);
            result.hasMorphWeights = true;
        }
        return result;
    };
    auto applyControllerLayers = [&](AnimationSample baseSample,
                                     AnimationPlayer& player,
                                     const AnimationController& controller,
                                     double timeSeconds,
                                     bool loop,
                                     const std::unordered_map<int32_t, Entity*>& entityForSourceNode) {
        const std::vector<AnimationController::Layer>& layers = controller.layers();
        if (layers.size() <= 1u) {
            return baseSample;
        }
        auto resolveMask = [&](std::string_view maskName) -> const AnimationController::AvatarMask* {
            if (maskName.empty()) {
                return nullptr;
            }
            const std::vector<AnimationController::AvatarMask>& masks = controller.avatarMasks();
            const auto it = std::find_if(masks.begin(), masks.end(), [&](const AnimationController::AvatarMask& mask) {
                return mask.name == maskName;
            });
            return it != masks.end() ? &*it : nullptr;
        };
        for (size_t layerIndex = 1u; layerIndex < layers.size(); ++layerIndex) {
            const AnimationController::Layer& layer = layers[layerIndex];
            if (layer.weight <= 0.0f || (layer.clipGuid.empty() && layer.clipPath.empty())) {
                continue;
            }
            AnimationPlayer layerPlayer = player;
            layerPlayer.animationGuid = layer.clipGuid;
            layerPlayer.animationPath = layer.clipPath;
            const AnimationClip* layerClip = animationClipForPlayer(layerPlayer);
            if (layerClip == nullptr) {
                continue;
            }
            const AnimationSample layerSample = layerClip->sample(timeSeconds, loop);
            const AnimationController::AvatarMask* mask = resolveMask(layer.mask);
            for (const auto& [nodeIndex, overlayNode] : layerSample.nodes) {
                if (!layerAffectsNode(mask, nodeIndex, entityForSourceNode)) {
                    continue;
                }
                const auto baseIt = baseSample.nodes.find(nodeIndex);
                const AnimationNodeSample* baseNode = baseIt != baseSample.nodes.end() ? &baseIt->second : nullptr;
                baseSample.nodes[nodeIndex] = overlayNodeSamples(baseNode, overlayNode, layer.weight, layer.additive);
            }
        }
        return baseSample;
    };
    auto animationEventCrossed = [](double eventTime, double previousTime, double currentTime, double startTime, double endTime, bool loop, bool forward) {
        if (forward) {
            if (!loop || currentTime >= previousTime) {
                return eventTime > previousTime && eventTime <= currentTime;
            }
            return (eventTime > previousTime && eventTime <= endTime) || (eventTime >= startTime && eventTime <= currentTime);
        }
        if (!loop || currentTime <= previousTime) {
            return eventTime < previousTime && eventTime >= currentTime;
        }
        return (eventTime < previousTime && eventTime >= startTime) || (eventTime <= endTime && eventTime >= currentTime);
    };
    auto publishAnimationEvents = [&](Entity& playerEntity, const AnimationClip& clip, double previousTime, double currentTime, bool loop, bool forward) {
        if (clip.events().empty() || previousTime == currentTime) {
            return;
        }
        const double startTime = clip.startTime();
        const double endTime = clip.endTime();
        for (const AnimationClip::Event& event : clip.events()) {
            if (!animationEventCrossed(event.timeSeconds, previousTime, currentTime, startTime, endTime, loop, forward)) {
                continue;
            }
            SceneEvent sceneEvent{SceneEventType::AnimationEventFired, playerEntity.id, {}, SceneUpdateKind::None};
            sceneEvent.animationEventName = event.name;
            sceneEvent.animationEventPayloadJson = event.payloadJson;
            sceneEvent.animationEventTimeSeconds = event.timeSeconds;
            sceneEventBus_.publish(sceneEvent);
        }
    };
    auto publishActiveClipEvents = [&](Entity& playerEntity,
                                       const AnimationClip& primaryClip,
                                       const AnimationClip* blendClip,
                                       double previousTime,
                                       double currentTime,
                                       bool loop,
                                       bool forward) {
        publishAnimationEvents(playerEntity, primaryClip, previousTime, currentTime, loop, forward);
        if (blendClip != nullptr && blendClip != &primaryClip) {
            publishAnimationEvents(playerEntity, *blendClip, previousTime, currentTime, loop, forward);
        }
    };
    auto publishControllerEvents = [&](Entity& playerEntity, const std::vector<AnimationController::Event>& events, double eventTime) {
        for (const AnimationController::Event& event : events) {
            if (event.name.empty()) {
                continue;
            }
            SceneEvent sceneEvent{SceneEventType::AnimationEventFired, playerEntity.id, {}, SceneUpdateKind::None};
            sceneEvent.animationEventName = event.name;
            sceneEvent.animationEventPayloadJson = event.payloadJson;
            sceneEvent.animationEventTimeSeconds = eventTime;
            sceneEventBus_.publish(sceneEvent);
        }
    };
    auto collectRootMotionCandidates = [](const AnimationClip& primaryClip, const AnimationClip* blendClip) {
        std::vector<AnimationClip::RootMotionCandidate> candidates;
        std::unordered_set<uint64_t> seenChannels;
        auto appendCandidates = [&](const AnimationClip& clip) {
            for (const AnimationClip::RootMotionCandidate& candidate : clip.rootMotionCandidates()) {
                const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(candidate.node)) << 32u) |
                    static_cast<uint64_t>(static_cast<uint32_t>(candidate.path));
                if (seenChannels.insert(key).second) {
                    candidates.push_back(candidate);
                }
            }
        };
        appendCandidates(primaryClip);
        if (blendClip != nullptr && blendClip != &primaryClip) {
            appendCandidates(*blendClip);
        }
        return candidates;
    };
    auto applyRootMotionDelta = [&](Entity& playerEntity,
                                    const std::vector<AnimationClip::RootMotionCandidate>& candidates,
                                    const AnimationSample& previousSample,
                                    const AnimationSample& currentSample) -> bool {
        bool changed = false;
        glm::vec3 translationDelta{0.0f};
        glm::quat rotationDelta{1.0f, 0.0f, 0.0f, 0.0f};
        bool hasTranslationDelta = false;
        bool hasRotationDelta = false;
        std::unordered_set<int32_t> translationNodes;
        std::unordered_set<int32_t> rotationNodes;
        for (const AnimationClip::RootMotionCandidate& candidate : candidates) {
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
    const bool gpuSkinningJointRefreshAvailable = !latestGpuSkinningPlan_.empty();
    const float clampedDelta = std::max(0.0f, deltaSeconds);
    for (Entity* playerEntity : entities) {
        if (playerEntity == nullptr || !playerEntity->animationPlayer.has_value()) {
            continue;
        }
        AnimationPlayer& player = *playerEntity->animationPlayer;
        if (!player.enabled) {
            continue;
        }
        const AnimationClip* clip = nullptr;
        const AnimationClip* blendToClip = nullptr;
        float blendAlpha = 0.0f;
        bool effectiveLoop = player.loop;
        float effectivePlaybackSpeed = player.playbackSpeed;
        std::vector<AnimationController::Event> routedControllerEvents;
        const AnimationController* activeController = nullptr;
        if (!player.controllerGuid.empty() || !player.controllerPath.empty()) {
            if (const AnimationController* controller = animationControllerForPlayer(player)) {
                activeController = controller;
                clip = controllerClipForPlayer(player, *controller, &routedControllerEvents, &blendToClip, &blendAlpha);
                if (const AnimationController::State* state = controller->state(player.controllerState)) {
                    effectiveLoop = state->loop;
                    effectivePlaybackSpeed *= state->speed;
                }
            }
        }
        if (clip == nullptr) {
            clip = animationClipForPlayer(player);
        }
        if (clip == nullptr) {
            continue;
        }

        const double previousTime = player.currentTimeSeconds;
        double sampleTime = previousTime;
        if (player.playing) {
            sampleTime += static_cast<double>(clampedDelta) * static_cast<double>(effectivePlaybackSpeed);
        }
        if (!effectiveLoop && clip->duration() > 0.0) {
            sampleTime = std::clamp(sampleTime, clip->startTime(), clip->endTime());
        }
        AnimationSample previousSample = clip->sample(previousTime, effectiveLoop);
        AnimationSample sample = clip->sample(sampleTime, effectiveLoop);
        if (blendToClip != nullptr && blendAlpha > 0.0f) {
            previousSample = blendAnimationSamples(previousSample, blendToClip->sample(previousTime, effectiveLoop), blendAlpha);
            sample = blendAnimationSamples(sample, blendToClip->sample(sampleTime, effectiveLoop), blendAlpha);
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
        if (activeController != nullptr) {
            previousSample = applyControllerLayers(previousSample, player, *activeController, previousTime, effectiveLoop, entityForSourceNode);
            sample = applyControllerLayers(sample, player, *activeController, sampleTime, effectiveLoop, entityForSourceNode);
        }
        player.previousTimeSeconds = previousSample.timeSeconds;
        player.previousSampleValid = true;
        player.currentTimeSeconds = sample.timeSeconds;
        publishControllerEvents(*playerEntity, routedControllerEvents, player.currentTimeSeconds);
        if (player.playing) {
            publishActiveClipEvents(*playerEntity, *clip, blendToClip, previousSample.timeSeconds, sample.timeSeconds, effectiveLoop, effectivePlaybackSpeed >= 0.0f);
        }
        std::unordered_set<uint64_t> rootMotionChannels;
        const std::vector<AnimationClip::RootMotionCandidate> activeRootMotionCandidates = collectRootMotionCandidates(*clip, blendToClip);
        if (player.applyRootMotion && player.playing && !activeRootMotionCandidates.empty()) {
            const AnimationSample rootPreviousSample = previousSample.timeSeconds <= sample.timeSeconds || !effectiveLoop
                ? previousSample
                : (blendToClip != nullptr && blendAlpha > 0.0f)
                    ? blendAnimationSamples(clip->sample(clip->startTime(), false), blendToClip->sample(clip->startTime(), false), blendAlpha)
                    : clip->sample(clip->startTime(), false);
            if (applyRootMotionDelta(*playerEntity, activeRootMotionCandidates, rootPreviousSample, sample)) {
                sceneTransformChanged = true;
            }
            for (const AnimationClip::RootMotionCandidate& candidate : activeRootMotionCandidates) {
                rootMotionChannels.insert(pathKey(candidate.node, candidate.path));
            }
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
        for (int32_t nodeIndex : skinDrivenTransformNodes) {
            const auto entityIt = entityForSourceNode.find(nodeIndex);
            if (entityIt == entityForSourceNode.end() || entityIt->second == nullptr) {
                continue;
            }
            entityIt->second->previousAnimationTransform = entityIt->second->transform;
            entityIt->second->previousAnimationTransformValid = player.previousSampleValid;
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
                    if (gpuSkinningJointRefreshAvailable) {
                        sceneTransformChanged = true;
                    } else {
                        sceneTopologyChanged = true;
                    }
                } else {
                    sceneTransformChanged = true;
                }
            }
            if (player.applyMorphWeights && nodeSample.hasMorphWeights && target.meshRenderer.has_value() &&
                differentFloatVector(target.meshRenderer->morphWeights, nodeSample.morphWeights)) {
                target.meshRenderer->morphWeights = nodeSample.morphWeights;
                sceneTopologyChanged = true;
            }
            if (nodeSample.hasMeshVertexPositions && target.meshRenderer.has_value()) {
                MeshRenderer& renderer = *target.meshRenderer;
                MeshAsset* mesh = assets_.mesh(renderer.mesh);
                if (mesh != nullptr && nodeSample.meshVertexPositions.size() == mesh->vertices.size() && differentVec3Vector(nodeSample.meshVertexPositions, *mesh)) {
                    std::vector<glm::vec3>& basePositions = animationMeshBasePositions_[renderer.mesh.index];
                    if (basePositions.size() != mesh->vertices.size()) {
                        basePositions.clear();
                        basePositions.reserve(mesh->vertices.size());
                        for (const MeshVertex& vertex : mesh->vertices) {
                            basePositions.push_back(vertex.position);
                        }
                    }
                    for (size_t i = 0; i < mesh->vertices.size(); ++i) {
                        mesh->vertices[i].position = nodeSample.meshVertexPositions[i];
                    }
                    mesh->cachedLocalBvhNodes.clear();
                    mesh->cachedLocalBvhTriangles.clear();
                    sceneTopologyChanged = true;
                }
            }
            if (target.camera.has_value()) {
                Camera& camera = *target.camera;
                bool cameraChanged = false;
                if (nodeSample.hasCameraYfov && std::abs(camera.verticalFovRadians - nodeSample.cameraYfov) > 1.0e-5f) {
                    camera.verticalFovRadians = nodeSample.cameraYfov;
                    cameraChanged = true;
                }
                if (nodeSample.hasCameraAspectRatio && std::abs(camera.aspectRatio - nodeSample.cameraAspectRatio) > 1.0e-5f) {
                    camera.aspectRatio = nodeSample.cameraAspectRatio;
                    cameraChanged = true;
                }
                if (nodeSample.hasCameraOrthoXmag && std::abs(camera.orthographicXmag - nodeSample.cameraOrthoXmag) > 1.0e-5f) {
                    camera.orthographicXmag = nodeSample.cameraOrthoXmag;
                    cameraChanged = true;
                }
                if (nodeSample.hasCameraOrthoYmag && std::abs(camera.orthographicYmag - nodeSample.cameraOrthoYmag) > 1.0e-5f) {
                    camera.orthographicYmag = nodeSample.cameraOrthoYmag;
                    cameraChanged = true;
                }
                if (nodeSample.hasCameraNearFar &&
                    (std::abs(camera.nearPlane - nodeSample.cameraNearFar.x) > 1.0e-5f ||
                     std::abs(camera.farPlane - nodeSample.cameraNearFar.y) > 1.0e-5f)) {
                    camera.nearPlane = nodeSample.cameraNearFar.x;
                    camera.farPlane = nodeSample.cameraNearFar.y;
                    cameraChanged = true;
                }
                if (cameraChanged) {
                    sceneDocument_.markDirty(SceneUpdateKind::CameraOnly);
                    sceneTransformChanged = true;
                }
            }
            if (target.light.has_value()) {
                Light& light = *target.light;
                bool lightChanged = false;
                if (nodeSample.hasLightColor && differentVec3(light.color, nodeSample.lightColor)) {
                    light.color = nodeSample.lightColor;
                    lightChanged = true;
                }
                if (nodeSample.hasLightIntensity && std::abs(light.intensity - nodeSample.lightIntensity) > 1.0e-5f) {
                    light.intensity = nodeSample.lightIntensity;
                    lightChanged = true;
                }
                if (nodeSample.hasLightRadius && std::abs(light.sizeOrRadius - nodeSample.lightRadius) > 1.0e-5f) {
                    light.sizeOrRadius = nodeSample.lightRadius;
                    lightChanged = true;
                }
                if (nodeSample.hasLightConeAngles &&
                    (std::abs(light.innerConeRadians - nodeSample.lightConeAngles.x) > 1.0e-5f ||
                     std::abs(light.outerConeRadians - nodeSample.lightConeAngles.y) > 1.0e-5f)) {
                    light.innerConeRadians = nodeSample.lightConeAngles.x;
                    light.outerConeRadians = nodeSample.lightConeAngles.y;
                    lightChanged = true;
                }
                if (lightChanged) {
                    sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
                    sceneTransformChanged = true;
                }
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
                loader.setNativeTextureFormatSupport(nativeTextureFormatSupportForContext(context_.get()));
                importedScene_ = loader.loadWithCache(*gltfPath_);
            }
            rebuildGpuSceneAsset();
            preparePathTracerForRendererReplacement(previousSettings);
            std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
                gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
                gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
                currentSceneCachePolicyForRenderer(),
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

void Application::configureCaptureReady(uint32_t afterFrames, bool log) {
    captureReadyAfterFrames_ = std::max(1u, afterFrames);
    captureReadyLog_ = log;
    captureReadyUploadSnapshotValid_ = false;
    captureReadyImageUploadCount_ = 0;
}

void Application::setRendererOnlyLingerAfterCaptureReadyMs(uint32_t milliseconds) {
    rendererOnlyLingerAfterCaptureReadyMs_ = milliseconds;
}

void Application::setCaptureReadyFilePath(std::optional<std::filesystem::path> path) {
    captureReadyFilePath_ = std::move(path);
}

void Application::setSavePresentFramePath(std::optional<std::filesystem::path> path) {
    savePresentFramePath_ = std::move(path);
    initialPresentFrameSaveComplete_ = false;
}

void Application::setSavePresentFrameOnHotkeyPath(std::optional<std::filesystem::path> path) {
    savePresentFrameOnHotkeyPath_ = std::move(path);
}

bool Application::savePresentFrame(const std::filesystem::path& path) {
    if (pathTracer_ == nullptr || context_ == nullptr || allocator_ == nullptr) {
        return false;
    }
    const VkExtent2D extent = pathTracer_->displayExtent();
    if (extent.width == 0u || extent.height == 0u || pathTracer_->presentationImage() == VK_NULL_HANDLE) {
        return false;
    }
    DiagnosticImageExport exporter(*context_, *allocator_);
    if (!exporter.initialize(VK_FORMAT_R8G8B8A8_UNORM, extent)) {
        return false;
    }
    const bool ok = exporter.exportView(*pathTracer_, pathTracer_->settings().debugView, path, 0);
    if (ok) {
        std::cout << "Saved present frame: " << path.string() << '\n';
    } else {
        std::cerr << "Warning: failed to save present frame: " << path.string() << '\n';
    }
    return ok;
}

void Application::dumpRendererOnlyProfileJson(const std::filesystem::path& path) {
    HeadlessDiagnosticsConfig config;
    config.profile = true;
    config.profileJsonPath = path;
    HeadlessDiagnostics diag(config);
    diag.run(*this);
    diag.writeProfileJson(path);
    std::cout << "Wrote renderer profile JSON: " << path.string() << '\n';
}

void Application::exportRendererOnlyDebugViews(const std::filesystem::path& dir) {
    HeadlessDiagnosticsConfig config;
    config.saveDebugViewsDir = dir;
    HeadlessDiagnostics diag(config);
    diag.exportDebugViews(*this, dir);
    std::cout << "Wrote renderer debug views: " << dir.string() << '\n';
}

std::string Application::activeCaptureSceneName() const {
    if (scenePath_.has_value()) {
        return scenePath_->filename().string();
    }
    if (gltfPath_.has_value()) {
        return gltfPath_->filename().string();
    }
    if (nativePackageScenePath_.has_value()) {
        return nativePackageScenePath_->filename().string();
    }
    return "untitled";
}

void Application::printCaptureReadyMarker() {
    captureReadyPrinted_ = true;
    captureReadyFrameSerial_ = frameSerial_;
    captureReadyPrintedAt_ = std::chrono::steady_clock::now();
    const std::string marker = "CAPTURE_READY frame=" + std::to_string(frameSerial_) +
        " scene=" + activeCaptureSceneName() +
        " steady_frames=" + std::to_string(captureReadyRenderedFrames_);
    std::cout << marker << '\n';
    if (captureReadyFilePath_.has_value()) {
        std::error_code ec;
        if (const auto parent = captureReadyFilePath_->parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(*captureReadyFilePath_, std::ios::trunc);
        if (out) {
            out << marker << '\n';
        }
    }
}

void Application::processRendererOnlyRequests(const RendererOnlyRequests& requests) {
    if (pathTracer_ == nullptr) {
        return;
    }
    if (requests.cameraMoveSpeed.has_value()) {
        cameraController_.setMoveSpeed(std::clamp(*requests.cameraMoveSpeed, 0.05f, 100.0f));
    }
    if (requests.cameraFastMoveSpeed.has_value()) {
        cameraController_.setFastMoveSpeed(std::clamp(*requests.cameraFastMoveSpeed, 0.05f, 250.0f));
    }
    if (requests.settings.has_value()) {
        if (pathTracer_->applySettings(*requests.settings)) {
            syncDocumentRenderSettings(sceneDocument_, pathTracer_->settings());
        }
    }
    if (requests.resetAccumulation.has_value()) {
        pathTracer_->resetAccumulation(*requests.resetAccumulation);
    }
    if (requests.savePresentFrame) {
        (void)savePresentFrame(savePresentFramePath_.value_or(std::filesystem::path("out/diagnostic/present.png")));
    }
    if (requests.saveDebugViews) {
        exportRendererOnlyDebugViews("out/diagnostic/debug_views");
    }
    if (requests.dumpProfileJson) {
        dumpRendererOnlyProfileJson("out/diagnostic/profile.json");
    }
    if (requests.printCaptureReady) {
        printCaptureReadyMarker();
    }
    if (requests.startNsightPerfReport.has_value()) {
        if (requestNsightPerfReport(*requests.startNsightPerfReport)) {
            std::cout << "Queued Nsight Perf SDK report: "
                      << requests.startNsightPerfReport->outputDirectory << '\n';
        } else {
            std::cerr << "Failed to queue Nsight Perf SDK report: "
                      << nsightPerfMarkerStatus().unavailableReason << '\n';
        }
    }
    if (requests.cancelNsightPerfReport) {
        cancelNsightPerfReport();
        std::cout << "Cancelled Nsight Perf SDK report collection\n";
    }
    if (requests.openNsightPerfReport) {
        const NsightPerfMarkerStatus status = nsightPerfMarkerStatus();
        if (!status.lastReportDirectory.empty()) {
#if defined(_WIN32)
            (void)ShellExecuteA(
                nullptr,
                "open",
                status.lastReportDirectory.c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL);
#endif
        }
    }
    if (requests.runQuickExperimentMatrix) {
        if (!scenePath_.has_value()) {
            std::cerr << "Quick A/B matrix currently requires a --scene source\n";
        } else {
#if defined(_WIN32)
            if (launchQuickNsightExperimentMatrix(*scenePath_)) {
                std::cout << "Launched quick Nsight A/B matrix for " << scenePath_->string() << '\n';
                if (window_ != nullptr) {
                    glfwSetWindowShouldClose(window_, GLFW_TRUE);
                }
            } else {
                std::cerr << "Failed to launch quick Nsight A/B matrix\n";
            }
#endif
        }
    }
}

void Application::updateCaptureReadyState(uint32_t frameNumber) {
    if (!rendererOnly_ || pathTracer_ == nullptr) {
        return;
    }
    const bool validImage = pathTracer_->presentationImage() != VK_NULL_HANDLE &&
        pathTracer_->displayExtent().width > 0u &&
        pathTracer_->displayExtent().height > 0u;
    const bool validTimedFrame = pathTracer_->timings().totalMs() > 0.0f;
    emitNsightFrameBoundary(
        context_ != nullptr ? context_->graphicsQueue() : VK_NULL_HANDLE,
        pathTracer_->presentationImage());
    if (validImage && validTimedFrame) {
        ++captureReadyRenderedFrames_;
    } else {
        captureReadyRenderedFrames_ = 0;
    }

    if (!captureReadyPrinted_ && captureReadyRenderedFrames_ >= captureReadyAfterFrames_) {
        if (uploader_ != nullptr) {
            captureReadyImageUploadCount_ = uploader_->stats().imageUploadCount;
            captureReadyUploadSnapshotValid_ = true;
        }
        if (captureReadyLog_) {
            printCaptureReadyMarker();
        } else {
            captureReadyPrinted_ = true;
            captureReadyFrameSerial_ = frameNumber;
        }
        if (savePresentFramePath_.has_value() && !initialPresentFrameSaveComplete_) {
            initialPresentFrameSaveComplete_ = savePresentFrame(*savePresentFramePath_);
        }
    }

    if (savePresentFrameOnHotkeyPath_.has_value() && window_ != nullptr) {
        const bool down = glfwGetKey(window_, GLFW_KEY_F10) == GLFW_PRESS;
        if (down && !savePresentFrameHotkeyDown_) {
            (void)savePresentFrame(*savePresentFrameOnHotkeyPath_);
        }
        savePresentFrameHotkeyDown_ = down;
    }
}

void Application::initWindow() {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    const bool explicitStartupScene = scenePath_.has_value() || gltfPath_.has_value() || nativePackageScenePath_.has_value();
    mainWindowHiddenUntilRenderer_ = !headless_ && !rendererOnly_ && !explicitStartupScene;
    glfwWindowHint(GLFW_VISIBLE, mainWindowHiddenUntilRenderer_ ? GLFW_FALSE : GLFW_TRUE);
    const int windowWidth = rendererOnly_ ? static_cast<int>(headlessExtent_.width) : initialWidth;
    const int windowHeight = rendererOnly_ ? static_cast<int>(headlessExtent_.height) : initialHeight;
    window_ = glfwCreateWindow(windowWidth, windowHeight, "Vibode Engine", nullptr, nullptr);
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
        swapchain_ = std::make_unique<Swapchain>(*context_, headlessExtent_);
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
    const bool explicitStartupScene = scenePath_.has_value() || gltfPath_.has_value() || nativePackageScenePath_.has_value();
    if (explicitStartupScene && uiOverlay_ != nullptr) {
        uiOverlay_->editor().dismissProjectManager();
    }
    const std::optional<std::filesystem::path> startupProjectOverride = (!headless_ && !rendererOnly_) ? startupProjectOverridePath() : std::nullopt;
    const bool hasStartupProjectOverride = startupProjectOverride.has_value() && std::filesystem::exists(*startupProjectOverride);
    const std::filesystem::path startupProjectPath = hasStartupProjectOverride
        ? *startupProjectOverride
        : (startupPrefs != nullptr ? std::filesystem::path(startupPrefs->lastOpenedProject) : std::filesystem::path{});
    const bool openLastProjectOnStartup = !rendererOnly_ && (hasStartupProjectOverride ||
        (startupPrefs != nullptr && startupPrefs->openLastProject &&
            !startupPrefs->lastOpenedProject.empty() && std::filesystem::exists(startupPrefs->lastOpenedProject)));
    const bool deferRendererForProjectManager = !headless_ && !rendererOnly_ && !explicitStartupScene && !openLastProjectOnStartup;
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
            loader.setNativeTextureFormatSupport(nativeTextureFormatSupportForContext(context_.get()));
            importedScene_ = loader.loadWithCache(*gltfPath_);
        }
        undoStack_.clear();
        std::cout << "Loaded scene JSON: " << scenePath_->string() << '\n';
    } else if (nativePackageScenePath_.has_value()) {
        NativeAssetRuntimeLoader loader;
        NativeRuntimeLoadOptions loadOptions;
        loadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
        NativeRuntimeLoadReport loadReport = loader.loadLooseRoot(*nativePackageScenePath_, &assets_, loadOptions);
        if (!loadReport.ok || !loadReport.sceneAssetPlan.rendererPlaceable) {
            std::string reason = loadReport.errors.empty() ? std::string("package did not produce a renderer-placeable scene") : loadReport.errors.front().message;
            throw std::runtime_error("Native package scene load failed: " + nativePackageScenePath_->string() + " " + reason);
        }
        importedScene_ = std::move(loadReport.sceneAsset);
        sceneDocument_.importSceneAsset(*importedScene_);
        const bool attachedPackageAnimation = attachNativePackageAnimationPlayer(loadReport);
        undoStack_.clear();
        std::cout << "Loaded native package scene: " << nativePackageScenePath_->string()
                  << " meshes=" << importedScene_->meshes.size()
                  << " materials=" << importedScene_->materials.size()
                  << " textures=" << importedScene_->textures.size()
                  << " nodes=" << importedScene_->nodes.size()
                  << " animationPlayerAttached=" << (attachedPackageAnimation ? "true" : "false") << '\n';
    } else if (gltfPath_.has_value()) {
        GltfLoader loader(assets_);
        loader.setNativeTextureFormatSupport(nativeTextureFormatSupportForContext(context_.get()));
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
        startupSettings.restirGiMode = *restirGiOverride_
            ? RestirGiMode::Production
            : RestirGiMode::Off;
        startupSettings.restirGiReservoirLayout = *restirGiOverride_
            ? RestirGiReservoirLayout::ProductionPacked
            : RestirGiReservoirLayout::LegacyCachePacked;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (opacityMicromapOverride_.has_value()) {
        startupSettings.opacityMicromapsEnabled = *opacityMicromapOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (opacityMicromapBlendOverride_.has_value()) {
        startupSettings.opacityMicromapBlendEnabled = *opacityMicromapBlendOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
    }
    if (hardwareBackfaceCullingOverride_.has_value()) {
        startupSettings.hardwareBackfaceCullingEnabled = *hardwareBackfaceCullingOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
    }
    if (opacityMicromapSubdivisionOverride_.has_value()) {
        startupSettings.opacityMicromapSubdivisionLevel = *opacityMicromapSubdivisionOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
    }
    traceStartupPhase("initial_create_path_tracer_begin");
    createPathTracer(&startupSettings);
    traceStartupPhase("initial_create_path_tracer_end");
    traceStartupPhase("initial_sync_document_settings_begin");
    syncDocumentRenderSettings(sceneDocument_, pathTracer_->settings());
    traceStartupPhase("initial_sync_document_settings_end");
    traceStartupPhase("initial_apply_active_camera_begin");
    applyActiveSceneCamera();
    traceStartupPhase("initial_apply_active_camera_end");
    sceneDocument_.clearDirty();
    traceStartupPhase("initial_scene_clear_dirty_end");
    if (!headless_ && !rendererOnly_) {
        if (loadedSceneDocument) {
            traceStartupPhase("initial_deserialize_editor_scene_data_begin");
            deserializeEditorSceneData();
            traceStartupPhase("initial_deserialize_editor_scene_data_end");
        }
        if (openLastProjectOnStartup && !startupProjectPath.empty()) {
            traceStartupPhase("initial_open_startup_project_begin");
            if (openProjectFromFile(startupProjectPath, false)) {
                traceStartupPhase("initial_open_startup_project_end");
                traceStartupPhase("initial_apply_pending_scene_update_begin");
                (void)applyPendingSceneUpdate(true);
                traceStartupPhase("initial_apply_pending_scene_update_end");
            } else if (hasStartupProjectOverride) {
                std::cerr << "Startup project override failed: " << startupProjectPath.string() << '\n';
            }
        }
    }
    traceStartupPhase("initial_command_system_set_path_tracer_begin");
    commandSystem_->setPathTracer(pathTracer_.get());
    traceStartupPhase("initial_command_system_set_path_tracer_end");
    traceStartupPhase("initial_show_main_window_begin");
    showMainWindowIfHidden();
    traceStartupPhase("initial_show_main_window_end");
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

            frameWorkScheduler_.tick();
            if (frameWorkProbeCompletionPending_) {
                frameWorkProbeCompletionPending_ = !frameWorkScheduler_.completeFence(frameWorkProbeJobId_);
            }
            stepEditorTicketProbeQueues();
            updateAnimationPlayers(deltaSeconds);
            commandSystem_->drawFrame(seconds, deltaSeconds);
            if (pathTracer_) {
                updateFrameWorkAccelerationStructureBudgetFeedback(pathTracer_->timings());
            }
            ++frameSerial_;
            releaseRetiredPathTracers();
            ++frameCount;
        }
        commandSystem_->waitIdle();
        return;
    }

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        const auto profileFrameStart = std::chrono::steady_clock::now();
        traceMainLoopPhase(frameCount, "poll_events_begin");
        glfwPollEvents();
        traceMainLoopPhase(frameCount, "poll_events_end");

        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - start).count();
        const float rawDeltaSeconds = std::max(0.0f, seconds - lastFrameSeconds_);
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;

        if (uiOverlay_) {
            traceMainLoopPhase(frameCount, "ui_begin_frame_begin");
            uiOverlay_->beginFrame();
            traceMainLoopPhase(frameCount, "ui_begin_frame_end");
        }
        traceMainLoopPhase(frameCount, "runtime_controls_begin");
        processRuntimeControls(deltaSeconds);
        traceMainLoopPhase(frameCount, "runtime_controls_end");
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
        frameWorkScheduler_.tick();
        traceMainLoopPhase(frameCount, "job_snapshots_begin");
        stepEditorTicketProbeQueues();
        stepStreamingGpuWorkQueue();
        stepStreamingGpuSceneUpdateQueue();
        pollProgressiveRuntimeLoadJob();
        EditorJobCenterState jobCenter;
        jobCenter.frameWorkScheduler = frameWorkScheduler_.snapshot();
        jobCenter.frameWorkSchedulerAvailable = true;
        jobCenter.gpuUploadTickets = editorGpuUploadTicketSnapshots(false);
        jobCenter.gpuUploadNextTimelineValue = editorGpuUploadNextTimelineValue();
        jobCenter.gpuUploadTicketsAvailable = true;
        jobCenter.mainThreadApplyTickets = editorMainThreadApplyTickets_.snapshots(false);
        jobCenter.mainThreadApplyTicketsAvailable = true;
        jobCenter.topologyRebuildTickets = editorTopologyRebuildTickets_.snapshots(false);
        jobCenter.topologyRebuildLatestGeneration = editorTopologyRebuildTickets_.latestGeneration();
        jobCenter.topologyRebuildNextTimelineValue = editorTopologyRebuildTickets_.nextTimelineValue();
        jobCenter.topologyRebuildTicketsAvailable = true;
        jobCenter.mountedNativePackageWatchesAvailable = true;
        jobCenter.mountedNativePackageWatches.reserve(mountedNativePackages_.size());
        for (const MountedNativePackageWatch& watch : mountedNativePackages_) {
            EditorMountedNativePackageWatchSnapshot snapshot;
            snapshot.packagePath = watch.packagePath;
            snapshot.generation = watch.generation;
            snapshot.lastWriteTimeTicks = watch.lastWriteTime.time_since_epoch().count();
            snapshot.detectedWriteTimeTicks = watch.detectedWriteTime.time_since_epoch().count();
            snapshot.textureCount = watch.textureCount;
            snapshot.materialCount = watch.materialCount;
            snapshot.meshCount = watch.meshCount;
            snapshot.changeDetected = watch.changeDetected;
            if (watch.changeDetected) {
                snapshot.detectionReportPath = editorNativePackageRefreshDetectionReportPath(project_, watch.packagePath);
            }
            jobCenter.mountedNativePackageWatches.push_back(std::move(snapshot));
        }
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
        if (activeNativeFileMigrationJob_.has_value()) {
            float migrationProgress = 0.05f;
            std::string migrationStage = "Queued";
            double migrationWorkerElapsedMs = 0.0;
            if (activeNativeFileMigrationJob_->progress != nullptr) {
                std::lock_guard<std::mutex> lock(activeNativeFileMigrationJob_->progress->mutex);
                migrationProgress = activeNativeFileMigrationJob_->progress->progress;
                migrationStage = activeNativeFileMigrationJob_->progress->stage;
                if (activeNativeFileMigrationJob_->progress->workerStartedAt.time_since_epoch().count() != 0) {
                    migrationWorkerElapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - activeNativeFileMigrationJob_->progress->workerStartedAt).count();
                }
            }
            jobCenter.nativeFileMigrationJobSerial = activeNativeFileMigrationJob_->request.serial;
            jobCenter.nativeFileMigrationRunning = true;
            jobCenter.nativeFileMigrationProgress = std::clamp(migrationProgress, 0.0f, 1.0f);
            jobCenter.nativeFileMigrationPackage = activeNativeFileMigrationJob_->request.package;
            jobCenter.nativeFileMigrationDryRun = activeNativeFileMigrationJob_->request.dryRun;
            jobCenter.nativeFileMigrationTitle = activeNativeFileMigrationJob_->request.package ? "Migrate Package" : "Migrate Native Asset";
            jobCenter.nativeFileMigrationStatus = migrationStage + ": " + activeNativeFileMigrationJob_->request.sourcePath.filename().string();
            jobCenter.nativeFileMigrationSourcePath = activeNativeFileMigrationJob_->request.sourcePath;
            jobCenter.nativeFileMigrationReportPath = activeNativeFileMigrationJob_->request.reportPath;
            jobCenter.nativeFileMigrationWorkerElapsedMs = migrationWorkerElapsedMs;
            jobCenter.queuedNativeFileMigrations = pendingNativeFileMigrationJobs_.size();
        }
        if (!activeNativeFileMigrationJob_.has_value()) {
            jobCenter.queuedNativeFileMigrations = pendingNativeFileMigrationJobs_.size();
        }
        if (completedNativeFileMigrationJob_.completedNativeFileMigrationSerial != 0) {
            jobCenter.completedNativeFileMigrationSerial = completedNativeFileMigrationJob_.completedNativeFileMigrationSerial;
            jobCenter.completedNativeFileMigrationSuccess = completedNativeFileMigrationJob_.completedNativeFileMigrationSuccess;
            jobCenter.completedNativeFileMigrationPackage = completedNativeFileMigrationJob_.completedNativeFileMigrationPackage;
            jobCenter.completedNativeFileMigrationDryRun = completedNativeFileMigrationJob_.completedNativeFileMigrationDryRun;
            jobCenter.completedNativeFileMigrationMutationAttempted = completedNativeFileMigrationJob_.completedNativeFileMigrationMutationAttempted;
            jobCenter.completedNativeFileMigrationMutated = completedNativeFileMigrationJob_.completedNativeFileMigrationMutated;
            jobCenter.completedNativeFileMigrationRequired = completedNativeFileMigrationJob_.completedNativeFileMigrationRequired;
            jobCenter.completedNativeFileMigrationAvailable = completedNativeFileMigrationJob_.completedNativeFileMigrationAvailable;
            jobCenter.completedNativeFileMigrationTitle = completedNativeFileMigrationJob_.completedNativeFileMigrationTitle;
            jobCenter.completedNativeFileMigrationStatus = completedNativeFileMigrationJob_.completedNativeFileMigrationStatus;
            jobCenter.completedNativeFileMigrationSourcePath = completedNativeFileMigrationJob_.completedNativeFileMigrationSourcePath;
            jobCenter.completedNativeFileMigrationReportPath = completedNativeFileMigrationJob_.completedNativeFileMigrationReportPath;
            jobCenter.completedNativeFileMigrationBackupPath = completedNativeFileMigrationJob_.completedNativeFileMigrationBackupPath;
            jobCenter.completedNativeFileMigrationErrors = completedNativeFileMigrationJob_.completedNativeFileMigrationErrors;
            jobCenter.completedNativeFileMigrationWarnings = completedNativeFileMigrationJob_.completedNativeFileMigrationWarnings;
            jobCenter.completedNativeFileMigrationWorkerTotalMs = completedNativeFileMigrationJob_.completedNativeFileMigrationWorkerTotalMs;
        }
        if (rendererOnly_ && uiOverlay_ && pathTracer_) {
            traceMainLoopPhase(frameCount, "ui_build_renderer_only_begin");
            RendererOnlyRequests rendererOnlyRequests = uiOverlay_->buildRendererOnly(
                *pathTracer_,
                swapchain_->extent(),
                gltfPath_,
                scenePath_,
                nativePackageScenePath_,
                &cameraController_,
                rawDeltaSeconds * 1000.0f,
                captureReadyPrinted_,
                captureReadyRenderedFrames_,
                captureReadyAfterFrames_);
            traceMainLoopPhase(frameCount, "ui_build_renderer_only_end");
            processRendererOnlyRequests(rendererOnlyRequests);
        } else if (uiOverlay_ && pathTracer_) {
            traceMainLoopPhase(frameCount, "ui_build_editor_begin");
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
            traceMainLoopPhase(frameCount, "ui_build_editor_end");
        } else if (uiOverlay_ != nullptr) {
            traceMainLoopPhase(frameCount, "ui_build_project_manager_begin");
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
            traceMainLoopPhase(frameCount, "ui_build_project_manager_end");
        }
        if (!rendererOnly_) {
            traceMainLoopPhase(frameCount, "apply_requests_pre_render_begin");
            pollMountedNativePackageChanges(editorRequests);
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
            traceMainLoopPhase(frameCount, "apply_requests_pre_render_end");
        }
        if (frameWorkProbeCompletionPending_) {
            frameWorkProbeCompletionPending_ = !frameWorkScheduler_.completeFence(frameWorkProbeJobId_);
        }
        updateAnimationPlayers(deltaSeconds);
        if (!rendererOnly_) {
            traceMainLoopPhase(frameCount, "prepare_editor_render_job_begin");
            prepareEditorRenderJobFrame();
            traceMainLoopPhase(frameCount, "prepare_editor_render_job_end");
        }
        if (beginFrameCapture_) {
            beginFrameCapture_(frameCount + 1u);
        }
        traceMainLoopPhase(frameCount, "draw_frame_begin");
        commandSystem_->drawFrame(seconds, deltaSeconds);
        traceMainLoopPhase(frameCount, "draw_frame_end");
        if (pathTracer_) {
            updateFrameWorkAccelerationStructureBudgetFeedback(pathTracer_->timings());
        }
        if (uiOverlay_) {
            traceMainLoopPhase(frameCount, "render_platform_windows_begin");
            uiOverlay_->renderPlatformWindows();
            traceMainLoopPhase(frameCount, "render_platform_windows_end");
        }
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(frameCount + 1u);
        }
        updateCaptureReadyState(frameCount + 1u);
        if (!rendererOnly_) {
            traceMainLoopPhase(frameCount, "post_render_editor_begin");
            updateEditorRenderJob(deltaSeconds);
            applyEditorRequests(editorRequests, true);
            pollAsyncSceneLoad();
            pollAssetImportWorker();
            pollCookProjectJob();
            pollNativeFileMigrationJob();
            captureProjectThumbnailIfReady();
            traceMainLoopPhase(frameCount, "post_render_editor_end");
        }
        traceMainLoopPhase(frameCount, "update_window_title_begin");
        updateWindowTitle(seconds);
        traceMainLoopPhase(frameCount, "update_window_title_end");

        if (interactiveProfileCollectionEnabled_) {
            const auto profileFrameEnd = std::chrono::steady_clock::now();
            cpuFrameTimings_.push_back(
                std::chrono::duration<float, std::milli>(profileFrameEnd - profileFrameStart).count());
            if (pathTracer_ != nullptr) {
                gpuFrameTimings_.push_back(pathTracer_->timings().totalMs());
                perFrameGpuTimings_.push_back(pathTracer_->timings());
            }
        }

        ++frameCount;
        if (maxFrames > 0 && frameCount >= maxFrames) {
            if (rendererOnly_ && rendererOnlyLingerAfterCaptureReadyMs_ > 0u && captureReadyPrinted_) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - captureReadyPrintedAt_);
                if (elapsed.count() < static_cast<int64_t>(rendererOnlyLingerAfterCaptureReadyMs_)) {
                    continue;
                }
            }
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
        loader.setNativeTextureFormatSupport(nativeTextureFormatSupportForContext(context_.get()));
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
    request.nativeTextureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
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

bool Application::queueLiveMainThreadApplyBatch(std::string label, std::vector<LiveMainThreadApplyOperation> operations) {
    operations.erase(
        std::remove_if(
            operations.begin(),
            operations.end(),
            [](const LiveMainThreadApplyOperation& operation) {
                switch (operation.kind) {
                case LiveMainThreadApplyOperationKind::ImportAsset:
                case LiveMainThreadApplyOperationKind::ImportAndPlaceAsset:
                    return operation.importRequest.sourcePath.empty();
                case LiveMainThreadApplyOperationKind::MergeScene:
                    return operation.scenePath.empty();
                case LiveMainThreadApplyOperationKind::PlacePrefabAsset:
                    return operation.prefabGuid.empty();
                case LiveMainThreadApplyOperationKind::PlaceMeshAsset:
                    return operation.meshPlacement.meshGuid.empty();
                case LiveMainThreadApplyOperationKind::PlaceMeshScatterAssets:
                    return operation.meshScatterPlacement.instances.empty();
                case LiveMainThreadApplyOperationKind::CreateEntity:
                    return false;
                case LiveMainThreadApplyOperationKind::DuplicateEntity:
                    return !operation.duplicateEntity.valid();
                case LiveMainThreadApplyOperationKind::DeleteEntity:
                    return !operation.deleteEntity.valid();
                case LiveMainThreadApplyOperationKind::DeleteEntities:
                    return operation.deleteEntities.empty();
                case LiveMainThreadApplyOperationKind::RenameEntity:
                    return !operation.renameEntity.entity.valid();
                case LiveMainThreadApplyOperationKind::ReparentEntity:
                    return !operation.reparentEntity.first.valid();
                case LiveMainThreadApplyOperationKind::SetEntityVisibility:
                case LiveMainThreadApplyOperationKind::SetEntityLocked:
                    return !operation.entityBoolChange.entity.valid();
                case LiveMainThreadApplyOperationKind::SetEntityTransform:
                    return !operation.entityTransform.entity.valid();
                case LiveMainThreadApplyOperationKind::SetEntityTransforms:
                    return operation.entityTransforms.changes.empty();
                case LiveMainThreadApplyOperationKind::SetMeshRenderer:
                    return !operation.meshRendererChange.entity.valid();
                case LiveMainThreadApplyOperationKind::AddComponent:
                case LiveMainThreadApplyOperationKind::RemoveComponent:
                    return !operation.componentRequest.entity.valid();
                case LiveMainThreadApplyOperationKind::SetLight:
                    return !operation.lightChange.entity.valid();
                case LiveMainThreadApplyOperationKind::SetSun:
                    return !operation.sunChange.entity.valid();
                case LiveMainThreadApplyOperationKind::SetCamera:
                    return !operation.cameraChange.entity.valid();
                case LiveMainThreadApplyOperationKind::UpdateMaterial:
                    return operation.materialUpdate.materialId == UINT32_MAX;
                case LiveMainThreadApplyOperationKind::AssignMaterial:
                    return !operation.materialAssignment.material.valid() ||
                        (!operation.materialAssignment.entity.valid() && !operation.materialAssignment.mesh.valid());
                case LiveMainThreadApplyOperationKind::AssignMaterialAsset:
                    return operation.materialAssetAssignment.materialGuid.empty() || !operation.materialAssetAssignment.entity.valid();
                case LiveMainThreadApplyOperationKind::AlignDistributeEntities:
                    return operation.alignDistributeRequest.entities.empty();
                case LiveMainThreadApplyOperationKind::AssignEnvironmentPath:
                    return operation.environmentPath.empty();
                case LiveMainThreadApplyOperationKind::AssignEnvironmentAsset:
                    return operation.environmentGuid.empty();
                case LiveMainThreadApplyOperationKind::ApplySceneSnapshot:
                case LiveMainThreadApplyOperationKind::EnsurePrimarySun:
                case LiveMainThreadApplyOperationKind::TogglePrimarySun:
                case LiveMainThreadApplyOperationKind::UpdateAssetTags:
                case LiveMainThreadApplyOperationKind::RenameAsset:
                case LiveMainThreadApplyOperationKind::BulkAddAssetTag:
                case LiveMainThreadApplyOperationKind::BulkRemoveAssetTag:
                case LiveMainThreadApplyOperationKind::MoveAssetsToFolder:
                case LiveMainThreadApplyOperationKind::DeleteAssets:
                case LiveMainThreadApplyOperationKind::ReimportAsset:
                case LiveMainThreadApplyOperationKind::RelinkAssetSource:
                case LiveMainThreadApplyOperationKind::ReplaceAssetReferences:
                case LiveMainThreadApplyOperationKind::RepairMissingAssetDependencies:
                case LiveMainThreadApplyOperationKind::UpdateTimeline:
                case LiveMainThreadApplyOperationKind::UpdateProjectSettings:
                case LiveMainThreadApplyOperationKind::MarkSceneUpdate:
                case LiveMainThreadApplyOperationKind::ApplyRendererSettings:
                case LiveMainThreadApplyOperationKind::ToggleDenoiser:
                case LiveMainThreadApplyOperationKind::ToggleDebugView:
                case LiveMainThreadApplyOperationKind::CycleIntermediateView:
                case LiveMainThreadApplyOperationKind::RestoreRecoveryAutosaves:
                case LiveMainThreadApplyOperationKind::DiscardRecovery:
                    return false;
                case LiveMainThreadApplyOperationKind::MountNativePackage:
                    return operation.mountNativePackageRequest.packagePath.empty();
                case LiveMainThreadApplyOperationKind::UnloadNativePackage:
                    return operation.unloadNativePackageRequest.packagePath.empty();
                case LiveMainThreadApplyOperationKind::RefreshNativePackage:
                    return operation.refreshNativePackageRequest.packagePath.empty();
                }
                return true;
            }),
        operations.end());
    if (operations.empty()) {
        return false;
    }

    std::vector<MainThreadApplyOperationDesc> ticketOperations;
    ticketOperations.reserve(operations.size());
    for (const LiveMainThreadApplyOperation& operation : operations) {
        MainThreadApplyOperationDesc desc{};
        switch (operation.kind) {
        case LiveMainThreadApplyOperationKind::ImportAsset:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.5;
            desc.label = "Queue import " + operation.importRequest.sourcePath.filename().string();
            break;
        case LiveMainThreadApplyOperationKind::ImportAndPlaceAsset:
            desc.kind = MainThreadApplyOperationKind::SelectionHandoff;
            desc.estimatedCostMs = 0.5;
            desc.label = "Queue import-and-place " + operation.importRequest.sourcePath.filename().string();
            break;
        case LiveMainThreadApplyOperationKind::MergeScene:
            desc.kind = MainThreadApplyOperationKind::EntityCreation;
            desc.estimatedCostMs = 1.0;
            desc.label = "Queue merge scene " + operation.scenePath.filename().string();
            break;
        case LiveMainThreadApplyOperationKind::PlacePrefabAsset:
            desc.kind = MainThreadApplyOperationKind::EntityCreation;
            desc.estimatedCostMs = 1.0;
            desc.label = "Place prefab asset " + operation.prefabGuid;
            break;
        case LiveMainThreadApplyOperationKind::PlaceMeshAsset:
            desc.kind = operation.meshPlacement.replaceEntity.valid() || operation.meshPlacement.attachEntity.valid()
                ? MainThreadApplyOperationKind::MeshBinding
                : MainThreadApplyOperationKind::EntityCreation;
            desc.entity = operation.meshPlacement.replaceEntity.valid()
                ? operation.meshPlacement.replaceEntity.index
                : (operation.meshPlacement.attachEntity.valid() ? operation.meshPlacement.attachEntity.index : 0u);
            desc.estimatedCostMs = 1.0;
            desc.label = "Place mesh asset " + operation.meshPlacement.meshGuid;
            break;
        case LiveMainThreadApplyOperationKind::PlaceMeshScatterAssets:
            desc.kind = MainThreadApplyOperationKind::EntityCreation;
            desc.estimatedCostMs = std::max(1.0, static_cast<double>(operation.meshScatterPlacement.instances.size()) * 0.25);
            desc.label = operation.meshScatterPlacement.label.empty()
                ? "Place mesh scatter assets"
                : "Place mesh scatter assets " + operation.meshScatterPlacement.label;
            break;
        case LiveMainThreadApplyOperationKind::CreateEntity:
            desc.kind = MainThreadApplyOperationKind::EntityCreation;
            desc.entity = operation.entityCreateRequest.parent.valid() ? operation.entityCreateRequest.parent.index : 0u;
            desc.estimatedCostMs = 0.5;
            desc.label = std::string("Create ") + createEntityKindLabel(operation.entityCreateRequest.kind);
            break;
        case LiveMainThreadApplyOperationKind::DuplicateEntity:
            desc.kind = MainThreadApplyOperationKind::EntityCreation;
            desc.entity = operation.duplicateEntity.index;
            desc.estimatedCostMs = 0.75;
            desc.label = "Duplicate entity";
            break;
        case LiveMainThreadApplyOperationKind::DeleteEntity:
            desc.kind = MainThreadApplyOperationKind::EntityDeletion;
            desc.entity = operation.deleteEntity.index;
            desc.estimatedCostMs = 0.5;
            desc.label = "Delete entity";
            break;
        case LiveMainThreadApplyOperationKind::DeleteEntities:
            desc.kind = MainThreadApplyOperationKind::EntityDeletion;
            desc.entity = !operation.deleteEntities.empty() ? operation.deleteEntities.front().index : 0u;
            desc.estimatedCostMs = std::max(0.5, static_cast<double>(operation.deleteEntities.size()) * 0.2);
            desc.label = "Delete " + std::to_string(operation.deleteEntities.size()) + " entities";
            break;
        case LiveMainThreadApplyOperationKind::RenameEntity:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.renameEntity.entity.index;
            desc.estimatedCostMs = 0.2;
            desc.label = "Rename entity";
            break;
        case LiveMainThreadApplyOperationKind::ReparentEntity:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.reparentEntity.first.index;
            desc.estimatedCostMs = 0.35;
            desc.label = "Reparent entity";
            break;
        case LiveMainThreadApplyOperationKind::SetEntityVisibility:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.entityBoolChange.entity.index;
            desc.estimatedCostMs = 0.25;
            desc.label = operation.entityBoolChange.value ? "Show entity" : "Hide entity";
            break;
        case LiveMainThreadApplyOperationKind::SetEntityLocked:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.entityBoolChange.entity.index;
            desc.estimatedCostMs = 0.2;
            desc.label = operation.entityBoolChange.value ? "Lock entity" : "Unlock entity";
            break;
        case LiveMainThreadApplyOperationKind::SetEntityTransform:
            desc.kind = MainThreadApplyOperationKind::TransformUpdate;
            desc.entity = operation.entityTransform.entity.index;
            desc.estimatedCostMs = 0.3;
            desc.label = "Set entity transform";
            break;
        case LiveMainThreadApplyOperationKind::SetEntityTransforms:
            desc.kind = MainThreadApplyOperationKind::TransformUpdate;
            desc.entity = !operation.entityTransforms.changes.empty() ? operation.entityTransforms.changes.front().entity.index : 0u;
            desc.estimatedCostMs = std::max(0.3, static_cast<double>(operation.entityTransforms.changes.size()) * 0.1);
            desc.label = "Set " + std::to_string(operation.entityTransforms.changes.size()) + " entity transforms";
            break;
        case LiveMainThreadApplyOperationKind::SetMeshRenderer:
            desc.kind = MainThreadApplyOperationKind::MeshBinding;
            desc.entity = operation.meshRendererChange.entity.index;
            desc.estimatedCostMs = 0.5;
            desc.label = "Set mesh renderer";
            break;
        case LiveMainThreadApplyOperationKind::AddComponent:
            desc.kind = MainThreadApplyOperationKind::ComponentCreation;
            desc.entity = operation.componentRequest.entity.index;
            desc.estimatedCostMs = 0.35;
            desc.label = "Add component";
            break;
        case LiveMainThreadApplyOperationKind::RemoveComponent:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.componentRequest.entity.index;
            desc.estimatedCostMs = 0.35;
            desc.label = "Remove component";
            break;
        case LiveMainThreadApplyOperationKind::SetLight:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.lightChange.entity.index;
            desc.estimatedCostMs = 0.25;
            desc.label = "Edit light component";
            break;
        case LiveMainThreadApplyOperationKind::SetSun:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.sunChange.entity.index;
            desc.estimatedCostMs = 0.25;
            desc.label = "Edit sun component";
            break;
        case LiveMainThreadApplyOperationKind::SetCamera:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.entity = operation.cameraChange.entity.index;
            desc.estimatedCostMs = 0.25;
            desc.label = "Edit camera component";
            break;
        case LiveMainThreadApplyOperationKind::UpdateMaterial:
            desc.kind = MainThreadApplyOperationKind::MaterialBinding;
            desc.estimatedCostMs = 0.5;
            desc.label = "Edit material";
            break;
        case LiveMainThreadApplyOperationKind::AssignMaterial:
            desc.kind = MainThreadApplyOperationKind::MaterialBinding;
            desc.entity = operation.materialAssignment.entity.valid() ? operation.materialAssignment.entity.index : 0u;
            desc.estimatedCostMs = 0.4;
            desc.label = "Assign material";
            break;
        case LiveMainThreadApplyOperationKind::AssignMaterialAsset:
            desc.kind = MainThreadApplyOperationKind::MaterialBinding;
            desc.entity = operation.materialAssetAssignment.entity.index;
            desc.estimatedCostMs = 0.4;
            desc.label = "Assign material asset";
            break;
        case LiveMainThreadApplyOperationKind::AlignDistributeEntities:
            desc.kind = MainThreadApplyOperationKind::TransformUpdate;
            desc.entity = !operation.alignDistributeRequest.entities.empty() ? operation.alignDistributeRequest.entities.front().index : 0u;
            desc.estimatedCostMs = std::max(0.4, static_cast<double>(operation.alignDistributeRequest.entities.size()) * 0.1);
            desc.label = "Align/distribute entities";
            break;
        case LiveMainThreadApplyOperationKind::AssignEnvironmentPath:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.5;
            desc.label = "Assign environment";
            break;
        case LiveMainThreadApplyOperationKind::AssignEnvironmentAsset:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.5;
            desc.label = "Assign environment asset";
            break;
        case LiveMainThreadApplyOperationKind::ApplySceneSnapshot:
            if (operation.sceneSnapshotChange.updateKind == SceneUpdateKind::MaterialOnly) {
                desc.kind = MainThreadApplyOperationKind::MaterialBinding;
            } else if (operation.sceneSnapshotChange.updateKind == SceneUpdateKind::TransformOnly) {
                desc.kind = MainThreadApplyOperationKind::TransformUpdate;
            } else {
                desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            }
            desc.estimatedCostMs = 0.5;
            desc.label = operation.sceneSnapshotChange.label.empty() ? "Edit scene" : operation.sceneSnapshotChange.label;
            break;
        case LiveMainThreadApplyOperationKind::EnsurePrimarySun:
            desc.kind = MainThreadApplyOperationKind::EntityCreation;
            desc.estimatedCostMs = 0.5;
            desc.label = "Ensure primary sun";
            break;
        case LiveMainThreadApplyOperationKind::TogglePrimarySun:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.25;
            desc.label = "Toggle primary sun";
            break;
        case LiveMainThreadApplyOperationKind::UpdateAssetTags:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.25;
            desc.label = "Update asset tags";
            break;
        case LiveMainThreadApplyOperationKind::RenameAsset:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.25;
            desc.label = "Rename asset";
            break;
        case LiveMainThreadApplyOperationKind::BulkAddAssetTag:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = std::max(0.25, static_cast<double>(operation.bulkAssetTagRequest.guids.size()) * 0.05);
            desc.label = "Bulk add asset tag";
            break;
        case LiveMainThreadApplyOperationKind::BulkRemoveAssetTag:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = std::max(0.25, static_cast<double>(operation.bulkAssetTagRequest.guids.size()) * 0.05);
            desc.label = "Bulk remove asset tag";
            break;
        case LiveMainThreadApplyOperationKind::MoveAssetsToFolder:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = std::max(0.25, static_cast<double>(operation.moveAssetsRequest.guids.size()) * 0.05);
            desc.label = "Move assets to folder";
            break;
        case LiveMainThreadApplyOperationKind::DeleteAssets:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = std::max(0.25, static_cast<double>(operation.deleteAssetsRequest.guids.size()) * (operation.deleteAssetsRequest.deleteGeneratedFiles ? 0.01 : 0.005));
            desc.label = operation.deleteAssetsRequest.deleteGeneratedFiles ? "Delete generated asset files" : "Delete asset records";
            break;
        case LiveMainThreadApplyOperationKind::ReimportAsset:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.5;
            desc.label = "Reimport asset";
            break;
        case LiveMainThreadApplyOperationKind::RelinkAssetSource:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.35;
            desc.label = "Relink asset source";
            break;
        case LiveMainThreadApplyOperationKind::ReplaceAssetReferences:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.75;
            desc.label = "Replace asset references";
            break;
        case LiveMainThreadApplyOperationKind::RepairMissingAssetDependencies:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.5;
            desc.label = "Repair missing asset dependencies";
            break;
        case LiveMainThreadApplyOperationKind::MountNativePackage:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 1.0;
            desc.label = "Mount native package " + operation.mountNativePackageRequest.packagePath.filename().string();
            break;
        case LiveMainThreadApplyOperationKind::UnloadNativePackage:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 1.0;
            desc.label = "Unload native package " + operation.unloadNativePackageRequest.packagePath.filename().string();
            break;
        case LiveMainThreadApplyOperationKind::RefreshNativePackage:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 2.0;
            desc.label = "Refresh native package " + operation.refreshNativePackageRequest.packagePath.filename().string();
            break;
        case LiveMainThreadApplyOperationKind::UpdateTimeline:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.25;
            desc.label = "Edit timeline";
            break;
        case LiveMainThreadApplyOperationKind::UpdateProjectSettings:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.25;
            desc.label = "Update project settings";
            break;
        case LiveMainThreadApplyOperationKind::MarkSceneUpdate:
            if (operation.sceneUpdateKind == SceneUpdateKind::MaterialOnly) {
                desc.kind = MainThreadApplyOperationKind::MaterialBinding;
            } else if (operation.sceneUpdateKind == SceneUpdateKind::TransformOnly) {
                desc.kind = MainThreadApplyOperationKind::TransformUpdate;
            } else if (operation.sceneUpdateKind == SceneUpdateKind::TopologyChanged) {
                desc.kind = MainThreadApplyOperationKind::MeshBinding;
            } else {
                desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            }
            desc.estimatedCostMs = 0.2;
            desc.label = "Mark scene update";
            break;
        case LiveMainThreadApplyOperationKind::ApplyRendererSettings:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.25;
            desc.label = "Apply renderer settings";
            break;
        case LiveMainThreadApplyOperationKind::ToggleDenoiser:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.2;
            desc.label = "Toggle denoiser";
            break;
        case LiveMainThreadApplyOperationKind::ToggleDebugView:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.2;
            desc.label = "Toggle debug view";
            break;
        case LiveMainThreadApplyOperationKind::CycleIntermediateView:
            desc.kind = MainThreadApplyOperationKind::EntityStateUpdate;
            desc.estimatedCostMs = 0.2;
            desc.label = "Cycle intermediate view";
            break;
        case LiveMainThreadApplyOperationKind::RestoreRecoveryAutosaves:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.75;
            desc.label = "Restore recovery autosaves";
            break;
        case LiveMainThreadApplyOperationKind::DiscardRecovery:
            desc.kind = MainThreadApplyOperationKind::DependencyRestore;
            desc.estimatedCostMs = 0.2;
            desc.label = "Discard recovery marker";
            break;
        }
        ticketOperations.push_back(std::move(desc));
    }

    const uint64_t ticketId = editorMainThreadApplyTickets_.create(std::move(label), std::move(ticketOperations));
    if (ticketId == 0) {
        notifications_.notify("Main-thread apply batch blocked", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
        return false;
    }

    liveMainThreadApplyBatches_.push_back(LiveMainThreadApplyBatch{
        .ticketId = ticketId,
        .label = liveMainThreadApplyBatches_.empty() ? std::string("Live main-thread apply batch") : liveMainThreadApplyBatches_.back().label,
        .operations = std::move(operations),
    });
    liveMainThreadApplyBatches_.back().label = label.empty() ? "Live main-thread apply batch" : std::move(label);
    notifications_.notify("Main-thread apply batch queued", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 4.0f);
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().log().add(EditorLogCategory::Command, "Queued main-thread apply batch: " + liveMainThreadApplyBatches_.back().label);
    }
    return true;
}

void Application::executeLiveMainThreadApplyOperations(const MainThreadApplyStepResult& applyResult) {
    for (const MainThreadApplyAppliedOperation& applied : applyResult.appliedOperationRecords) {
        const auto batchIt = std::find_if(liveMainThreadApplyBatches_.begin(), liveMainThreadApplyBatches_.end(), [&](const LiveMainThreadApplyBatch& batch) {
            return batch.ticketId == applied.ticketId;
        });
        if (batchIt == liveMainThreadApplyBatches_.end() || applied.index >= batchIt->operations.size()) {
            continue;
        }

        LiveMainThreadApplyOperation& operation = batchIt->operations[applied.index];
        if (operation.executed) {
            continue;
        }
        operation.executed = true;
        switch (operation.kind) {
        case LiveMainThreadApplyOperationKind::ImportAsset:
            (void)queueAssetImportNonMutating(operation.importRequest, false);
            break;
        case LiveMainThreadApplyOperationKind::ImportAndPlaceAsset:
            if (operation.importRequest.mode.empty()) {
                operation.importRequest.mode = "ImportAndPlace";
            }
            (void)queueAssetImportNonMutating(operation.importRequest, true);
            break;
        case LiveMainThreadApplyOperationKind::MergeScene:
            queueMergeScenes(std::vector<std::filesystem::path>{operation.scenePath});
            break;
        case LiveMainThreadApplyOperationKind::PlacePrefabAsset:
            (void)placePrefabAsset(operation.prefabGuid, operation.prefabPlacementTransform);
            break;
        case LiveMainThreadApplyOperationKind::PlaceMeshAsset:
            (void)placeMeshAsset(operation.meshPlacement);
            break;
        case LiveMainThreadApplyOperationKind::PlaceMeshScatterAssets:
            (void)placeMeshScatterAssets(operation.meshScatterPlacement);
            break;
        case LiveMainThreadApplyOperationKind::CreateEntity:
            if (createEntityFromEditor(operation.entityCreateRequest)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::DuplicateEntity:
            if (duplicateEntityFromEditor(operation.duplicateEntity)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::DeleteEntity:
            if (deleteEntityFromEditor(operation.deleteEntity)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::DeleteEntities:
            if (deleteEntitiesFromEditor(operation.deleteEntities)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::RenameEntity:
            if (renameEntityFromEditor(operation.renameEntity)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::ReparentEntity:
            if (reparentEntityFromEditor(operation.reparentEntity.first, operation.reparentEntity.second)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetEntityVisibility:
            if (setEntityVisibilityFromEditor(operation.entityBoolChange)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetEntityLocked:
            if (setEntityLockedFromEditor(operation.entityBoolChange)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetEntityTransform:
            if (setEntityTransformFromEditor(operation.entityTransform)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetEntityTransforms:
            if (setEntityTransformsFromEditor(operation.entityTransforms)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetMeshRenderer:
            if (setMeshRendererFromEditor(operation.meshRendererChange)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::AddComponent:
            if (addComponentFromEditor(operation.componentRequest)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::RemoveComponent:
            if (removeComponentFromEditor(operation.componentRequest)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetLight:
            if (setLightFromEditor(operation.lightChange)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetSun:
            if (setSunFromEditor(operation.sunChange)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::SetCamera:
            if (setCameraFromEditor(operation.cameraChange)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::UpdateMaterial:
            if (updateMaterialFromEditor(operation.materialUpdate)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::AssignMaterial:
            if (assignMaterialFromEditor(operation.materialAssignment)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::AssignMaterialAsset:
            if (assignMaterialAssetToEntity(operation.materialAssetAssignment)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::AlignDistributeEntities:
            if (alignDistributeEntitiesFromEditor(operation.alignDistributeRequest)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::AssignEnvironmentPath:
            (void)assignEnvironmentPathFromEditor(operation.environmentPath, true);
            break;
        case LiveMainThreadApplyOperationKind::AssignEnvironmentAsset:
            (void)assignEnvironmentAssetFromEditor(operation.environmentGuid, true);
            break;
        case LiveMainThreadApplyOperationKind::ApplySceneSnapshot:
            if (applySceneSnapshotFromEditor(operation.sceneSnapshotChange)) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::EnsurePrimarySun:
            if (ensurePrimarySunFromEditor()) {
                (void)applyPendingSceneUpdate(true);
            }
            break;
        case LiveMainThreadApplyOperationKind::TogglePrimarySun:
            (void)togglePrimarySunFromEditor(true);
            break;
        case LiveMainThreadApplyOperationKind::UpdateAssetTags:
            (void)updateAssetTags(operation.assetTagsRequest);
            break;
        case LiveMainThreadApplyOperationKind::RenameAsset:
            (void)renameAssetRecord(operation.renameAssetRequest);
            break;
        case LiveMainThreadApplyOperationKind::BulkAddAssetTag:
            (void)bulkAddAssetTag(operation.bulkAssetTagRequest);
            break;
        case LiveMainThreadApplyOperationKind::BulkRemoveAssetTag:
            (void)bulkRemoveAssetTag(operation.bulkAssetTagRequest);
            break;
        case LiveMainThreadApplyOperationKind::MoveAssetsToFolder:
            (void)moveAssetsToFolder(operation.moveAssetsRequest);
            break;
        case LiveMainThreadApplyOperationKind::DeleteAssets:
            (void)deleteAssetsFromRegistry(operation.deleteAssetsRequest);
            break;
        case LiveMainThreadApplyOperationKind::ReimportAsset:
            (void)queueAssetReimport(operation.assetGuid);
            break;
        case LiveMainThreadApplyOperationKind::RelinkAssetSource:
            (void)relinkAssetSource(operation.relinkAssetSourceRequest);
            break;
        case LiveMainThreadApplyOperationKind::ReplaceAssetReferences:
            (void)replaceAssetReferences(operation.replaceAssetReferencesRequest, true);
            break;
        case LiveMainThreadApplyOperationKind::RepairMissingAssetDependencies:
            (void)repairMissingAssetDependencies(operation.repairMissingDependenciesRequest);
            break;
        case LiveMainThreadApplyOperationKind::MountNativePackage:
            (void)mountNativePackageFromEditor(operation.mountNativePackageRequest.packagePath);
            break;
        case LiveMainThreadApplyOperationKind::UnloadNativePackage:
            (void)unloadNativePackageFromEditor(operation.unloadNativePackageRequest.packagePath);
            break;
        case LiveMainThreadApplyOperationKind::RefreshNativePackage:
            (void)refreshNativePackageFromEditor(operation.refreshNativePackageRequest.packagePath);
            break;
        case LiveMainThreadApplyOperationKind::UpdateTimeline:
            (void)updateTimelineFromEditor(operation.timelineJson);
            break;
        case LiveMainThreadApplyOperationKind::UpdateProjectSettings:
            (void)updateProjectSettingsFromEditor(operation.projectSettingsUpdate);
            break;
        case LiveMainThreadApplyOperationKind::MarkSceneUpdate:
            (void)markSceneUpdateFromEditor(operation.sceneUpdateKind, true);
            break;
        case LiveMainThreadApplyOperationKind::ApplyRendererSettings:
            (void)applyRendererSettingsFromEditor(operation.rendererSettings, true);
            break;
        case LiveMainThreadApplyOperationKind::ToggleDenoiser:
            (void)toggleDenoiserFromEditor(true);
            break;
        case LiveMainThreadApplyOperationKind::ToggleDebugView:
            (void)toggleDebugViewFromEditor(true);
            break;
        case LiveMainThreadApplyOperationKind::CycleIntermediateView:
            (void)cycleIntermediateViewFromEditor(true);
            break;
        case LiveMainThreadApplyOperationKind::RestoreRecoveryAutosaves:
            (void)restoreRecoveryAutosavesFromEditor();
            break;
        case LiveMainThreadApplyOperationKind::DiscardRecovery:
            (void)discardRecoveryFromEditor();
            break;
        }
    }

    liveMainThreadApplyBatches_.erase(
        std::remove_if(
            liveMainThreadApplyBatches_.begin(),
            liveMainThreadApplyBatches_.end(),
            [](const LiveMainThreadApplyBatch& batch) {
                return std::all_of(batch.operations.begin(), batch.operations.end(), [](const LiveMainThreadApplyOperation& operation) {
                    return operation.executed;
                });
            }),
        liveMainThreadApplyBatches_.end());
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
                NativeRuntimeLoadOptions nativeLoadOptions;
                nativeLoadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
                nativeLoadOptions.validatePayloadHashes = false;
                nativeLoadOptions.retainLoadedPayloadsInReport = false;
                if (std::string bindError; !appendPrefabRuntimeAssets(*recordIt, prefab, registryRoot, &assetRegistry_, result.assets, prefabBindings, nativeLoadOptions, &bindError)) {
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
        if (result.mode == SceneLoadMode::LoadProjectStartupScene) {
            (void)mountProjectStartupNativePackage(result.assets);
        }
        prepareDocumentMs = elapsedMs(prepareDocumentStart);

        std::cout << sceneLoadModeLabel(result.mode) << " apply stage: scene_builder path="
                  << result.sourcePath.string() << '\n' << std::flush;
        const auto sceneBuildStart = std::chrono::steady_clock::now();
        const SceneGpuBuildResult build = sceneBuilder_.build(nextDocument, &result.assets, reloadSettings);
        sceneBuildMs = elapsedMs(sceneBuildStart);
        const std::optional<std::filesystem::path> cachePath = sceneDirtyAfterApply
            ? SceneCache::cachePathFor(result.sourcePath)
            : (nextGltfPath.has_value() ? SceneCache::cachePathFor(*nextGltfPath) : std::optional<std::filesystem::path>{});
        SceneCachePolicy rendererCachePolicy;
        if (result.importedScene.has_value() && cachePath.has_value()) {
            const bool geometrySignatureMatches =
                build.sceneAsset.meshes.size() == result.importedScene->meshes.size() &&
                build.sceneAsset.materials.size() == result.importedScene->materials.size() &&
                build.sceneAsset.textures.size() == result.importedScene->textures.size();
            const bool fullSceneSignatureMatches =
                geometrySignatureMatches &&
                build.sceneAsset.nodes.size() == result.importedScene->nodes.size() &&
                nextDocument.prefabInstances().empty();
            if (fullSceneSignatureMatches) {
                rendererCachePolicy = SceneCachePolicy{
                    .mode = SceneCacheMode::FullReadWrite,
                    .path = cachePath,
                };
            } else if (geometrySignatureMatches) {
                rendererCachePolicy = SceneCachePolicy{
                    .mode = SceneCacheMode::GeometryReadOnly,
                    .path = cachePath,
                };
            }
        }

        std::cout << sceneLoadModeLabel(result.mode) << " apply stage: renderer_create meshes="
                  << build.sceneAsset.meshes.size()
                  << " materials=" << build.sceneAsset.materials.size()
                  << " textures=" << build.sceneAsset.textures.size()
                  << " path=" << result.sourcePath.string() << '\n' << std::flush;
        const auto rendererCreateStart = std::chrono::steady_clock::now();
        RendererSettings replacementSettings = build.rendererSettings;
        traceStartupPhase("renderer_create_settings_ready");
        if (disableDlssForRendererReplacement(replacementSettings)) {
            syncDocumentRenderSettings(nextDocument, replacementSettings);
            const std::string message = "DLSS disabled for scene rebuild; re-enable it after the scene is loaded.";
            std::cerr << message << '\n';
            notifications_.notify(message, NotificationType::Warning, NotificationAction::OpenRenderSettings, "Render Settings", 6.0f);
        }
        const bool reuseExistingFallbackRenderer =
            pathTracer_ != nullptr &&
            build.sceneAsset.meshes.empty() &&
            (!gpuSceneAsset_.has_value() || gpuSceneAsset_->meshes.empty()) &&
            nextHdrPath == hdrPath_;
        std::unique_ptr<PathTracerRenderer> nextPathTracer;
        if (reuseExistingFallbackRenderer) {
            traceStartupPhase("renderer_reuse_empty_scene_begin");
            (void)pathTracer_->applySettings(replacementSettings);
            (void)pathTracer_->updateSceneLights(build.sceneAsset, true);
            traceStartupPhase("renderer_reuse_empty_scene_end");
        } else {
            traceStartupPhase("renderer_create_prepare_begin");
            preparePathTracerForRendererReplacement(pathTracer_ != nullptr ? pathTracer_->settings() : replacementSettings);
            traceStartupPhase("renderer_create_prepare_end");
            traceStartupPhase("renderer_create_make_begin");
            nextPathTracer = makePathTracer(
                build.sceneAsset.meshes.empty() ? nullptr : &build.sceneAsset,
                build.sceneAsset.meshes.empty() ? nullptr : &result.assets,
                std::move(rendererCachePolicy),
                &replacementSettings);
            traceStartupPhase("renderer_create_make_end");
        }
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
        latestAnimatedGeometryStats_ = build.animatedGeometry;
        latestGpuSkinningPlan_ = build.gpuSkinningPlan;
        latestGpuSkinningJointMatrices_ = build.gpuSkinningJointMatrices;
        latestGpuSkinningPreviousJointMatrices_ = build.gpuSkinningPreviousJointMatrices;
        latestGpuSkinningSourceVertices_ = build.gpuSkinningSourceVertices;
        latestGpuSkinningMorphDeltas_ = build.gpuSkinningMorphDeltas;
        if (!reuseExistingFallbackRenderer) {
            retirePathTracer(std::move(pathTracer_));
            pathTracer_ = std::move(nextPathTracer);
        }
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
    const std::string extension = lowerPathExtension(result.sourcePath);
    EntityId root{};
    if ((extension == ".rtlevel" || extension == ".mscene") && result.stagedScene != nullptr) {
        const AssetGuid parentSceneGuid = sceneDocument_.rtLevelHeader().sceneGuid;
        const AssetGuid childSceneGuid = result.stagedScene->rtLevelHeader().sceneGuid;
        const bool sameSceneGuid = !parentSceneGuid.empty() && parentSceneGuid == childSceneGuid;
        const bool sameScenePath = scenePath_.has_value() && normalizedPathForCompare(*scenePath_) == normalizedPathForCompare(result.sourcePath);
        const bool childReferencesParent = std::any_of(result.stagedScene->sublevels().begin(), result.stagedScene->sublevels().end(), [&](const SceneSublevelRecord& sublevel) {
            return (!parentSceneGuid.empty() && sublevel.sceneGuid == parentSceneGuid) ||
                (scenePath_.has_value() && !sublevel.scenePath.empty() && normalizedPathForCompare(sublevel.scenePath) == normalizedPathForCompare(*scenePath_));
        });
        if (sameSceneGuid || sameScenePath || childReferencesParent) {
            assets_ = std::move(previousAssets);
            sceneLoadingStatus_ = "Level instance cycle rejected: " + result.sourcePath.string();
            recordCompletedSceneLoadJob(result, false, false, sceneLoadingStatus_, "Recursive level inclusion is not allowed", result.warningMessage);
            notifications_.notify("Level instance cycle rejected", NotificationType::Error);
            std::cerr << sceneLoadingStatus_ << '\n';
            return false;
        }
        SceneSublevelRecord sublevel;
        sublevel.sceneGuid = result.stagedScene->rtLevelHeader().sceneGuid;
        sublevel.scenePath = result.sourcePath;
        sublevel.visible = true;
        sublevel.loaded = true;
        sublevel.editable = false;
        sublevel.sourceHash = assetSourceHashForPath(result.sourcePath);
        root = sceneOps.mergeLevelInstanceAsset(sceneToMerge, std::move(sublevel), "Level Instance " + result.sourcePath.stem().string());
    } else {
        root = sceneOps.mergeSceneAsset(sceneToMerge, rootName);
    }
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
        dirtyBuckets.push_back("Parent level: " + sceneName);
        if (sceneDocument_.hasSublevelDirtyState()) {
            dirtyBuckets.push_back("Level instances/sublevels: overrides or source sublevel edits");
        }
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
    const auto& runtimeMaterials = assets_.materials();
    for (uint32_t materialIndex = 0; materialIndex < runtimeMaterials.size(); ++materialIndex) {
        if (runtimeMaterials[materialIndex].nativeGuid == record.guid) {
            return materialIndex;
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
    const auto& runtimeMeshes = assets_.meshes();
    for (uint32_t meshIndex = 0; meshIndex < runtimeMeshes.size(); ++meshIndex) {
        if (runtimeMeshes[meshIndex].nativeGuid == record.guid) {
            return meshIndex;
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
        (void)mountProjectStartupNativePackage(assets_);
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

bool Application::mountProjectStartupNativePackage(AssetManager& assets) {
    if (!project_.has_value()) {
        return false;
    }
    const ProjectContext& project = *project_;
    const std::filesystem::path packagePath = projectCookPackagePath(project);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(packagePath, ec)) {
        return false;
    }

    NativeRuntimeLoadOptions loadOptions;
    loadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
    const uint64_t packageBytes = regularFileSizeOrZero(packagePath);
    const std::filesystem::path reportPath = projectStartupNativePackageMountReportPath(project);
    if (packageBytes >= loadOptions.eagerCpuLoadWarningBytes) {
        nlohmann::json report = legacyCpuPackageMountBlockedJson(
            "ProjectStartupNativePackageMountV1",
            "ApplicationProjectOpen",
            packagePath,
            packageBytes,
            loadOptions);
        report["project"] = {
            {"name", project.name},
            {"projectFile", project.projectFile.generic_string()},
            {"buildRoot", project.buildRoot.generic_string()},
        };
        report["package"]["automaticStartupMount"] = true;
        report["policy"] = "Project-open automatic CPU package mount is skipped for large .rtpkg files. Large project payloads should use progressive native streaming and DirectStorage-backed upload tickets.";
        std::string writeError;
        if (!writeCookJsonArtifact(reportPath, report, &writeError)) {
            std::cerr << "Startup native package blocked-report write failed: " << writeError << '\n';
        }
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Startup native package CPU mount skipped for large package: " + packagePath.string());
        }
        notifications_.notify("Startup package uses streaming path", NotificationType::Info, NotificationAction::OpenProjectManager, "Project Manager", 5.0f);
        return false;
    }

    NativeAssetRuntimeLoader loader;
    NativeRuntimeLoadReport loadReport = loader.loadLooseRoot(packagePath, &assets, loadOptions);
    nlohmann::json report = nativeRuntimeLoadReportToJson(loadReport);
    report["schema"] = "ProjectStartupNativePackageMountV1";
    report["inspectionSource"] = "ApplicationProjectOpen";
    report["project"] = {
        {"name", project.name},
        {"projectFile", project.projectFile.generic_string()},
        {"buildRoot", project.buildRoot.generic_string()},
    };
    report["package"] = {
        {"path", packagePath.generic_string()},
        {"exists", true},
        {"automaticStartupMount", true},
    };
    report["assetManagerAfterMount"] = {
        {"textureCount", assets.textures().size()},
        {"materialCount", assets.materials().size()},
        {"meshCount", assets.meshes().size()},
    };
    report["rendererPlacementFromPackageImplemented"] = true;
    report["directRendererResourceUploadFromPackageImplemented"] = false;
    report["policy"] = "Project open automatically decodes the deterministic cooked .rtpkg into the CPU AssetManager before startup scene GPU build. Mesh placement and material assignment can resolve package-mounted native mesh/material GUIDs through the CPU AssetManager; direct renderer resource upload remains separate work.";

    std::string writeError;
    if (!writeCookJsonArtifact(reportPath, report, &writeError)) {
        std::cerr << "Startup native package mount report write failed: " << writeError << '\n';
    }

    if (loadReport.ok) {
        rememberMountedNativePackage(packagePath, loadReport);
        const std::string message = "Mounted startup native package: " + packagePath.string() +
            " meshes=" + std::to_string(loadReport.meshCount) +
            " materials=" + std::to_string(loadReport.materialCount) +
            " textures=" + std::to_string(loadReport.textureCount);
        std::cout << message << '\n';
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Project, message);
        }
        return true;
    }

    std::cerr << "Startup native package mount failed: " << packagePath.string() << " report=" << reportPath.string() << '\n';
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Startup native package mount failed: " + packagePath.string());
    }
    notifications_.notify("Startup native package mount failed", NotificationType::Warning, NotificationAction::OpenProjectManager, "Project Manager", 6.0f);
    return false;
}

void Application::rememberMountedNativePackage(const std::filesystem::path& packagePath, const NativeRuntimeLoadReport& loadReport) {
    if (packagePath.empty() || !loadReport.ok) {
        return;
    }
    const std::filesystem::path key = normalizedPackagePathKey(packagePath);
    MountedNativePackageWatch* watch = nullptr;
    for (MountedNativePackageWatch& candidate : mountedNativePackages_) {
        if (normalizedPackagePathKey(candidate.packagePath) == key) {
            watch = &candidate;
            break;
        }
    }
    if (watch == nullptr) {
        mountedNativePackages_.push_back(MountedNativePackageWatch{});
        watch = &mountedNativePackages_.back();
        watch->generation = nativePackageWatchGeneration_++;
    }
    watch->packagePath = packagePath;
    watch->lastWriteTime = nativePackageLastWriteTimeOrZero(packagePath);
    watch->textureCount = loadReport.textureCount;
    watch->materialCount = loadReport.materialCount;
    watch->meshCount = loadReport.meshCount;
    watch->changeDetected = false;
    watch->detectedWriteTime = {};
}

void Application::forgetMountedNativePackage(const std::filesystem::path& packagePath) {
    if (packagePath.empty()) {
        return;
    }
    const std::filesystem::path key = normalizedPackagePathKey(packagePath);
    mountedNativePackages_.erase(
        std::remove_if(
            mountedNativePackages_.begin(),
            mountedNativePackages_.end(),
            [&](const MountedNativePackageWatch& watch) {
                return normalizedPackagePathKey(watch.packagePath) == key;
            }),
        mountedNativePackages_.end());
}

void Application::pollMountedNativePackageChanges(const EditorRequests& requests) {
    if (requests.refreshNativePackage.has_value() || mountedNativePackages_.empty()) {
        return;
    }
    for (MountedNativePackageWatch& watch : mountedNativePackages_) {
        if (watch.packagePath.empty() || watch.changeDetected) {
            continue;
        }
        const std::filesystem::file_time_type currentStamp = nativePackageLastWriteTimeOrZero(watch.packagePath);
        if (currentStamp == std::filesystem::file_time_type{} || watch.lastWriteTime == std::filesystem::file_time_type{} || currentStamp == watch.lastWriteTime) {
            continue;
        }
        watch.changeDetected = true;
        watch.detectedWriteTime = currentStamp;
        const std::filesystem::path reportPath = editorNativePackageRefreshDetectionReportPath(project_, watch.packagePath);
        nlohmann::json report = {
            {"schema", "ContentBrowserPackageRefreshDetectionV1"},
            {"inspectionSource", "MountedPackageTimestampPoll"},
            {"package", {{"path", watch.packagePath.generic_string()}, {"exists", true}, {"watchGeneration", watch.generation}}},
            {"mountedAssetCounts", {{"textures", watch.textureCount}, {"materials", watch.materialCount}, {"meshes", watch.meshCount}}},
            {"previousWriteTimeTicks", watch.lastWriteTime.time_since_epoch().count()},
            {"detectedWriteTimeTicks", currentStamp.time_since_epoch().count()},
            {"mutationExecuted", false},
            {"refreshQueuedAutomatically", false},
            {"recommendedAction", "Use the Content Browser Refresh Package action after reviewing the changed .rtpkg file."},
            {"policy", "Mounted package timestamp polling detects changed package files and writes this report, but does not mutate runtime assets automatically. Refresh remains an explicit selected-action workflow with confirmation."},
            {"limitations", nlohmann::json::array({
                "Provider-level conflict resolution remains separate source-control work.",
                "Direct NativeAssetStore-to-GPU upload and renderer-owned native store handles remain open roadmap work."
            })},
        };
        std::string writeError;
        const bool wroteReport = writeCookJsonArtifact(reportPath, report, &writeError);
        notifications_.notify("Package changed on disk; refresh available", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Project,
                "Detected mounted package change; explicit refresh required: " + watch.packagePath.string() +
                (wroteReport ? " report=" + reportPath.string() : " report_write_failed=" + writeError));
        }
        return;
    }
}

bool Application::mountNativePackageFromEditor(const std::filesystem::path& packagePath) {
    std::error_code ec;
    if (packagePath.empty() || nativeAssetKindFromExtension(packagePath) != NativeAssetKind::Package || !std::filesystem::is_regular_file(packagePath, ec)) {
        notifications_.notify("Package mount failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Package mount failed: invalid package path " + packagePath.string());
        }
        return false;
    }

    NativeRuntimeLoadOptions loadOptions;
    loadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
    const std::filesystem::path reportPath = editorNativePackageMountReportPath(project_, packagePath);
    const uint64_t packageBytes = regularFileSizeOrZero(packagePath);
    if (packageBytes >= loadOptions.eagerCpuLoadWarningBytes) {
        nlohmann::json report = legacyCpuPackageMountBlockedJson(
            "ContentBrowserPackageMountV1",
            "ContentBrowserDiagnosticCpuMountPackage",
            packagePath,
            packageBytes,
            loadOptions);
        report["package"]["mountedFromUi"] = true;
        report["project"] = project_.has_value()
            ? nlohmann::json{{"name", project_->name}, {"projectFile", project_->projectFile.generic_string()}, {"projectRoot", project_->projectRoot.generic_string()}}
            : nlohmann::json{{"name", ""}, {"projectFile", ""}, {"projectRoot", ""}};
        report["policy"] = "Content Browser diagnostic CPU package mount is blocked for large .rtpkg files before payload decode. Use package inspection, validation, and progressive streaming instead.";
        std::string writeError;
        const bool wroteReport = writeCookJsonArtifact(reportPath, report, &writeError);
        notifications_.notify("Diagnostic CPU mount blocked", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Diagnostic CPU package mount blocked for large package: " + packagePath.string() + (wroteReport ? " report=" + reportPath.string() : std::string{}));
        }
        if (wroteReport) {
            (void)openFileInShell(reportPath);
        }
        return false;
    }

    const size_t textureCountBefore = assets_.textures().size();
    const size_t materialCountBefore = assets_.materials().size();
    const size_t meshCountBefore = assets_.meshes().size();

    NativeAssetRuntimeLoader loader;
    NativeRuntimeLoadReport loadReport = loader.loadLooseRoot(packagePath, &assets_, loadOptions);

    nlohmann::json report = nativeRuntimeLoadReportToJson(loadReport);
    report["schema"] = "ContentBrowserPackageMountV1";
    report["inspectionSource"] = "ContentBrowserMountPackage";
    report["package"] = {
        {"path", packagePath.generic_string()},
        {"exists", true},
        {"mountedFromUi", true},
    };
    report["project"] = project_.has_value()
        ? nlohmann::json{{"name", project_->name}, {"projectFile", project_->projectFile.generic_string()}, {"projectRoot", project_->projectRoot.generic_string()}}
        : nlohmann::json{{"name", ""}, {"projectFile", ""}, {"projectRoot", ""}};
    report["assetManagerBeforeMount"] = {
        {"textureCount", textureCountBefore},
        {"materialCount", materialCountBefore},
        {"meshCount", meshCountBefore},
    };
    report["assetManagerAfterMount"] = {
        {"textureCount", assets_.textures().size()},
        {"materialCount", assets_.materials().size()},
        {"meshCount", assets_.meshes().size()},
    };
    report["mutationExecuted"] = true;
    report["mutatedState"] = "CPU AssetManager";
    report["rendererPlacementFromPackageImplemented"] = true;
    report["directRendererResourceUploadFromPackageImplemented"] = false;
    report["policy"] = "Content Browser Diagnostic CPU Mount decodes small .rtpkg mesh, material, and texture payloads into the active CPU AssetManager after explicit confirmation. Large packages are blocked before payload decode and should use progressive native streaming instead.";

    std::string writeError;
    const bool wroteReport = writeCookJsonArtifact(reportPath, report, &writeError);
    if (!wroteReport) {
        std::cerr << "Content Browser package mount report write failed: " << writeError << '\n';
    }

    const std::string summary = "Diagnostic CPU mounted package from Content Browser: " + packagePath.string() +
        " meshes=" + std::to_string(loadReport.meshCount) +
        " materials=" + std::to_string(loadReport.materialCount) +
        " textures=" + std::to_string(loadReport.textureCount);
    if (loadReport.ok) {
        rememberMountedNativePackage(packagePath, loadReport);
        notifications_.notify("Diagnostic CPU package mounted", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        std::cout << summary << '\n';
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, summary + (wroteReport ? " report=" + reportPath.string() : std::string{}));
        }
        if (wroteReport) {
            (void)openFileInShell(reportPath);
        }
        return true;
    }

    notifications_.notify("Diagnostic CPU package mount failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
    std::cerr << "Content Browser diagnostic CPU package mount failed: " << packagePath.string();
    if (wroteReport) {
        std::cerr << " report=" << reportPath.string();
    }
    std::cerr << '\n';
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Diagnostic CPU package mount failed: " + packagePath.string() + (wroteReport ? " report=" + reportPath.string() : std::string{}));
    }
    if (wroteReport) {
        (void)openFileInShell(reportPath);
    }
    return false;
}

bool Application::rebuildRendererAfterNativePackageUnload(bool affectedActiveRenderer, nlohmann::json& report) {
    report["rendererRetirement"] = {
        {"activeRendererExisted", pathTracer_ != nullptr},
        {"activeRendererAffected", affectedActiveRenderer},
        {"retiredRendererQueued", false},
        {"resourceRetirementPolicy", "No active renderer resources referenced the unloaded package, so no renderer replacement was required."},
    };
    if (!affectedActiveRenderer || pathTracer_ == nullptr || commandSystem_ == nullptr) {
        rebuildGpuSceneAsset();
        return true;
    }

    const RendererSettings previousSettings = pathTracer_->settings();
    try {
        rebuildGpuSceneAsset();
        preparePathTracerForRendererReplacement(previousSettings);
        std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
            currentSceneCachePolicyForRenderer(),
            &previousSettings);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->invalidateRendererTextures();
            uiOverlay_->editor().invalidateAssetThumbnails();
        }
        retirePathTracer(std::move(pathTracer_));
        pathTracer_ = std::move(nextPathTracer);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(AccumulationResetReason::SceneChanged);
        commandSystem_->setPathTracer(pathTracer_.get());
        report["rendererRetirement"] = {
            {"activeRendererExisted", true},
            {"activeRendererAffected", true},
            {"retiredRendererQueued", true},
            {"retiredRendererQueueDepth", retiredPathTracers_.size()},
            {"releaseFrame", frameSerial_ + CommandSystem::framesInFlight + 1u},
            {"resourceRetirementPolicy", "Active package-backed renderer resources were retired by replacing the PathTracerRenderer and queueing the old renderer until the frames-in-flight fence window has passed."},
        };
        return true;
    } catch (const std::exception& error) {
        report["rendererRetirement"]["error"] = error.what();
        return false;
    }
}

bool Application::unloadNativePackageFromEditor(const std::filesystem::path& packagePath) {
    if (packagePath.empty() || nativeAssetKindFromExtension(packagePath) != NativeAssetKind::Package) {
        notifications_.notify("Package unload failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Package unload failed: invalid package path " + packagePath.string());
        }
        return false;
    }

    const size_t textureCountBefore = assets_.textures().size();
    const size_t materialCountBefore = assets_.materials().size();
    const size_t meshCountBefore = assets_.meshes().size();

    std::vector<uint8_t> removeTextures(textureCountBefore, 0u);
    std::vector<uint8_t> removeMaterials(materialCountBefore, 0u);
    std::vector<uint8_t> removeMeshes(meshCountBefore, 0u);
    nlohmann::json removedGuids = nlohmann::json::object();
    removedGuids["textures"] = nlohmann::json::array();
    removedGuids["materials"] = nlohmann::json::array();
    removedGuids["meshes"] = nlohmann::json::array();

    for (size_t i = 0; i < textureCountBefore; ++i) {
        const TextureAsset& texture = assets_.textures()[i];
        if (texture.nativeSource == "package" && nativeRuntimePathBelongsToPackage(texture.nativePath, packagePath)) {
            removeTextures[i] = 1u;
            removedGuids["textures"].push_back(texture.nativeGuid);
        }
    }
    for (size_t i = 0; i < materialCountBefore; ++i) {
        const MaterialAsset& material = assets_.materials()[i];
        if (material.nativeSource == "package" && nativeRuntimePathBelongsToPackage(material.nativePath, packagePath)) {
            removeMaterials[i] = 1u;
            removedGuids["materials"].push_back(material.nativeGuid);
        }
    }
    for (size_t i = 0; i < meshCountBefore; ++i) {
        const MeshAsset& mesh = assets_.meshes()[i];
        if (mesh.nativeSource == "package" && nativeRuntimePathBelongsToPackage(mesh.nativePath, packagePath)) {
            removeMeshes[i] = 1u;
            removedGuids["meshes"].push_back(mesh.nativeGuid);
        }
    }

    auto countRemoved = [](const std::vector<uint8_t>& flags) {
        return static_cast<size_t>(std::count(flags.begin(), flags.end(), 1u));
    };
    const size_t removedTextureCount = countRemoved(removeTextures);
    const size_t removedMaterialCount = countRemoved(removeMaterials);
    const size_t removedMeshCount = countRemoved(removeMeshes);

    const std::filesystem::path reportPath = editorNativePackageUnloadReportPath(project_, packagePath);
    nlohmann::json report = {
        {"schema", "ContentBrowserPackageUnloadV1"},
        {"inspectionSource", "ContentBrowserUnloadPackage"},
        {"package", {{"path", packagePath.generic_string()}, {"unloadedFromUi", true}}},
        {"project", project_.has_value()
            ? nlohmann::json{{"name", project_->name}, {"projectFile", project_->projectFile.generic_string()}, {"projectRoot", project_->projectRoot.generic_string()}}
            : nlohmann::json{{"name", ""}, {"projectFile", ""}, {"projectRoot", ""}}},
        {"assetManagerBeforeUnload", {{"textureCount", textureCountBefore}, {"materialCount", materialCountBefore}, {"meshCount", meshCountBefore}}},
        {"removedAssetCounts", {{"textures", removedTextureCount}, {"materials", removedMaterialCount}, {"meshes", removedMeshCount}}},
        {"removedGuids", removedGuids},
        {"mutationExecuted", false},
        {"mutatedState", "None"},
        {"directRendererResourceUploadFromPackageImplemented", false},
        {"policy", "Content Browser Unload Package removes package-backed CPU AssetManager assets, remaps or clears scene/imported handles, rebuilds the GpuScene, and retires active renderer resources through the existing renderer replacement queue when those assets were in use."},
    };

    if (removedTextureCount == 0 && removedMaterialCount == 0 && removedMeshCount == 0) {
        report["ok"] = false;
        report["warnings"] = nlohmann::json::array({"No active CPU runtime assets matched this package path. Mount the package first, or select the exact mounted package file."});
        std::string writeError;
        const bool wroteReport = writeCookJsonArtifact(reportPath, report, &writeError);
        notifications_.notify("No package assets to unload", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Package unload found no mounted runtime assets: " + packagePath.string());
        }
        if (wroteReport) {
            (void)openFileInShell(reportPath);
        }
        return false;
    }

    bool affectedActiveRenderer = false;
    for (Entity* entity : sceneDocument_.registry().entities()) {
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            continue;
        }
        MeshRenderer& renderer = *entity->meshRenderer;
        if (renderer.mesh.valid() && renderer.mesh.index < removeMeshes.size() && removeMeshes[renderer.mesh.index]) {
            affectedActiveRenderer = true;
        }
        if (renderer.mesh.valid() && renderer.mesh.index < assets_.meshes().size()) {
            const MeshAsset& mesh = assets_.meshes()[renderer.mesh.index];
            for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
                if (primitive.material.valid() && primitive.material.index < removeMaterials.size() && removeMaterials[primitive.material.index]) {
                    affectedActiveRenderer = true;
                }
            }
        }
        for (const MaterialSlot& slot : renderer.materialSlots) {
            if (slot.material.valid() && slot.material.index < removeMaterials.size() && removeMaterials[slot.material.index]) {
                affectedActiveRenderer = true;
            }
            if (slot.overrideMaterial.has_value() && slot.overrideMaterial->valid() && slot.overrideMaterial->index < removeMaterials.size() && removeMaterials[slot.overrideMaterial->index]) {
                affectedActiveRenderer = true;
            }
            if (slot.textureReference.valid() && slot.textureReference.index < removeTextures.size() && removeTextures[slot.textureReference.index]) {
                affectedActiveRenderer = true;
            }
        }
    }

    AssetManager compactedAssets;
    std::vector<uint32_t> textureRemap(textureCountBefore, UINT32_MAX);
    std::vector<uint32_t> materialRemap(materialCountBefore, UINT32_MAX);
    std::vector<uint32_t> meshRemap(meshCountBefore, UINT32_MAX);
    for (size_t i = 0; i < textureCountBefore; ++i) {
        if (!removeTextures[i]) {
            textureRemap[i] = compactedAssets.addTexture(assets_.textures()[i]).index;
        }
    }
    for (size_t i = 0; i < materialCountBefore; ++i) {
        if (!removeMaterials[i]) {
            MaterialAsset material = assets_.materials()[i];
            remapMaterialTextureHandles(material, textureRemap);
            materialRemap[i] = compactedAssets.addMaterial(std::move(material)).index;
        }
    }
    for (size_t i = 0; i < meshCountBefore; ++i) {
        if (!removeMeshes[i]) {
            MeshAsset mesh = assets_.meshes()[i];
            remapMeshMaterialHandles(mesh, materialRemap);
            meshRemap[i] = compactedAssets.addMesh(std::move(mesh)).index;
        }
    }

    size_t clearedMeshRenderers = 0;
    size_t remappedMeshRenderers = 0;
    size_t clearedMaterialSlots = 0;
    size_t remappedMaterialSlots = 0;
    size_t clearedTextureReferences = 0;
    size_t remappedTextureReferences = 0;
    for (Entity* entity : sceneDocument_.registry().entities()) {
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            continue;
        }
        MeshRenderer& renderer = *entity->meshRenderer;
        const MeshAssetHandle oldMesh = renderer.mesh;
        renderer.mesh = remapMeshHandle(renderer.mesh, meshRemap);
        if (oldMesh.valid() && !renderer.mesh.valid()) {
            ++clearedMeshRenderers;
        } else if (oldMesh.valid() && oldMesh.index != renderer.mesh.index) {
            ++remappedMeshRenderers;
        }
        for (MaterialSlot& slot : renderer.materialSlots) {
            const MaterialAssetHandle oldMaterial = slot.material;
            slot.material = remapMaterialHandle(slot.material, materialRemap);
            if (oldMaterial.valid() && !slot.material.valid()) {
                ++clearedMaterialSlots;
            } else if (oldMaterial.valid() && oldMaterial.index != slot.material.index) {
                ++remappedMaterialSlots;
            }
            if (slot.overrideMaterial.has_value()) {
                const MaterialAssetHandle oldOverride = *slot.overrideMaterial;
                const MaterialAssetHandle remapped = remapMaterialHandle(*slot.overrideMaterial, materialRemap);
                if (remapped.valid()) {
                    *slot.overrideMaterial = remapped;
                    if (oldOverride.index != remapped.index) {
                        ++remappedMaterialSlots;
                    }
                } else {
                    slot.overrideMaterial.reset();
                    ++clearedMaterialSlots;
                }
            }
            const TextureAssetHandle oldTexture = slot.textureReference;
            slot.textureReference = remapTextureHandle(slot.textureReference, textureRemap);
            if (oldTexture.valid() && !slot.textureReference.valid()) {
                ++clearedTextureReferences;
            } else if (oldTexture.valid() && oldTexture.index != slot.textureReference.index) {
                ++remappedTextureReferences;
            }
        }
    }

    assets_ = std::move(compactedAssets);
    if (importedScene_.has_value()) {
        remapSceneAssetHandles(*importedScene_, textureRemap, materialRemap, meshRemap);
    }
    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
    sceneUnsavedDirty_ = true;

    report["mutationExecuted"] = true;
    report["mutatedState"] = "CPU AssetManager, SceneDocument runtime handles, GpuScene, PathTracerRenderer retirement queue when affected";
    report["assetManagerAfterUnload"] = {
        {"textureCount", assets_.textures().size()},
        {"materialCount", assets_.materials().size()},
        {"meshCount", assets_.meshes().size()},
    };
    report["handleRemap"] = {
        {"clearedMeshRenderers", clearedMeshRenderers},
        {"remappedMeshRenderers", remappedMeshRenderers},
        {"clearedMaterialSlots", clearedMaterialSlots},
        {"remappedMaterialSlots", remappedMaterialSlots},
        {"clearedTextureReferences", clearedTextureReferences},
        {"remappedTextureReferences", remappedTextureReferences},
        {"importedSceneRemapped", importedScene_.has_value()},
    };

    const bool rendererOk = rebuildRendererAfterNativePackageUnload(affectedActiveRenderer, report);
    report["ok"] = rendererOk;
    if (rendererOk) {
        forgetMountedNativePackage(packagePath);
    }
    report["limitations"] = nlohmann::json::array({
        "Direct NativeAssetStore-to-GPU upload and renderer-owned native store handles remain open roadmap work.",
        "This unload path retires resources created through the existing CPU AssetManager-to-GpuScene renderer upload path."
    });

    std::string writeError;
    const bool wroteReport = writeCookJsonArtifact(reportPath, report, &writeError);
    if (!wroteReport) {
        std::cerr << "Content Browser package unload report write failed: " << writeError << '\n';
    }

    if (rendererOk) {
        notifications_.notify("Package unloaded", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        const std::string summary = "Unloaded package from Content Browser: " + packagePath.string() +
            " meshes=" + std::to_string(removedMeshCount) +
            " materials=" + std::to_string(removedMaterialCount) +
            " textures=" + std::to_string(removedTextureCount) +
            " rendererRetired=" + (affectedActiveRenderer ? "true" : "false");
        std::cout << summary << '\n';
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, summary + (wroteReport ? " report=" + reportPath.string() : std::string{}));
        }
    } else {
        notifications_.notify("Package unload renderer rebuild failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
    }
    if (wroteReport) {
        (void)openFileInShell(reportPath);
    }
    return rendererOk;
}

bool Application::refreshNativePackageFromEditor(const std::filesystem::path& packagePath) {
    std::error_code ec;
    const std::filesystem::path reportPath = editorNativePackageRefreshReportPath(project_, packagePath);
    if (packagePath.empty() || nativeAssetKindFromExtension(packagePath) != NativeAssetKind::Package || !std::filesystem::is_regular_file(packagePath, ec)) {
        nlohmann::json report = {
            {"schema", "ContentBrowserPackageRefreshV1"},
            {"inspectionSource", "ContentBrowserRefreshPackage"},
            {"ok", false},
            {"package", {{"path", packagePath.generic_string()}, {"exists", false}, {"refreshedFromUi", true}}},
            {"mutationExecuted", false},
            {"error", "Invalid or missing .rtpkg package path."},
        };
        std::string writeError;
        (void)writeCookJsonArtifact(reportPath, report, &writeError);
        notifications_.notify("Package refresh failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Package refresh failed: invalid package path " + packagePath.string());
        }
        return false;
    }

    NativeRuntimeLoadOptions loadOptions;
    loadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
    const uint64_t packageBytes = regularFileSizeOrZero(packagePath);
    if (packageBytes >= loadOptions.eagerCpuLoadWarningBytes) {
        nlohmann::json report = legacyCpuPackageMountBlockedJson(
            "ContentBrowserPackageRefreshV1",
            "ContentBrowserDiagnosticCpuRefreshPackage",
            packagePath,
            packageBytes,
            loadOptions);
        report["package"]["refreshedFromUi"] = true;
        report["project"] = project_.has_value()
            ? nlohmann::json{{"name", project_->name}, {"projectFile", project_->projectFile.generic_string()}, {"projectRoot", project_->projectRoot.generic_string()}}
            : nlohmann::json{{"name", ""}, {"projectFile", ""}, {"projectRoot", ""}};
        report["unloadAttempted"] = false;
        report["mountAttempted"] = false;
        report["policy"] = "Content Browser diagnostic CPU refresh is blocked for large .rtpkg files before unload/remount. Use package inspection, validation, and progressive streaming instead.";
        std::string writeError;
        const bool wroteReport = writeCookJsonArtifact(reportPath, report, &writeError);
        notifications_.notify("Diagnostic CPU refresh blocked", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, "Diagnostic CPU package refresh blocked for large package: " + packagePath.string() + (wroteReport ? " report=" + reportPath.string() : std::string{}));
        }
        if (wroteReport) {
            (void)openFileInShell(reportPath);
        }
        return false;
    }

    size_t mountedTextureCount = 0;
    size_t mountedMaterialCount = 0;
    size_t mountedMeshCount = 0;
    for (const TextureAsset& texture : assets_.textures()) {
        if (texture.nativeSource == "package" && nativeRuntimePathBelongsToPackage(texture.nativePath, packagePath)) {
            ++mountedTextureCount;
        }
    }
    for (const MaterialAsset& material : assets_.materials()) {
        if (material.nativeSource == "package" && nativeRuntimePathBelongsToPackage(material.nativePath, packagePath)) {
            ++mountedMaterialCount;
        }
    }
    for (const MeshAsset& mesh : assets_.meshes()) {
        if (mesh.nativeSource == "package" && nativeRuntimePathBelongsToPackage(mesh.nativePath, packagePath)) {
            ++mountedMeshCount;
        }
    }

    bool unloadOk = true;
    const bool hadMountedAssets = mountedTextureCount != 0 || mountedMaterialCount != 0 || mountedMeshCount != 0;
    if (hadMountedAssets) {
        unloadOk = unloadNativePackageFromEditor(packagePath);
    }
    const bool mountOk = unloadOk && mountNativePackageFromEditor(packagePath);

    const std::filesystem::path unloadReportPath = editorNativePackageUnloadReportPath(project_, packagePath);
    const std::filesystem::path mountReportPath = editorNativePackageMountReportPath(project_, packagePath);
    nlohmann::json report = {
        {"schema", "ContentBrowserPackageRefreshV1"},
        {"inspectionSource", "ContentBrowserRefreshPackage"},
        {"ok", mountOk},
        {"package", {{"path", packagePath.generic_string()}, {"exists", true}, {"refreshedFromUi", true}}},
        {"project", project_.has_value()
            ? nlohmann::json{{"name", project_->name}, {"projectFile", project_->projectFile.generic_string()}, {"projectRoot", project_->projectRoot.generic_string()}}
            : nlohmann::json{{"name", ""}, {"projectFile", ""}, {"projectRoot", ""}}},
        {"mountedAssetCountsBeforeRefresh", {{"textures", mountedTextureCount}, {"materials", mountedMaterialCount}, {"meshes", mountedMeshCount}}},
        {"unloadAttempted", hadMountedAssets},
        {"unloadSucceeded", unloadOk},
        {"unloadSkippedReason", hadMountedAssets ? std::string{} : std::string("No currently mounted package-backed CPU assets matched this package path.")},
        {"mountAttempted", unloadOk},
        {"mountSucceeded", mountOk},
        {"unloadReportPath", hadMountedAssets ? unloadReportPath.generic_string() : std::string{}},
        {"mountReportPath", mountReportPath.generic_string()},
        {"mutationExecuted", mountOk || hadMountedAssets},
        {"mutatedState", "CPU AssetManager through diagnostic package unload/remount; SceneDocument/GpuScene/PathTracerRenderer retirement path only when unload affected active package-backed assets"},
        {"directRendererResourceUploadFromPackageImplemented", false},
        {"policy", "Content Browser Diagnostic CPU Refresh is available for small packages as an explicit selected-action workflow and as a timestamp-polled mounted-package refresh path. Large packages are blocked before unload/remount and should use progressive native streaming instead."},
        {"limitations", nlohmann::json::array({
            "Continuous mounted-package refresh is timestamp-poll based and routes changed packages through the existing refresh request path; provider-level conflict resolution remains separate source-control work.",
            "Direct NativeAssetStore-to-GPU upload and renderer-owned native store handles remain open roadmap work."
        })},
    };
    std::string writeError;
    const bool wroteReport = writeCookJsonArtifact(reportPath, report, &writeError);
    if (!wroteReport) {
        std::cerr << "Content Browser package refresh report write failed: " << writeError << '\n';
    }

    const std::string summary = "Diagnostic CPU refreshed package from Content Browser: " + packagePath.string() +
        " previousMeshes=" + std::to_string(mountedMeshCount) +
        " previousMaterials=" + std::to_string(mountedMaterialCount) +
        " previousTextures=" + std::to_string(mountedTextureCount) +
        " unload=" + (hadMountedAssets ? (unloadOk ? "ok" : "failed") : "skipped") +
        " mount=" + (mountOk ? "ok" : "failed");
    if (mountOk) {
        notifications_.notify("Diagnostic CPU package refreshed", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        std::cout << summary << '\n';
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Command, summary + (wroteReport ? " report=" + reportPath.string() : std::string{}));
        }
        if (wroteReport) {
            (void)openFileInShell(reportPath);
        }
    } else {
        notifications_.notify("Diagnostic CPU package refresh failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
        std::cerr << summary << '\n';
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().log().add(EditorLogCategory::Warning, summary + (wroteReport ? " report=" + reportPath.string() : std::string{}));
        }
        if (wroteReport) {
            (void)openFileInShell(reportPath);
        }
    }
    return mountOk;
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

bool Application::restoreRecoveryAutosavesFromEditor() {
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
    return restored;
}

bool Application::discardRecoveryFromEditor() {
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
    return true;
}

bool Application::updateProjectSettingsFromEditor(const ProjectContext& updatedProject) {
    if (!project_.has_value()) {
        return false;
    }

    const std::filesystem::path requestedStartupScene = updatedProject.startupScene;
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
        project_->preferredWorkspacePreset != updatedProject.preferredWorkspacePreset ||
        project_->autosaveEnabled != updatedProject.autosaveEnabled ||
        project_->autosaveIntervalMinutes != std::clamp(updatedProject.autosaveIntervalMinutes, 1, 120);
    if (startupSceneChanged && startupSceneAccepted) {
        project_->startupScene = requestedStartupScene;
    }
    project_->preferredWorkspacePreset = updatedProject.preferredWorkspacePreset;
    if (uiOverlay_ != nullptr) {
        if (project_->preferredWorkspacePreset >= 0) {
            uiOverlay_->editor().setProjectWorkspacePreset(project_->preferredWorkspacePreset);
        } else {
            uiOverlay_->editor().clearProjectWorkspacePreset();
        }
    }
    project_->autosaveEnabled = updatedProject.autosaveEnabled;
    project_->autosaveIntervalMinutes = std::clamp(updatedProject.autosaveIntervalMinutes, 1, 120);
    if (changed) {
        projectSettingsDirty_ = true;
        notifications_.notify("Project settings updated", NotificationType::Info);
    }
    return changed;
}

bool Application::markSceneUpdateFromEditor(SceneUpdateKind updateKind, bool allowResourceRebuild) {
    sceneDocument_.markDirty(updateKind);
    return applyPendingSceneUpdate(allowResourceRebuild);
}

std::optional<AssetImportWorkspace> Application::prepareAssetImportWorkspace(const std::filesystem::path& sourcePath) {
    AssetImportWorkspace workspace;
    workspace.nativeTextureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
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

    const AssetLoadStats beforeAssetStats = assets_.stats();
    PrefabRuntimeBindings bindings;
    NativeRuntimeLoadOptions nativeLoadOptions;
    nativeLoadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
    nativeLoadOptions.validatePayloadHashes = false;
    nativeLoadOptions.retainLoadedPayloadsInReport = false;
    const std::vector<std::filesystem::path> runtimeAllowList = nativeRuntimeCacheAllowListForRecord(*prefabRecord, root, &assetRegistry_);
    const uint64_t runtimePayloadBytes = existingFileBytes(runtimeAllowList);
    constexpr uint64_t kMaxEagerPrefabRuntimePayloadBytes = 4ull * 1024ull * 1024ull * 1024ull;
    const bool deferRuntimePayloadLoad = runtimePayloadBytes > kMaxEagerPrefabRuntimePayloadBytes;
    if (deferRuntimePayloadLoad) {
        std::cout << "Prefab runtime payload deferred for placement: " << prefabGuid
                  << " files=" << runtimeAllowList.size()
                  << " bytes=" << runtimePayloadBytes
                  << " threshold=" << kMaxEagerPrefabRuntimePayloadBytes << '\n';
    } else {
        if (std::string bindError; !appendPrefabRuntimeAssets(*prefabRecord, prefab, root, &assetRegistry_, assets_, bindings, nativeLoadOptions, &bindError)) {
            assets_.truncateTo(beforeAssetStats);
            notifications_.notify("Prefab runtime binding failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            std::cerr << "Prefab runtime binding failed: " << bindError << '\n';
            return false;
        }
    }

    const SceneDocument beforeDocument = sceneDocument_;

    SceneOperations ops(sceneDocument_, &sceneEventBus_);
    PrefabInstance instance = ops.placePrefab(prefab, &bindings);
    if (!instance.instanceRoot.valid()) {
        sceneDocument_ = beforeDocument;
        assets_.truncateTo(beforeAssetStats);
        notifications_.notify("Prefab placement failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        return false;
    }
    if (placementTransform.has_value()) {
        if (Entity* rootEntity = sceneDocument_.registry().entity(instance.instanceRoot)) {
            rootEntity->transform = *placementTransform;
            rootEntity->defaultTransform = *placementTransform;
        }
    }
    if (deferRuntimePayloadLoad) {
        undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
            sceneDocument_,
            beforeDocument,
            sceneDocument_,
            SceneUpdateKind::TopologyChanged,
            "Place Prefab Asset"));
        if (streamingOptions_.enabled) {
            (void)queueProgressiveRuntimeLoadForPrefab(
                *prefabRecord,
                prefab,
                root,
                instance.instanceRoot,
                runtimeAllowList,
                runtimePayloadBytes);
        }
    } else {
        undoStack_.pushCommand(std::make_unique<SceneAndNativeAssetAppendReloadCommand>(
            sceneDocument_,
            assets_,
            assetRegistry_,
            *prefabRecord,
            root,
            std::optional<PrefabAsset>{prefab},
            beforeDocument,
            beforeAssetStats,
            sceneDocument_,
            nativeLoadOptions,
            SceneUpdateKind::TopologyChanged,
            "Place Prefab Asset"));
    }
    sceneUnsavedDirty_ = true;
    const bool streamingPlacementActive = deferRuntimePayloadLoad && activeProgressiveRuntimeLoadJob_.has_value();
    const bool deferRendererRebuild = streamingPlacementActive || shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
    if (streamingPlacementActive) {
        std::cerr << "[streaming] renderer rebuild DEFERRED: streaming in progress\n";
    }
    (void)applyPendingSceneUpdate(!deferRendererRebuild);
    if (deferRendererRebuild) {
        notifications_.notify("Prefab placed; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
    }
    if (deferRuntimePayloadLoad) {
        notifications_.notify("Large prefab placed with runtime payload deferred", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 7.0f);
    }
    editorPlacement_.entity = instance.instanceRoot;
    editorPlacement_.serial = nextEditorPlacementSerial_++;
    editorPlacement_.label = prefab.name.empty() ? "Prefab asset" : prefab.name;
    notifications_.notify("Prefab placed and selected", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Placed prefab asset: " << prefabGuid << " root=" << instance.instanceRoot.index << '\n';
    return true;
}

bool Application::queueProgressiveRuntimeLoadForPrefab(
    const AssetRecord& prefabRecord,
    const PrefabAsset& prefab,
    const std::filesystem::path& root,
    EntityId rootEntity,
    const std::vector<std::filesystem::path>& files,
    uint64_t totalBytes) {
    if (!streamingOptions_.enabled || files.empty()) {
        return false;
    }
    if (activeProgressiveRuntimeLoadJob_.has_value()) {
        streamingRuntimeState_.pushEvent("progressive runtime load skipped because another root is active: " + prefabRecord.guid);
        return false;
    }

    nativeAssetCatalog_.buildFromRegistry(assetRegistry_, root);
    for (const AssetGuid& guid : nativeAssetCatalog_.dependencyClosure(prefabRecord.guid)) {
        streamingRuntimeState_.catalogAsset(guid);
        streamingRuntimeState_.setAssetState(guid, StreamingAssetState::Requested);
        if (const NativeAssetCatalogEntry* entry = nativeAssetCatalog_.find(guid); entry != nullptr && entry->streamable) {
            nativeGpuAssetCache_.upsert(NativeGpuAssetDesc{
                .kind = nativeGpuAssetKindForCatalogEntry(*entry),
                .guid = guid,
                .label = entry->displayName.empty() ? guid : entry->displayName,
                .cpuBytes = entry->estimatedCpuBytes,
                .gpuBytes = entry->estimatedGpuBytes,
                .uploadBytes = entry->fileBytes,
                .fallbackDescriptorBound = true,
            });
        }
    }

    std::filesystem::path nativeLoadRoot = root;
    if (!files.empty()) {
        nativeLoadRoot = files.front().parent_path();
        for (const std::filesystem::path& file : files) {
            std::filesystem::path parent = file.parent_path();
            while (!parent.empty()) {
                const std::string parentString = parent.lexically_normal().generic_string();
                const std::string rootString = nativeLoadRoot.lexically_normal().generic_string();
                if (rootString.rfind(parentString, 0) == 0) {
                    nativeLoadRoot = parent;
                    break;
                }
                parent = parent.parent_path();
            }
        }
    }

    ActiveProgressiveRuntimeLoadJob job;
    job.serial = nextProgressiveRuntimeLoadJobSerial_++;
    job.rootGuid = prefabRecord.guid;
    job.rootEntity = rootEntity;
    job.label = prefab.name.empty() ? (prefabRecord.displayName.empty() ? std::string("Prefab") : prefabRecord.displayName) : prefab.name;
    job.record = prefabRecord;
    job.root = root;
    job.nativeLoadRoot = nativeLoadRoot;
    job.files = files;
    job.totalBytes = totalBytes;
    activeProgressiveRuntimeLoadJob_ = std::move(job);

    streamingRuntimeState_.setActiveRoot(StreamingRootSnapshot{
        .serial = activeProgressiveRuntimeLoadJob_->serial,
        .rootGuid = activeProgressiveRuntimeLoadJob_->rootGuid,
        .label = activeProgressiveRuntimeLoadJob_->label,
        .rootPath = activeProgressiveRuntimeLoadJob_->nativeLoadRoot,
        .totalFiles = static_cast<uint32_t>(activeProgressiveRuntimeLoadJob_->files.size()),
        .queuedBytes = activeProgressiveRuntimeLoadJob_->totalBytes,
        .active = true,
        .status = "queued",
    });
    streamingRuntimeState_.pushEvent("queued progressive runtime load for prefab " + prefabRecord.guid);
    notifications_.notify("Progressive runtime streaming queued", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
    startNextProgressiveRuntimeLoadBatch();
    return true;
}

void Application::startNextProgressiveRuntimeLoadBatch() {
    if (!activeProgressiveRuntimeLoadJob_.has_value()) {
        return;
    }
    ActiveProgressiveRuntimeLoadJob& job = *activeProgressiveRuntimeLoadJob_;
    if (job.cancelled || job.batchInFlight || job.nextFile >= job.files.size()) {
        return;
    }

    const uint64_t batchLimit = std::max<uint64_t>(1ull * 1024ull * 1024ull, streamingOptions_.cpuBatchBytes);
    uint64_t batchBytes = 0;
    std::vector<std::filesystem::path> batchFiles;
    const uint32_t firstFile = job.nextFile;
    while (job.nextFile < job.files.size()) {
        const std::filesystem::path& path = job.files[job.nextFile];
        uint64_t fileBytes = 0;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            const uintmax_t size = std::filesystem::file_size(path, ec);
            if (!ec) {
                fileBytes = static_cast<uint64_t>(size);
            }
        }
        if (!batchFiles.empty() && batchBytes + fileBytes > batchLimit) {
            break;
        }
        batchBytes += fileBytes;
        batchFiles.push_back(path);
        ++job.nextFile;
        if (batchBytes >= batchLimit) {
            break;
        }
    }
    if (batchFiles.empty()) {
        return;
    }

    std::vector<std::tuple<std::filesystem::path, AssetGuid, AssetType>> registryCachePaths;
    registryCachePaths.reserve(assetRegistry_.records().size());
    for (const AssetRecord& record : assetRegistry_.records()) {
        if (record.cachePath.empty() || record.guid.empty()) {
            continue;
        }
        std::filesystem::path path = record.cachePath;
        if (!path.is_absolute()) {
            path = job.root / path;
        }
        registryCachePaths.emplace_back(path.lexically_normal(), record.guid, record.type);
    }

    const uint64_t serial = job.serial;
    const std::filesystem::path nativeLoadRoot = job.nativeLoadRoot;
    const StreamingRuntimeOptions streamingOptions = streamingOptions_;
    NativeRuntimeLoadOptions loadOptions;
    loadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
    loadOptions.validatePayloadHashes = false;
    loadOptions.retainLoadedPayloadsInReport = false;
    loadOptions.looseFileAllowList = batchFiles;

    job.batchInFlight = true;
    for (const std::filesystem::path& file : batchFiles) {
        streamingRuntimeState_.pushEvent("queued native streaming file " + file.filename().string());
    }
    streamingRuntimeState_.setActiveRoot(StreamingRootSnapshot{
        .serial = job.serial,
        .rootGuid = job.rootGuid,
        .label = job.label,
        .rootPath = job.nativeLoadRoot,
        .totalFiles = static_cast<uint32_t>(job.files.size()),
        .loadedFiles = job.loadedFiles,
        .failedFiles = job.failedFiles,
        .queuedBytes = job.totalBytes,
        .loadedBytes = job.loadedBytes,
        .appliedBytes = job.appliedBytes,
        .reboundRenderers = job.reboundRenderers,
        .active = true,
        .status = "loading batch",
    });

    job.future = std::async(std::launch::async, [serial, firstFile, batchBytes, batchFiles = std::move(batchFiles), registryCachePaths = std::move(registryCachePaths), nativeLoadRoot, loadOptions, streamingOptions]() mutable {
        ProgressiveRuntimeLoadBatchResult result;
        result.serial = serial;
        result.firstFile = firstFile;
        result.fileCount = static_cast<uint32_t>(batchFiles.size());
        result.bytes = batchBytes;
        result.files = batchFiles;

        try {
        std::unique_ptr<StreamingIoBackend> ioBackend = makeStreamingIoBackend(streamingOptions);
        std::vector<StreamingIoReadRequest> ioRequests;
        ioRequests.reserve(batchFiles.size());
        for (const std::filesystem::path& file : batchFiles) {
            uint64_t fileBytes = 0;
            std::error_code ec;
            if (std::filesystem::is_regular_file(file, ec)) {
                const uintmax_t size = std::filesystem::file_size(file, ec);
                if (!ec) {
                    fileBytes = static_cast<uint64_t>(size);
                }
            }
            StreamingIoReadRequest request;
            request.path = file;
            request.size = fileBytes;
            request.label = "progressive runtime streaming read " + file.filename().string();
            ioRequests.push_back(std::move(request));
        }
        bool ioOk = true;
        const std::vector<StreamingIoReadResult> ioReads = ioBackend->readBatch(ioRequests);
        for (const StreamingIoReadResult& read : ioReads) {
            if (!read.ok) {
                ioOk = false;
                result.ioErrors.push_back(read.path.string() + ": " + read.error);
            }
        }
        result.ioMetrics = ioBackend->metrics();
        if (!ioOk) {
            result.ok = false;
            return result;
        }

        NativeAssetRuntimeLoader loader;
        NativeRuntimeLoadReport report = loader.loadLooseRoot(nativeLoadRoot, &result.assets, loadOptions);
        result.ok = report.ok;
        if (result.ok) {
            result.loadedFiles = result.fileCount;
        }
        for (const NativeBinaryError& error : report.errors) {
            result.errors.push_back(error.message);
        }

        auto registryGuidForNativeAsset = [&](const NativeRuntimeLoadedAsset& asset) -> AssetGuid {
            if (asset.path.empty()) {
                return asset.guid;
            }
            const std::filesystem::path assetPath = asset.path.lexically_normal();
            for (const auto& [cachePath, guid, type] : registryCachePaths) {
                const bool typeMatches =
                    (asset.kind == NativeAssetKind::Mesh && type == AssetType::Mesh) ||
                    (asset.kind == NativeAssetKind::Texture && type == AssetType::Texture) ||
                    (asset.kind == NativeAssetKind::Material && type == AssetType::Material);
                if (typeMatches && cachePath == assetPath) {
                    return guid;
                }
            }
            return asset.guid;
        };

        for (const NativeRuntimeLoadedAsset& asset : report.assets) {
            if (!asset.ok) {
                continue;
            }
            const AssetGuid bindingGuid = registryGuidForNativeAsset(asset);
            if (bindingGuid.empty()) {
                continue;
            }
            if (asset.kind == NativeAssetKind::Mesh && asset.meshHandle.valid()) {
                result.bindings.meshes[bindingGuid] = asset.meshHandle;
            } else if (asset.kind == NativeAssetKind::Texture && asset.textureHandle.valid()) {
                result.textures[bindingGuid] = asset.textureHandle;
            } else if (asset.kind == NativeAssetKind::Material && asset.materialHandle.valid()) {
                result.bindings.materials[bindingGuid] = asset.materialHandle;
                const AssetGuid materialBindingKey = asset.guid.empty() ? bindingGuid : asset.guid;
                auto& bindings = result.materialTextureBindings[materialBindingKey];
                bindings.reserve(asset.materialTextureBindings.size());
                for (const NativeRuntimeTextureBinding& binding : asset.materialTextureBindings) {
                    if (!binding.textureGuid.empty()) {
                        bindings.push_back(Application::StreamedMaterialTextureBinding{
                            .slot = binding.slot,
                            .textureGuid = binding.textureGuid,
                        });
                    }
                }
            }
        }
        return result;
        } catch (const std::exception& e) {
            result.ok = false;
            result.errors.push_back(std::string("exception: ") + e.what());
            std::cerr << "[streaming batch " << firstFile << "] CRASH in async worker: " << e.what() << '\n';
            return result;
        } catch (...) {
            result.ok = false;
            result.errors.push_back("unknown exception");
            std::cerr << "[streaming batch " << firstFile << "] CRASH in async worker: unknown exception\n";
            return result;
        }
    });
}

void Application::applyProgressiveRuntimeLoadBatch(ProgressiveRuntimeLoadBatchResult&& result) {
    if (!activeProgressiveRuntimeLoadJob_.has_value() || activeProgressiveRuntimeLoadJob_->serial != result.serial) {
        return;
    }
    ActiveProgressiveRuntimeLoadJob& job = *activeProgressiveRuntimeLoadJob_;
    job.batchInFlight = false;
    streamingIoMetrics_ = result.ioMetrics;
    for (const std::string& error : result.ioErrors) {
        streamingRuntimeState_.pushEvent("streaming I/O failed: " + error);
    }

    std::cerr << "[streaming] batch APPLY START loadedFiles=" << result.fileCount
              << " bytes=" << result.bytes
              << " ok=" << (result.ok ? "yes" : "no")
              << " errors=" << result.errors.size()
              << " ioErrors=" << result.ioErrors.size() << '\n';
    if (!result.ok) {
        job.failed = true;
        job.failedFiles += result.fileCount;
        job.loadedFiles += result.loadedFiles;
        job.loadedBytes += result.loadedFiles != 0 ? result.bytes : 0ull;
        for (const std::string& error : result.errors) {
            streamingRuntimeState_.pushEvent("streaming batch failed: " + error);
        }
        for (const std::string& error : result.ioErrors) {
            streamingRuntimeState_.pushEvent("streaming batch failed: " + error);
        }
        startNextProgressiveRuntimeLoadBatch();
        return;
    }

    ImportedAssetHandleRemap remap = appendImportedAssets(assets_, result.assets);
    for (auto& [guid, bindings] : result.materialTextureBindings) {
        if (!guid.empty() && !bindings.empty()) {
            job.materialTextureBindings[guid] = std::move(bindings);
        }
    }

    auto repairStreamedMaterialTextureBindings = [&]() {
        std::unordered_map<AssetGuid, TextureAssetHandle> textureByGuid;
        const auto& textures = assets_.textures();
        textureByGuid.reserve(textures.size());
        for (uint32_t i = 0; i < textures.size(); ++i) {
            if (!textures[i].nativeGuid.empty()) {
                textureByGuid[textures[i].nativeGuid] = TextureAssetHandle{i};
            }
        }

        uint32_t repairedSlots = 0;
        const auto& materials = assets_.materials();
        for (uint32_t i = 0; i < materials.size(); ++i) {
            const MaterialAsset& materialView = materials[i];
            if (materialView.nativeGuid.empty()) {
                continue;
            }
            const auto bindingIt = job.materialTextureBindings.find(materialView.nativeGuid);
            if (bindingIt == job.materialTextureBindings.end()) {
                continue;
            }
            MaterialAsset* material = assets_.material(MaterialAssetHandle{i});
            if (material == nullptr) {
                continue;
            }
            for (const StreamedMaterialTextureBinding& binding : bindingIt->second) {
                const auto textureIt = textureByGuid.find(binding.textureGuid);
                if (textureIt == textureByGuid.end() || !textureIt->second.valid()) {
                    continue;
                }
                assignStreamingMaterialTextureSlot(*material, binding.slot, textureIt->second);
                assets_.markTextureResident(textureIt->second, true);
                ++repairedSlots;
            }
        }
        return repairedSlots;
    };
    const uint32_t repairedTextureSlots = repairStreamedMaterialTextureBindings();
    if (repairedTextureSlots > 0) {
        streamingRuntimeState_.pushEvent("repaired streamed material texture bindings: " + std::to_string(repairedTextureSlots));
        std::cerr << "[streaming] repaired streamed material texture bindings: " << repairedTextureSlots << '\n';
        sceneDocument_.markDirty(SceneUpdateKind::MaterialOnly);
    }

    PrefabRuntimeBindings liveBindings;
    const bool deferLiveStreamingGpuWork = job.state == ActiveProgressiveRuntimeLoadJob::State::Loading ||
        job.state == ActiveProgressiveRuntimeLoadJob::State::Completing;
    for (const auto& [guid, handle] : result.textures) {
        if (handle.valid() && handle.index < remap.textures.size()) {
            const TextureAssetHandle liveHandle = remap.textures[handle.index];
            const TextureAsset* texture = assets_.texture(liveHandle);
            if (deferLiveStreamingGpuWork) {
                streamingRuntimeState_.setAssetState(guid, StreamingAssetState::CpuReadyTransient);
                continue;
            }
            const uint32_t mipCount = texture != nullptr
                ? static_cast<uint32_t>(std::max(1, texture->mipLevels))
                : 1u;
            const uint64_t uploadBytes = texture != nullptr ? estimatedTextureUploadBytes(*texture) : 0ull;
            nativeGpuAssetCache_.upsert(NativeGpuAssetDesc{
                .kind = NativeGpuAssetKind::Texture,
                .guid = guid,
                .label = texture != nullptr && !texture->name.empty() ? texture->name : guid,
                .cpuBytes = uploadBytes,
                .gpuBytes = uploadBytes,
                .uploadBytes = uploadBytes,
                .mipCount = mipCount,
                .residentMipCount = 0,
                .fallbackDescriptorBound = true,
            });
            bool textureImageBacked = false;
            if (texture != nullptr && uploadBytes > 0 && allocator_ != nullptr) {
                const VkFormat imageFormat = texture->isCompressed && texture->compressedFormat != VK_FORMAT_UNDEFINED
                    ? texture->compressedFormat
                    : texture->format;
                const std::string debugName = "streaming texture payload " + guid;
                try {
                    textureImageBacked = nativeGpuAssetCache_.ensureImageResource(
                        *allocator_,
                        guid,
                        ImageDesc{
                            .width = std::max(texture->width, 1u),
                            .height = std::max(texture->height, 1u),
                            .depth = 1,
                            .mipLevels = mipCount,
                            .format = imageFormat,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .createDefaultView = true,
                            .debugName = debugName.c_str(),
                        },
                        uploadBytes);
                } catch (const std::exception& e) {
                    streamingRuntimeState_.pushEvent("streaming texture GPU image allocation failed for " + guid + ": " + e.what());
                    textureImageBacked = false;
                }
            }
            streamingRuntimeState_.setAssetState(guid, StreamingAssetState::UploadQueued);
            uint64_t firstUploadTicket = 0;
            auto mipUploadBytes = [&](uint32_t mipLevel) -> uint64_t {
                if (texture != nullptr && mipLevel < texture->mipData.size() && texture->mipData[mipLevel].size != 0) {
                    return texture->mipData[mipLevel].size;
                }
                if (mipLevel == 0 && texture != nullptr && !texture->rgba8.empty()) {
                    return static_cast<uint64_t>(texture->rgba8.size());
                }
                return std::max<uint64_t>(1ull, uploadBytes / std::max<uint32_t>(1u, mipCount));
            };
            for (uint32_t queuedMip = 0; queuedMip < mipCount; ++queuedMip) {
                const uint32_t mipLevel = mipCount - 1u - queuedMip;
                const uint64_t mipBytes = mipUploadBytes(mipLevel);
                std::vector<uint8_t> mipPayload;
                uint32_t mipWidth = texture != nullptr ? std::max(1u, texture->width >> mipLevel) : 1u;
                uint32_t mipHeight = texture != nullptr ? std::max(1u, texture->height >> mipLevel) : 1u;
                if (textureImageBacked && texture != nullptr && mipBytes > 0) {
                    mipPayload = textureMipUploadPayload(*texture, mipLevel);
                    if (mipLevel < texture->mipData.size()) {
                        mipWidth = std::max(texture->mipData[mipLevel].width, 1u);
                        mipHeight = std::max(texture->mipData[mipLevel].height, 1u);
                    }
                }
                const bool mipPayloadBacked = !mipPayload.empty();
                const uint64_t uploadTicket = streamingGpuWorkQueue_.enqueue(StreamingGpuWorkDesc{
                    .kind = StreamingGpuWorkKind::ImageMipUpload,
                    .label = "stream texture mip " + std::to_string(mipLevel) + "/" + std::to_string(mipCount) + " " + guid,
                    .ownerGuid = guid,
                    .bytes = mipBytes,
                    .estimatedGpuMs = std::clamp(static_cast<double>(mipBytes) / (64.0 * 1024.0 * 1024.0), 0.03, 0.75),
                    .textureMipLevel = mipLevel,
                    .textureMipCount = mipCount,
                    .payloadBacked = mipPayloadBacked,
                });
                if (mipPayloadBacked) {
                    streamingGpuImageMipUploadPayloads_[uploadTicket] = StreamingGpuImageMipUploadPayload{
                        .ownerGuid = guid,
                        .mipLevel = mipLevel,
                        .width = mipWidth,
                        .height = mipHeight,
                        .bytes = std::move(mipPayload),
                    };
                }
                if (firstUploadTicket == 0) {
                    firstUploadTicket = uploadTicket;
                }
            }
            (void)nativeGpuAssetCache_.markUploading(guid, firstUploadTicket);
        }
    }
    for (const auto& [guid, handle] : result.bindings.meshes) {
        if (handle.valid() && handle.index < remap.meshes.size()) {
            liveBindings.meshes[guid] = remap.meshes[handle.index];
            if (deferLiveStreamingGpuWork) {
                streamingRuntimeState_.setAssetState(guid, StreamingAssetState::CpuReadyTransient);
                continue;
            }
            streamingRuntimeState_.setAssetState(guid, StreamingAssetState::UploadQueued);
            const MeshAssetHandle liveHandle = remap.meshes[handle.index];
            const MeshAsset* mesh = assets_.mesh(liveHandle);
            const uint64_t uploadBytes = mesh != nullptr ? estimatedMeshUploadBytes(*mesh) : 0ull;
            nativeGpuAssetCache_.upsert(NativeGpuAssetDesc{
                .kind = NativeGpuAssetKind::Mesh,
                .guid = guid,
                .label = mesh != nullptr && !mesh->name.empty() ? mesh->name : guid,
                .cpuBytes = uploadBytes,
                .gpuBytes = uploadBytes,
                .uploadBytes = uploadBytes,
                .fallbackDescriptorBound = true,
            });
            bool meshPayloadBacked = false;
            bool meshBlasBacked = false;
            uint64_t meshBlasIndexOffset = 0;
            if (mesh != nullptr && uploadBytes > 0 && allocator_ != nullptr) {
                const VkBufferUsageFlags usage =
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
                const std::string debugName = "streaming mesh payload " + guid;
                try {
                    meshPayloadBacked = nativeGpuAssetCache_.ensureBufferResource(
                        *allocator_,
                        guid,
                        uploadBytes,
                        usage,
                        debugName.c_str());
                } catch (const std::exception& e) {
                    streamingRuntimeState_.pushEvent("streaming mesh GPU buffer allocation failed for " + guid + ": " + e.what());
                    meshPayloadBacked = false;
                }
                if (meshPayloadBacked && context_ != nullptr) {
                    const std::optional<MeshBlasBuildSizing> blasSizing = queryMeshBlasBuildSizing(context_->device(), *mesh);
                    if (blasSizing.has_value()) {
                        const std::string blasDebugName = "streaming mesh BLAS " + guid;
                        try {
                            meshBlasBacked = nativeGpuAssetCache_.ensureBlasResource(
                                context_->device(),
                                *allocator_,
                                guid,
                                blasSizing->accelerationStructureBytes,
                                blasSizing->scratchBytes,
                                context_->rayTracingInfo().accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment,
                                blasDebugName.c_str());
                            meshBlasIndexOffset = static_cast<uint64_t>(mesh->vertices.size()) * static_cast<uint64_t>(sizeof(MeshVertex));
                        } catch (const std::exception& e) {
                            streamingRuntimeState_.pushEvent("streaming mesh BLAS allocation failed for " + guid + ": " + e.what());
                            meshBlasBacked = false;
                        }
                    }
                }
            }
            const uint64_t uploadChunkBytes = std::max<uint64_t>(1ull * 1024ull * 1024ull, streamingOptions_.uploadBytesPerFrame);
            uint64_t uploaded = 0;
            uint64_t firstUploadTicket = 0;
            do {
                const uint64_t chunkBytes = uploadBytes == 0
                    ? 0
                    : std::min(uploadChunkBytes, uploadBytes - uploaded);
                const uint64_t uploadTicket = streamingGpuWorkQueue_.enqueue(StreamingGpuWorkDesc{
                    .kind = StreamingGpuWorkKind::BufferUpload,
                    .label = "stream mesh buffers " + guid,
                    .ownerGuid = guid,
                    .bytes = chunkBytes,
                    .estimatedGpuMs = 0.10,
                    .payloadBacked = meshPayloadBacked && chunkBytes > 0,
                });
                if (meshPayloadBacked && chunkBytes > 0 && mesh != nullptr) {
                    streamingGpuBufferUploadPayloads_[uploadTicket] = StreamingGpuBufferUploadPayload{
                        .ownerGuid = guid,
                        .destinationOffset = uploaded,
                        .bytes = meshUploadPayloadChunk(*mesh, uploaded, chunkBytes),
                    };
                }
                if (firstUploadTicket == 0) {
                    firstUploadTicket = uploadTicket;
                }
                uploaded += chunkBytes;
            } while (uploaded < uploadBytes);
            (void)nativeGpuAssetCache_.markUploading(guid, firstUploadTicket);
            const uint64_t blasTicket = streamingGpuWorkQueue_.enqueue(StreamingGpuWorkDesc{
                .kind = StreamingGpuWorkKind::BlasBuild,
                .label = "build streamed mesh BLAS " + guid,
                .ownerGuid = guid,
                .estimatedGpuMs = mesh != nullptr ? estimatedBlasBuildMs(*mesh) : 0.25,
                .blasBuilds = 1,
                .payloadBacked = meshBlasBacked,
            });
            if (meshBlasBacked && mesh != nullptr) {
                streamingGpuBlasBuildPayloads_[blasTicket] = StreamingGpuBlasBuildPayload{
                    .ownerGuid = guid,
                    .indexDataOffset = meshBlasIndexOffset,
                    .vertexCount = static_cast<uint32_t>(mesh->vertices.size()),
                    .indexCount = static_cast<uint32_t>(mesh->indices.size()),
                    .vertexStride = sizeof(MeshVertex),
                };
            }
            (void)nativeGpuAssetCache_.markBlasBuildQueued(guid, blasTicket);
            const uint64_t tlasTicket = streamingGpuWorkQueue_.enqueue(StreamingGpuWorkDesc{
                .kind = StreamingGpuWorkKind::TlasPatch,
                .label = "patch streamed mesh TLAS " + guid,
                .ownerGuid = guid,
                .estimatedGpuMs = 0.08,
                .tlasPatches = 1,
            });
            (void)nativeGpuAssetCache_.markTlasPatchQueued(guid, tlasTicket);
        }
    }
    for (const auto& [guid, handle] : result.bindings.materials) {
        if (handle.valid() && handle.index < remap.materials.size()) {
            liveBindings.materials[guid] = remap.materials[handle.index];
            if (deferLiveStreamingGpuWork) {
                streamingRuntimeState_.setAssetState(guid, StreamingAssetState::CpuReadyTransient);
                continue;
            }
            streamingRuntimeState_.setAssetState(guid, StreamingAssetState::UploadQueued);
            const MaterialAssetHandle liveHandle = remap.materials[handle.index];
            const MaterialAsset* material = assets_.material(liveHandle);
            const uint32_t dependencyCount = material != nullptr ? materialTextureDependencyCount(*material) : 0u;
            nativeGpuAssetCache_.upsert(NativeGpuAssetDesc{
                .kind = NativeGpuAssetKind::Material,
                .guid = guid,
                .label = material != nullptr && !material->name.empty() ? material->name : guid,
                .descriptorDependencyCount = dependencyCount,
                .descriptorResidentDependencyCount = 0,
                .fallbackDescriptorBound = true,
                .descriptorPatchPending = true,
                .restirLightCandidate = material != nullptr && materialContributesRestirLightCandidate(*material),
            });
            const uint64_t descriptorTicket = streamingGpuWorkQueue_.enqueue(StreamingGpuWorkDesc{
                .kind = StreamingGpuWorkKind::DescriptorUpdate,
                .label = "patch streamed material descriptors " + guid,
                .ownerGuid = guid,
                .estimatedGpuMs = 0.03,
                .descriptorUpdates = std::max<uint32_t>(1u, dependencyCount),
            });
            (void)nativeGpuAssetCache_.markUploading(guid, descriptorTicket);
            (void)nativeGpuAssetCache_.markDescriptorPatchQueued(guid, descriptorTicket);
        }
    }

    const uint32_t rebound = rebindGuidBackedRenderers(sceneDocument_, liveBindings);
    job.reboundRenderers += rebound;
    job.loadedFiles += result.fileCount;
    job.loadedBytes += result.bytes;
    job.appliedBytes += result.bytes;

    if (rebound > 0) {
        sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
        (void)applyPendingSceneUpdate(false);
    }

    const bool complete = job.nextFile >= job.files.size();
    streamingRuntimeState_.setActiveRoot(StreamingRootSnapshot{
        .serial = job.serial,
        .rootGuid = job.rootGuid,
        .label = job.label,
        .rootPath = job.nativeLoadRoot,
        .totalFiles = static_cast<uint32_t>(job.files.size()),
        .loadedFiles = job.loadedFiles,
        .failedFiles = job.failedFiles,
        .queuedBytes = job.totalBytes,
        .loadedBytes = job.loadedBytes,
        .appliedBytes = job.appliedBytes,
        .reboundRenderers = job.reboundRenderers,
        .active = !complete,
        .complete = complete,
        .failed = job.failed,
        .status = complete ? "complete" : "applied batch",
    });

    if (complete) {
        job.state = ActiveProgressiveRuntimeLoadJob::State::Completing;
        streamingRuntimeState_.pushEvent("completed progressive runtime load for prefab " + job.rootGuid);
        std::cerr << "[streaming] progressive load COMPLETE: loadedFiles=" << job.loadedFiles
                  << "/" << job.files.size()
                  << " failed=" << job.failedFiles
                  << " bytes=" << job.loadedBytes << '\n';
        std::cerr << "[streaming] triggering final deferred rebuild\n";
        job.state = ActiveProgressiveRuntimeLoadJob::State::FinalRebuild;
        sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
        const bool rebuilt = applyPendingSceneUpdate(true);
        if (!rebuilt) {
            streamingRuntimeState_.pushEvent("progressive runtime final renderer rebuild was not applied");
            std::cerr << "[streaming] final deferred rebuild did not run\n";
        }
        streamingGpuSceneUpdateQueue_ = IncrementalGpuSceneUpdateQueue();
        lastStreamingGpuSceneSnapshots_.clear();
        streamingGpuWorkQueue_ = StreamingGpuWorkQueue();
        streamingGpuWorkTimelineMarkers_.clear();
        streamingGpuBufferUploadPayloads_.clear();
        streamingGpuImageMipUploadPayloads_.clear();
        streamingGpuBlasBuildPayloads_.clear();
        streamingGpuBlasCompactionPayloads_.clear();
        streamingGpuWorkCompletedTimeline_ = 0;
        job.state = ActiveProgressiveRuntimeLoadJob::State::Done;
        notifications_.notify("Progressive runtime streaming complete", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        activeProgressiveRuntimeLoadJob_.reset();
        return;
    }
    startNextProgressiveRuntimeLoadBatch();
}

void Application::pollProgressiveRuntimeLoadJob() {
    if (!activeProgressiveRuntimeLoadJob_.has_value()) {
        return;
    }
    ActiveProgressiveRuntimeLoadJob& job = *activeProgressiveRuntimeLoadJob_;
    if (job.rootEntity.valid() && sceneDocument_.registry().entity(job.rootEntity) == nullptr) {
        job.cancelled = true;
    }
    if (job.cancelled) {
        if (job.future.valid()) {
            try {
                (void)job.future.get();
            } catch (const std::exception& e) {
                streamingRuntimeState_.pushEvent(std::string("streaming batch cancel consumed exception: ") + e.what());
            } catch (...) {
                streamingRuntimeState_.pushEvent("streaming batch cancel consumed unknown exception");
            }
        }
        streamingRuntimeState_.setActiveRoot(StreamingRootSnapshot{
            .serial = job.serial,
            .rootGuid = job.rootGuid,
            .label = job.label,
            .rootPath = job.nativeLoadRoot,
            .totalFiles = static_cast<uint32_t>(job.files.size()),
            .loadedFiles = job.loadedFiles,
            .failedFiles = job.failedFiles,
            .queuedBytes = job.totalBytes,
            .loadedBytes = job.loadedBytes,
            .appliedBytes = job.appliedBytes,
            .reboundRenderers = job.reboundRenderers,
            .cancelled = true,
            .status = "cancelled",
        });
        activeProgressiveRuntimeLoadJob_.reset();
        return;
    }
    if (!job.batchInFlight || !job.future.valid()) {
        startNextProgressiveRuntimeLoadBatch();
        return;
    }
    if (job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        applyProgressiveRuntimeLoadBatch(job.future.get());
    }
}

bool Application::placeMeshAsset(const EditorMeshAssetPlacement& request, bool deferSceneUpdate) {
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

    const AssetLoadStats beforeAssetStats = assets_.stats();
    bool restoredRuntimeAssets = false;
    std::optional<uint32_t> meshIndex = loadedMeshIndexForRecord(*meshRecord);
    std::filesystem::path root = project_.has_value() ? project_->projectRoot : std::filesystem::current_path();
    if (!project_.has_value() && assetRegistry_.state().path.has_parent_path()) {
        root = assetRegistry_.state().path.parent_path();
    }
    NativeRuntimeLoadOptions nativeLoadOptions;
    nativeLoadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
    nativeLoadOptions.validatePayloadHashes = false;
    nativeLoadOptions.retainLoadedPayloadsInReport = false;
    if (!meshIndex.has_value()) {
        PrefabRuntimeBindings bindings;
        std::string bindError;
        if (!appendCachedPrefabRuntimeAssets(
                *meshRecord,
                root,
                resolveAssetSourcePath(*meshRecord, root),
                resolveAssetCachePath(*meshRecord, root),
                &assetRegistry_,
                assets_,
                bindings,
                nativeLoadOptions,
                &bindError)) {
            assets_.truncateTo(beforeAssetStats);
            notifications_.notify("Mesh cooked payload is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
            if (!bindError.empty()) {
                std::cerr << "Mesh runtime binding failed: " << bindError << '\n';
            }
            return false;
        }
        const auto restoredMesh = bindings.meshes.find(request.meshGuid);
        if (restoredMesh == bindings.meshes.end() || !restoredMesh->second.valid()) {
            assets_.truncateTo(beforeAssetStats);
            notifications_.notify("Mesh cooked payload does not contain this asset", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            std::cerr << "Mesh runtime binding did not expose GUID: " << request.meshGuid << '\n';
            return false;
        }
        meshIndex = restoredMesh->second.index;
        restoredRuntimeAssets = true;
    }
    const MeshAssetHandle meshHandle{*meshIndex};
    if (assets_.mesh(meshHandle) == nullptr) {
        notifications_.notify("Mesh runtime data is unavailable", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
        if (restoredRuntimeAssets) {
            assets_.truncateTo(beforeAssetStats);
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
    if (request.attachEntity.valid()) {
        Entity* target = sceneDocument_.registry().entity(request.attachEntity);
        if (target == nullptr) {
            if (restoredRuntimeAssets) {
                assets_.truncateTo(beforeAssetStats);
            }
            notifications_.notify("Mesh hierarchy target is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
            return false;
        }
        target->meshRenderer = makeRenderer();
        if (request.placementTransform.has_value()) {
            target->transform = *request.placementTransform;
        }
        target->defaultTransform = target->transform;
        sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
        sceneUnsavedDirty_ = true;
        if (restoredRuntimeAssets) {
            undoStack_.pushCommand(std::make_unique<SceneAndNativeAssetAppendReloadCommand>(
                sceneDocument_,
                assets_,
                assetRegistry_,
                *meshRecord,
                root,
                std::nullopt,
                beforeDocument,
                beforeAssetStats,
                sceneDocument_,
                nativeLoadOptions,
                SceneUpdateKind::TopologyChanged,
                "Place Mesh Asset In Hierarchy"));
        } else {
            undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
                sceneDocument_,
                beforeDocument,
                sceneDocument_,
                SceneUpdateKind::TopologyChanged,
                "Place Mesh Asset In Hierarchy"));
        }
        const bool deferRendererRebuild = deferSceneUpdate || shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
        if (!deferSceneUpdate) {
            (void)applyPendingSceneUpdate(!deferRendererRebuild);
            if (deferRendererRebuild) {
                notifications_.notify("Mesh placed; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
            }
        }
        editorPlacement_.entity = request.attachEntity;
        editorPlacement_.serial = nextEditorPlacementSerial_++;
        editorPlacement_.label = target->name.empty() ? (meshRecord->displayName.empty() ? "Mesh Asset" : meshRecord->displayName) : target->name;
        notifications_.notify("Mesh asset placed in hierarchy", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
        return true;
    }

    if (request.replaceEntity.valid()) {
        Entity* target = sceneDocument_.registry().entity(request.replaceEntity);
        if (target == nullptr || !target->meshRenderer.has_value()) {
            if (restoredRuntimeAssets) {
                assets_.truncateTo(beforeAssetStats);
            }
            notifications_.notify("Mesh replacement target is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 5.0f);
            return false;
        }
        target->meshRenderer = makeRenderer();
        sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
        sceneUnsavedDirty_ = true;
        if (restoredRuntimeAssets) {
            undoStack_.pushCommand(std::make_unique<SceneAndNativeAssetAppendReloadCommand>(
                sceneDocument_,
                assets_,
                assetRegistry_,
                *meshRecord,
                root,
                std::nullopt,
                beforeDocument,
                beforeAssetStats,
                sceneDocument_,
                nativeLoadOptions,
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
        const bool deferRendererRebuild = deferSceneUpdate || shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
        if (!deferSceneUpdate) {
            (void)applyPendingSceneUpdate(!deferRendererRebuild);
            if (deferRendererRebuild) {
                notifications_.notify("Mesh replaced; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
            }
        }
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
            assets_.truncateTo(beforeAssetStats);
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
        undoStack_.pushCommand(std::make_unique<SceneAndNativeAssetAppendReloadCommand>(
            sceneDocument_,
            assets_,
            assetRegistry_,
            *meshRecord,
            root,
            std::nullopt,
            beforeDocument,
            beforeAssetStats,
            sceneDocument_,
            nativeLoadOptions,
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
    const bool deferRendererRebuild = deferSceneUpdate || shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
    if (!deferSceneUpdate) {
        (void)applyPendingSceneUpdate(!deferRendererRebuild);
        if (deferRendererRebuild) {
            notifications_.notify("Mesh placed; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
    }
    editorPlacement_.entity = created;
    editorPlacement_.serial = nextEditorPlacementSerial_++;
    editorPlacement_.label = entityName;
    notifications_.notify("Mesh asset placed and selected", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    return true;
}

std::optional<Application::UsdRuntimeMeshHierarchyPlacementResult> Application::placeUsdRuntimeMeshHierarchy(
    const AssetRecord& sceneRecord,
    const std::filesystem::path& root,
    const std::vector<EditorMeshAssetPlacement>& meshPlacements) {
    if (sceneRecord.type != AssetType::Scene) {
        return std::nullopt;
    }
    const std::optional<nlohmann::json> runtimePayload = runtimePayloadForImportedRecord(sceneRecord, root);
    if (!runtimePayload.has_value() || runtimePayload->value("kind", std::string{}) != "UsdStageMetadataPayload") {
        return std::nullopt;
    }
    const std::unordered_map<std::string, nlohmann::json> primByPath = usdPrimMetadataByPath(*runtimePayload);
    if (meshPlacements.empty() || primByPath.empty()) {
        return UsdRuntimeMeshHierarchyPlacementResult{};
    }

    std::unordered_map<std::string, EntityId> entityByPrimPath;
    SceneOperations ops(sceneDocument_, &sceneEventBus_);
    ops.setUndoStack(nullptr);
    const SceneDocument beforeHierarchyDocument = sceneDocument_;
    UsdRuntimeMeshHierarchyPlacementResult result;
    SceneUpdateMask hierarchyUpdateMask = SceneUpdateMaskNone;

    std::function<EntityId(const std::string&)> ensureUsdPrimEntity = [&](const std::string& path) -> EntityId {
        if (path.empty() || path == "/") {
            return {};
        }
        if (const auto existing = entityByPrimPath.find(path); existing != entityByPrimPath.end()) {
            return existing->second;
        }
        const std::string tag = usdPrimPathEntityTag(path);
        if (const EntityId tagged = findEntityWithTag(sceneDocument_, tag); tagged.valid()) {
            entityByPrimPath[path] = tagged;
            return tagged;
        }
        const auto primIt = primByPath.find(path);
        if (primIt == primByPath.end()) {
            return {};
        }
        const nlohmann::json& prim = primIt->second;
        const EntityId parent = ensureUsdPrimEntity(prim.value("parentPath", std::string{}));
        std::string name = prim.value("name", std::string{});
        if (name.empty()) {
            name = usdPrimNameFromPath(path, "Prim", result.hierarchyEntityCount);
        }
        const EntityId id = ops.createEntity("USD: " + name, parent, SceneUpdateKind::TopologyChanged);
        if (Entity* entity = sceneDocument_.registry().entity(id)) {
            if (const auto transform = usdPrimLocalTransform(prim)) {
                entity->transform = *transform;
            }
            entity->sourceNodeIndex = prim.value("index", -1);
            entity->defaultTransform = entity->transform;
            addEntityTagIfMissing(*entity, tag);
            applyUsdPrimRuntimeCulling(*entity, prim);
        }
        entityByPrimPath[path] = id;
        ++result.hierarchyEntityCount;
        hierarchyUpdateMask |= SceneUpdateMaskTopology;
        return id;
    };

    struct PendingMeshAttach {
        EditorMeshAssetPlacement placement;
        std::string primPath;
        EntityId entity{};
        bool hasLocalTransform = false;
    };
    std::vector<PendingMeshAttach> pendingAttaches;
    pendingAttaches.reserve(meshPlacements.size());
    for (const EditorMeshAssetPlacement& placement : meshPlacements) {
        const AssetRecord* meshRecord = nullptr;
        for (const AssetRecord& record : assetRegistry_.records()) {
            if (record.guid == placement.meshGuid && record.type == AssetType::Mesh) {
                meshRecord = &record;
                break;
            }
        }
        if (meshRecord == nullptr) {
            continue;
        }
        const std::string primPath = usdMeshSourcePrimPathForRecord(*meshRecord, root);
        const EntityId entity = ensureUsdPrimEntity(primPath);
        if (!entity.valid()) {
            continue;
        }
        EditorMeshAssetPlacement attach = placement;
        attach.attachEntity = entity;
        attach.replaceEntity = {};
        const std::optional<Transform> localTransform = usdMeshLocalPlacementTransformForRecord(*meshRecord, root);
        if (localTransform.has_value()) {
            attach.placementTransform = *localTransform;
        }
        pendingAttaches.push_back(PendingMeshAttach{std::move(attach), primPath, entity, localTransform.has_value()});
    }

    if (pendingAttaches.empty()) {
        sceneDocument_ = beforeHierarchyDocument;
        return result;
    }
    if (result.hierarchyEntityCount > 0) {
        sceneDocument_.markDirty(hierarchyUpdateMask);
        sceneUnsavedDirty_ = true;
        undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
            sceneDocument_,
            beforeHierarchyDocument,
            sceneDocument_,
            sceneUpdateKindFromMask(hierarchyUpdateMask),
            "Place USD Mesh Hierarchy"));
    }

    for (const PendingMeshAttach& pending : pendingAttaches) {
        if (!placeMeshAsset(pending.placement, true)) {
            continue;
        }
        if (Entity* entity = sceneDocument_.registry().entity(pending.entity)) {
            if (const auto primIt = primByPath.find(pending.primPath); primIt != primByPath.end()) {
                applyUsdPrimRuntimeCulling(*entity, primIt->second);
                sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
                sceneUnsavedDirty_ = true;
            }
        }
        ++result.meshCount;
        if (pending.hasLocalTransform) {
            ++result.transformCount;
        }
    }
    if (result.meshCount > 0) {
        const bool deferRendererRebuild = shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
        (void)applyPendingSceneUpdate(!deferRendererRebuild);
        if (deferRendererRebuild) {
            notifications_.notify("USD mesh hierarchy placed; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
        }
    }
    return result;
}

std::optional<Application::UsdRuntimeScenePlacementResult> Application::placeUsdRuntimeSceneEntities(
    const AssetRecord& sceneRecord,
    const std::filesystem::path& root) {
    if (sceneRecord.type != AssetType::Scene) {
        return std::nullopt;
    }
    const std::optional<nlohmann::json> runtimePayload = runtimePayloadForImportedRecord(sceneRecord, root);
    if (!runtimePayload.has_value() || runtimePayload->value("kind", std::string{}) != "UsdStageMetadataPayload") {
        return std::nullopt;
    }
    const nlohmann::json cameras = runtimePayload->value("runtimeCameras", nlohmann::json::array());
    const nlohmann::json lights = runtimePayload->value("runtimeLights", nlohmann::json::array());
    if ((!cameras.is_array() || cameras.empty()) && (!lights.is_array() || lights.empty())) {
        return UsdRuntimeScenePlacementResult{};
    }
    const std::unordered_map<std::string, nlohmann::json> primByPath = usdPrimMetadataByPath(*runtimePayload);

    const SceneDocument beforeDocument = sceneDocument_;
    UsdRuntimeScenePlacementResult result;
    SceneUpdateMask updateMask = SceneUpdateMaskNone;
    const bool hadActiveCamera = sceneDocument_.activeCamera().valid();
    EntityId firstCreatedCamera{};
    EntityId firstCreatedEntity{};
    std::unordered_map<std::string, EntityId> entityByPrimPath;
    SceneOperations ops(sceneDocument_, &sceneEventBus_);
    ops.setUndoStack(nullptr);

    auto applyDefaultTransform = [](Entity& entity) {
        entity.defaultTransform = entity.transform;
    };

    std::function<EntityId(const std::string&)> ensureUsdPrimEntity = [&](const std::string& path) -> EntityId {
        if (path.empty() || path == "/") {
            return {};
        }
        if (const auto existing = entityByPrimPath.find(path); existing != entityByPrimPath.end()) {
            return existing->second;
        }
        const std::string tag = usdPrimPathEntityTag(path);
        if (const EntityId tagged = findEntityWithTag(sceneDocument_, tag); tagged.valid()) {
            entityByPrimPath[path] = tagged;
            return tagged;
        }
        const auto primIt = primByPath.find(path);
        if (primIt == primByPath.end()) {
            return {};
        }
        const nlohmann::json& prim = primIt->second;
        const std::string parentPath = prim.value("parentPath", std::string{});
        const EntityId parent = ensureUsdPrimEntity(parentPath);
        std::string name = prim.value("name", std::string{});
        if (name.empty()) {
            name = usdPrimNameFromPath(path, "Prim", result.hierarchyEntityCount);
        }
        const EntityId id = ops.createEntity("USD: " + name, parent, SceneUpdateKind::TransformOnly);
        Entity* entity = sceneDocument_.registry().entity(id);
        if (entity != nullptr) {
            if (const auto transform = usdPrimLocalTransform(prim)) {
                entity->transform = *transform;
            }
            entity->sourceNodeIndex = prim.value("index", -1);
            applyDefaultTransform(*entity);
            addEntityTagIfMissing(*entity, tag);
            applyUsdPrimRuntimeCulling(*entity, prim);
        }
        entityByPrimPath[path] = id;
        ++result.hierarchyEntityCount;
        updateMask |= SceneUpdateMaskTransform;
        if (!firstCreatedEntity.valid()) {
            firstCreatedEntity = id;
        }
        return id;
    };

    auto createFlatRuntimeEntity = [&](const nlohmann::json& source, std::string_view label, size_t index, SceneUpdateKind updateKind) {
        const EntityId id = ops.createEntity(
            std::string("USD ") + std::string(label) + ": " + usdRuntimeEntityName(source, label, index),
            {},
            updateKind);
        if (Entity* entity = sceneDocument_.registry().entity(id)) {
            if (const auto transform = usdRuntimeEntityTransform(source)) {
                entity->transform = *transform;
            }
            applyDefaultTransform(*entity);
        }
        if (!firstCreatedEntity.valid()) {
            firstCreatedEntity = id;
        }
        return id;
    };

    if (cameras.is_array()) {
        for (const nlohmann::json& cameraJson : cameras) {
            if (!cameraJson.is_object()) {
                continue;
            }
            const std::string primPath = cameraJson.value("primPath", std::string{});
            EntityId id = ensureUsdPrimEntity(primPath);
            if (!id.valid()) {
                id = createFlatRuntimeEntity(cameraJson, "Camera", result.cameraCount, SceneUpdateKind::CameraOnly);
            }
            Entity* entity = sceneDocument_.registry().entity(id);
            if (entity == nullptr) {
                continue;
            }
            const bool active = !hadActiveCamera && !firstCreatedCamera.valid();
            sceneDocument_.registry().addCamera(id, cameraFromUsdRuntimeJson(cameraJson, active));
            if (const auto primIt = primByPath.find(primPath); primIt != primByPath.end()) {
                applyUsdPrimRuntimeCulling(*entity, primIt->second);
            }
            if (active) {
                firstCreatedCamera = id;
            }
            ++result.cameraCount;
            updateMask |= SceneUpdateMaskCamera;
        }
    }

    if (lights.is_array()) {
        for (const nlohmann::json& lightJson : lights) {
            if (!lightJson.is_object()) {
                continue;
            }
            const std::string primPath = lightJson.value("primPath", std::string{});
            EntityId id = ensureUsdPrimEntity(primPath);
            if (!id.valid()) {
                id = createFlatRuntimeEntity(lightJson, "Light", result.lightCount, SceneUpdateKind::LightOnly);
            }
            Entity* entity = sceneDocument_.registry().entity(id);
            if (entity == nullptr) {
                continue;
            }
            sceneDocument_.registry().addLight(id, lightFromUsdRuntimeJson(lightJson));
            if (const auto primIt = primByPath.find(primPath); primIt != primByPath.end()) {
                applyUsdPrimRuntimeCulling(*entity, primIt->second);
            }
            ++result.lightCount;
            updateMask |= SceneUpdateMaskLight;
        }
    }

    if (result.cameraCount == 0 && result.lightCount == 0) {
        sceneDocument_ = beforeDocument;
        return result;
    }
    if (firstCreatedCamera.valid()) {
        sceneDocument_.setActiveCamera(firstCreatedCamera);
        updateMask |= SceneUpdateMaskCamera;
    }
    sceneDocument_.markDirty(updateMask);
    sceneUnsavedDirty_ = true;
    undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
        sceneDocument_,
        beforeDocument,
        sceneDocument_,
        sceneUpdateKindFromMask(updateMask),
        result.hierarchyEntityCount > result.cameraCount + result.lightCount ? "Place USD Camera/Light Hierarchy" : "Place USD Cameras And Lights"));
    const bool deferRendererRebuild = shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
    (void)applyPendingSceneUpdate(!deferRendererRebuild);
    if (deferRendererRebuild) {
        notifications_.notify("USD cameras/lights placed; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
    }
    if (firstCreatedEntity.valid()) {
        editorPlacement_.entity = firstCreatedEntity;
        editorPlacement_.serial = nextEditorPlacementSerial_++;
        editorPlacement_.label = "USD cameras/lights";
    }
    notifications_.notify("USD cameras/lights hierarchy placed", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    return result;
}

bool Application::placeMeshScatterAssets(const EditorMeshScatterPlacement& request) {
    if (request.instances.empty()) {
        notifications_.notify("Scatter palette is empty", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 4.0f);
        return false;
    }

    const AssetManager beforeAssets = assets_;
    AssetManager nextAssets = std::move(assets_);
    std::unordered_map<AssetGuid, MeshAssetHandle> meshHandles;
    bool restoredRuntimeAssets = false;
    const std::filesystem::path root = project_.has_value()
        ? project_->projectRoot
        : (assetRegistry_.state().path.has_parent_path() ? assetRegistry_.state().path.parent_path() : std::filesystem::current_path());

    auto findRecord = [&](const AssetGuid& guid, AssetType expectedType) -> const AssetRecord* {
        for (const AssetRecord& record : assetRegistry_.records()) {
            if (record.guid == guid && record.type == expectedType) {
                return &record;
            }
        }
        return nullptr;
    };

    for (const EditorMeshScatterInstancePlacement& instance : request.instances) {
        if (meshHandles.find(instance.meshGuid) != meshHandles.end()) {
            continue;
        }
        const AssetRecord* meshRecord = findRecord(instance.meshGuid, AssetType::Mesh);
        if (meshRecord == nullptr) {
            notifications_.notify("Scatter mesh asset not found", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            return false;
        }
        if (assetPlacementBlocked(*meshRecord)) {
            notifications_.notify("Scatter mesh placement blocked", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
            std::cerr << "Scatter mesh placement blocked for asset " << instance.meshGuid << ": " << assetPlacementBlockReason(*meshRecord) << '\n';
            return false;
        }

        if (const std::optional<uint32_t> loadedMesh = loadedMeshIndexForRecord(*meshRecord)) {
            meshHandles.emplace(instance.meshGuid, MeshAssetHandle{*loadedMesh});
            continue;
        }

        PrefabRuntimeBindings bindings;
        std::string bindError;
        NativeRuntimeLoadOptions nativeLoadOptions;
        nativeLoadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
        nativeLoadOptions.validatePayloadHashes = false;
        nativeLoadOptions.retainLoadedPayloadsInReport = false;
        if (!appendCachedPrefabRuntimeAssets(
                *meshRecord,
                root,
                resolveAssetSourcePath(*meshRecord, root),
                resolveAssetCachePath(*meshRecord, root),
                &assetRegistry_,
                nextAssets,
                bindings,
                nativeLoadOptions,
                &bindError)) {
            notifications_.notify("Scatter mesh cooked payload is unavailable", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
            if (!bindError.empty()) {
                std::cerr << "Scatter mesh runtime binding failed: " << bindError << '\n';
            }
            return false;
        }
        const auto restoredMesh = bindings.meshes.find(instance.meshGuid);
        if (restoredMesh == bindings.meshes.end() || !restoredMesh->second.valid()) {
            notifications_.notify("Scatter mesh payload did not expose requested asset", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            return false;
        }
        meshHandles.emplace(instance.meshGuid, restoredMesh->second);
        restoredRuntimeAssets = true;
    }

    assets_ = std::move(nextAssets);
    auto materialOverride = [&](const AssetGuid& materialGuid) -> MaterialAssetHandle {
        if (materialGuid.empty()) {
            return {};
        }
        const AssetRecord* materialRecord = findRecord(materialGuid, AssetType::Material);
        if (materialRecord == nullptr) {
            return {};
        }
        if (const std::optional<uint32_t> materialIndex = loadedMaterialIndexForRecord(*materialRecord)) {
            return MaterialAssetHandle{*materialIndex};
        }
        return {};
    };

    auto makeRenderer = [&](const EditorMeshScatterInstancePlacement& instance) {
        MeshRenderer renderer;
        renderer.mesh = meshHandles.at(instance.meshGuid);
        renderer.meshGuid = instance.meshGuid;
        ensureMaterialSlotsForRenderer(renderer, assets_);
        const MaterialAssetHandle overrideMaterial = materialOverride(instance.materialGuid);
        for (MaterialSlot& slot : renderer.materialSlots) {
            if (overrideMaterial.valid()) {
                slot.material = overrideMaterial;
                slot.materialGuid = instance.materialGuid;
                continue;
            }
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
    SceneOperations ops(sceneDocument_, &sceneEventBus_);
    ops.setUndoStack(nullptr);
    EntityId firstCreated{};
    size_t createdCount = 0;
    for (const EditorMeshScatterInstancePlacement& instance : request.instances) {
        const AssetRecord* meshRecord = findRecord(instance.meshGuid, AssetType::Mesh);
        const std::string entityName = meshRecord != nullptr && !meshRecord->displayName.empty() ? meshRecord->displayName : "Scatter Mesh";
        const EntityId created = ops.createEntity(entityName, {}, SceneUpdateKind::TopologyChanged);
        Entity* entity = sceneDocument_.registry().entity(created);
        if (entity == nullptr) {
            sceneDocument_ = beforeDocument;
            assets_ = beforeAssets;
            notifications_.notify("Scatter placement failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 6.0f);
            return false;
        }
        entity->meshRenderer = makeRenderer(instance);
        entity->transform = instance.transform;
        entity->defaultTransform = entity->transform;
        if (!firstCreated.valid()) {
            firstCreated = created;
        }
        ++createdCount;
    }

    if (createdCount == 0) {
        sceneDocument_ = beforeDocument;
        assets_ = std::move(beforeAssets);
        return false;
    }

    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
    sceneUnsavedDirty_ = true;
    const std::string undoLabel = request.label.empty() ? "Scatter Mesh Palette" : request.label;
    if (restoredRuntimeAssets) {
        undoStack_.pushCommand(std::make_unique<SceneAndAssetsSnapshotCommand>(
            sceneDocument_,
            assets_,
            beforeDocument,
            beforeAssets,
            sceneDocument_,
            assets_,
            SceneUpdateKind::TopologyChanged,
            undoLabel));
    } else {
        undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
            sceneDocument_,
            beforeDocument,
            sceneDocument_,
            SceneUpdateKind::TopologyChanged,
            undoLabel));
    }
    const bool deferRendererRebuild = shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
    (void)applyPendingSceneUpdate(!deferRendererRebuild);
    if (deferRendererRebuild) {
        notifications_.notify("Scatter placed; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
    }
    editorPlacement_.entity = firstCreated;
    editorPlacement_.serial = nextEditorPlacementSerial_++;
    editorPlacement_.label = undoLabel;
    notifications_.notify("Scatter palette placed", NotificationType::Success, NotificationAction::OpenContent, "Open Content", 5.0f);
    std::cout << "Placed scatter palette instances: " << createdCount << " seed=" << request.seed << '\n';
    return true;
}

bool Application::createEntityFromEditor(const EditorEntityCreateRequest& request) {
    const SceneUpdateKind createUpdateKind = createEntityUpdateKind(request.kind);
    const SceneDocument beforeDocument = sceneDocument_;
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(nullptr);

    EntityId created{};
    switch (request.kind) {
    case EditorEntityCreateKind::Empty:
        created = sceneOps.createEntity("Entity", request.parent, createUpdateKind);
        break;
    case EditorEntityCreateKind::Camera:
        created = sceneOps.createEntity("Camera", request.parent, createUpdateKind);
        if (created.valid()) {
            Camera camera;
            camera.active = true;
            (void)sceneOps.addCameraComponent(created, camera);
        }
        break;
    case EditorEntityCreateKind::Light:
        created = sceneOps.createEntity("Point Light", request.parent, createUpdateKind);
        if (created.valid()) {
            Light light;
            light.intensity = 100.0f;
            (void)sceneOps.addLightComponent(created, light);
        }
        break;
    case EditorEntityCreateKind::Sun:
        created = sceneOps.createEntity("Sun", request.parent, createUpdateKind);
        if (created.valid()) {
            Sun sun;
            sun.elevation = sceneDocument_.renderSettings().sunElevation;
            sun.azimuth = sceneDocument_.renderSettings().sunAzimuth;
            (void)sceneOps.addSunComponent(created, sun);
        }
        break;
    case EditorEntityCreateKind::SpotLight:
        created = sceneOps.createEntity("Spot Light", request.parent, createUpdateKind);
        if (created.valid()) {
            Light light;
            light.type = LightType::Spot;
            light.intensity = 100.0f;
            (void)sceneOps.addLightComponent(created, light);
        }
        break;
    case EditorEntityCreateKind::AreaLight:
        created = sceneOps.createEntity("Area Light", request.parent, createUpdateKind);
        if (created.valid()) {
            Light light;
            light.type = LightType::Area;
            light.sizeOrRadius = 1.0f;
            light.intensity = 8.0f;
            (void)sceneOps.addLightComponent(created, light);
        }
        break;
    case EditorEntityCreateKind::EnvironmentLight:
        created = sceneOps.createEntity("Environment Light", request.parent, createUpdateKind);
        if (Entity* entity = sceneDocument_.registry().entity(created)) {
            entity->environmentLight = EnvironmentLight{};
            sceneDocument_.worldSettings().activeEnvironment = created;
        }
        break;
    case EditorEntityCreateKind::SkyAtmosphere:
        created = sceneOps.createEntity("Sky Atmosphere", request.parent, createUpdateKind);
        if (Entity* entity = sceneDocument_.registry().entity(created)) {
            entity->skyAtmosphere = SkyAtmosphere{};
            sceneDocument_.worldSettings().skyAtmosphere = created;
        }
        break;
    case EditorEntityCreateKind::HeightFog:
        created = sceneOps.createEntity("Height Fog", request.parent, createUpdateKind);
        if (Entity* entity = sceneDocument_.registry().entity(created)) {
            entity->heightFog = HeightFog{};
            sceneDocument_.worldSettings().heightFog = created;
        }
        break;
    case EditorEntityCreateKind::VolumetricCloud:
        created = sceneOps.createEntity("Volumetric Cloud", request.parent, createUpdateKind);
        if (Entity* entity = sceneDocument_.registry().entity(created)) {
            entity->volumetricCloud = VolumetricCloud{};
        }
        break;
    case EditorEntityCreateKind::PostProcessVolume:
        created = sceneOps.createEntity("Post Process Volume", request.parent, createUpdateKind);
        if (Entity* entity = sceneDocument_.registry().entity(created)) {
            entity->postProcessVolume = PostProcessVolume{};
            sceneDocument_.worldSettings().postProcessVolume = created;
        }
        break;
    }

    if (!created.valid()) {
        return false;
    }

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
    return true;
}

bool Application::duplicateEntityFromEditor(EntityId entity) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    const EntityId duplicate = sceneOps.duplicateEntity(entity);
    if (!duplicate.valid()) {
        return false;
    }
    editorPlacement_.entity = duplicate;
    editorPlacement_.serial = nextEditorPlacementSerial_++;
    editorPlacement_.label = "Duplicated entity";
    sceneUnsavedDirty_ = true;
    return true;
}

void Application::clearDeletedEditorEntityState(EntityId id) {
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
}

bool Application::alignDistributeEntitiesFromEditor(const EditorAlignDistributeRequest& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.alignDistributeEntities(request.entities, request.bounds, request.axis, request.mode)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::updateMaterialFromEditor(const EditorMaterialUpdate& request) {
    MaterialAsset* material = assets_.material(MaterialAssetHandle{request.materialId});
    if (material == nullptr) {
        return false;
    }

    const SceneDocument beforeDocument = sceneDocument_;
    const AssetManager beforeAssets = assets_;
    const std::optional<AssetRecord> materialRecord = materialAssetRecordForMaterial(request.materialId);
    *material = request.material;
    for (uint32_t meshIndex = 0; meshIndex < assets_.meshes().size(); ++meshIndex) {
        MeshAsset* mesh = assets_.mesh(MeshAssetHandle{meshIndex});
        if (mesh == nullptr) {
            continue;
        }
        for (MeshPrimitiveAsset& primitive : mesh->primitives) {
            if (primitive.material.index == request.materialId) {
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
    return true;
}

bool Application::deleteEntityFromEditor(EntityId entity) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.deleteEntity(entity)) {
        return false;
    }
    clearDeletedEditorEntityState(entity);
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::deleteEntitiesFromEditor(const std::vector<EntityId>& entities) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.deleteEntities(entities)) {
        return false;
    }
    for (EntityId entity : entities) {
        clearDeletedEditorEntityState(entity);
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::renameEntityFromEditor(const EditorEntityRenameRequest& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.renameEntity(request.entity, request.name)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::reparentEntityFromEditor(EntityId child, EntityId newParent) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.reparentEntity(child, newParent)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::setEntityVisibilityFromEditor(const EditorEntityBoolChange& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.setVisibility(request.entity, request.value)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::setEntityLockedFromEditor(const EditorEntityBoolChange& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.setLocked(request.entity, request.value)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::setEntityTransformFromEditor(const EditorEntityTransformChange& request) {
    const Entity* entity = sceneDocument_.registry().entity(request.entity);
    if (entity == nullptr || entity->locked) {
        return false;
    }
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    sceneOps.setTransformGizmoDrag(request.entity, request.oldTransform, request.newTransform);
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::setEntityTransformsFromEditor(const EditorEntityTransformBatchChange& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.setTransformGizmoDragBatch(request.changes)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::setMeshRendererFromEditor(const EditorMeshRendererChange& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.setMeshRenderer(request.entity, request.oldRenderer, request.newRenderer, request.updateKind)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::addComponentFromEditor(const EditorComponentRequest& request) {
    const SceneDocument beforeDocument = sceneDocument_;
    bool directComponentAdded = false;
    SceneUpdateKind directUpdateKind = SceneUpdateKind::TopologyChanged;
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    switch (request.kind) {
    case EditorComponentKind::Light:
        {
            Light light;
            light.intensity = 100.0f;
            (void)sceneOps.addLightComponent(request.entity, light);
        }
        break;
    case EditorComponentKind::Sun:
        (void)sceneOps.addSunComponent(request.entity, Sun{});
        break;
    case EditorComponentKind::Camera:
        (void)sceneOps.addCameraComponent(request.entity, Camera{});
        break;
    case EditorComponentKind::MeshRenderer:
        (void)sceneOps.addMeshRendererComponent(request.entity, MeshRenderer{});
        break;
    case EditorComponentKind::EnvironmentLight:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && !entity->environmentLight.has_value()) {
            entity->environmentLight = EnvironmentLight{};
            sceneDocument_.worldSettings().activeEnvironment = entity->id;
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            directComponentAdded = true;
        }
        break;
    case EditorComponentKind::SkyAtmosphere:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && !entity->skyAtmosphere.has_value()) {
            entity->skyAtmosphere = SkyAtmosphere{};
            sceneDocument_.worldSettings().skyAtmosphere = entity->id;
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            directComponentAdded = true;
        }
        break;
    case EditorComponentKind::HeightFog:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && !entity->heightFog.has_value()) {
            entity->heightFog = HeightFog{};
            sceneDocument_.worldSettings().heightFog = entity->id;
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            directComponentAdded = true;
        }
        break;
    case EditorComponentKind::VolumetricCloud:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && !entity->volumetricCloud.has_value()) {
            entity->volumetricCloud = VolumetricCloud{};
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            directComponentAdded = true;
        }
        break;
    case EditorComponentKind::PostProcessVolume:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && !entity->postProcessVolume.has_value()) {
            entity->postProcessVolume = PostProcessVolume{};
            sceneDocument_.worldSettings().postProcessVolume = entity->id;
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            directComponentAdded = true;
        }
        break;
    case EditorComponentKind::CameraPostProcess:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->camera.has_value() && !entity->cameraPostProcess.has_value()) {
            entity->cameraPostProcess = CameraPostProcess{};
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            directComponentAdded = true;
        }
        break;
    case EditorComponentKind::AnimationPlayer:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && !entity->animationPlayer.has_value()) {
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
    return true;
}

bool Application::removeComponentFromEditor(const EditorComponentRequest& request) {
    const SceneDocument beforeDocument = sceneDocument_;
    bool removed = false;
    SceneUpdateKind directUpdateKind = SceneUpdateKind::TopologyChanged;
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    switch (request.kind) {
    case EditorComponentKind::Light:
        removed = sceneOps.removeLightComponent(request.entity);
        break;
    case EditorComponentKind::Sun:
        removed = sceneOps.removeSunComponent(request.entity);
        break;
    case EditorComponentKind::Camera:
        removed = sceneOps.removeCameraComponent(request.entity);
        break;
    case EditorComponentKind::MeshRenderer:
        removed = sceneOps.removeMeshRendererComponent(request.entity);
        break;
    case EditorComponentKind::EnvironmentLight:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->environmentLight.has_value()) {
            entity->environmentLight.reset();
            if (sceneDocument_.worldSettings().activeEnvironment == entity->id) sceneDocument_.worldSettings().activeEnvironment = {};
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            removed = true;
        }
        break;
    case EditorComponentKind::SkyAtmosphere:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->skyAtmosphere.has_value()) {
            entity->skyAtmosphere.reset();
            if (sceneDocument_.worldSettings().skyAtmosphere == entity->id) sceneDocument_.worldSettings().skyAtmosphere = {};
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            removed = true;
        }
        break;
    case EditorComponentKind::HeightFog:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->heightFog.has_value()) {
            entity->heightFog.reset();
            if (sceneDocument_.worldSettings().heightFog == entity->id) sceneDocument_.worldSettings().heightFog = {};
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            removed = true;
        }
        break;
    case EditorComponentKind::VolumetricCloud:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->volumetricCloud.has_value()) {
            entity->volumetricCloud.reset();
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            removed = true;
        }
        break;
    case EditorComponentKind::PostProcessVolume:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->postProcessVolume.has_value()) {
            entity->postProcessVolume.reset();
            if (sceneDocument_.worldSettings().postProcessVolume == entity->id) sceneDocument_.worldSettings().postProcessVolume = {};
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            removed = true;
        }
        break;
    case EditorComponentKind::CameraPostProcess:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->cameraPostProcess.has_value()) {
            entity->cameraPostProcess.reset();
            applySceneWorldComponentsToDocumentSettings(sceneDocument_);
            sceneDocument_.markDirty(SceneUpdateKind::RendererSettingsOnly);
            directUpdateKind = SceneUpdateKind::RendererSettingsOnly;
            removed = true;
        }
        break;
    case EditorComponentKind::AnimationPlayer:
        if (Entity* entity = sceneDocument_.registry().entity(request.entity); entity != nullptr && entity->animationPlayer.has_value()) {
            entity->animationPlayer.reset();
            sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
            directUpdateKind = SceneUpdateKind::TransformOnly;
            removed = true;
        }
        break;
    }
    if (removed) {
        if (request.kind >= EditorComponentKind::EnvironmentLight) {
            undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
                sceneDocument_, beforeDocument, sceneDocument_, directUpdateKind, "Remove Component"));
        }
        sceneUnsavedDirty_ = true;
    }
    return removed;
}

bool Application::setLightFromEditor(const EditorLightChange& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.setLight(request.entity, request.oldLight, request.newLight)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::setSunFromEditor(const EditorSunChange& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.setSun(request.entity, request.oldSun, request.newSun)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::setCameraFromEditor(const EditorCameraChange& request) {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.setCamera(
            request.entity,
            request.oldCamera,
            request.newCamera,
            request.oldActiveCamera,
            request.newActiveCamera)) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::assignMaterialFromEditor(const EditorMaterialAssignment& request) {
    const SceneDocument beforeDocument = sceneDocument_;
    const AssetManager beforeAssets = assets_;
    bool assigned = false;
    if (request.entity.valid()) {
        if (Entity* entity = sceneDocument_.registry().entity(request.entity);
            entity != nullptr && entity->meshRenderer.has_value()) {
            MeshRenderer& renderer = *entity->meshRenderer;
            ensureMaterialSlotsForRenderer(renderer, assets_);
            const MaterialAssetHandle material = request.material;
            if (request.primitiveIndex == UINT32_MAX) {
                for (MaterialSlot& slot : renderer.materialSlots) {
                    slot.overrideMaterial = material.index == slot.material.index
                        ? std::optional<MaterialAssetHandle>{}
                        : std::optional<MaterialAssetHandle>{material};
                }
                assigned = true;
            } else if (request.primitiveIndex < renderer.materialSlots.size()) {
                MaterialSlot& slot = renderer.materialSlots[request.primitiveIndex];
                slot.overrideMaterial = material.index == slot.material.index
                    ? std::optional<MaterialAssetHandle>{}
                    : std::optional<MaterialAssetHandle>{material};
                assigned = true;
            }
        }
    }
    MeshAsset* mesh = assigned ? nullptr : assets_.mesh(request.mesh);
    if (mesh != nullptr && request.primitiveIndex == UINT32_MAX) {
        for (MeshPrimitiveAsset& primitive : mesh->primitives) {
            primitive.material = request.material;
            updatePrimitiveAlphaClassification(primitive, assets_.material(request.material));
        }
        assigned = true;
    } else if (mesh != nullptr && request.primitiveIndex < mesh->primitives.size()) {
        MeshPrimitiveAsset& primitive = mesh->primitives[request.primitiveIndex];
        primitive.material = request.material;
        updatePrimitiveAlphaClassification(primitive, assets_.material(request.material));
        assigned = true;
    }
    if (!assigned) {
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
        "Assign Material"));
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

bool Application::assignEnvironmentPathFromEditor(const std::filesystem::path& environmentPath, bool allowResourceRebuild) {
    if (!assignEnvironmentPath(environmentPath, allowResourceRebuild, "Assign Environment", "Environment assigned")) {
        return false;
    }
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().editorPrefs().addRecentFile(environmentPath);
        (void)saveActiveEditorPreferences();
    }
    std::cout << "Assigned HDR environment from editor: " << environmentPath.string() << '\n';
    return true;
}

bool Application::assignEnvironmentAssetFromEditor(const AssetGuid& environmentGuid, bool allowResourceRebuild) {
    return assignEnvironmentAsset(environmentGuid, allowResourceRebuild);
}

bool Application::applySceneSnapshotFromEditor(const EditorSceneSnapshotChange& request) {
    sceneDocument_.markDirty(request.updateKind);
    undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
        sceneDocument_,
        request.before,
        sceneDocument_,
        request.updateKind,
        request.label.empty() ? "Edit Scene" : request.label));
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::updateTimelineFromEditor(const nlohmann::json& timelineJson) {
    const SceneDocument before = sceneDocument_;
    sceneDocument_.setTimelineJson(timelineJson);
    SceneDocument after = sceneDocument_;
    after.markDirty(SceneUpdateKind::None);
    undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
        sceneDocument_, before, std::move(after), SceneUpdateKind::None, "Edit Timeline"));
    sceneUnsavedDirty_ = true;
    notifications_.notify("Timeline updated", NotificationType::Info);
    return true;
}

bool Application::ensurePrimarySunFromEditor() {
    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (!sceneOps.ensurePrimarySun()) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    return true;
}

bool Application::togglePrimarySunFromEditor(bool allowResourceRebuild) {
    const SceneDocument before = sceneDocument_;
    const bool hadPrimarySun = SunController::primarySunEntity(sceneDocument_).valid();
    const EntityId sunId = SunController::ensurePrimarySun(sceneDocument_);
    Entity* sun = sceneDocument_.registry().entity(sunId);
    if (sun == nullptr || !sun->sun.has_value()) {
        sceneDocument_ = before;
        return false;
    }

    sun->sun->enabled = hadPrimarySun ? !sun->sun->enabled : true;
    sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
    SceneDocument after = sceneDocument_;
    after.markDirty(SceneUpdateKind::LightOnly);
    undoStack_.pushCommand(std::make_unique<AppSceneDocumentSnapshotCommand>(
        sceneDocument_, before, std::move(after), SceneUpdateKind::LightOnly, "Toggle Primary Sun"));
    sceneUnsavedDirty_ = true;
    return applyPendingSceneUpdate(allowResourceRebuild);
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
    size_t deletedDirectories = 0;
    size_t skippedFiles = 0;
    if (request.deleteGeneratedFiles) {
        std::vector<std::filesystem::path> candidates;
        std::vector<std::filesystem::path> cleanupDirectories;
        std::unordered_set<std::string> remainingReferencedPaths;
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
                path = normalizedPathForCompare(path);
                candidates.push_back(path);
                if (path.has_parent_path()) {
                    std::filesystem::path cleanup = path.parent_path();
                    while (!cleanup.empty() && pathIsInsideDirectory(cleanup, root)) {
                        cleanupDirectories.push_back(cleanup);
                        const std::filesystem::path parent = cleanup.parent_path();
                        if (parent == cleanup) {
                            break;
                        }
                        cleanup = parent;
                    }
                }
            }
        }
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
                remainingReferencedPaths.insert(normalizedPathForCompare(recordPath).generic_string());
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        std::sort(cleanupDirectories.begin(), cleanupDirectories.end(), [](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
            const auto lhsDepth = std::distance(lhs.begin(), lhs.end());
            const auto rhsDepth = std::distance(rhs.begin(), rhs.end());
            if (lhsDepth != rhsDepth) {
                return lhsDepth > rhsDepth;
            }
            return lhs.generic_string() < rhs.generic_string();
        });
        cleanupDirectories.erase(std::unique(cleanupDirectories.begin(), cleanupDirectories.end()), cleanupDirectories.end());

        for (const std::filesystem::path& path : candidates) {
            if (path.empty() || remainingReferencedPaths.find(path.generic_string()) != remainingReferencedPaths.end() || !pathIsInsideDirectory(path, root)) {
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

        const std::filesystem::path normalizedRoot = normalizedPathForCompare(root);
        for (const std::filesystem::path& directory : cleanupDirectories) {
            if (directory.empty() || directory == normalizedRoot || !pathIsInsideDirectory(directory, normalizedRoot)) {
                continue;
            }
            std::error_code ec;
            if (std::filesystem::is_directory(directory, ec) && std::filesystem::is_empty(directory, ec) && std::filesystem::remove(directory, ec) && !ec) {
                ++deletedDirectories;
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
              << " directories_deleted=" << deletedDirectories
              << " files_skipped=" << skippedFiles
              << " saved=" << (saved ? "true" : "false") << '\n';
    return saved;
}

void Application::recordCompletedNativeFileMigrationJob(const EditorNativeFileMigrationJobResult& result) {
    completedNativeFileMigrationJob_ = EditorJobCenterState{};
    completedNativeFileMigrationJob_.completedNativeFileMigrationSerial = result.serial;
    completedNativeFileMigrationJob_.completedNativeFileMigrationSuccess = result.success;
    completedNativeFileMigrationJob_.completedNativeFileMigrationPackage = result.package;
    completedNativeFileMigrationJob_.completedNativeFileMigrationDryRun = result.dryRun;
    completedNativeFileMigrationJob_.completedNativeFileMigrationMutationAttempted = result.mutationAttempted;
    completedNativeFileMigrationJob_.completedNativeFileMigrationMutated = result.mutated;
    completedNativeFileMigrationJob_.completedNativeFileMigrationRequired = result.migrationRequired;
    completedNativeFileMigrationJob_.completedNativeFileMigrationAvailable = result.migrationAvailable;
    completedNativeFileMigrationJob_.completedNativeFileMigrationTitle = result.title.empty()
        ? (result.package ? std::string("Migrate Package") : std::string("Migrate Native Asset"))
        : result.title;
    completedNativeFileMigrationJob_.completedNativeFileMigrationStatus = result.status;
    completedNativeFileMigrationJob_.completedNativeFileMigrationSourcePath = result.sourcePath;
    completedNativeFileMigrationJob_.completedNativeFileMigrationReportPath = result.reportPath;
    completedNativeFileMigrationJob_.completedNativeFileMigrationBackupPath = result.backupPath;
    completedNativeFileMigrationJob_.completedNativeFileMigrationErrors = result.errors;
    completedNativeFileMigrationJob_.completedNativeFileMigrationWarnings = result.warnings;
    completedNativeFileMigrationJob_.completedNativeFileMigrationWorkerTotalMs = result.elapsedMs;
    const std::string notificationTitle = completedNativeFileMigrationJob_.completedNativeFileMigrationTitle;
    notifications_.notify(
        result.success ? (notificationTitle + " complete") : (notificationTitle + " failed"),
        result.success ? NotificationType::Success : NotificationType::Error,
        NotificationAction::OpenContent,
        "Open Content",
        5.0f);
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().log().add(
            result.success ? EditorLogCategory::Project : EditorLogCategory::Error,
            notificationTitle + ": " + (result.status.empty() ? result.sourcePath.string() : result.status));
    }
}

bool Application::startNativeFileMigrationJob(EditorNativeFileMigrationJobRequest request) {
    if (request.sourcePath.empty()) {
        return false;
    }
    if (request.serial == 0) {
        request.serial = nextNativeFileMigrationJobSerial_++;
    } else {
        nextNativeFileMigrationJobSerial_ = std::max(nextNativeFileMigrationJobSerial_, request.serial + 1u);
    }
    pendingNativeFileMigrationJobs_.push_back(request);
    startNextNativeFileMigrationWorker();
    const bool queuedBehindActive = activeNativeFileMigrationJob_.has_value() && activeNativeFileMigrationJob_->request.serial != request.serial;
    notifications_.notify(
        queuedBehindActive ? "Native migration queued" : "Native migration started",
        NotificationType::Info,
        NotificationAction::OpenContent,
        "Open Content",
        4.0f);
    return true;
}

void Application::startNextNativeFileMigrationWorker() {
    if (activeNativeFileMigrationJob_.has_value() || pendingNativeFileMigrationJobs_.empty()) {
        return;
    }

    EditorNativeFileMigrationJobRequest request = pendingNativeFileMigrationJobs_.front();
    pendingNativeFileMigrationJobs_.pop_front();
    auto progress = std::make_shared<NativeFileMigrationWorkerProgress>();
    progress->progress = 0.02f;
    progress->stage = "Queued";
    progress->workerStartedAt = std::chrono::steady_clock::now();
    progress->stageStartedAt = progress->workerStartedAt;
    activeNativeFileMigrationJob_.emplace(ActiveNativeFileMigrationJob{
        request,
        progress,
        std::async(std::launch::async, [request, progress]() mutable {
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

            EditorNativeFileMigrationJobResult result;
            result.serial = request.serial;
            result.package = request.package;
            result.dryRun = request.dryRun;
            result.sourcePath = request.sourcePath;
            result.reportPath = request.reportPath;
            result.title = request.package
                ? (request.dryRun ? std::string("Migrate Package Dry Run") : std::string("Migrate Package"))
                : (request.dryRun ? std::string("Migrate Native Asset Dry Run") : std::string("Migrate Native Asset"));
            const auto startedAt = std::chrono::steady_clock::now();
            try {
                updateProgress(0.12f, "Inspecting");
                NativeAssetMigrationOptions options;
                options.dryRun = request.dryRun;
                options.package = request.package;
                updateProgress(request.dryRun ? 0.45f : 0.25f, request.dryRun ? "Planning" : "Migrating");
                const NativeAssetMigrationReport migration = migrateNativeAssetFile(request.sourcePath, options);
                updateProgress(0.82f, "Writing report");
                const nlohmann::json json = nativeAssetMigrationReportToJson(migration);
                std::error_code ec;
                const std::filesystem::path parent = request.reportPath.parent_path();
                if (!parent.empty()) {
                    std::filesystem::create_directories(parent, ec);
                }
                bool wroteReport = false;
                std::string reportError;
                if (ec) {
                    reportError = "Could not create migration report folder: " + ec.message();
                } else {
                    std::ofstream out(request.reportPath, std::ios::trunc);
                    if (!out.is_open()) {
                        reportError = "Could not write migration report: " + request.reportPath.string();
                    } else {
                        out << json.dump(2);
                        wroteReport = true;
                    }
                }
                result.success = wroteReport && migration.ok;
                result.mutationAttempted = migration.mutationAttempted;
                result.mutated = migration.mutated;
                result.migrationRequired = migration.migrationRequired;
                result.migrationAvailable = migration.migrationAvailable;
                result.reportPath = wroteReport ? request.reportPath : std::filesystem::path{};
                result.backupPath = migration.backupPath;
                if (!wroteReport) {
                    result.status = reportError.empty() ? std::string("Migration report write failed") : reportError;
                } else if (!migration.ok) {
                    result.status = reportError.empty() ? std::string("Migration report contains errors") : reportError;
                } else if (request.dryRun) {
                    result.status = migration.migrationRequired ? "Dry run complete: migration available" : "Dry run complete: no migration required";
                } else if (migration.mutated) {
                    result.status = "Migration completed with backup: " + migration.backupPath.string();
                } else if (migration.migrationRequired && !migration.migrationAvailable) {
                    result.status = "Migration unavailable";
                } else {
                    result.status = "Migration complete: no mutation required";
                }
                for (const NativeBinaryError& error : migration.errors) {
                    std::string message = error.message.empty() ? std::string(nativeBinaryErrorCodeName(error.code)) : error.message;
                    if (!error.table.empty()) {
                        message += " (" + error.table + ")";
                    }
                    result.errors.push_back(std::move(message));
                }
                if (!reportError.empty() && (!wroteReport || !migration.ok)) {
                    result.errors.push_back(reportError);
                }
                result.warnings = migration.warnings;
            } catch (const std::exception& error) {
                result.success = false;
                result.status = error.what();
                result.errors.push_back(error.what());
            } catch (...) {
                result.success = false;
                result.status = "Unknown native migration worker failure";
                result.errors.push_back(result.status);
            }
            result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count();
            updateProgress(1.0f, "Complete");
            return result;
        })});

    completedNativeFileMigrationJob_ = EditorJobCenterState{};
}
void Application::pollNativeFileMigrationJob() {
    using namespace std::chrono_literals;
    if (!activeNativeFileMigrationJob_.has_value()) {
        return;
    }
    if (activeNativeFileMigrationJob_->future.wait_for(0s) != std::future_status::ready) {
        return;
    }
    EditorNativeFileMigrationJobResult result = activeNativeFileMigrationJob_->future.get();
    activeNativeFileMigrationJob_.reset();
    recordCompletedNativeFileMigrationJob(result);
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
    seed.nativeTextureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
    seed.emitNativeTextureTargetSets = request.emitNativeTextureTargetSets;
    seed.nativeTextureTargetSetProfile = request.nativeTextureTargetSetProfile;
    seed.customNativeTextureTargetSet = request.customNativeTextureTargetSet;
    seed.customNativeTextureTargetSetLibrary = request.customNativeTextureTargetSetLibrary;
    if (seed.emitNativeTextureTargetSets) {
        seed.packageTextureTargetSetJson = nativeTextureTargetSetProfileJson(
            seed.nativeTextureTargetSetProfile,
            seed.nativeTextureFormatSupport,
            seed.customNativeTextureTargetSet,
            seed.customNativeTextureTargetSetLibrary);
    }

    activeCookProjectJob_.emplace(ActiveCookProjectJob{
        seed.serial,
        seed.projectFile,
        seed.outputDir,
        seed.manifestPath,
        seed.validationReportPath,
        seed.logPath,
        seed.emitNativeTextureTargetSets,
        seed.nativeTextureTargetSetProfile,
        seed.customNativeTextureTargetSet,
        seed.customNativeTextureTargetSetLibrary,
        std::async(std::launch::async, [seed]() mutable {
            const auto start = std::chrono::steady_clock::now();
            const std::filesystem::path exe = currentExecutablePath();
            seed.exitCode = runCookProjectProcess(
                exe,
                seed.projectFile,
                seed.outputDir,
                seed.manifestPath,
                seed.logPath,
                seed.nativeTextureFormatSupport,
                seed.packageTextureTargetSetJson,
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
    workspace.nativeTextureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
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
    const auto placeableUsdMeshChildPlacements = [&]() {
        std::vector<EditorMeshAssetPlacement> placements;
        if (result.record.type != AssetType::Scene) {
            return placements;
        }
        for (const AssetRecord& candidate : result.records) {
            if (candidate.guid.empty() || candidate.guid == result.record.guid || candidate.type != AssetType::Mesh) {
                continue;
            }
            const bool sameRoot = (!candidate.importRootGuid.empty() && candidate.importRootGuid == result.record.guid) ||
                (!result.record.importGroupId.empty() && candidate.importGroupId == result.record.importGroupId);
            if (!sameRoot) {
                continue;
            }
            if (candidate.status == AssetImportStatus::Failed || candidate.status == AssetImportStatus::Missing || candidate.cachePath.empty()) {
                continue;
            }
            placements.push_back(EditorMeshAssetPlacement{
                .meshGuid = candidate.guid,
                .placementTransform = usdMeshPlacementTransformForRecord(candidate, job.workspace.root),
            });
        }
        return placements;
    };
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
        std::string placementStatus = "Import and Place completed";
        if (job.placeAfterImport) {
            try {
            bool placed = false;
            if (result.record.type == AssetType::Prefab) {
                placed = placePrefabAsset(importedGuid);
            } else if (result.record.type == AssetType::Mesh) {
                placed = placeMeshAsset(EditorMeshAssetPlacement{.meshGuid = importedGuid});
            } else if (result.record.type == AssetType::Scene) {
                size_t placedCount = 0;
                size_t transformPlacementCount = 0;
                size_t meshHierarchyEntityCount = 0;
                bool placementNeedsSceneUpdate = false;
                const std::vector<EditorMeshAssetPlacement> usdMeshPlacements = placeableUsdMeshChildPlacements();
                const auto meshHierarchyPlacement = placeUsdRuntimeMeshHierarchy(result.record, job.workspace.root, usdMeshPlacements);
                if (meshHierarchyPlacement.has_value() && meshHierarchyPlacement->meshCount > 0) {
                    placed = true;
                    placementNeedsSceneUpdate = true;
                    placedCount = meshHierarchyPlacement->meshCount;
                    transformPlacementCount = meshHierarchyPlacement->transformCount;
                    meshHierarchyEntityCount = meshHierarchyPlacement->hierarchyEntityCount;
                } else {
                    for (const EditorMeshAssetPlacement& usdMeshPlacement : usdMeshPlacements) {
                        if (!placeMeshAsset(usdMeshPlacement, true)) {
                            placed = false;
                            break;
                        }
                        placed = true;
                        placementNeedsSceneUpdate = true;
                        ++placedCount;
                        if (usdMeshPlacement.placementTransform.has_value()) {
                            ++transformPlacementCount;
                        }
                    }
                }
                size_t cameraCount = 0;
                size_t lightCount = 0;
                size_t hierarchyEntityCount = 0;
                if (placed || usdMeshPlacements.empty()) {
                    if (const auto sceneEntities = placeUsdRuntimeSceneEntities(result.record, job.workspace.root)) {
                        cameraCount = sceneEntities->cameraCount;
                        lightCount = sceneEntities->lightCount;
                        hierarchyEntityCount = sceneEntities->hierarchyEntityCount;
                        placed = placed || cameraCount > 0 || lightCount > 0;
                        placementNeedsSceneUpdate = placementNeedsSceneUpdate || hierarchyEntityCount > 0;
                    }
                }
                bool animationPlayerAttached = false;
                if (placed) {
                    if (const std::optional<nlohmann::json> runtimePayload = runtimePayloadForImportedRecord(result.record, job.workspace.root)) {
                        animationPlayerAttached = attachUsdRuntimeAnimationPlayer(sceneDocument_, *runtimePayload, job.workspace.root);
                        if (animationPlayerAttached) {
                            sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
                            sceneUnsavedDirty_ = true;
                            placementNeedsSceneUpdate = true;
                        }
                    }
                }
                if (placementNeedsSceneUpdate || sceneDocument_.dirty()) {
                    const bool deferRendererRebuild = shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
                    (void)applyPendingSceneUpdate(!deferRendererRebuild);
                    if (deferRendererRebuild) {
                        notifications_.notify("Import placed; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
                    }
                }
                if (placed) {
                    std::vector<std::string> parts;
                    if (placedCount == 1) {
                        parts.push_back(transformPlacementCount == 1 ? "1 USD mesh placed with composed authored transform" : "1 USD mesh placed");
                    } else if (placedCount > 1) {
                        parts.push_back(std::to_string(placedCount) + " USD meshes placed" +
                            (transformPlacementCount > 0 ? ("; " + std::to_string(transformPlacementCount) + " composed authored transforms applied") : std::string{}));
                    }
                    if (cameraCount > 0) {
                        parts.push_back(std::to_string(cameraCount) + (cameraCount == 1 ? " USD camera placed" : " USD cameras placed"));
                    }
                    if (lightCount > 0) {
                        parts.push_back(std::to_string(lightCount) + (lightCount == 1 ? " USD light placed" : " USD lights placed"));
                    }
                    if (meshHierarchyEntityCount > 0) {
                        parts.push_back(std::to_string(meshHierarchyEntityCount) + " USD mesh hierarchy entities created");
                    }
                    if (hierarchyEntityCount > cameraCount + lightCount) {
                        parts.push_back(std::to_string(hierarchyEntityCount) + " USD hierarchy entities created");
                    }
                    if (animationPlayerAttached) {
                        parts.push_back("USD transform animation attached");
                    }
                    std::string summary;
                    for (const std::string& part : parts) {
                        if (!summary.empty()) {
                            summary += "; ";
                        }
                        summary += part;
                    }
                    placementStatus = "Import staged; " + summary;
                }
            } else {
                result.warnings.push_back(std::string("Import staged, but imported asset type is not placeable in the viewport: ") + assetTypeName(result.record.type));
            }
            if (!placed) {
                result.warnings.push_back("Import staged, but viewport placement failed. See editor log for placement error details.");
                recordCompletedImportJob(false, "Import staged; placement failed", result.warnings);
                return false;
            }
            } catch (const std::bad_alloc&) {
                const std::string message = "Import staged, but viewport placement ran out of system memory. Try import-only, close other memory-heavy applications, or place a smaller/streamed subset.";
                result.warnings.push_back(message);
                notifications_.notify("Import placement ran out of memory", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 8.0f);
                std::cerr << message << '\n';
                recordCompletedImportJob(false, "Import staged; placement ran out of memory", result.warnings);
                return false;
            } catch (const std::exception& ex) {
                const std::string message = std::string("Import staged, but viewport placement failed: ") + ex.what();
                result.warnings.push_back(message);
                notifications_.notify("Import placement failed", NotificationType::Error, NotificationAction::OpenContent, "Open Content", 8.0f);
                std::cerr << message << '\n';
                recordCompletedImportJob(false, "Import staged; placement failed", result.warnings);
                return false;
            }
        }
        recordCompletedImportJob(true, job.placeAfterImport ? placementStatus : "Import Asset staged");
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
            NativeRuntimeLoadOptions nativeLoadOptions;
            nativeLoadOptions.textureFormatSupport = nativeTextureFormatSupportForContext(context_.get());
            nativeLoadOptions.validatePayloadHashes = false;
            nativeLoadOptions.retainLoadedPayloadsInReport = false;
            if (std::string bindError; appendPrefabRuntimeAssets(*refreshedRecord, prefab, job.workspace.root, &assetRegistry_, nextAssets, bindings, nativeLoadOptions, &bindError)) {
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
                    const bool deferRendererRebuild = shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
                    (void)applyPendingSceneUpdate(!deferRendererRebuild);
                    if (deferRendererRebuild) {
                        notifications_.notify("Reimport rebound prefab; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
                    }
                }
            } else {
                notifications_.notify("Reimported metadata; runtime refresh failed", NotificationType::Warning, NotificationAction::OpenContent, "Open Content", 6.0f);
                std::cerr << "Reimport runtime refresh failed: " << bindError << '\n';
                result.warnings.push_back("Runtime refresh failed: " + bindError);
            }
        }
    } else if (job.originalType == AssetType::Scene && result.record.type == AssetType::Scene) {
        const std::vector<EditorMeshAssetPlacement> usdMeshPlacements = placeableUsdMeshChildPlacements();
        const std::optional<nlohmann::json> runtimePayload = runtimePayloadForImportedRecord(result.record, job.workspace.root);
        if (sceneHasUsdRuntimePlacement(sceneDocument_, usdMeshPlacements, runtimePayload)) {
            size_t refreshedMeshes = 0;
            size_t refreshedMeshTransforms = 0;
            size_t refreshedHierarchyEntities = 0;
            size_t refreshedCameras = 0;
            size_t refreshedLights = 0;
            size_t refreshedSceneHierarchyEntities = 0;

            if (const auto meshHierarchyPlacement = placeUsdRuntimeMeshHierarchy(result.record, job.workspace.root, usdMeshPlacements)) {
                refreshedMeshes = meshHierarchyPlacement->meshCount;
                refreshedMeshTransforms = meshHierarchyPlacement->transformCount;
                refreshedHierarchyEntities = meshHierarchyPlacement->hierarchyEntityCount;
            }
            if (const auto sceneEntities = placeUsdRuntimeSceneEntities(result.record, job.workspace.root)) {
                refreshedCameras = sceneEntities->cameraCount;
                refreshedLights = sceneEntities->lightCount;
                refreshedSceneHierarchyEntities = sceneEntities->hierarchyEntityCount;
            }
            const bool refreshedAnimationPlayer = runtimePayload.has_value()
                ? attachUsdRuntimeAnimationPlayer(sceneDocument_, *runtimePayload, job.workspace.root)
                : false;
            if (refreshedAnimationPlayer) {
                sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
                sceneUnsavedDirty_ = true;
                const bool deferRendererRebuild = shouldDeferInteractiveTopologyRebuild(sceneDocument_, assets_);
                (void)applyPendingSceneUpdate(!deferRendererRebuild);
                if (deferRendererRebuild) {
                    notifications_.notify("Reimport refreshed USD animation; renderer rebuild deferred for large scene", NotificationType::Info, NotificationAction::OpenContent, "Open Content", 5.0f);
                }
            }
            std::cout << "USD scene runtime refresh after reimport: meshes=" << refreshedMeshes
                      << " meshTransforms=" << refreshedMeshTransforms
                      << " meshHierarchyEntities=" << refreshedHierarchyEntities
                      << " cameras=" << refreshedCameras
                      << " lights=" << refreshedLights
                      << " sceneHierarchyEntities=" << refreshedSceneHierarchyEntities
                      << " animationPlayer=" << (refreshedAnimationPlayer ? "true" : "false")
                      << " source=" << job.request.sourcePath.string() << '\n';
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
        loader.setNativeTextureFormatSupport(nativeTextureFormatSupportForContext(context_.get()));
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
            std::vector<LiveMainThreadApplyOperation> operations;
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::ApplyRendererSettings,
                .rendererSettings = *requests.settings,
            });
            if (!queueLiveMainThreadApplyBatch("Apply Renderer Settings", std::move(operations))) {
                (void)applyRendererSettingsFromEditor(*requests.settings, false);
            }
        }
        bool interactiveLightPreview = false;
        if (requests.previewEntityTransform.has_value()) {
            sceneUnsavedDirty_ = true;
            if (Entity* entity = sceneDocument_.registry().entity(requests.previewEntityTransform->entity)) {
                entity->transform = requests.previewEntityTransform->transform;
                entity->transform.dirty = true;
                sceneDocument_.markDirty(requests.previewEntityTransform->updateKind);
                interactiveLightPreview = requests.previewEntityTransform->updateKind == SceneUpdateKind::LightOnly;
            }
        }
        if (requests.previewEntityTransforms.has_value()) {
            for (const EditorEntityTransformPreview& preview : requests.previewEntityTransforms->previews) {
                if (Entity* entity = sceneDocument_.registry().entity(preview.entity)) {
                    entity->transform = preview.transform;
                    entity->transform.dirty = true;
                    sceneDocument_.markDirty(preview.updateKind);
                    sceneUnsavedDirty_ = true;
                    interactiveLightPreview = interactiveLightPreview || preview.updateKind == SceneUpdateKind::LightOnly;
                }
            }
        }
        for (const EditorTimelineTransformSample& sample : requests.timelinePlaybackTransforms) {
            if (Entity* entity = sceneDocument_.registry().entity(sample.entity)) {
                entity->transform = sample.transform;
                entity->transform.dirty = true;
                sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
            }
        }
        (void)applyPendingSceneUpdate(false, interactiveLightPreview);
        if (requests.toggleDenoiser) {
            std::vector<LiveMainThreadApplyOperation> operations;
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::ToggleDenoiser,
            });
            if (!queueLiveMainThreadApplyBatch("Toggle Denoiser", std::move(operations))) {
                (void)toggleDenoiserFromEditor(false);
            }
        }
        if (requests.togglePrimarySun) {
            std::vector<LiveMainThreadApplyOperation> operations;
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::TogglePrimarySun,
            });
            if (!queueLiveMainThreadApplyBatch("Toggle Primary Sun", std::move(operations))) {
                (void)togglePrimarySunFromEditor(false);
            }
        }
        if (requests.toggleDebugView) {
            std::vector<LiveMainThreadApplyOperation> operations;
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::ToggleDebugView,
            });
            if (!queueLiveMainThreadApplyBatch("Toggle Debug View", std::move(operations))) {
                (void)toggleDebugViewFromEditor(false);
            }
        }
        if (requests.cycleIntermediateView) {
            std::vector<LiveMainThreadApplyOperation> operations;
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::CycleIntermediateView,
            });
            if (!queueLiveMainThreadApplyBatch("Cycle Intermediate View", std::move(operations))) {
                (void)cycleIntermediateViewFromEditor(false);
            }
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
        if (requests.timelineChanged.has_value()) {
            std::vector<LiveMainThreadApplyOperation> operations;
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::UpdateTimeline,
                .timelineJson = *requests.timelineChanged,
            });
            if (!queueLiveMainThreadApplyBatch("Edit Timeline", std::move(operations))) {
                (void)updateTimelineFromEditor(*requests.timelineChanged);
            }
        }
        return;
    }

    if (pendingPostFrameSettings_.has_value()) {
        RendererSettings pending = *pendingPostFrameSettings_;
        pendingPostFrameSettings_.reset();
        applyRendererSettingsSafely(pending, true);
    }
    if (requests.sceneUpdate.has_value()) {
        (void)markSceneUpdateFromEditor(*requests.sceneUpdate, true);
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
    if (requests.nativeFileMigrationJobRequest.has_value()) {
        (void)startNativeFileMigrationJob(*requests.nativeFileMigrationJobRequest);
    }
    for (const EditorNativeFileMigrationJobRequest& migrationRequest : requests.nativeFileMigrationJobRequests) {
        (void)startNativeFileMigrationJob(migrationRequest);
    }
    if (requests.nativeFileMigrationJobResult.has_value()) {
        EditorNativeFileMigrationJobResult result = *requests.nativeFileMigrationJobResult;
        if (result.serial == 0) {
            result.serial = nextNativeFileMigrationJobSerial_++;
        } else {
            nextNativeFileMigrationJobSerial_ = std::max(nextNativeFileMigrationJobSerial_, result.serial + 1u);
        }
        recordCompletedNativeFileMigrationJob(result);
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
    if (requests.mountNativePackage.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::MountNativePackage,
            .mountNativePackageRequest = *requests.mountNativePackage,
        });
        if (!queueLiveMainThreadApplyBatch("Mount Native Package", std::move(operations))) {
            (void)mountNativePackageFromEditor(requests.mountNativePackage->packagePath);
        }
    }
    if (requests.unloadNativePackage.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::UnloadNativePackage,
            .unloadNativePackageRequest = *requests.unloadNativePackage,
        });
        if (!queueLiveMainThreadApplyBatch("Unload Native Package", std::move(operations))) {
            (void)unloadNativePackageFromEditor(requests.unloadNativePackage->packagePath);
        }
    }
    if (requests.refreshNativePackage.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::RefreshNativePackage,
            .refreshNativePackageRequest = *requests.refreshNativePackage,
        });
        if (!queueLiveMainThreadApplyBatch("Refresh Native Package", std::move(operations))) {
            (void)refreshNativePackageFromEditor(requests.refreshNativePackage->packagePath);
        }
    }
    if (requests.restoreAutosave) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::RestoreRecoveryAutosaves,
        });
        if (!queueLiveMainThreadApplyBatch("Restore Recovery Autosaves", std::move(operations))) {
            (void)restoreRecoveryAutosavesFromEditor();
        }
    }
    if (requests.discardRecovery) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::DiscardRecovery,
        });
        if (!queueLiveMainThreadApplyBatch("Discard Recovery", std::move(operations))) {
            (void)discardRecoveryFromEditor();
        }
    }
    if (requests.projectSettingsUpdate.has_value() && project_.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::UpdateProjectSettings,
            .projectSettingsUpdate = *requests.projectSettingsUpdate,
        });
        if (!queueLiveMainThreadApplyBatch("Update Project Settings", std::move(operations))) {
            (void)updateProjectSettingsFromEditor(*requests.projectSettingsUpdate);
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
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::AssignEnvironmentPath,
            .environmentPath = *requests.loadHdr,
        });
        if (!queueLiveMainThreadApplyBatch("Assign Environment", std::move(operations))) {
            (void)assignEnvironmentPathFromEditor(*requests.loadHdr, allowResourceRebuild);
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
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::UpdateMaterial,
            .materialUpdate = *requests.materialUpdate,
        });
        if (!queueLiveMainThreadApplyBatch("Edit Material", std::move(operations))) {
            (void)updateMaterialFromEditor(*requests.materialUpdate);
        }
    }

    if (requests.materialAssignment.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::AssignMaterial,
            .materialAssignment = *requests.materialAssignment,
        });
        if (!queueLiveMainThreadApplyBatch("Assign Material", std::move(operations))) {
            (void)assignMaterialFromEditor(*requests.materialAssignment);
        }
    }

    if (requests.materialAssetAssignment.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::AssignMaterialAsset,
            .materialAssetAssignment = *requests.materialAssetAssignment,
        });
        if (!queueLiveMainThreadApplyBatch("Assign Material Asset", std::move(operations))) {
            (void)assignMaterialAssetToEntity(*requests.materialAssetAssignment);
        }
    }

    if (requests.meshAssetPlacement.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::PlaceMeshAsset,
            .meshPlacement = *requests.meshAssetPlacement,
        });
        if (!queueLiveMainThreadApplyBatch("Place Mesh Asset", std::move(operations))) {
            (void)placeMeshAsset(*requests.meshAssetPlacement);
        }
    }

    if (requests.meshScatterPlacement.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::PlaceMeshScatterAssets,
            .meshScatterPlacement = *requests.meshScatterPlacement,
        });
        if (!queueLiveMainThreadApplyBatch("Place Mesh Scatter Assets", std::move(operations))) {
            (void)placeMeshScatterAssets(*requests.meshScatterPlacement);
        }
    }

    if (requests.environmentAssetAssignment.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::AssignEnvironmentAsset,
            .environmentGuid = *requests.environmentAssetAssignment,
        });
        if (!queueLiveMainThreadApplyBatch("Assign Environment Asset", std::move(operations))) {
            (void)assignEnvironmentAssetFromEditor(*requests.environmentAssetAssignment, allowResourceRebuild);
        }
    }

    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (requests.createEntity.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::CreateEntity,
            .entityCreateRequest = *requests.createEntity,
        });
        if (!queueLiveMainThreadApplyBatch("Create Entity", std::move(operations))) {
            (void)createEntityFromEditor(*requests.createEntity);
        }
    }

    if (requests.ensurePrimarySun) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::EnsurePrimarySun,
        });
        if (!queueLiveMainThreadApplyBatch("Ensure Primary Sun", std::move(operations))) {
            (void)ensurePrimarySunFromEditor();
        }
    }

    if (requests.duplicateEntity.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::DuplicateEntity,
            .duplicateEntity = *requests.duplicateEntity,
        });
        if (!queueLiveMainThreadApplyBatch("Duplicate Entity", std::move(operations))) {
            (void)duplicateEntityFromEditor(*requests.duplicateEntity);
        }
    }

    if (requests.deleteEntity.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::DeleteEntity,
            .deleteEntity = *requests.deleteEntity,
        });
        if (!queueLiveMainThreadApplyBatch("Delete Entity", std::move(operations))) {
            (void)deleteEntityFromEditor(*requests.deleteEntity);
        }
    }

    if (!requests.deleteEntities.empty()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::DeleteEntities,
            .deleteEntities = requests.deleteEntities,
        });
        if (!queueLiveMainThreadApplyBatch("Delete Entities", std::move(operations))) {
            (void)deleteEntitiesFromEditor(requests.deleteEntities);
        }
    }

    if (requests.renameEntity.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::RenameEntity,
            .renameEntity = *requests.renameEntity,
        });
        if (!queueLiveMainThreadApplyBatch("Rename Entity", std::move(operations))) {
            (void)renameEntityFromEditor(*requests.renameEntity);
        }
    }

    if (requests.reparentEntity.has_value()) {
        const auto [child, newParent] = *requests.reparentEntity;
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::ReparentEntity,
            .reparentEntity = std::make_pair(child, newParent),
        });
        if (!queueLiveMainThreadApplyBatch("Reparent Entity", std::move(operations))) {
            (void)reparentEntityFromEditor(child, newParent);
        }
    }

    if (requests.setEntityVisibility.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetEntityVisibility,
            .entityBoolChange = *requests.setEntityVisibility,
        });
        if (!queueLiveMainThreadApplyBatch("Set Entity Visibility", std::move(operations))) {
            (void)setEntityVisibilityFromEditor(*requests.setEntityVisibility);
        }
    }

    if (requests.setEntityLocked.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetEntityLocked,
            .entityBoolChange = *requests.setEntityLocked,
        });
        if (!queueLiveMainThreadApplyBatch("Set Entity Locked", std::move(operations))) {
            (void)setEntityLockedFromEditor(*requests.setEntityLocked);
        }
    }

    if (requests.setEntityTransform.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetEntityTransform,
            .entityTransform = *requests.setEntityTransform,
        });
        if (!queueLiveMainThreadApplyBatch("Set Entity Transform", std::move(operations))) {
            (void)setEntityTransformFromEditor(*requests.setEntityTransform);
        }
    }

    if (requests.setEntityTransforms.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetEntityTransforms,
            .entityTransforms = *requests.setEntityTransforms,
        });
        if (!queueLiveMainThreadApplyBatch("Set Entity Transforms", std::move(operations))) {
            (void)setEntityTransformsFromEditor(*requests.setEntityTransforms);
        }
    }

    if (requests.alignDistributeEntities.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::AlignDistributeEntities,
            .alignDistributeRequest = *requests.alignDistributeEntities,
        });
        if (!queueLiveMainThreadApplyBatch("Align/Distribute Entities", std::move(operations))) {
            (void)alignDistributeEntitiesFromEditor(*requests.alignDistributeEntities);
        }
    }

    if (requests.setMeshRenderer.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetMeshRenderer,
            .meshRendererChange = *requests.setMeshRenderer,
        });
        if (!queueLiveMainThreadApplyBatch("Set Mesh Renderer", std::move(operations))) {
            (void)setMeshRendererFromEditor(*requests.setMeshRenderer);
        }
    }

    if (requests.addComponent.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::AddComponent,
            .componentRequest = *requests.addComponent,
        });
        if (!queueLiveMainThreadApplyBatch("Add Component", std::move(operations))) {
            (void)addComponentFromEditor(*requests.addComponent);
        }
    }

    if (requests.removeComponent.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::RemoveComponent,
            .componentRequest = *requests.removeComponent,
        });
        if (!queueLiveMainThreadApplyBatch("Remove Component", std::move(operations))) {
            (void)removeComponentFromEditor(*requests.removeComponent);
        }
    }

    if (requests.sceneSnapshot.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::ApplySceneSnapshot,
            .sceneSnapshotChange = *requests.sceneSnapshot,
        });
        if (!queueLiveMainThreadApplyBatch("Apply Scene Snapshot", std::move(operations))) {
            (void)applySceneSnapshotFromEditor(*requests.sceneSnapshot);
        }
    }

    if (requests.setLight.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetLight,
            .lightChange = *requests.setLight,
        });
        if (!queueLiveMainThreadApplyBatch("Set Light", std::move(operations))) {
            (void)setLightFromEditor(*requests.setLight);
        }
    }

    if (requests.setSun.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetSun,
            .sunChange = *requests.setSun,
        });
        if (!queueLiveMainThreadApplyBatch("Set Sun", std::move(operations))) {
            (void)setSunFromEditor(*requests.setSun);
        }
    }

    if (requests.setCamera.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::SetCamera,
            .cameraChange = *requests.setCamera,
        });
        if (!queueLiveMainThreadApplyBatch("Set Camera", std::move(operations))) {
            (void)setCameraFromEditor(*requests.setCamera);
        }
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

    if (!requests.importAssets.empty()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.reserve(requests.importAssets.size());
        for (const EditorImportAssetRequest& importRequest : requests.importAssets) {
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::ImportAsset,
                .importRequest = importRequest,
            });
        }
        if (!queueLiveMainThreadApplyBatch("Batch Import Assets", std::move(operations))) {
            for (const EditorImportAssetRequest& importRequest : requests.importAssets) {
                (void)queueAssetImportNonMutating(importRequest, false);
            }
        }
    }

    if (requests.importAsset.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::ImportAsset,
            .importRequest = *requests.importAsset,
        });
        if (!queueLiveMainThreadApplyBatch("Import Asset", std::move(operations))) {
            (void)queueAssetImportNonMutating(*requests.importAsset, false);
        }
    }

    if (!requests.importAndPlaceAssets.empty()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.reserve(requests.importAndPlaceAssets.size());
        for (EditorImportAssetRequest importRequest : requests.importAndPlaceAssets) {
            if (importRequest.mode.empty()) {
                importRequest.mode = "ImportAndPlace";
            }
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::ImportAndPlaceAsset,
                .importRequest = std::move(importRequest),
            });
        }
        if (!queueLiveMainThreadApplyBatch("Batch Import And Place Assets", std::move(operations))) {
            for (EditorImportAssetRequest importRequest : requests.importAndPlaceAssets) {
                if (importRequest.mode.empty()) {
                    importRequest.mode = "ImportAndPlace";
                }
                (void)queueAssetImportNonMutating(importRequest, true);
            }
        }
    }

    if (requests.importAndPlace.has_value()) {
        EditorImportAssetRequest importRequest = *requests.importAndPlace;
        if (importRequest.mode.empty()) {
            importRequest.mode = "ImportAndPlace";
        }
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::ImportAndPlaceAsset,
            .importRequest = importRequest,
        });
        if (!queueLiveMainThreadApplyBatch("Import And Place Asset", std::move(operations))) {
            (void)queueAssetImportNonMutating(importRequest, true);
        }
    }

    if (requests.reimportAsset.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::ReimportAsset,
            .assetGuid = *requests.reimportAsset,
        });
        if (!queueLiveMainThreadApplyBatch("Reimport Asset", std::move(operations))) {
            (void)queueAssetReimport(*requests.reimportAsset);
        }
    }

    if (requests.relinkAssetSource.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::RelinkAssetSource,
            .relinkAssetSourceRequest = *requests.relinkAssetSource,
        });
        if (!queueLiveMainThreadApplyBatch("Relink Asset Source", std::move(operations))) {
            (void)relinkAssetSource(*requests.relinkAssetSource);
        }
    }

    if (requests.replaceAssetReferences.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::ReplaceAssetReferences,
            .replaceAssetReferencesRequest = *requests.replaceAssetReferences,
        });
        if (!queueLiveMainThreadApplyBatch("Replace Asset References", std::move(operations))) {
            (void)replaceAssetReferences(*requests.replaceAssetReferences, allowResourceRebuild);
        }
    }

    if (requests.repairMissingAssetDependencies.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::RepairMissingAssetDependencies,
            .repairMissingDependenciesRequest = *requests.repairMissingAssetDependencies,
        });
        if (!queueLiveMainThreadApplyBatch("Repair Missing Asset Dependencies", std::move(operations))) {
            (void)repairMissingAssetDependencies(*requests.repairMissingAssetDependencies);
        }
    }

    if (requests.updateAssetTags.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::UpdateAssetTags,
            .assetTagsRequest = *requests.updateAssetTags,
        });
        if (!queueLiveMainThreadApplyBatch("Update Asset Tags", std::move(operations))) {
            (void)updateAssetTags(*requests.updateAssetTags);
        }
    }

    if (requests.renameAsset.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::RenameAsset,
            .renameAssetRequest = *requests.renameAsset,
        });
        if (!queueLiveMainThreadApplyBatch("Rename Asset", std::move(operations))) {
            (void)renameAssetRecord(*requests.renameAsset);
        }
    }

    if (requests.bulkAddAssetTag.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::BulkAddAssetTag,
            .bulkAssetTagRequest = *requests.bulkAddAssetTag,
        });
        if (!queueLiveMainThreadApplyBatch("Bulk Add Asset Tag", std::move(operations))) {
            (void)bulkAddAssetTag(*requests.bulkAddAssetTag);
        }
    }

    if (requests.bulkRemoveAssetTag.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::BulkRemoveAssetTag,
            .bulkAssetTagRequest = *requests.bulkRemoveAssetTag,
        });
        if (!queueLiveMainThreadApplyBatch("Bulk Remove Asset Tag", std::move(operations))) {
            (void)bulkRemoveAssetTag(*requests.bulkRemoveAssetTag);
        }
    }

    if (requests.moveAssetsToFolder.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::MoveAssetsToFolder,
            .moveAssetsRequest = *requests.moveAssetsToFolder,
        });
        if (!queueLiveMainThreadApplyBatch("Move Assets To Folder", std::move(operations))) {
            (void)moveAssetsToFolder(*requests.moveAssetsToFolder);
        }
    }

    if (requests.deleteAssets.has_value()) {
        constexpr size_t maxAssetDeleteGuidsPerApplyOperation = 64;
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.reserve((requests.deleteAssets->guids.size() + maxAssetDeleteGuidsPerApplyOperation - 1u) / maxAssetDeleteGuidsPerApplyOperation);
        for (size_t offset = 0; offset < requests.deleteAssets->guids.size(); offset += maxAssetDeleteGuidsPerApplyOperation) {
            const size_t end = std::min(requests.deleteAssets->guids.size(), offset + maxAssetDeleteGuidsPerApplyOperation);
            EditorDeleteAssetRequest chunkRequest;
            chunkRequest.deleteGeneratedFiles = requests.deleteAssets->deleteGeneratedFiles;
            chunkRequest.guids.insert(chunkRequest.guids.end(), requests.deleteAssets->guids.begin() + static_cast<std::ptrdiff_t>(offset), requests.deleteAssets->guids.begin() + static_cast<std::ptrdiff_t>(end));
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::DeleteAssets,
                .deleteAssetsRequest = std::move(chunkRequest),
            });
        }
        if (!queueLiveMainThreadApplyBatch("Delete Assets", std::move(operations))) {
            (void)deleteAssetsFromRegistry(*requests.deleteAssets);
        }
    }

    if (requests.cookProject.has_value()) {
        (void)startCookProject(*requests.cookProject);
    }

    if (requests.placeAsset.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::PlacePrefabAsset,
            .prefabGuid = *requests.placeAsset,
            .prefabPlacementTransform = requests.placeAssetTransform,
        });
        if (!queueLiveMainThreadApplyBatch("Place Prefab Asset", std::move(operations))) {
            (void)placePrefabAsset(*requests.placeAsset, requests.placeAssetTransform);
        }
    }

    if (requests.mergeScene.has_value()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.push_back(LiveMainThreadApplyOperation{
            .kind = LiveMainThreadApplyOperationKind::MergeScene,
            .scenePath = *requests.mergeScene,
        });
        if (!queueLiveMainThreadApplyBatch("Merge Scene", std::move(operations))) {
            SceneLoadRequest request;
            request.mode = SceneLoadMode::MergeSceneIntoCurrent;
            request.sourcePath = *requests.mergeScene;
            if (project_.has_value()) {
                request.projectSnapshot = *project_;
            }
            (void)requestSceneLoad(std::move(request));
        }
    }

    if (!requests.mergeScenes.empty()) {
        std::vector<LiveMainThreadApplyOperation> operations;
        operations.reserve(requests.mergeScenes.size());
        for (const std::filesystem::path& scenePath : requests.mergeScenes) {
            operations.push_back(LiveMainThreadApplyOperation{
                .kind = LiveMainThreadApplyOperationKind::MergeScene,
                .scenePath = scenePath,
            });
        }
        if (!queueLiveMainThreadApplyBatch("Batch Merge Scenes", std::move(operations))) {
            queueMergeScenes(requests.mergeScenes);
        }
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
    applySceneWorldComponentsToDocumentSettings(sceneDocument_);
    applyRendererSettingsSafely(rendererSettingsFromDocument(sceneDocument_, pathTracer_->settings()), false);
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
            sceneUnsavedDirty_ = true;
            sceneDocument_.clearDirty();
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

bool Application::applyPendingSceneUpdate(bool allowResourceRebuild, bool interactiveLightPreview) {
    if (!pathTracer_ || !sceneDocument_.dirty()) {
        return false;
    }

    const auto applyStart = std::chrono::steady_clock::now();
    const SceneUpdateMask pendingMask = sceneDocument_.pendingUpdateMask();
    SceneUpdateRoute route = SceneUpdateRouter::route(pendingMask);
    const uint64_t topologyGeneration = route.requiresRendererRebuild ? ++topologyRouteGeneration_ : 0;
    const std::string routeKindName = sceneUpdateMaskName(pendingMask);
    const std::string routeActionName = sceneUpdateGpuActionMaskName(route.actionMask);
    auto recordRouteAndReturn = [&](bool result, std::string_view suffix = {}, std::string_view schedulerStatus = {}) {
        const auto applyEnd = std::chrono::steady_clock::now();
        const double cpuMs = std::chrono::duration<double, std::milli>(applyEnd - applyStart).count();
        std::string actionName = routeActionName;
        if (!suffix.empty()) {
            actionName += suffix;
        }
        if (pathTracer_ != nullptr) {
            pathTracer_->validationLog().recordSceneUpdateRoute(routeKindName, std::move(actionName), cpuMs);
            if (route.requiresRendererRebuild && topologyGeneration != 0 && !schedulerStatus.empty()) {
                pathTracer_->validationLog().recordSchedulerQueueEvent(
                    "GpuSceneBuild",
                    "TopologyRebuild",
                    std::string(schedulerStatus),
                    topologyGeneration,
                    cpuMs);
            }
        }
        return result;
    };
    if (route.actionMask == 0u) {
        sceneDocument_.clearDirty();
        return recordRouteAndReturn(true);
    }
    if (route.requiresRendererRebuild && activeProgressiveRuntimeLoadJob_.has_value()) {
        const ActiveProgressiveRuntimeLoadJob& streamingJob = *activeProgressiveRuntimeLoadJob_;
        if (streamingJob.state != ActiveProgressiveRuntimeLoadJob::State::FinalRebuild &&
            streamingJob.state != ActiveProgressiveRuntimeLoadJob::State::Done) {
            if (lastStreamingTopologyBlockLogSerial_ != streamingJob.serial) {
                lastStreamingTopologyBlockLogSerial_ = streamingJob.serial;
                std::cerr << "[streaming] topology rebuild blocked until progressive load completes (job "
                          << streamingJob.serial << ")\n";
            }
            return recordRouteAndReturn(false, "+StreamingDeferred", "deferred_streaming");
        }
    }
    if (!allowResourceRebuild &&
        route.requiresRendererRebuild) {
        return recordRouteAndReturn(false, "+Deferred", "deferred");
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
        gpuSceneAsset_ = std::move(sceneBuild.sceneAsset);
        gpuInstanceEntities_ = std::move(sceneBuild.instanceEntities);
        latestAnimatedGeometryStats_ = sceneBuild.animatedGeometry;
        latestGpuSkinningPlan_ = std::move(sceneBuild.gpuSkinningPlan);
        latestGpuSkinningJointMatrices_ = std::move(sceneBuild.gpuSkinningJointMatrices);
        latestGpuSkinningPreviousJointMatrices_ = std::move(sceneBuild.gpuSkinningPreviousJointMatrices);
        latestGpuSkinningSourceVertices_ = std::move(sceneBuild.gpuSkinningSourceVertices);
        latestGpuSkinningMorphDeltas_ = std::move(sceneBuild.gpuSkinningMorphDeltas);
        preparePathTracerForRendererReplacement(previousSettings);
        const bool streamingFinalRebuild =
            activeProgressiveRuntimeLoadJob_.has_value() &&
            activeProgressiveRuntimeLoadJob_->state == ActiveProgressiveRuntimeLoadJob::State::FinalRebuild;
        const uint32_t materialTextureMaxDimension = streamingFinalRebuild
            ? streamingFinalRebuildMaterialTexturePreviewMaxDimension
            : 0u;
        if (materialTextureMaxDimension != 0u && gpuSceneAsset_.has_value() && !gpuSceneAsset_->textures.empty()) {
            std::cerr << "[streaming] final rebuild using material texture preview cap: max_dim="
                      << materialTextureMaxDimension
                      << " textures=" << gpuSceneAsset_->textures.size() << '\n';
        }
        std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
            currentSceneCachePolicyForRenderer(),
            &replacementSettings,
            materialTextureMaxDimension);
        retirePathTracer(std::move(pathTracer_));
        pathTracer_ = std::move(nextPathTracer);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(route.resetReason);
        commandSystem_->setPathTracer(pathTracer_.get());
    };

    auto syncDerivedSceneSettings = [&]() {
        applySceneWorldComponentsToDocumentSettings(sceneDocument_);
    };

    bool builtSceneCommitted = false;
    auto syncBuiltScene = [&]() {
        if (builtSceneCommitted) {
            return;
        }
        SceneGpuBuildResult& sceneBuild = ensureBuild();
        gpuSceneAsset_ = std::move(sceneBuild.sceneAsset);
        gpuInstanceEntities_ = std::move(sceneBuild.instanceEntities);
        latestAnimatedGeometryStats_ = sceneBuild.animatedGeometry;
        latestGpuSkinningPlan_ = std::move(sceneBuild.gpuSkinningPlan);
        latestGpuSkinningJointMatrices_ = std::move(sceneBuild.gpuSkinningJointMatrices);
        latestGpuSkinningPreviousJointMatrices_ = std::move(sceneBuild.gpuSkinningPreviousJointMatrices);
        latestGpuSkinningSourceVertices_ = std::move(sceneBuild.gpuSkinningSourceVertices);
        latestGpuSkinningMorphDeltas_ = std::move(sceneBuild.gpuSkinningMorphDeltas);
        builtSceneCommitted = true;
    };

    auto completeAfterRebuild = [&]() {
        sceneDocument_.clearDirty();
        notifications_.notify("Scene topology rebuilt", NotificationType::Info);
        return true;
    };

    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::RebuildTopology)) {
        if (!allowResourceRebuild) {
            return recordRouteAndReturn(false, "+Deferred", "deferred");
        }
        const uint64_t liveTicketId = editorTopologyRebuildTickets_.create(
            "Live topology rebuild",
            makeTopologyRebuildStagePlan("Live topology rebuild"));
        const uint64_t liveTicketGeneration = editorTopologyRebuildTickets_.latestGeneration();
        if (pathTracer_ != nullptr) {
            pathTracer_->validationLog().recordSchedulerQueueEvent(
                "GpuSceneBuild",
                "TopologyRebuildTicket",
                "live_ticket_queued",
                liveTicketGeneration,
                0.0);
        }
        rebuildRenderer();
        if (pathTracer_ != nullptr) {
            pathTracer_->validationLog().recordSchedulerQueueEvent(
                "GpuSceneBuild",
                "TopologyRebuildTicket",
                "live_ticket_queued",
                liveTicketGeneration,
                0.0);
        }
        const TopologyRebuildStepResult topologyResult = editorTopologyRebuildTickets_.stepFrame(TopologyRebuildFrameBudget{
            .maxCpuMs = 1000.0,
            .maxStages = 0,
        });
        if (pathTracer_ != nullptr) {
            pathTracer_->validationLog().recordSchedulerQueueEvent(
                "GpuSceneBuild",
                "TopologyRebuildTicket",
                topologyResult.ticketComplete ? "live_ticket_complete" : "live_ticket_building",
                liveTicketGeneration,
                topologyResult.consumedMs);
        }
        editorTopologyRebuildCompletedTimeline_ = editorTopologyRebuildTickets_.nextTimelineValue() > 0 ? editorTopologyRebuildTickets_.nextTimelineValue() - 1ull : 0ull;
        if (editorTopologyRebuildCompletedTimeline_ != 0 && editorTopologyRebuildTickets_.completeRetirementFence(editorTopologyRebuildCompletedTimeline_)) {
            if (pathTracer_ != nullptr) {
                pathTracer_->validationLog().recordSchedulerQueueEvent(
                    "RendererSwap",
                    "TopologyRebuildTicket",
                    "live_ticket_retirement_fence_complete",
                    liveTicketGeneration,
                    0.0);
                pathTracer_->validationLog().recordSchedulerQueueEvent(
                    "GpuSceneBuild",
                    "TopologyRebuildTicket",
                    "live_ticket_complete",
                    liveTicketGeneration,
                    0.0);
            }
        }
        (void)liveTicketId;
        return recordRouteAndReturn(completeAfterRebuild(), {}, "completed_sync");
    }

    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateCamera)) {
        applyActiveSceneCamera();
    }
    if (sceneUpdateRouteHasAction(route, SceneUpdateGpuAction::UpdateLights)) {
        syncBuiltScene();
        applyRendererSettingsSafely(ensureBuild().rendererSettings, allowResourceRebuild);
        (void)interactiveLightPreview;
        if (!pathTracer_->updateSceneLights(*gpuSceneAsset_, true)) {
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
        if (!latestGpuSkinningPlan_.empty()) {
            (void)pathTracer_->updateGpuSkinningJointPayloads(
                latestGpuSkinningJointMatrices_,
                latestGpuSkinningPreviousJointMatrices_);
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

bool Application::applyRendererSettingsFromEditor(const RendererSettings& settings, bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return false;
    }
    sceneUnsavedDirty_ = true;
    RendererSettings nextSettings = settings;
    applySceneWorldComponentsToRendererSettings(sceneDocument_, nextSettings);
    applyRendererSettingsSafely(nextSettings, allowRenderResolutionChange);
    return true;
}

bool Application::toggleDenoiserFromEditor(bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return false;
    }
    RendererSettings settings = pathTracer_->settings();
    settings.denoiserEnabled = !settings.denoiserEnabled;
    applyRendererSettingsSafely(settings, allowRenderResolutionChange);
    return true;
}

bool Application::toggleDebugViewFromEditor(bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return false;
    }
    RendererSettings settings = pathTracer_->settings();
    settings.debugView = nextDebugView(settings.debugView);
    applyRendererSettingsSafely(settings, allowRenderResolutionChange);
    return true;
}

bool Application::cycleIntermediateViewFromEditor(bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return false;
    }
    RendererSettings settings = pathTracer_->settings();
    constexpr int count = sizeof(intermediateViews) / sizeof(intermediateViews[0]);
    int idx = 0;
    for (int i = 0; i < count; ++i) {
        if (intermediateViews[i] == settings.debugView) {
            idx = (i + 1) % count;
            break;
        }
    }
    settings.debugView = intermediateViews[idx];
    applyRendererSettingsSafely(settings, allowRenderResolutionChange);
    return true;
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
        currentSceneCachePolicyForRenderer(),
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

void Application::initializeEditorTicketProbeQueues() {
    if (editorGpuUploadTickets_.snapshots().empty()) {
        const uint64_t uploadTicketId = editorGpuUploadTickets_.create(GpuUploadTicketDesc{
            .kind = GpuUploadResourceKind::Image,
            .label = "Editor Job Center GPU upload probe",
            .totalBytes = 48ull * 1024ull * 1024ull,
            .chunkBytes = 16ull * 1024ull * 1024ull,
        });
        (void)uploadTicketId;
    }

    if (editorMainThreadApplyTickets_.snapshots().empty()) {
        std::vector<MainThreadApplyOperationDesc> operations;
        operations.reserve(6);
        for (size_t i = 0; i < 6; ++i) {
            operations.push_back(MainThreadApplyOperationDesc{
                .kind = i % 2 == 0 ? MainThreadApplyOperationKind::TransformUpdate : MainThreadApplyOperationKind::DependencyRestore,
                .entity = 1000ull + static_cast<uint64_t>(i),
                .estimatedCostMs = 0.75,
                .label = "Editor Job Center apply probe operation",
            });
        }
        const uint64_t applyTicketId = editorMainThreadApplyTickets_.create("Editor Job Center main-thread apply probe", std::move(operations));
        (void)applyTicketId;
    }

    if (editorTopologyRebuildTickets_.snapshots().empty()) {
        const uint64_t rebuildTicketId = editorTopologyRebuildTickets_.create(
            "Editor Job Center topology rebuild probe",
            makeTopologyRebuildStagePlan("Editor Job Center topology"));
        (void)rebuildTicketId;
    }
}

void Application::stepEditorTicketProbeQueues() {
    auto recordTicketQueueEvent = [&](std::string queue, std::string job, std::string status, uint64_t generation, double cpuMs) {
        if (pathTracer_ != nullptr) {
            pathTracer_->validationLog().recordSchedulerQueueEvent(std::move(queue), std::move(job), std::move(status), generation, cpuMs);
        }
    };

    if (editorGpuUploadCompletedTimeline_ != 0) {
        if (editorGpuUploadTickets_.completeTimeline(editorGpuUploadCompletedTimeline_)) {
            recordTicketQueueEvent("GpuUploadSubmit", "GpuUploadTicket", "fence_complete", 0, 0.0);
        }
    }
    if (editorTopologyRebuildCompletedTimeline_ != 0) {
        if (editorTopologyRebuildTickets_.completeRetirementFence(editorTopologyRebuildCompletedTimeline_)) {
            recordTicketQueueEvent("RendererSwap", "TopologyRebuildTicket", "retirement_fence_complete", editorTopologyRebuildTickets_.latestGeneration(), 0.0);
        }
    }

    const GpuUploadSubmitResult uploadResult = editorGpuUploadTickets_.submitFrame(GpuUploadFrameBudget{
        .maxBytes = 16ull * 1024ull * 1024ull,
        .maxSubmissions = 1,
    });
    if (uploadResult.submittedChunks > 0) {
        recordTicketQueueEvent(
            "GpuUploadSubmit",
            "GpuUploadTicket",
            uploadResult.ticketComplete ? "complete" : (uploadResult.budgetExhausted ? "budget_exhausted" : "submitted"),
            0,
            0.0);
    }
    editorGpuUploadCompletedTimeline_ = editorGpuUploadTickets_.nextTimelineValue() > 0 ? editorGpuUploadTickets_.nextTimelineValue() - 1ull : 0ull;

    const MainThreadApplyStepResult applyResult = editorMainThreadApplyTickets_.applyFrame(MainThreadApplyFrameBudget{
        .maxApplyMs = 1.5,
        .maxOperations = 2,
    });
    executeLiveMainThreadApplyOperations(applyResult);
    if (applyResult.appliedOperations > 0) {
        recordTicketQueueEvent(
            "MainThreadApply",
            "MainThreadApplyTicket",
            applyResult.ticketComplete ? "complete" : (applyResult.budgetExhausted ? "budget_exhausted" : "applying"),
            0,
            applyResult.consumedMs);
    }

    const TopologyRebuildStepResult topologyResult = editorTopologyRebuildTickets_.stepFrame(TopologyRebuildFrameBudget{
        .maxCpuMs = 3.0,
        .maxStages = 2,
    });
    if (topologyResult.completedStages > 0) {
        recordTicketQueueEvent(
            "GpuSceneBuild",
            "TopologyRebuildTicket",
            topologyResult.ticketComplete ? "complete" : (topologyResult.budgetExhausted ? "budget_exhausted" : "building"),
            editorTopologyRebuildTickets_.latestGeneration(),
            topologyResult.consumedMs);
    }
    editorTopologyRebuildCompletedTimeline_ = editorTopologyRebuildTickets_.nextTimelineValue() > 0 ? editorTopologyRebuildTickets_.nextTimelineValue() - 1ull : 0ull;
}

void Application::shutdownStreamingRuntime() {
    std::cerr << "[shutdown] shutdownStreamingRuntime begin\n";

    if (activeProgressiveRuntimeLoadJob_.has_value()) {
        activeProgressiveRuntimeLoadJob_->cancelled = true;
        if (activeProgressiveRuntimeLoadJob_->future.valid()) {
            try {
                ProgressiveRuntimeLoadBatchResult unused = activeProgressiveRuntimeLoadJob_->future.get();
                std::cerr << "[shutdown] consumed batch result: ok=" << (unused.ok ? "yes" : "no")
                          << " loadedFiles=" << unused.loadedFiles
                          << " errors=" << unused.errors.size() << "\n";
            } catch (const std::exception& e) {
                std::cerr << "[shutdown] future.get() exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[shutdown] future.get() unknown exception\n";
            }
        }
        std::cerr << "[shutdown] progressive job cancelled: loaded="
                  << activeProgressiveRuntimeLoadJob_->loadedFiles
                  << "/" << activeProgressiveRuntimeLoadJob_->files.size()
                  << " batchInFlight=" << (activeProgressiveRuntimeLoadJob_->batchInFlight ? "yes" : "no") << "\n";
        activeProgressiveRuntimeLoadJob_.reset();
    }

    streamingGpuWorkQueue_ = StreamingGpuWorkQueue();
    streamingGpuSceneUpdateQueue_ = IncrementalGpuSceneUpdateQueue();
    lastStreamingGpuSceneSnapshots_.clear();
    streamingGpuWorkTimelineMarkers_.clear();
    streamingGpuBufferUploadPayloads_.clear();
    streamingGpuImageMipUploadPayloads_.clear();
    streamingGpuBlasBuildPayloads_.clear();
    streamingGpuBlasCompactionPayloads_.clear();
    streamingGpuWorkCompletedTimeline_ = 0;

    if (streamingGpuTransferExecutorReady_) {
        streamingGpuTransferExecutor_.shutdown();
        streamingGpuTransferExecutorReady_ = false;
    }

    nativeGpuAssetCache_.clear();

    std::cerr << "[shutdown] shutdownStreamingRuntime end\n";
}

void Application::stepStreamingGpuWorkQueue() {
    // Gate work-queue completion on real device-timeline progress when a live
    // transfer executor is available. The work queue remains the accounting
    // layer; live completion advances only after the executor's device timeline
    // marker signals.
    const bool liveDeviceAvailable = context_ != nullptr && allocator_ != nullptr;
    if (!streamingGpuTransferExecutorInitAttempted_ && liveDeviceAvailable) {
        streamingGpuTransferExecutorInitAttempted_ = true;
        const uint64_t stagingCapacity = std::max<uint64_t>(
            4ull * 1024ull * 1024ull, streamingOptions_.uploadBytesPerFrame * 4ull);
        streamingGpuTransferExecutorReady_ =
            streamingGpuTransferExecutor_.initialize(*context_, *allocator_, stagingCapacity);
        if (!streamingGpuTransferExecutorReady_) {
            streamingRuntimeState_.pushEvent("streaming GPU transfer executor initialization failed; GPU work tickets remain pending");
        }
    }

    if (streamingGpuTransferExecutorReady_) {
        const uint64_t completedMarker = streamingGpuTransferExecutor_.poll();
        uint64_t completedWorkQueueTimeline = streamingGpuWorkCompletedTimeline_;
        while (!streamingGpuWorkTimelineMarkers_.empty() &&
               streamingGpuWorkTimelineMarkers_.front().second <= completedMarker) {
            completedWorkQueueTimeline = streamingGpuWorkTimelineMarkers_.front().first;
            streamingGpuWorkTimelineMarkers_.pop_front();
        }
        streamingGpuWorkCompletedTimeline_ = completedWorkQueueTimeline;
    }
    const uint32_t retiredStreamingResources = nativeGpuAssetCache_.retireCompletedResources(streamingGpuWorkCompletedTimeline_);
    if (retiredStreamingResources > 0) {
        streamingRuntimeState_.pushEvent(
            "released fence-retired streaming GPU resources: " + std::to_string(retiredStreamingResources));
    }

    {
        if (streamingGpuWorkCompletedTimeline_ != 0) {
            (void)streamingGpuWorkQueue_.completeTimeline(streamingGpuWorkCompletedTimeline_);
        }
        const std::vector<StreamingGpuWorkSnapshot> snapshots = streamingGpuWorkQueue_.snapshots();
        std::unordered_map<AssetGuid, bool> ownerComplete;
        std::unordered_map<AssetGuid, bool> ownerFailed;
        std::unordered_map<AssetGuid, bool> ownerPayloadBacked;
        std::unordered_map<AssetGuid, NativeGpuAssetSnapshot> nativeSnapshotByGuid;
        for (const NativeGpuAssetSnapshot& snapshot : nativeGpuAssetCache_.snapshots()) {
            if (!snapshot.guid.empty()) {
                nativeSnapshotByGuid[snapshot.guid] = snapshot;
            }
        }
        std::unordered_set<AssetGuid> ownerDeferredForCompaction;
        for (const StreamingGpuWorkSnapshot& ticket : snapshots) {
            if (ticket.ownerGuid.empty()) {
                continue;
            }
            if (ticket.payloadBacked &&
                ticket.kind == StreamingGpuWorkKind::ImageMipUpload &&
                ticket.state == StreamingGpuWorkState::Complete &&
                ticket.textureMipLevel != UINT32_MAX) {
                (void)nativeGpuAssetCache_.markTextureMipResident(ticket.ownerGuid, ticket.textureMipLevel);
            }
            if (ticket.payloadBacked &&
                ticket.kind == StreamingGpuWorkKind::DescriptorUpdate &&
                ticket.state == StreamingGpuWorkState::Complete) {
                (void)nativeGpuAssetCache_.markDescriptorPatchComplete(ticket.ownerGuid);
            }
            if (ticket.payloadBacked &&
                ticket.kind == StreamingGpuWorkKind::BlasBuild &&
                ticket.state == StreamingGpuWorkState::Complete) {
                bool compactionQueued = false;
                const auto nativeIt = nativeSnapshotByGuid.find(ticket.ownerGuid);
                const bool compactionAlreadyHandled = nativeIt != nativeSnapshotByGuid.end() &&
                    (nativeIt->second.blasReady ||
                     nativeIt->second.blasCompactionPending ||
                     nativeIt->second.blasCompacted);
                AccelerationStructure* blas = nativeGpuAssetCache_.blasResource(ticket.ownerGuid);
                const uint64_t compactedSize =
                    streamingGpuTransferExecutorReady_ && blas != nullptr
                    ? streamingGpuTransferExecutor_.consumeCompactedBlasSize(*blas)
                    : 0ull;
                if (!compactionAlreadyHandled &&
                    compactedSize != 0 &&
                    blas != nullptr &&
                    compactedSize < static_cast<uint64_t>(blas->size()) &&
                    context_ != nullptr &&
                    allocator_ != nullptr) {
                    const std::string compactDebugName = "streaming compacted BLAS " + ticket.ownerGuid;
                    try {
                        compactionQueued = nativeGpuAssetCache_.ensurePendingCompactedBlasResource(
                            context_->device(),
                            *allocator_,
                            ticket.ownerGuid,
                            compactedSize,
                            compactDebugName.c_str());
                    } catch (const std::exception& e) {
                        streamingRuntimeState_.pushEvent("streaming BLAS compaction allocation failed for " + ticket.ownerGuid + ": " + e.what());
                        compactionQueued = false;
                    }
                    if (compactionQueued) {
                        const uint64_t compactionTicket = streamingGpuWorkQueue_.enqueue(StreamingGpuWorkDesc{
                            .kind = StreamingGpuWorkKind::BlasCompaction,
                            .label = "compact streamed mesh BLAS " + ticket.ownerGuid,
                            .ownerGuid = ticket.ownerGuid,
                            .estimatedGpuMs = 0.20,
                            .blasBuilds = 1,
                            .payloadBacked = true,
                        });
                        streamingGpuBlasCompactionPayloads_[compactionTicket] = ticket.ownerGuid;
                        (void)nativeGpuAssetCache_.markBlasCompactionQueued(ticket.ownerGuid, compactionTicket);
                        ownerDeferredForCompaction.insert(ticket.ownerGuid);
                    }
                }
                if (!compactionQueued && !compactionAlreadyHandled) {
                    (void)nativeGpuAssetCache_.markBlasReady(ticket.ownerGuid);
                }
            }
            if (ticket.payloadBacked &&
                ticket.kind == StreamingGpuWorkKind::BlasCompaction &&
                ticket.state == StreamingGpuWorkState::Complete) {
                if (!nativeGpuAssetCache_.markBlasCompacted(ticket.ownerGuid, streamingGpuWorkCompletedTimeline_)) {
                    (void)nativeGpuAssetCache_.markBlasReady(ticket.ownerGuid);
                }
            }
            if (ticket.payloadBacked &&
                ticket.kind == StreamingGpuWorkKind::TlasPatch &&
                ticket.state == StreamingGpuWorkState::Complete) {
                (void)nativeGpuAssetCache_.markTlasVisible(ticket.ownerGuid);
            }
            auto completeIt = ownerComplete.find(ticket.ownerGuid);
            if (completeIt == ownerComplete.end()) {
                completeIt = ownerComplete.emplace(ticket.ownerGuid, true).first;
            }
            if (ticket.state != StreamingGpuWorkState::Complete) {
                completeIt->second = false;
            }
            if (ownerDeferredForCompaction.find(ticket.ownerGuid) != ownerDeferredForCompaction.end()) {
                completeIt->second = false;
            }
            auto payloadBackedIt = ownerPayloadBacked.find(ticket.ownerGuid);
            if (payloadBackedIt == ownerPayloadBacked.end()) {
                payloadBackedIt = ownerPayloadBacked.emplace(ticket.ownerGuid, true).first;
            }
            if (!ticket.payloadBacked) {
                payloadBackedIt->second = false;
            }
            if (ticket.state == StreamingGpuWorkState::Cancelled || ticket.state == StreamingGpuWorkState::Failed) {
                ownerFailed[ticket.ownerGuid] = true;
                streamingGpuBufferUploadPayloads_.erase(ticket.id);
                streamingGpuImageMipUploadPayloads_.erase(ticket.id);
                streamingGpuBlasBuildPayloads_.erase(ticket.id);
                streamingGpuBlasCompactionPayloads_.erase(ticket.id);
            }
        }
        for (const auto& [guid, complete] : ownerComplete) {
            const bool payloadBacked = ownerPayloadBacked[guid];
            const auto nativeIt = nativeSnapshotByGuid.find(guid);
            const bool rendererVisibleComplete = nativeIt != nativeSnapshotByGuid.end() &&
                (nativeIt->second.tlasVisible ||
                 nativeIt->second.descriptorPatchComplete ||
                 nativeIt->second.residency == NativeGpuAssetResidency::Resident);
            if (ownerFailed[guid]) {
                streamingRuntimeState_.setAssetState(guid, StreamingAssetState::Failed, "streaming GPU work failed or was cancelled");
                (void)nativeGpuAssetCache_.markFailed(guid);
            } else if (complete && payloadBacked && streamingRuntimeState_.assetState(guid) != StreamingAssetState::GpuResident) {
                streamingRuntimeState_.setAssetState(guid, StreamingAssetState::GpuResident);
                (void)nativeGpuAssetCache_.markResident(guid);
                streamingRuntimeState_.pushEvent("streaming GPU work completed for " + guid);
            } else if (complete && !payloadBacked && rendererVisibleComplete) {
                if (streamingRuntimeState_.assetState(guid) != StreamingAssetState::GpuResident) {
                    streamingRuntimeState_.setAssetState(guid, StreamingAssetState::GpuResident);
                }
            } else if (complete && !payloadBacked) {
                if (streamingRuntimeState_.assetState(guid) != StreamingAssetState::Uploading) {
                    streamingRuntimeState_.setAssetState(guid, StreamingAssetState::Uploading);
                }
                if (!streamingGpuMarkerOnlyCompletionEventEmitted_) {
                    streamingRuntimeState_.pushEvent("streaming GPU timeline marker completed logical tickets; renderer-visible TLAS/descriptor patches must complete before assets become resident");
                    streamingGpuMarkerOnlyCompletionEventEmitted_ = true;
                }
            }
        }
    }

    const StreamingGpuWorkFrameResult result = streamingGpuWorkQueue_.submitFrame(StreamingGpuWorkBudget{
        .maxUploadBytes = std::max<uint64_t>(1ull * 1024ull * 1024ull, streamingOptions_.uploadBytesPerFrame),
        .maxStagingBytes = std::max<uint64_t>(2ull * 1024ull * 1024ull, streamingOptions_.uploadBytesPerFrame * 2ull),
        .maxGpuMs = 2.0,
        .maxSubmissions = 8,
        .maxBlasBuilds = 2,
        .maxTlasPatches = 4,
        .maxDescriptorUpdates = 512,
    });
    if (result.submittedTickets > 0) {
        streamingRuntimeState_.pushEvent("submitted streaming GPU work tickets: " + std::to_string(result.submittedTickets));
    }

    if (streamingGpuTransferExecutorReady_) {
        bool submittedPayloadWork = false;
        for (const StreamingGpuSubmittedTicket& ticket : result.submitted) {
            if (!ticket.payloadBacked) {
                continue;
            }
            if (ticket.kind == StreamingGpuWorkKind::BufferUpload) {
                auto payloadIt = streamingGpuBufferUploadPayloads_.find(ticket.id);
                Buffer* destination = payloadIt != streamingGpuBufferUploadPayloads_.end()
                    ? nativeGpuAssetCache_.bufferResource(payloadIt->second.ownerGuid)
                    : nullptr;
                if (payloadIt == streamingGpuBufferUploadPayloads_.end() ||
                    destination == nullptr ||
                    payloadIt->second.bytes.empty() ||
                    payloadIt->second.bytes.size() != ticket.bytes ||
                    !streamingGpuTransferExecutor_.stageBufferUpload(
                        *destination,
                        payloadIt->second.bytes.data(),
                        static_cast<uint64_t>(payloadIt->second.bytes.size()),
                        payloadIt->second.destinationOffset)) {
                    (void)streamingGpuWorkQueue_.fail(ticket.id);
                    streamingRuntimeState_.pushEvent("streaming GPU buffer payload staging failed for ticket " + std::to_string(ticket.id));
                    if (payloadIt != streamingGpuBufferUploadPayloads_.end()) {
                        streamingGpuBufferUploadPayloads_.erase(payloadIt);
                    }
                    continue;
                }
                submittedPayloadWork = true;
                streamingGpuBufferUploadPayloads_.erase(payloadIt);
            } else if (ticket.kind == StreamingGpuWorkKind::ImageMipUpload) {
                auto payloadIt = streamingGpuImageMipUploadPayloads_.find(ticket.id);
                Image* destination = payloadIt != streamingGpuImageMipUploadPayloads_.end()
                    ? nativeGpuAssetCache_.imageResource(payloadIt->second.ownerGuid)
                    : nullptr;
                if (payloadIt == streamingGpuImageMipUploadPayloads_.end() ||
                    destination == nullptr ||
                    payloadIt->second.bytes.empty() ||
                    payloadIt->second.bytes.size() != ticket.bytes ||
                    !streamingGpuTransferExecutor_.stageImageMipUpload(
                        *destination,
                        payloadIt->second.bytes.data(),
                        static_cast<uint64_t>(payloadIt->second.bytes.size()),
                        payloadIt->second.mipLevel,
                        payloadIt->second.width,
                        payloadIt->second.height)) {
                    (void)streamingGpuWorkQueue_.fail(ticket.id);
                    streamingRuntimeState_.pushEvent("streaming GPU image mip payload staging failed for ticket " + std::to_string(ticket.id));
                    if (payloadIt != streamingGpuImageMipUploadPayloads_.end()) {
                        streamingGpuImageMipUploadPayloads_.erase(payloadIt);
                    }
                    continue;
                }
                submittedPayloadWork = true;
                streamingGpuImageMipUploadPayloads_.erase(payloadIt);
            } else if (ticket.kind == StreamingGpuWorkKind::BlasBuild) {
                auto payloadIt = streamingGpuBlasBuildPayloads_.find(ticket.id);
                Buffer* meshBuffer = payloadIt != streamingGpuBlasBuildPayloads_.end()
                    ? nativeGpuAssetCache_.bufferResource(payloadIt->second.ownerGuid)
                    : nullptr;
                AccelerationStructure* blas = payloadIt != streamingGpuBlasBuildPayloads_.end()
                    ? nativeGpuAssetCache_.blasResource(payloadIt->second.ownerGuid)
                    : nullptr;
                Buffer* scratch = payloadIt != streamingGpuBlasBuildPayloads_.end()
                    ? nativeGpuAssetCache_.blasScratchResource(payloadIt->second.ownerGuid)
                    : nullptr;
                if (payloadIt == streamingGpuBlasBuildPayloads_.end() ||
                    meshBuffer == nullptr ||
                    blas == nullptr ||
                    scratch == nullptr ||
                    !streamingGpuTransferExecutor_.stageBlasBuild(StreamingGpuTransferExecutor::BlasTriangleBuild{
                        .destination = blas,
                        .scratch = scratch,
                        .vertexBuffer = meshBuffer,
                        .indexBuffer = meshBuffer,
                        .vertexDataOffset = 0,
                        .indexDataOffset = payloadIt->second.indexDataOffset,
                        .vertexCount = payloadIt->second.vertexCount,
                        .indexCount = payloadIt->second.indexCount,
                        .vertexStride = payloadIt->second.vertexStride,
                        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
                    })) {
                    (void)streamingGpuWorkQueue_.fail(ticket.id);
                    streamingRuntimeState_.pushEvent("streaming GPU BLAS payload staging failed for ticket " + std::to_string(ticket.id));
                    if (payloadIt != streamingGpuBlasBuildPayloads_.end()) {
                        streamingGpuBlasBuildPayloads_.erase(payloadIt);
                    }
                    continue;
                }
                submittedPayloadWork = true;
                streamingGpuBlasBuildPayloads_.erase(payloadIt);
            } else if (ticket.kind == StreamingGpuWorkKind::BlasCompaction) {
                auto payloadIt = streamingGpuBlasCompactionPayloads_.find(ticket.id);
                AccelerationStructure* source = payloadIt != streamingGpuBlasCompactionPayloads_.end()
                    ? nativeGpuAssetCache_.blasResource(payloadIt->second)
                    : nullptr;
                AccelerationStructure* destination = payloadIt != streamingGpuBlasCompactionPayloads_.end()
                    ? nativeGpuAssetCache_.pendingCompactedBlasResource(payloadIt->second)
                    : nullptr;
                if (payloadIt == streamingGpuBlasCompactionPayloads_.end() ||
                    source == nullptr ||
                    destination == nullptr ||
                    !streamingGpuTransferExecutor_.stageBlasCompaction(*source, *destination)) {
                    (void)streamingGpuWorkQueue_.fail(ticket.id);
                    streamingRuntimeState_.pushEvent("streaming GPU BLAS compaction staging failed for ticket " + std::to_string(ticket.id));
                    if (payloadIt != streamingGpuBlasCompactionPayloads_.end()) {
                        streamingGpuBlasCompactionPayloads_.erase(payloadIt);
                    }
                    continue;
                }
                submittedPayloadWork = true;
                streamingGpuBlasCompactionPayloads_.erase(payloadIt);
            }
        }
        const uint64_t workQueueTimeline = result.highestSubmittedTimeline;
        if (workQueueTimeline != 0 &&
            (streamingGpuWorkTimelineMarkers_.empty() ||
             streamingGpuWorkTimelineMarkers_.back().first < workQueueTimeline)) {
            const uint64_t marker = streamingGpuTransferExecutor_.submitTimelineMarker();
            if (marker != 0) {
                streamingGpuWorkTimelineMarkers_.emplace_back(workQueueTimeline, marker);
                if (submittedPayloadWork) {
                    streamingRuntimeState_.pushEvent("submitted payload-backed streaming GPU transfers through device timeline " + std::to_string(marker));
                }
            }
        }
    } else if (!liveDeviceAvailable) {
        // Offline/CPU-only paths keep the accounting simulation behavior.
        streamingGpuWorkCompletedTimeline_ = streamingGpuWorkQueue_.nextTimelineValue() > 0 ? streamingGpuWorkQueue_.nextTimelineValue() - 1ull : 0ull;
    } else {
        // Live Vulkan path failed to initialize its executor. Do not fake GPU
        // completion; tickets stay pending so diagnostics expose the failure.
        streamingGpuWorkCompletedTimeline_ = 0;
    }

    if (streamingOptions_.evictionEnabled) {
        lastStreamingEviction_ = nativeGpuAssetCache_.evictToBudget(NativeGpuAssetCacheBudget{
            .maxGpuBytes = streamingOptions_.gpuMemoryBudgetBytes,
            .maxCpuBytes = streamingOptions_.cpuMemoryBudgetBytes,
        }, streamingGpuWorkCompletedTimeline_);
        if (lastStreamingEviction_.evictedAssets > 0 || !lastStreamingEviction_.budgetMet) {
            streamingEvictionHistory_.emplace_back(frameSerial_, lastStreamingEviction_);
            while (streamingEvictionHistory_.size() > 128u) {
                streamingEvictionHistory_.pop_front();
            }
        }
        if (lastStreamingEviction_.evictedAssets > 0) {
            streamingRuntimeState_.pushEvent(
                "streaming cache eviction retired " + std::to_string(lastStreamingEviction_.evictedAssets) +
                " assets, pending release of " + std::to_string(lastStreamingEviction_.pendingRetiredGpuBytes) +
                " GPU resource bytes");
            for (const AssetGuid& guid : lastStreamingEviction_.evictedGuids) {
                streamingRuntimeState_.setAssetState(guid, StreamingAssetState::Evicted);
            }
        } else if (!lastStreamingEviction_.budgetMet) {
            streamingRuntimeState_.pushEvent("streaming cache remains over budget; no evictable assets were available");
        }
    }
}

void Application::stepStreamingGpuSceneUpdateQueue() {
    if (activeProgressiveRuntimeLoadJob_.has_value()) {
        const ActiveProgressiveRuntimeLoadJob::State state = activeProgressiveRuntimeLoadJob_->state;
        if (state != ActiveProgressiveRuntimeLoadJob::State::FinalRebuild &&
            state != ActiveProgressiveRuntimeLoadJob::State::Done) {
            lastStreamingGpuSceneApply_ = IncrementalGpuSceneApplyFrameResult{};
            return;
        }
    }

    GpuSceneStreamingState gpuSceneStreaming;
    gpuSceneStreaming.rebuild(sceneDocument_, gpuInstanceEntities_, &nativeGpuAssetCache_);
    const GpuSceneStreamingUpdatePlan updatePlan = buildGpuSceneStreamingUpdatePlan(
        lastStreamingGpuSceneSnapshots_,
        gpuSceneStreaming.instances());
    if (!updatePlan.entries.empty()) {
        streamingGpuSceneUpdateQueue_.enqueueUpdatePlan(updatePlan);
        if (updatePlan.becameRenderableInstances > 0 || updatePlan.becameNonRenderableInstances > 0) {
            streamingRuntimeState_.pushEvent(
                "queued incremental GpuScene updates: +" + std::to_string(updatePlan.becameRenderableInstances) +
                " renderable, -" + std::to_string(updatePlan.becameNonRenderableInstances) + " renderable");
        }
    }
    lastStreamingGpuSceneSnapshots_ = gpuSceneStreaming.instances();

    lastStreamingGpuSceneApply_ = streamingGpuSceneUpdateQueue_.applyFrame(IncrementalGpuSceneApplyBudget{
        .maxApplyMs = 1.0,
        .maxOperations = 16,
        .maxTlasPatches = 4,
        .maxDescriptorPatches = 64,
        .maxResetMasks = 64,
    });
    if (lastStreamingGpuSceneApply_.appliedOperations > 0) {
        if (pathTracer_ != nullptr) {
            auto uniqueAssetGuids = [](const std::vector<AssetGuid>& guids) {
                std::vector<AssetGuid> result;
                std::unordered_set<AssetGuid> seen;
                result.reserve(guids.size());
                seen.reserve(guids.size());
                for (const AssetGuid& guid : guids) {
                    if (!guid.empty() && seen.insert(guid).second) {
                        result.push_back(guid);
                    }
                }
                return result;
            };
            auto collectMaterialGuids = [&](const std::vector<uint64_t>& entityUuids) {
                std::unordered_set<uint64_t> requestedEntities;
                requestedEntities.reserve(entityUuids.size());
                for (uint64_t uuid : entityUuids) {
                    if (uuid != 0) {
                        requestedEntities.insert(uuid);
                    }
                }
                std::vector<AssetGuid> result;
                if (requestedEntities.empty()) {
                    return result;
                }
                std::unordered_set<AssetGuid> seen;
                for (const Entity* entity : sceneDocument_.registry().entities()) {
                    if (entity == nullptr ||
                        requestedEntities.find(entity->uuid) == requestedEntities.end() ||
                        !entity->meshRenderer.has_value()) {
                        continue;
                    }
                    for (const MaterialSlot& slot : entity->meshRenderer->materialSlots) {
                        const AssetGuid& guid = slot.overrideMaterialGuid.value_or(slot.materialGuid);
                        if (!guid.empty() && seen.insert(guid).second) {
                            result.push_back(guid);
                        }
                    }
                }
                return result;
            };
            std::unordered_map<uint64_t, AssetGuid> meshGuidByEntityUuid;
            meshGuidByEntityUuid.reserve(gpuSceneStreaming.instances().size());
            for (const GpuSceneStreamingInstanceSnapshot& snapshot : gpuSceneStreaming.instances()) {
                if (snapshot.entityUuid != 0 && !snapshot.meshGuid.empty()) {
                    meshGuidByEntityUuid[snapshot.entityUuid] = snapshot.meshGuid;
                }
            }
            auto meshGuidForEntityUuid = [&](uint64_t uuid) -> AssetGuid {
                const auto found = meshGuidByEntityUuid.find(uuid);
                return found != meshGuidByEntityUuid.end() ? found->second : AssetGuid{};
            };
            bool rebuiltStreamingGpuScene = false;
            auto ensureStreamingGpuSceneAsset = [&]() {
                if (!rebuiltStreamingGpuScene &&
                    (!gpuSceneAsset_.has_value() || gpuSceneAsset_->meshes.empty())) {
                    rebuildGpuSceneAsset();
                    rebuiltStreamingGpuScene = true;
                }
            };

            if (lastStreamingGpuSceneApply_.appliedDescriptorPatches > 0) {
                ensureStreamingGpuSceneAsset();
                const bool descriptorsPatched =
                    gpuSceneAsset_.has_value() &&
                    !gpuSceneAsset_->meshes.empty() &&
                    pathTracer_->updateMaterials(*gpuSceneAsset_, assets_);
                if (descriptorsPatched) {
                    for (const AssetGuid& guid : collectMaterialGuids(lastStreamingGpuSceneApply_.descriptorPatchEntityUuids)) {
                        (void)nativeGpuAssetCache_.markDescriptorPatchComplete(guid);
                        streamingRuntimeState_.setAssetState(guid, StreamingAssetState::GpuResident);
                    }
                    streamingRuntimeState_.pushEvent(
                        "patched streamed material descriptors: " +
                        std::to_string(lastStreamingGpuSceneApply_.appliedDescriptorPatches));
                } else {
                    streamingRuntimeState_.pushEvent("streamed material descriptor patch could not be applied to the live renderer");
                }
            }

            if (lastStreamingGpuSceneApply_.appliedTlasPatches > 0) {
                ensureStreamingGpuSceneAsset();
                const bool tlasPatched =
                    gpuSceneAsset_.has_value() &&
                    !gpuSceneAsset_->meshes.empty() &&
                    pathTracer_->updateSceneVisibility(*gpuSceneAsset_, assets_);
                if (tlasPatched) {
                    for (const AssetGuid& guid : uniqueAssetGuids(lastStreamingGpuSceneApply_.tlasPatchMeshGuids)) {
                        (void)nativeGpuAssetCache_.markTlasVisible(guid);
                        (void)nativeGpuAssetCache_.markResident(guid);
                        streamingRuntimeState_.setAssetState(guid, StreamingAssetState::GpuResident);
                    }
                    streamingRuntimeState_.pushEvent(
                        "patched streamed TLAS visibility: " +
                        std::to_string(lastStreamingGpuSceneApply_.appliedTlasPatches));
                } else {
                    streamingRuntimeState_.pushEvent("streamed TLAS patch could not be applied to the live renderer");
                }
            }

            std::unordered_map<uint64_t, uint32_t> instanceIndexByEntityUuid;
            instanceIndexByEntityUuid.reserve(gpuSceneStreaming.instances().size());
            for (const GpuSceneStreamingInstanceSnapshot& snapshot : gpuSceneStreaming.instances()) {
                if (snapshot.entityUuid != 0 && snapshot.gpuInstanceIndex != UINT32_MAX) {
                    instanceIndexByEntityUuid.emplace(snapshot.entityUuid, snapshot.gpuInstanceIndex);
                }
            }
            auto collectInstanceIndices = [&](const std::vector<uint64_t>& entityUuids) {
                std::vector<uint32_t> result;
                result.reserve(entityUuids.size());
                for (uint64_t uuid : entityUuids) {
                    const auto found = instanceIndexByEntityUuid.find(uuid);
                    if (found != instanceIndexByEntityUuid.end()) {
                        result.push_back(found->second);
                    }
                }
                return result;
            };
            std::vector<uint32_t> temporalInstanceIndices =
                collectInstanceIndices(lastStreamingGpuSceneApply_.temporalResetEntityUuids);
            std::vector<uint32_t> restirInstanceIndices =
                collectInstanceIndices(lastStreamingGpuSceneApply_.restirResetEntityUuids);
            pathTracer_->applyStreamingResetMasks(
                lastStreamingGpuSceneApply_.temporalResetEntityUuids,
                lastStreamingGpuSceneApply_.restirResetEntityUuids,
                temporalInstanceIndices,
                restirInstanceIndices);
        }
        streamingRuntimeState_.pushEvent(
            "applied incremental GpuScene update operations: " +
            std::to_string(lastStreamingGpuSceneApply_.appliedOperations));
    }
}

void Application::preparePathTracerForRendererReplacement(const RendererSettings& previousSettings) {
    if (pathTracer_ == nullptr || !rendererSettingsRequestDlss(previousSettings)) {
        return;
    }
    if (commandSystem_ != nullptr) {
        traceStartupPhase("renderer_replacement_wait_idle_begin");
        commandSystem_->waitIdle();
        traceStartupPhase("renderer_replacement_wait_idle_end");
    }
    traceStartupPhase("renderer_replacement_release_exclusive_begin");
    pathTracer_->releaseExclusiveRuntimeForRendererReplacement();
    traceStartupPhase("renderer_replacement_release_exclusive_end");
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
    const SceneCachePolicy policy = currentSceneCachePolicyForRenderer();
    return policy.path;
}

SceneCachePolicy Application::currentSceneCachePolicyForRenderer() const {
    if (!gltfPath_.has_value() || !gpuSceneAsset_.has_value() || gpuSceneAsset_->meshes.empty()) {
        return {};
    }
    if (sceneUnsavedDirty_ || !importedScene_.has_value()) {
        return {};
    }
    const bool geometrySignatureMatches =
        gpuSceneAsset_->meshes.size() == importedScene_->meshes.size() &&
        gpuSceneAsset_->materials.size() == importedScene_->materials.size() &&
        gpuSceneAsset_->textures.size() == importedScene_->textures.size();
    if (!geometrySignatureMatches) {
        return {};
    }
    const bool fullSceneSignatureMatches =
        sceneDocument_.prefabInstances().empty() &&
        gpuSceneAsset_->nodes.size() == importedScene_->nodes.size();
    return SceneCachePolicy{
        .mode = fullSceneSignatureMatches ? SceneCacheMode::FullReadWrite : SceneCacheMode::GeometryReadOnly,
        .path = SceneCache::cachePathFor(*gltfPath_),
    };
}

std::unique_ptr<PathTracerRenderer> Application::makePathTracer(
    const SceneAsset* sceneAsset,
    const AssetManager* assets,
    SceneCachePolicy sceneCachePolicy,
    const RendererSettings* settingsToRestore,
    uint32_t materialTextureMaxDimension) {
    const auto projectRoot = resolveProjectRoot();
    const auto shaderDir = projectRoot / "native" / "vulkan" / "shaders";
    const auto shaderOutDir = projectRoot / "native" / "vulkan" / "build" / "shaders";
    PathTracerRenderer::GpuSkinningResourcePlan skinningResourcePlan;
    if (sceneAsset != nullptr) {
        skinningResourcePlan.candidateInstanceCount = latestAnimatedGeometryStats_.gpuSkinningCandidateInstanceCount;
        skinningResourcePlan.dispatchRecordCount = latestAnimatedGeometryStats_.gpuSkinningDispatchRecordCount;
        skinningResourcePlan.jointMatrixCount = latestAnimatedGeometryStats_.gpuSkinningJointMatrixCount;
        skinningResourcePlan.jointUploadBytes = latestAnimatedGeometryStats_.gpuSkinningJointUploadBytes;
        skinningResourcePlan.previousJointUploadBytes = latestAnimatedGeometryStats_.gpuSkinningPreviousJointUploadBytes;
        skinningResourcePlan.sourceVertexUploadBytes = latestAnimatedGeometryStats_.gpuSkinningSourceVertexUploadBytes;
        skinningResourcePlan.morphDeltaUploadBytes = latestAnimatedGeometryStats_.gpuSkinningMorphDeltaUploadBytes;
        skinningResourcePlan.currentVertexBufferBytes = latestAnimatedGeometryStats_.gpuSkinningCurrentVertexBufferBytes;
        skinningResourcePlan.previousVertexBufferBytes = latestAnimatedGeometryStats_.gpuSkinningPreviousVertexBufferBytes;
        skinningResourcePlan.dispatchRecords.reserve(latestGpuSkinningPlan_.size());
        for (const GpuSkinningInstancePlan& plan : latestGpuSkinningPlan_) {
            skinningResourcePlan.dispatchRecords.push_back(PathTracerRenderer::GpuSkinningDispatchRecord{
                .meshHandleIndex = plan.meshHandleIndex,
                .vertexCount = plan.vertexCount,
                .jointMatrixOffset = plan.jointMatrixOffset,
                .sourceVertexOffset = plan.sourceVertexOffset,
                .currentVertexOffset = plan.currentVertexOffset,
                .previousVertexOffset = plan.previousVertexOffset,
                .morphDeltaOffset = plan.morphDeltaOffset,
                .morphDeltaCount = plan.morphDeltaCount,
                .morphWeight = plan.morphWeight,
                .morphBeforeSkinning = plan.morphBeforeSkinning,
            });
        }
        skinningResourcePlan.jointMatrices = latestGpuSkinningJointMatrices_;
        skinningResourcePlan.previousJointMatrices = latestGpuSkinningPreviousJointMatrices_;
        skinningResourcePlan.sourceVertices = latestGpuSkinningSourceVertices_;
        skinningResourcePlan.morphDeltas = latestGpuSkinningMorphDeltas_;
        skinningResourcePlan.cpuFallbackActive = latestAnimatedGeometryStats_.gpuSkinningCpuFallbackInstanceCount > 0;
    }
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
        std::move(sceneCachePolicy),
        !disableResourceAliasing_,
        skinningResourcePlan,
        settingsToRestore,
        materialTextureMaxDimension);
    traceStartupPhase("make_path_tracer_constructed");
    if (settingsToRestore != nullptr) {
        traceStartupPhase("make_path_tracer_apply_settings_begin");
        renderer->applySettings(*settingsToRestore);
        traceStartupPhase("make_path_tracer_apply_settings_end");
    }
    return renderer;
}

void Application::createPathTracer(const RendererSettings* settingsToRestore) {
    const SceneAsset* sceneAsset = gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr;
    pathTracer_ = makePathTracer(sceneAsset, sceneAsset != nullptr ? &assets_ : nullptr, currentSceneCachePolicyForRenderer(), settingsToRestore);
    if (sceneAsset == nullptr && gpuSceneAsset_.has_value() && !gpuSceneAsset_->lights.empty()) {
        (void)pathTracer_->updateSceneLights(*gpuSceneAsset_, true);
    }
}

void Application::initializeRendererFromCurrentScene(const RendererSettings* settingsToRestore) {
    if (pathTracer_ != nullptr) {
        return;
    }

    const auto startupBegin = std::chrono::steady_clock::now();
    auto lastStartupPhase = startupBegin;
    auto logStartupPhase = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        const double deltaMs = std::chrono::duration<double, std::milli>(now - lastStartupPhase).count();
        const double totalMs = std::chrono::duration<double, std::milli>(now - startupBegin).count();
        std::cout << "Application startup timing: " << name << ' ' << deltaMs
                  << " ms (total " << totalMs << " ms)\n";
        lastStartupPhase = now;
    };

    rebuildGpuSceneAsset();
    logStartupPhase("rebuild_gpu_scene_asset");
    RendererSettings startupSettings = settingsToRestore != nullptr ? *settingsToRestore : RendererSettings{};
    if (settingsToRestore == nullptr) {
        startupSettings.debugView = debugView_;
        startupSettings = rendererSettingsFromDocument(sceneDocument_, startupSettings);
    }
    logStartupPhase("resolve_startup_settings");
    createPathTracer(&startupSettings);
    logStartupPhase("create_path_tracer");
    syncDocumentRenderSettings(sceneDocument_, pathTracer_->settings());
    applyActiveSceneCamera();
    sceneDocument_.clearDirty();
    logStartupPhase("sync_camera_document");
    if (commandSystem_ != nullptr) {
        commandSystem_->setPathTracer(pathTracer_.get());
    }
    showMainWindowIfHidden();
    if (uiOverlay_ != nullptr) {
        uiOverlay_->invalidateViewportTexture();
    }
    logStartupPhase("attach_renderer_ui");
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
    latestAnimatedGeometryStats_ = build.animatedGeometry;
    latestGpuSkinningPlan_ = build.gpuSkinningPlan;
    latestGpuSkinningJointMatrices_ = build.gpuSkinningJointMatrices;
    latestGpuSkinningPreviousJointMatrices_ = build.gpuSkinningPreviousJointMatrices;
    latestGpuSkinningSourceVertices_ = build.gpuSkinningSourceVertices;
    latestGpuSkinningMorphDeltas_ = build.gpuSkinningMorphDeltas;
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

void Application::updateFrameWorkAccelerationStructureBudgetFeedback(const GpuFrameTimings& timings) {
    if (timings.dynamicBlasUpdateMs > 0.0f) {
        frameWorkScheduler_.setPreviousAccelerationStructureGpuMs(timings.dynamicBlasUpdateMs);
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
    if (rendererOnly_) {
        title << "Renderer Only - ";
    }
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



