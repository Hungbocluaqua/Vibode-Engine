#include "rtv/Application.h"
#include "rtv/AnimationController.h"
#include "rtv/AssetImport.h"
#include "rtv/AssetRegistry.h"
#include "rtv/DiagnosticTools.h"
#include "rtv/HeadlessDiagnostics.h"
#include "rtv/NativeAssetMigration.h"
#include "rtv/NativeAssetCooker.h"
#include "rtv/NativeAssetRuntimeLoader.h"
#include "rtv/NativeAssetStore.h"
#include "rtv/NativeBinaryIO.h"
#include "rtv/NativeGpuAssetCache.h"
#include "rtv/NativeTextureFormatPolicy.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/Project.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/RenderGraphDump.h"
#include "rtv/RenderGraph.h"
#include "rtv/RtpkgIO.h"
#include "rtv/RuntimeSkeleton.h"
#include "rtv/StreamingIoBackend.h"
#include "rtv/StreamingGpuWorkQueue.h"
#include "rtv/StreamingRuntime.h"
#include "rtv/StreamingScheduler.h"
#include "rtv/TextureLoader.h"
#include "rtv/GpuSceneStreamingState.h"
#include "rtv/IncrementalGpuSceneUpdateQueue.h"
#include "rtv/GpuProfiler.h"
#include "rtv/GpuUploadTicket.h"
#include "rtv/MainThreadApplyTicket.h"
#include "rtv/TopologyRebuildTicket.h"

#include <exception>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <ktx.h>

#ifdef RTV_HAS_RENDERDOC
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <renderdoc_app.h>
static RENDERDOC_API_1_6_0* rdocApi = nullptr;
static std::filesystem::path rdocCapturePath;
static uint32_t rdocCaptureFrame = 60;
static bool rdocCaptureRequested = false;
static std::optional<std::filesystem::path> rdocDllPathOverride;
static std::string rdocCaptureTemplate;

static std::optional<std::filesystem::path> renderDocEnvPath(const char* name) {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(result);
}

static void addRenderDocCandidate(std::vector<std::filesystem::path>& candidates, std::filesystem::path path) {
    if (path.empty()) {
        return;
    }
    if (path.filename() != "renderdoc.dll") {
        path /= "renderdoc.dll";
    }
    candidates.push_back(std::move(path));
}

static void addRenderDocEnvCandidate(std::vector<std::filesystem::path>& candidates, const char* name) {
    if (std::optional<std::filesystem::path> path = renderDocEnvPath(name)) {
        addRenderDocCandidate(candidates, *path);
        addRenderDocCandidate(candidates, *path / "api" / "app");
        addRenderDocCandidate(candidates, path->parent_path().parent_path());
    }
}

static HMODULE loadRenderDocDll() {
    if (HMODULE existing = GetModuleHandleA("renderdoc.dll")) {
        return existing;
    }

    std::vector<std::filesystem::path> candidates;
    if (rdocDllPathOverride.has_value()) {
        addRenderDocCandidate(candidates, *rdocDllPathOverride);
    }
    addRenderDocEnvCandidate(candidates, "RENDERDOC_DLL_PATH");
    addRenderDocEnvCandidate(candidates, "RENDERDOC_DIR");
    addRenderDocEnvCandidate(candidates, "RENDERDOC_SDK_DIR");
    addRenderDocCandidate(candidates, std::filesystem::current_path());
    if (std::optional<std::filesystem::path> programFiles = renderDocEnvPath("ProgramFiles")) {
        addRenderDocCandidate(candidates, *programFiles / "RenderDoc");
    }
    if (std::optional<std::filesystem::path> programFilesX86 = renderDocEnvPath("ProgramFiles(x86)")) {
        addRenderDocCandidate(candidates, *programFilesX86 / "RenderDoc");
    }

    for (const std::filesystem::path& candidate : candidates) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        if (HMODULE loaded = LoadLibraryA(candidate.string().c_str())) {
            std::cout << "Loaded RenderDoc DLL: " << candidate.string() << "\n";
            return loaded;
        }
    }

    if (HMODULE loaded = LoadLibraryA("renderdoc.dll")) {
        std::cout << "Loaded RenderDoc DLL from DLL search path\n";
        return loaded;
    }
    return nullptr;
}

static void initRenderDoc() {
    HMODULE mod = loadRenderDocDll();
    if (mod == nullptr) {
        std::cerr << "Warning: RenderDoc DLL not loaded. Set --renderdoc-dll <path>, RENDERDOC_DLL_PATH, or install RenderDoc.\n";
        return;
    }
    auto getApi = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
    if (getApi != nullptr) {
        getApi(eRENDERDOC_API_Version_1_6_0, (void**)&rdocApi);
    }
    if (rdocApi == nullptr) {
        std::cerr << "Warning: RenderDoc API not available.\n";
    }
}
#endif

namespace {

std::filesystem::path resolveProjectPath(const std::filesystem::path& root, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    return path.is_absolute() ? path : root / path;
}

std::string genericPathString(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string relativePathString(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (ec) {
        return genericPathString(path);
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return genericPathString(path);
        }
    }
    return genericPathString(relative);
}

std::string lowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

rtv::NativeAssetKind parseNativeAssetKindName(std::string value) {
    value = lowerAscii(std::move(value));
    if (value == "mesh" || value == "rtmesh") return rtv::NativeAssetKind::Mesh;
    if (value == "material" || value == "rtmaterial") return rtv::NativeAssetKind::Material;
    if (value == "texture" || value == "rttexture") return rtv::NativeAssetKind::Texture;
    if (value == "skeleton" || value == "rtskeleton") return rtv::NativeAssetKind::Skeleton;
    if (value == "animation" || value == "rtanim") return rtv::NativeAssetKind::Animation;
    if (value == "animation-controller" || value == "animcontroller" || value == "rtanimcontroller") return rtv::NativeAssetKind::AnimationController;
    if (value == "skeletal-mesh" || value == "rtskeletalmesh") return rtv::NativeAssetKind::SkeletalMesh;
    throw std::runtime_error("Unknown native asset kind: " + value);
}

uint32_t parseNativeFixtureTextureFormat(std::string value) {
    value = lowerAscii(std::move(value));
    if (value == "rgba8_srgb" || value == "r8g8b8a8_srgb") return static_cast<uint32_t>(VK_FORMAT_R8G8B8A8_SRGB);
    if (value == "rgba8_unorm" || value == "r8g8b8a8_unorm") return static_cast<uint32_t>(VK_FORMAT_R8G8B8A8_UNORM);
    if (value == "rgba32f" || value == "rgba32_sfloat" || value == "r32g32b32a32_sfloat") return static_cast<uint32_t>(VK_FORMAT_R32G32B32A32_SFLOAT);
    if (value == "rgba16f" || value == "rgba16_sfloat" || value == "r16g16b16a16_sfloat") return static_cast<uint32_t>(VK_FORMAT_R16G16B16A16_SFLOAT);
    if (value == "bc7_srgb" || value == "bc7_srgb_block") return static_cast<uint32_t>(VK_FORMAT_BC7_SRGB_BLOCK);
    if (value == "bc7_unorm" || value == "bc7_unorm_block") return static_cast<uint32_t>(VK_FORMAT_BC7_UNORM_BLOCK);
    if (value == "bc5_unorm" || value == "bc5_unorm_block") return static_cast<uint32_t>(VK_FORMAT_BC5_UNORM_BLOCK);
    if (value == "bc4_unorm" || value == "bc4_unorm_block") return static_cast<uint32_t>(VK_FORMAT_BC4_UNORM_BLOCK);
    return static_cast<uint32_t>(std::stoul(value));
}

std::filesystem::path canonicalForCookCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical.lexically_normal();
}

std::string cookCopySourceKey(const std::filesystem::path& path) {
    std::string key = canonicalForCookCompare(path).generic_string();
#ifdef _WIN32
    key = lowerAscii(std::move(key));
#endif
    return key;
}

bool cookReferenceScanFileCandidate(const std::filesystem::path& path) {
    const std::string filename = lowerAscii(path.filename().string());
    const std::string ext = lowerAscii(path.extension().string());
    if (ext == ".rtlevel" || ext == ".mscene" || ext == ".vproject" || ext == ".rtproject") {
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

void appendCookScanRoot(std::vector<std::filesystem::path>& roots, const std::filesystem::path& root) {
    if (root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }
    const std::filesystem::path canonical = canonicalForCookCompare(root);
    for (const std::filesystem::path& existing : roots) {
        if (canonicalForCookCompare(existing) == canonical) {
            return;
        }
    }
    roots.push_back(canonical);
}

std::string cookJsonPathChild(std::string parent, const std::string& child) {
    if (parent.empty()) {
        parent = "$";
    }
    return parent + "/" + child;
}

std::optional<nlohmann::json> readCookJsonFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    try {
        nlohmann::json json;
        file >> json;
        return json;
    } catch (...) {
        return std::nullopt;
    }
}

bool readBinaryFileForCook(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return file.good() || size == 0;
}

std::filesystem::path resolveCookSceneReferencePath(
    const std::filesystem::path& sceneFile,
    const std::filesystem::path& projectRoot,
    const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    if (path.is_absolute()) {
        return path;
    }
    const std::filesystem::path sceneRelative = sceneFile.parent_path() / path;
    std::error_code ec;
    if (std::filesystem::is_regular_file(sceneRelative, ec)) {
        return sceneRelative;
    }
    return projectRoot / path;
}

void appendCookSceneSublevelReference(
    const nlohmann::json& value,
    const std::filesystem::path& sceneFile,
    const std::filesystem::path& projectRoot,
    nlohmann::json& references) {
    if (!value.is_object()) {
        return;
    }
    const std::string scenePath = value.value("scenePath", std::string{});
    if (scenePath.empty()) {
        return;
    }
    const std::filesystem::path resolved = resolveCookSceneReferencePath(sceneFile, projectRoot, scenePath);
    references.push_back({
        {"sceneGuid", value.value("sceneGuid", std::string{})},
        {"scenePath", scenePath},
        {"resolvedPath", resolved.generic_string()},
    });
}

nlohmann::json collectCookSceneSublevelReferences(
    const nlohmann::json& sceneJson,
    const std::filesystem::path& sceneFile,
    const std::filesystem::path& projectRoot) {
    nlohmann::json references = nlohmann::json::array();
    if (sceneJson.contains("sublevels") && sceneJson["sublevels"].is_array()) {
        for (const nlohmann::json& item : sceneJson["sublevels"]) {
            appendCookSceneSublevelReference(item, sceneFile, projectRoot, references);
        }
    }
    if (sceneJson.contains("entities") && sceneJson["entities"].is_array()) {
        for (const nlohmann::json& entity : sceneJson["entities"]) {
            if (entity.is_object() && entity.contains("levelInstance")) {
                appendCookSceneSublevelReference(entity["levelInstance"], sceneFile, projectRoot, references);
            }
        }
    }
    return references;
}

void collectCookProjectReferenceScanFiles(
    const rtv::ProjectContext& project,
    nlohmann::json& checkedRoots,
    std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> roots;
    appendCookScanRoot(roots, project.contentRoot);
    appendCookScanRoot(roots, project.scenesRoot);

    for (const std::filesystem::path& root : roots) {
        checkedRoots.push_back(root.generic_string());
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            std::error_code entryError;
            if (entry.is_regular_file(entryError) && cookReferenceScanFileCandidate(entry.path())) {
                files.push_back(canonicalForCookCompare(entry.path()));
            }
        }
    }
    if (!project.projectFile.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(project.projectFile, ec) && cookReferenceScanFileCandidate(project.projectFile)) {
            files.push_back(canonicalForCookCompare(project.projectFile));
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
}

bool cookGuidFieldCanReferenceAsset(std::string_view lowerKey) {
    return lowerKey.find("guid") != std::string_view::npos &&
        lowerKey != "projectguid" &&
        lowerKey != "sceneguid";
}

void appendInvalidCookSavedGuidReferences(
    const nlohmann::json& value,
    const std::unordered_set<rtv::AssetGuid>& registryGuids,
    const std::filesystem::path& filePath,
    const std::string& jsonPath,
    std::string objectKey,
    nlohmann::json& invalidReferences) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            appendInvalidCookSavedGuidReferences(it.value(), registryGuids, filePath, cookJsonPathChild(jsonPath, it.key()), it.key(), invalidReferences);
        }
        return;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            appendInvalidCookSavedGuidReferences(value[i], registryGuids, filePath, cookJsonPathChild(jsonPath, std::to_string(i)), objectKey, invalidReferences);
        }
        return;
    }
    if (!value.is_string()) {
        return;
    }
    const std::string keyLower = lowerAscii(std::move(objectKey));
    if (!cookGuidFieldCanReferenceAsset(keyLower)) {
        return;
    }
    const std::string guid = value.get<std::string>();
    if (guid.empty() || registryGuids.find(guid) != registryGuids.end()) {
        return;
    }
    invalidReferences.push_back({
        {"severity", "error"},
        {"kind", "InvalidSavedProjectReference"},
        {"file", filePath.generic_string()},
        {"jsonPath", jsonPath.empty() ? "$" : jsonPath},
        {"field", keyLower},
        {"guid", guid},
        {"detail", "Saved project metadata contains an asset GUID field whose value is not present in the loaded asset registry."},
    });
}

std::filesystem::path cookDestinationForPath(
    const std::filesystem::path& source,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cookRoot,
    size_t externalIndex) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(source, projectRoot, ec);
    if (!ec) {
        bool escapesRoot = false;
        for (const auto& part : relative) {
            if (part == "..") {
                escapesRoot = true;
                break;
            }
        }
        if (!escapesRoot) {
            return cookRoot / relative;
        }
    }
    const std::string filename = source.filename().empty() ? std::string("payload") : source.filename().string();
    return cookRoot / "ExternalPayloads" / (std::to_string(externalIndex) + "_" + filename);
}

bool copyCookFile(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    nlohmann::json& copiedFiles,
    std::vector<std::string>& errors,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cookRoot,
    std::string_view role,
    std::string_view ownerGuid) {
    if (source.empty()) {
        return true;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        errors.push_back(std::string(role) + " missing for " + std::string(ownerGuid) + ": " + source.string());
        return false;
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        errors.push_back("Could not create cook directory: " + destination.parent_path().string() + " (" + ec.message() + ")");
        return false;
    }
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        errors.push_back("Could not copy " + std::string(role) + " for " + std::string(ownerGuid) + ": " + ec.message());
        return false;
    }
    const uintmax_t bytes = std::filesystem::file_size(destination, ec);
    copiedFiles.push_back({
        {"role", role},
        {"ownerGuid", ownerGuid},
        {"source", relativePathString(source, projectRoot)},
        {"output", relativePathString(destination, cookRoot)},
        {"bytes", ec ? 0ull : static_cast<uint64_t>(bytes)},
    });
    return true;
}

struct CookFileCopyPlan {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::string role;
    std::string ownerGuid;
};

struct GpuUploadTicketSimulationArgs {
    bool enabled = false;
    uint64_t totalBytes = 160ull * 1024ull * 1024ull;
    uint64_t chunkBytes = 32ull * 1024ull * 1024ull;
    uint64_t frameByteLimit = 64ull * 1024ull * 1024ull;
    bool cancelBeforeSubmit = false;
    bool cancelAfterSubmit = false;
};

struct MainThreadApplySimulationArgs {
    bool enabled = false;
    uint32_t operationCount = 120;
    double operationCostMs = 0.25;
    double frameBudgetMs = 2.0;
    uint32_t cancelAfterFrame = 0;
};

struct TopologyRebuildSimulationArgs {
    bool enabled = false;
    double stageCostMs = 1.0;
    double frameBudgetMs = 3.0;
    uint32_t newerEditFrame = 2;
};

struct NativeTextureFormatPolicySimulationArgs {
    bool enabled = false;
};

struct StreamingSchedulerSimulationArgs {
    bool enabled = false;
    uint32_t taskCount = 32;
    rtv::StreamingSchedulerBudget budget{};
    uint32_t cancelAfterFrame = 0;
};

struct NativeGpuAssetCacheSimulationArgs {
    bool enabled = false;
    uint32_t assetCount = 48;
    rtv::NativeGpuAssetCacheBudget budget{};
};

struct GpuSceneStreamingStateSimulationArgs {
    bool enabled = false;
    uint32_t instanceCount = 32;
};

struct IncrementalGpuSceneUpdateSimulationArgs {
    bool enabled = false;
    uint32_t instanceCount = 32;
    uint32_t cancelAfterFrame = 0;
    rtv::IncrementalGpuSceneApplyBudget budget{};
};

struct StreamingGpuWorkQueueSimulationArgs {
    bool enabled = false;
    uint32_t ticketCount = 36;
    uint32_t completeLagFrames = 1;
    rtv::StreamingGpuWorkBudget budget{};
};

bool parseGpuUploadTicketSimulationArg(std::string_view arg, int argc, char** argv, int& index, GpuUploadTicketSimulationArgs& args) {
    if (arg == "--simulate-gpu-upload-ticket") {
        args.enabled = true;
        return true;
    }
    if (arg == "--upload-total-bytes" && index + 1 < argc) {
        args.totalBytes = static_cast<uint64_t>(std::stoull(argv[++index]));
        return true;
    }
    if (arg == "--upload-chunk-bytes" && index + 1 < argc) {
        args.chunkBytes = static_cast<uint64_t>(std::stoull(argv[++index]));
        return true;
    }
    if (arg == "--upload-frame-byte-limit" && index + 1 < argc) {
        args.frameByteLimit = static_cast<uint64_t>(std::stoull(argv[++index]));
        return true;
    }
    if (arg == "--upload-cancel-before-submit") {
        args.cancelBeforeSubmit = true;
        return true;
    }
    if (arg == "--upload-cancel-after-submit") {
        args.cancelAfterSubmit = true;
        return true;
    }
    return false;
}

bool parseMainThreadApplySimulationArg(std::string_view arg, int argc, char** argv, int& index, MainThreadApplySimulationArgs& args) {
    if (arg == "--simulate-main-thread-apply") {
        args.enabled = true;
        return true;
    }
    if (arg == "--apply-operation-count" && index + 1 < argc) {
        args.operationCount = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--apply-operation-cost-ms" && index + 1 < argc) {
        args.operationCostMs = std::stod(argv[++index]);
        return true;
    }
    if (arg == "--apply-frame-budget-ms" && index + 1 < argc) {
        args.frameBudgetMs = std::stod(argv[++index]);
        return true;
    }
    if (arg == "--apply-cancel-after-frame" && index + 1 < argc) {
        args.cancelAfterFrame = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    return false;
}

bool parseTopologyRebuildSimulationArg(std::string_view arg, int argc, char** argv, int& index, TopologyRebuildSimulationArgs& args) {
    if (arg == "--simulate-topology-rebuild") {
        args.enabled = true;
        return true;
    }
    if (arg == "--topology-stage-cost-ms" && index + 1 < argc) {
        args.stageCostMs = std::stod(argv[++index]);
        return true;
    }
    if (arg == "--topology-frame-budget-ms" && index + 1 < argc) {
        args.frameBudgetMs = std::stod(argv[++index]);
        return true;
    }
    if (arg == "--topology-newer-edit-frame" && index + 1 < argc) {
        args.newerEditFrame = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    return false;
}

bool parseNativeTextureFormatPolicySimulationArg(std::string_view arg, int, char**, int&, NativeTextureFormatPolicySimulationArgs& args) {
    if (arg == "--simulate-native-texture-format-policy") {
        args.enabled = true;
        return true;
    }
    return false;
}

bool parseStreamingSchedulerSimulationArg(std::string_view arg, int argc, char** argv, int& index, StreamingSchedulerSimulationArgs& args) {
    if (arg == "--simulate-streaming-scheduler") {
        args.enabled = true;
        return true;
    }
    if (arg == "--streaming-scheduler-task-count" && index + 1 < argc) {
        args.taskCount = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--streaming-scheduler-max-tasks" && index + 1 < argc) {
        args.budget.maxTasksPerFrame = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--streaming-scheduler-cpu-ms" && index + 1 < argc) {
        args.budget.maxCpuMs = std::stod(argv[++index]);
        return true;
    }
    if (arg == "--streaming-scheduler-io-mb" && index + 1 < argc) {
        args.budget.maxIoBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-scheduler-upload-mb" && index + 1 < argc) {
        args.budget.maxUploadBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-scheduler-memory-mb" && index + 1 < argc) {
        args.budget.maxTransientMemoryBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-scheduler-cancel-after-frame" && index + 1 < argc) {
        args.cancelAfterFrame = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    return false;
}

bool parseNativeGpuAssetCacheSimulationArg(std::string_view arg, int argc, char** argv, int& index, NativeGpuAssetCacheSimulationArgs& args) {
    if (arg == "--simulate-native-gpu-cache") {
        args.enabled = true;
        return true;
    }
    if (arg == "--native-gpu-cache-assets" && index + 1 < argc) {
        args.assetCount = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--native-gpu-cache-gpu-budget-mb" && index + 1 < argc) {
        args.budget.maxGpuBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--native-gpu-cache-cpu-budget-mb" && index + 1 < argc) {
        args.budget.maxCpuBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--native-gpu-cache-evict-selected") {
        args.budget.allowSelectedEviction = true;
        return true;
    }
    if (arg == "--native-gpu-cache-evict-pinned") {
        args.budget.allowPinnedEviction = true;
        return true;
    }
    return false;
}

bool parseGpuSceneStreamingStateSimulationArg(std::string_view arg, int argc, char** argv, int& index, GpuSceneStreamingStateSimulationArgs& args) {
    if (arg == "--simulate-gpu-scene-streaming") {
        args.enabled = true;
        return true;
    }
    if (arg == "--gpu-scene-streaming-instances" && index + 1 < argc) {
        args.instanceCount = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    return false;
}

bool parseIncrementalGpuSceneUpdateSimulationArg(std::string_view arg, int argc, char** argv, int& index, IncrementalGpuSceneUpdateSimulationArgs& args) {
    if (arg == "--simulate-incremental-gpu-scene-update") {
        args.enabled = true;
        return true;
    }
    if (arg == "--incremental-gpu-scene-instances" && index + 1 < argc) {
        args.instanceCount = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--incremental-gpu-scene-ms" && index + 1 < argc) {
        args.budget.maxApplyMs = std::stod(argv[++index]);
        return true;
    }
    if (arg == "--incremental-gpu-scene-operations" && index + 1 < argc) {
        args.budget.maxOperations = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--incremental-gpu-scene-tlas-patches" && index + 1 < argc) {
        args.budget.maxTlasPatches = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--incremental-gpu-scene-descriptor-patches" && index + 1 < argc) {
        args.budget.maxDescriptorPatches = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--incremental-gpu-scene-reset-masks" && index + 1 < argc) {
        args.budget.maxResetMasks = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--incremental-gpu-scene-cancel-after-frame" && index + 1 < argc) {
        args.cancelAfterFrame = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    return false;
}

bool parseStreamingGpuWorkQueueSimulationArg(std::string_view arg, int argc, char** argv, int& index, StreamingGpuWorkQueueSimulationArgs& args) {
    if (arg == "--simulate-streaming-gpu-work") {
        args.enabled = true;
        return true;
    }
    if (arg == "--streaming-gpu-work-tickets" && index + 1 < argc) {
        args.ticketCount = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--streaming-gpu-work-upload-mb" && index + 1 < argc) {
        args.budget.maxUploadBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-gpu-work-staging-mb" && index + 1 < argc) {
        args.budget.maxStagingBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-gpu-work-ms" && index + 1 < argc) {
        args.budget.maxGpuMs = std::stod(argv[++index]);
        return true;
    }
    if (arg == "--streaming-gpu-work-submissions" && index + 1 < argc) {
        args.budget.maxSubmissions = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--streaming-gpu-work-blas-builds" && index + 1 < argc) {
        args.budget.maxBlasBuilds = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--streaming-gpu-work-tlas-patches" && index + 1 < argc) {
        args.budget.maxTlasPatches = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--streaming-gpu-work-descriptor-updates" && index + 1 < argc) {
        args.budget.maxDescriptorUpdates = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    if (arg == "--streaming-gpu-work-complete-lag-frames" && index + 1 < argc) {
        args.completeLagFrames = static_cast<uint32_t>(std::stoul(argv[++index]));
        return true;
    }
    return false;
}

bool parseNativePackageAnimationSelectionArg(
    std::string_view arg,
    int argc,
    char** argv,
    int& index,
    rtv::NativePackageAnimationSelection& selection) {
    if (arg == "--native-package-controller-guid" && index + 1 < argc) {
        selection.controllerGuid = argv[++index];
        return true;
    }
    if (arg == "--native-package-controller-path" && index + 1 < argc) {
        selection.controllerPath = std::filesystem::path(argv[++index]);
        return true;
    }
    if (arg == "--native-package-animation-entity" && index + 1 < argc) {
        selection.entityName = argv[++index];
        return true;
    }
    if (arg == "--native-package-animation-entity-uuid" && index + 1 < argc) {
        selection.entityUuid = static_cast<uint64_t>(std::stoull(argv[++index]));
        return true;
    }
    return false;
}

bool parseStreamingRuntimeArg(
    std::string_view arg,
    int argc,
    char** argv,
    int& index,
    rtv::StreamingRuntimeOptions& options,
    std::optional<std::filesystem::path>& dumpStreamingPath,
    std::optional<std::filesystem::path>& streamingValidationScenePath) {
    if (arg == "--streaming" && index + 1 < argc) {
        options.enabled = rtv::parseStreamingOnOff(argv[++index]);
        return true;
    }
    if ((arg == "--streaming-budget-preset" || arg == "--streaming-preset") && index + 1 < argc) {
        const std::string_view preset(argv[++index]);
        if (!rtv::applyStreamingBudgetPreset(preset, options)) {
            throw std::runtime_error("Unknown streaming budget preset: " + std::string(preset));
        }
        return true;
    }
    if (arg == "--streaming-budget-mb" && index + 1 < argc) {
        options.budgetPreset = "custom";
        options.budgetBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-upload-mb-per-frame" && index + 1 < argc) {
        options.budgetPreset = "custom";
        options.uploadBytesPerFrame = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-cpu-memory-mb" && index + 1 < argc) {
        options.budgetPreset = "custom";
        options.cpuMemoryBudgetBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        options.cpuBatchBytes = std::max<uint64_t>(16ull * 1024ull * 1024ull, options.cpuMemoryBudgetBytes / 2ull);
        return true;
    }
    if (arg == "--streaming-gpu-memory-mb" && index + 1 < argc) {
        options.budgetPreset = "custom";
        options.gpuMemoryBudgetBytes = static_cast<uint64_t>(std::stoull(argv[++index])) * 1024ull * 1024ull;
        return true;
    }
    if (arg == "--streaming-io-backend" && index + 1 < argc) {
        options.ioBackend = rtv::parseStreamingIoBackendKind(argv[++index]);
        options.directStorageEnabled = options.ioBackend == rtv::StreamingIoBackendKind::DirectStorage;
        return true;
    }
    if (arg == "--streaming-directstorage" && index + 1 < argc) {
        options.directStorageEnabled = rtv::parseStreamingOnOff(argv[++index]);
        if (options.directStorageEnabled) {
            options.ioBackend = rtv::StreamingIoBackendKind::DirectStorage;
        } else if (options.ioBackend == rtv::StreamingIoBackendKind::DirectStorage) {
            options.ioBackend = rtv::StreamingIoBackendKind::Win32;
        }
        return true;
    }
    if (arg == "--streaming-force-cpu-decompress" && index + 1 < argc) {
        options.forceCpuDecompress = rtv::parseStreamingOnOff(argv[++index]);
        return true;
    }
    if (arg == "--streaming-disable-eviction") {
        options.evictionEnabled = false;
        return true;
    }
    if (arg == "--dump-streaming" && index + 1 < argc) {
        dumpStreamingPath = std::filesystem::path(argv[++index]);
        return true;
    }
    if (arg == "--streaming-validation-scene" && index + 1 < argc) {
        streamingValidationScenePath = std::filesystem::path(argv[++index]);
        return true;
    }
    return false;
}

bool writeJsonFile(const std::filesystem::path& path, const nlohmann::json& json, std::string* error) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error != nullptr) {
                *error = "could not create directory: " + parent.string() + " (" + ec.message() + ")";
            }
            return false;
        }
    }
    std::ofstream file(path);
    if (!file.is_open()) {
        if (error != nullptr) {
            *error = "could not write file: " + path.string();
        }
        return false;
    }
    file << json.dump(2);
    return true;
}

nlohmann::json nativeTextureFormatSupportJson(const rtv::NativeTextureFormatSupport& support) {
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

rtv::NativeTextureFormatSupport nativeTextureFormatSupportFromJson(const nlohmann::json& json) {
    rtv::NativeTextureFormatSupport support = rtv::nativeTextureOfflineFallbackFormatSupport();
    if (!json.is_object()) {
        return support;
    }
    support.platformName = json.value("platformName", support.platformName);
    support.queriedFromVulkan = json.value("queriedFromVulkan", support.queriedFromVulkan);
    support.bc1SrgbSampled = json.value("bc1SrgbSampled", support.bc1SrgbSampled);
    support.bc1UnormSampled = json.value("bc1UnormSampled", support.bc1UnormSampled);
    support.bc3SrgbSampled = json.value("bc3SrgbSampled", support.bc3SrgbSampled);
    support.bc3UnormSampled = json.value("bc3UnormSampled", support.bc3UnormSampled);
    support.bc7SrgbSampled = json.value("bc7SrgbSampled", support.bc7SrgbSampled);
    support.bc7UnormSampled = json.value("bc7UnormSampled", support.bc7UnormSampled);
    support.bc5UnormSampled = json.value("bc5UnormSampled", support.bc5UnormSampled);
    support.bc4UnormSampled = json.value("bc4UnormSampled", support.bc4UnormSampled);
    support.bc6hUfloatSampled = json.value("bc6hUfloatSampled", support.bc6hUfloatSampled);
    support.bc6hSfloatSampled = json.value("bc6hSfloatSampled", support.bc6hSfloatSampled);
    support.rgba8SrgbSampled = json.value("rgba8SrgbSampled", support.rgba8SrgbSampled);
    support.rgba8UnormSampled = json.value("rgba8UnormSampled", support.rgba8UnormSampled);
    support.rgba16fSampled = json.value("rgba16fSampled", support.rgba16fSampled);
    return support;
}

std::vector<rtv::NativeTextureFormatSupport> nativeTextureFormatSupportListFromJson(const nlohmann::json& json) {
    std::vector<rtv::NativeTextureFormatSupport> profiles;
    auto appendProfile = [&](const nlohmann::json& item) {
        rtv::NativeTextureFormatSupport support = nativeTextureFormatSupportFromJson(item);
        if (support.platformName.empty()) {
            support.platformName = "package-texture-target-" + std::to_string(profiles.size());
        }
        profiles.push_back(std::move(support));
    };

    if (json.is_array()) {
        for (const nlohmann::json& item : json) {
            appendProfile(item);
        }
    } else if (json.is_object() && json.contains("targetSets") && json["targetSets"].is_array()) {
        for (const nlohmann::json& item : json["targetSets"]) {
            appendProfile(item);
        }
    } else if (json.is_object() && json.contains("profiles") && json["profiles"].is_array()) {
        for (const nlohmann::json& item : json["profiles"]) {
            appendProfile(item);
        }
    } else if (json.is_object()) {
        appendProfile(json);
    }
    return profiles;
}

nlohmann::json nativeTextureFormatSupportListJson(const std::vector<rtv::NativeTextureFormatSupport>& profiles) {
    nlohmann::json out = nlohmann::json::array();
    for (const rtv::NativeTextureFormatSupport& support : profiles) {
        out.push_back(nativeTextureFormatSupportJson(support));
    }
    return out;
}

nlohmann::json nativeTextureFormatSelectionJson(const rtv::NativeTextureFormatSelection& selection) {
    nlohmann::json candidates = nlohmann::json::array();
    for (const VkFormat candidate : selection.candidates) {
        candidates.push_back(rtv::nativeTextureFormatName(candidate));
    }
    return {
        {"role", rtv::nativeTextureRoleName(selection.role)},
        {"colorSpace", rtv::nativeTextureColorSpaceName(selection.colorSpace)},
        {"selectedFormat", rtv::nativeTextureFormatName(selection.selectedFormat)},
        {"selectedFormatValue", static_cast<uint32_t>(selection.selectedFormat)},
        {"supported", selection.supported},
        {"fallbackUsed", selection.fallbackUsed},
        {"blockCompressed", selection.blockCompressed},
        {"compressionFamily", selection.compressionFamily},
        {"reason", selection.reason},
        {"fallbackReason", selection.fallbackReason},
        {"candidates", candidates},
    };
}

nlohmann::json nativeTextureFormatPolicyProfileJson(std::string_view name, const rtv::NativeTextureFormatSupport& support) {
    struct RoleCase {
        rtv::NativeTextureRole role;
        rtv::NativeTextureColorSpace colorSpace;
    };
    const RoleCase cases[] = {
        {rtv::NativeTextureRole::BaseColor, rtv::NativeTextureColorSpace::Srgb},
        {rtv::NativeTextureRole::Emissive, rtv::NativeTextureColorSpace::Srgb},
        {rtv::NativeTextureRole::Normal, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::MetallicRoughness, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::Metallic, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::Roughness, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::Occlusion, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::Opacity, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::Height, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::Thickness, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::EnvironmentHdr, rtv::NativeTextureColorSpace::HdrLinear},
        {rtv::NativeTextureRole::Data, rtv::NativeTextureColorSpace::Linear},
        {rtv::NativeTextureRole::Unknown, rtv::NativeTextureColorSpace::Srgb},
    };
    nlohmann::json selections = nlohmann::json::array();
    for (const RoleCase& roleCase : cases) {
        selections.push_back(nativeTextureFormatSelectionJson(rtv::selectNativeTextureFormat(roleCase.role, roleCase.colorSpace, support)));
    }
    return {
        {"name", std::string(name)},
        {"support", nativeTextureFormatSupportJson(support)},
        {"selections", selections},
    };
}

int simulateNativeTextureFormatPolicyCommand(const std::filesystem::path& jsonOut) {
    const nlohmann::json summary = {
        {"schema", "NativeTextureFormatPolicySimulationV1"},
        {"ok", true},
        {"profiles", nlohmann::json::array({
            nativeTextureFormatPolicyProfileJson("all-bc-supported", rtv::nativeTextureAllBcFormatSupportForAudit()),
            nativeTextureFormatPolicyProfileJson("rgba-fallback-only", rtv::nativeTextureOfflineFallbackFormatSupport()),
        })},
        {"open", nlohmann::json::array({
            "broader production package texture coverage remains open beyond decoded RGBA8/HDR sidecars and initial source-backed KTX2/BasisU project-cook sidecars",
            "direct renderer native-store texture upload remains open"
        })},
    };
    if (!jsonOut.empty()) {
        std::string writeError;
        if (!writeJsonFile(jsonOut, summary, &writeError)) {
            std::cerr << "Could not write native texture format policy JSON: " << writeError << '\n';
            return 1;
        }
    } else {
        std::cout << summary.dump(2) << '\n';
    }
    return 0;
}

int emitBasisuKtx2FixtureCommand(const std::filesystem::path& outputPath, const std::filesystem::path& jsonOut) {
    if (outputPath.empty()) {
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path parent = outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return 1;
        }
    }

    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 4;
    std::array<uint8_t, kWidth * kHeight * 4u> pixels{};
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t offset = static_cast<size_t>((y * kWidth + x) * 4u);
            pixels[offset + 0] = static_cast<uint8_t>(32u + x * 45u);
            pixels[offset + 1] = static_cast<uint8_t>(48u + y * 41u);
            pixels[offset + 2] = static_cast<uint8_t>(96u + (x + y) * 17u);
            pixels[offset + 3] = 255u;
        }
    }

    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
    createInfo.baseWidth = kWidth;
    createInfo.baseHeight = kHeight;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 1;
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
    if (result != KTX_SUCCESS || texture == nullptr) {
        return 1;
    }

    auto destroyTexture = [&texture]() {
        if (texture != nullptr) {
            ktxTexture_Destroy(ktxTexture(texture));
            texture = nullptr;
        }
    };

    result = ktxTexture_SetImageFromMemory(ktxTexture(texture), 0, 0, 0, pixels.data(), static_cast<ktx_size_t>(pixels.size()));
    if (result != KTX_SUCCESS) {
        destroyTexture();
        return 1;
    }

    ktxBasisParams basisParams{};
    basisParams.structSize = sizeof(basisParams);
    basisParams.uastc = KTX_TRUE;
    basisParams.threadCount = 1;
    basisParams.uastcFlags = KTX_PACK_UASTC_LEVEL_FASTEST;
    basisParams.uastcRDONoMultithreading = KTX_TRUE;
    result = ktxTexture2_CompressBasisEx(texture, &basisParams);
    if (result != KTX_SUCCESS) {
        destroyTexture();
        return 1;
    }

    result = ktxTexture_WriteToNamedFile(ktxTexture(texture), outputPath.string().c_str());
    destroyTexture();
    if (result != KTX_SUCCESS) {
        return 1;
    }

    const nlohmann::json summary = {
        {"schema", "BasisuKtx2FixtureV1"},
        {"ok", true},
        {"path", outputPath.generic_string()},
        {"width", kWidth},
        {"height", kHeight},
        {"sourceVkFormat", "R8G8B8A8_SRGB"},
        {"supercompression", "BasisU/UASTC"},
    };
    if (!jsonOut.empty()) {
        std::string writeError;
        if (!writeJsonFile(jsonOut, summary, &writeError)) {
            return 1;
        }
    }
    return 0;
}

nlohmann::json gpuUploadChunkSnapshotJson(const rtv::GpuUploadChunkSnapshot& chunk) {
    return {
        {"index", chunk.index},
        {"offset", chunk.offset},
        {"bytes", chunk.bytes},
        {"timelineValue", chunk.timelineValue},
        {"state", rtv::gpuUploadChunkStateName(chunk.state)},
        {"stagingRetained", chunk.stagingRetained},
    };
}

nlohmann::json gpuUploadTicketSnapshotJson(const rtv::GpuUploadTicketSnapshot& ticket) {
    nlohmann::json chunks = nlohmann::json::array();
    for (const rtv::GpuUploadChunkSnapshot& chunk : ticket.chunks) {
        chunks.push_back(gpuUploadChunkSnapshotJson(chunk));
    }
    return {
        {"id", ticket.id},
        {"kind", rtv::gpuUploadResourceKindName(ticket.kind)},
        {"state", rtv::gpuUploadTicketStateName(ticket.state)},
        {"label", ticket.label},
        {"totalBytes", ticket.totalBytes},
        {"submittedBytes", ticket.submittedBytes},
        {"completedBytes", ticket.completedBytes},
        {"retainedStagingBytes", ticket.retainedStagingBytes},
        {"chunkCount", ticket.chunkCount},
        {"pendingChunks", ticket.pendingChunks},
        {"submittedChunks", ticket.submittedChunks},
        {"completedChunks", ticket.completedChunks},
        {"cancellationRequested", ticket.cancellationRequested},
        {"canCancel", ticket.canCancel},
        {"canRetire", ticket.canRetire},
        {"chunks", chunks},
    };
}

nlohmann::json gpuUploadSnapshotsJson(const std::vector<rtv::GpuUploadTicketSnapshot>& snapshots) {
    nlohmann::json out = nlohmann::json::array();
    for (const rtv::GpuUploadTicketSnapshot& snapshot : snapshots) {
        out.push_back(gpuUploadTicketSnapshotJson(snapshot));
    }
    return out;
}

bool gpuUploadTicketsCompleteOrCancelled(const std::vector<rtv::GpuUploadTicketSnapshot>& snapshots) {
    return std::all_of(snapshots.begin(), snapshots.end(), [](const rtv::GpuUploadTicketSnapshot& ticket) {
        return ticket.state == rtv::GpuUploadTicketState::Complete || ticket.state == rtv::GpuUploadTicketState::Cancelled;
    });
}

nlohmann::json mainThreadApplyOperationSnapshotJson(const rtv::MainThreadApplyOperationSnapshot& operation) {
    return {
        {"index", operation.index},
        {"kind", rtv::mainThreadApplyOperationKindName(operation.kind)},
        {"state", rtv::mainThreadApplyOperationStateName(operation.state)},
        {"entity", operation.entity},
        {"estimatedCostMs", operation.estimatedCostMs},
        {"label", operation.label},
    };
}

nlohmann::json mainThreadApplyTicketSnapshotJson(const rtv::MainThreadApplyTicketSnapshot& ticket) {
    nlohmann::json operations = nlohmann::json::array();
    for (const rtv::MainThreadApplyOperationSnapshot& operation : ticket.operations) {
        operations.push_back(mainThreadApplyOperationSnapshotJson(operation));
    }
    return {
        {"id", ticket.id},
        {"state", rtv::mainThreadApplyTicketStateName(ticket.state)},
        {"label", ticket.label},
        {"operationCount", ticket.operationCount},
        {"pendingOperations", ticket.pendingOperations},
        {"appliedOperations", ticket.appliedOperations},
        {"cancelledOperations", ticket.cancelledOperations},
        {"progress", ticket.progress},
        {"undoSnapshotOpen", ticket.undoSnapshotOpen},
        {"undoSnapshotCommitted", ticket.undoSnapshotCommitted},
        {"cancellationRequested", ticket.cancellationRequested},
        {"canCancel", ticket.canCancel},
        {"lockedEntities", ticket.lockedEntities},
        {"operations", operations},
    };
}

nlohmann::json mainThreadApplySnapshotsJson(const std::vector<rtv::MainThreadApplyTicketSnapshot>& snapshots) {
    nlohmann::json out = nlohmann::json::array();
    for (const rtv::MainThreadApplyTicketSnapshot& snapshot : snapshots) {
        out.push_back(mainThreadApplyTicketSnapshotJson(snapshot));
    }
    return out;
}

std::vector<rtv::MainThreadApplyOperationDesc> makeMainThreadApplySimulationOperations(uint32_t operationCount, double operationCostMs) {
    std::vector<rtv::MainThreadApplyOperationDesc> operations;
    operations.reserve(operationCount);
    constexpr rtv::MainThreadApplyOperationKind kinds[] = {
        rtv::MainThreadApplyOperationKind::EntityCreation,
        rtv::MainThreadApplyOperationKind::EntityDeletion,
        rtv::MainThreadApplyOperationKind::EntityStateUpdate,
        rtv::MainThreadApplyOperationKind::ComponentCreation,
        rtv::MainThreadApplyOperationKind::TransformUpdate,
        rtv::MainThreadApplyOperationKind::MaterialBinding,
        rtv::MainThreadApplyOperationKind::MeshBinding,
        rtv::MainThreadApplyOperationKind::DependencyRestore,
        rtv::MainThreadApplyOperationKind::SelectionHandoff,
    };
    constexpr size_t kindCount = sizeof(kinds) / sizeof(kinds[0]);
    for (uint32_t i = 0; i < operationCount; ++i) {
        operations.push_back({
            .kind = kinds[i % kindCount],
            .entity = 1000ull + static_cast<uint64_t>(i / kindCount),
            .estimatedCostMs = operationCostMs,
            .label = "apply operation " + std::to_string(i),
        });
    }
    return operations;
}

int simulateMainThreadApplyCommand(
    const std::filesystem::path& jsonOut,
    uint32_t operationCount,
    double operationCostMs,
    double frameBudgetMs,
    uint32_t cancelAfterFrame) {
    rtv::MainThreadApplyTicketQueue queue;
    const uint64_t ticketId = queue.create(
        "simulated prefab apply",
        makeMainThreadApplySimulationOperations(operationCount, operationCostMs));
    const bool conflictRejected = queue.create(
        "conflicting destructive edit",
        std::vector<rtv::MainThreadApplyOperationDesc>{{
            .kind = rtv::MainThreadApplyOperationKind::TransformUpdate,
            .entity = 1000,
            .estimatedCostMs = operationCostMs,
            .label = "conflicting transform edit",
        }}) == 0;

    nlohmann::json frames = nlohmann::json::array();
    const rtv::MainThreadApplyFrameBudget budget{.maxApplyMs = frameBudgetMs};
    for (uint32_t frame = 1; frame <= 256; ++frame) {
        const rtv::MainThreadApplyStepResult step = queue.applyFrame(budget);
        frames.push_back({
            {"frame", frame},
            {"phase", "apply"},
            {"appliedOperations", step.appliedOperations},
            {"consumedMs", step.consumedMs},
            {"budgetExhausted", step.budgetExhausted},
            {"tickets", mainThreadApplySnapshotsJson(queue.snapshots(false))},
        });
        if (cancelAfterFrame != 0 && frame == cancelAfterFrame) {
            const bool cancelled = queue.requestCancel(ticketId);
            frames.push_back({
                {"frame", frame},
                {"phase", "cancel"},
                {"cancelled", cancelled},
                {"tickets", mainThreadApplySnapshotsJson(queue.snapshots(false))},
            });
            break;
        }
        const std::vector<rtv::MainThreadApplyTicketSnapshot> snapshots = queue.snapshots(false);
        const bool allDone = std::all_of(snapshots.begin(), snapshots.end(), [](const rtv::MainThreadApplyTicketSnapshot& snapshot) {
            return snapshot.state == rtv::MainThreadApplyTicketState::Complete || snapshot.state == rtv::MainThreadApplyTicketState::Cancelled;
        });
        if (allDone) {
            break;
        }
    }

    const std::vector<rtv::MainThreadApplyTicketSnapshot> finalSnapshots = queue.snapshots(true);
    const nlohmann::json summary = {
        {"schema", "MainThreadApplySimulationV1"},
        {"ok", true},
        {"operationCount", operationCount},
        {"operationCostMs", operationCostMs},
        {"frameBudgetMs", frameBudgetMs},
        {"cancelAfterFrame", cancelAfterFrame},
        {"conflictRejected", conflictRejected},
        {"frameCount", frames.size()},
        {"frames", frames},
        {"finalTickets", mainThreadApplySnapshotsJson(finalSnapshots)},
    };

    if (!jsonOut.empty()) {
        std::string writeError;
        if (!writeJsonFile(jsonOut, summary, &writeError)) {
            std::cerr << "Could not write main-thread apply simulation JSON: " << writeError << '\n';
            return 1;
        }
    } else {
        std::cout << summary.dump(2) << '\n';
    }
    return 0;
}

nlohmann::json topologyRebuildStageSnapshotJson(const rtv::TopologyRebuildStageSnapshot& stage) {
    return {
        {"index", stage.index},
        {"stage", rtv::topologyRebuildStageName(stage.stage)},
        {"state", rtv::topologyRebuildStageStateName(stage.state)},
        {"estimatedCostMs", stage.estimatedCostMs},
        {"label", stage.label},
    };
}

nlohmann::json topologyRebuildTicketSnapshotJson(const rtv::TopologyRebuildTicketSnapshot& ticket) {
    nlohmann::json stages = nlohmann::json::array();
    for (const rtv::TopologyRebuildStageSnapshot& stage : ticket.stages) {
        stages.push_back(topologyRebuildStageSnapshotJson(stage));
    }
    return {
        {"id", ticket.id},
        {"generation", ticket.generation},
        {"state", rtv::topologyRebuildTicketStateName(ticket.state)},
        {"label", ticket.label},
        {"stageCount", ticket.stageCount},
        {"pendingStages", ticket.pendingStages},
        {"completedStages", ticket.completedStages},
        {"cancelledStages", ticket.cancelledStages},
        {"progress", ticket.progress},
        {"previousRendererVisible", ticket.previousRendererVisible},
        {"finalRendererSwapped", ticket.finalRendererSwapped},
        {"oldRendererRetained", ticket.oldRendererRetained},
        {"oldRendererRetired", ticket.oldRendererRetired},
        {"retirementTimelineValue", ticket.retirementTimelineValue},
        {"cancellationRequested", ticket.cancellationRequested},
        {"staleGeneration", ticket.staleGeneration},
        {"stages", stages},
    };
}

nlohmann::json topologyRebuildSnapshotsJson(const std::vector<rtv::TopologyRebuildTicketSnapshot>& snapshots) {
    nlohmann::json out = nlohmann::json::array();
    for (const rtv::TopologyRebuildTicketSnapshot& snapshot : snapshots) {
        out.push_back(topologyRebuildTicketSnapshotJson(snapshot));
    }
    return out;
}

std::vector<rtv::TopologyRebuildStageDesc> makeTopologyRebuildSimulationStages(double stageCostMs) {
    constexpr rtv::TopologyRebuildStage stages[] = {
        rtv::TopologyRebuildStage::CpuSceneExtraction,
        rtv::TopologyRebuildStage::GpuSceneBufferBuild,
        rtv::TopologyRebuildStage::BufferUploads,
        rtv::TopologyRebuildStage::TextureUploads,
        rtv::TopologyRebuildStage::BlasBuildBatch,
        rtv::TopologyRebuildStage::BlasBuildBatch,
        rtv::TopologyRebuildStage::BlasBuildBatch,
        rtv::TopologyRebuildStage::TlasBuildOrRefit,
        rtv::TopologyRebuildStage::RendererDescriptorUpdate,
        rtv::TopologyRebuildStage::FinalRendererSwap,
        rtv::TopologyRebuildStage::RetireOldRenderer,
    };
    std::vector<rtv::TopologyRebuildStageDesc> out;
    out.reserve(sizeof(stages) / sizeof(stages[0]));
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        out.push_back({
            .stage = stages[i],
            .estimatedCostMs = stageCostMs,
            .label = std::string(rtv::topologyRebuildStageName(stages[i])) + " " + std::to_string(i),
        });
    }
    return out;
}

int simulateTopologyRebuildCommand(
    const std::filesystem::path& jsonOut,
    double stageCostMs,
    double frameBudgetMs,
    uint32_t newerEditFrame) {
    rtv::TopologyRebuildTicketQueue queue;
    (void)queue.create("topology rebuild generation 1", makeTopologyRebuildSimulationStages(stageCostMs));
    const rtv::TopologyRebuildFrameBudget budget{.maxCpuMs = frameBudgetMs};
    nlohmann::json frames = nlohmann::json::array();
    bool newerEditCreated = false;
    bool fenceCompleted = false;

    for (uint32_t frame = 1; frame <= 64; ++frame) {
        if (!newerEditCreated && newerEditFrame != 0 && frame == newerEditFrame) {
            (void)queue.create("topology rebuild generation 2", makeTopologyRebuildSimulationStages(stageCostMs));
            newerEditCreated = true;
            frames.push_back({
                {"frame", frame},
                {"phase", "newer-topology-edit"},
                {"tickets", topologyRebuildSnapshotsJson(queue.snapshots(false))},
            });
        }

        const rtv::TopologyRebuildStepResult step = queue.stepFrame(budget);
        frames.push_back({
            {"frame", frame},
            {"phase", "build"},
            {"completedStages", step.completedStages},
            {"consumedMs", step.consumedMs},
            {"budgetExhausted", step.budgetExhausted},
            {"tickets", topologyRebuildSnapshotsJson(queue.snapshots(false))},
        });

        const std::vector<rtv::TopologyRebuildTicketSnapshot> snapshots = queue.snapshots(false);
        const bool waitingForRetirement = std::any_of(snapshots.begin(), snapshots.end(), [](const rtv::TopologyRebuildTicketSnapshot& ticket) {
            return ticket.state == rtv::TopologyRebuildTicketState::WaitingForRetirementFence;
        });
        if (waitingForRetirement && !fenceCompleted) {
            frames.push_back({
                {"frame", frame},
                {"phase", "before-retirement-fence"},
                {"tickets", topologyRebuildSnapshotsJson(queue.snapshots(false))},
            });
            (void)queue.completeRetirementFence(queue.nextTimelineValue() - 1u);
            fenceCompleted = true;
            frames.push_back({
                {"frame", frame},
                {"phase", "after-retirement-fence"},
                {"tickets", topologyRebuildSnapshotsJson(queue.snapshots(false))},
            });
        }

        const std::vector<rtv::TopologyRebuildTicketSnapshot> current = queue.snapshots(false);
        const bool allTerminal = std::all_of(current.begin(), current.end(), [](const rtv::TopologyRebuildTicketSnapshot& ticket) {
            return ticket.state == rtv::TopologyRebuildTicketState::Complete || ticket.state == rtv::TopologyRebuildTicketState::Cancelled;
        });
        if (allTerminal && newerEditCreated) {
            break;
        }
    }

    const nlohmann::json summary = {
        {"schema", "TopologyRebuildSimulationV1"},
        {"ok", true},
        {"stageCostMs", stageCostMs},
        {"frameBudgetMs", frameBudgetMs},
        {"newerEditFrame", newerEditFrame},
        {"latestGeneration", queue.latestGeneration()},
        {"frameCount", frames.size()},
        {"frames", frames},
        {"finalTickets", topologyRebuildSnapshotsJson(queue.snapshots(true))},
    };

    if (!jsonOut.empty()) {
        std::string writeError;
        if (!writeJsonFile(jsonOut, summary, &writeError)) {
            std::cerr << "Could not write topology rebuild simulation JSON: " << writeError << '\n';
            return 1;
        }
    } else {
        std::cout << summary.dump(2) << '\n';
    }
    return 0;
}

int simulateGpuUploadTicketCommand(
    const std::filesystem::path& jsonOut,
    uint64_t totalBytes,
    uint64_t chunkBytes,
    uint64_t frameByteLimit,
    bool cancelBeforeSubmit,
    bool cancelAfterSubmit) {
    rtv::GpuUploadTicketQueue queue;
    const uint64_t ticketId = queue.create({
        .kind = rtv::GpuUploadResourceKind::Image,
        .label = "simulated large texture upload",
        .totalBytes = totalBytes,
        .chunkBytes = chunkBytes,
    });
    const rtv::GpuUploadFrameBudget budget{.maxBytes = frameByteLimit};

    nlohmann::json frames = nlohmann::json::array();
    auto appendFrame = [&](uint32_t frameIndex, const char* phase, const rtv::GpuUploadSubmitResult& submit) {
        frames.push_back({
            {"frame", frameIndex},
            {"phase", phase},
            {"submittedBytes", submit.submittedBytes},
            {"submittedChunks", submit.submittedChunks},
            {"budgetExhausted", submit.budgetExhausted},
            {"tickets", gpuUploadSnapshotsJson(queue.snapshots(true))},
        });
    };

    if (cancelBeforeSubmit) {
        (void)queue.requestCancel(ticketId, "cancel before submit");
        const rtv::GpuUploadSubmitResult submit = queue.submitFrame(budget);
        appendFrame(0, "cancel-before-submit", submit);
    } else if (cancelAfterSubmit) {
        const rtv::GpuUploadSubmitResult submit = queue.submitFrame(budget);
        appendFrame(1, "submit-before-cancel", submit);
        (void)queue.requestCancel(ticketId, "cancel after submit");
        appendFrame(1, "cancel-after-submit-before-fence", {});
        (void)queue.completeTimeline(queue.nextTimelineValue() - 1u);
        appendFrame(2, "cancel-after-submit-after-fence", {});
    } else {
        uint64_t previousFrameTimeline = 0;
        for (uint32_t frame = 1; frame <= 64; ++frame) {
            if (previousFrameTimeline != 0) {
                (void)queue.completeTimeline(previousFrameTimeline);
            }
            const rtv::GpuUploadSubmitResult submit = queue.submitFrame(budget);
            appendFrame(frame, "submit", submit);
            previousFrameTimeline = queue.nextTimelineValue() - 1u;
            const std::vector<rtv::GpuUploadTicketSnapshot> snapshots = queue.snapshots(false);
            if (gpuUploadTicketsCompleteOrCancelled(snapshots)) {
                break;
            }
        }
        (void)queue.completeTimeline(queue.nextTimelineValue() - 1u);
        appendFrame(static_cast<uint32_t>(frames.size() + 1u), "final-fence-complete", {});
    }

    const std::vector<rtv::GpuUploadTicketSnapshot> finalSnapshots = queue.snapshots(true);
    const nlohmann::json summary = {
        {"schema", "GpuUploadTicketSimulationV1"},
        {"ok", true},
        {"totalBytes", totalBytes},
        {"chunkBytes", chunkBytes},
        {"frameByteLimit", frameByteLimit},
        {"cancelBeforeSubmit", cancelBeforeSubmit},
        {"cancelAfterSubmit", cancelAfterSubmit},
        {"frameCount", frames.size()},
        {"frames", frames},
        {"finalTickets", gpuUploadSnapshotsJson(finalSnapshots)},
    };

    if (!jsonOut.empty()) {
        std::string writeError;
        if (!writeJsonFile(jsonOut, summary, &writeError)) {
            std::cerr << "Could not write upload-ticket simulation JSON: " << writeError << '\n';
            return 1;
        }
    } else {
        std::cout << summary.dump(2) << '\n';
    }
    return 0;
}

void appendCookValidationIssue(
    nlohmann::json& array,
    std::string_view severity,
    std::string_view kind,
    const rtv::AssetRecord& record,
    std::string detail,
    const std::filesystem::path& path = {}) {
    nlohmann::json issue = {
        {"severity", severity},
        {"kind", kind},
        {"ownerGuid", record.guid},
        {"ownerDisplayName", record.displayName},
        {"ownerAssetType", rtv::assetTypeName(record.type)},
        {"detail", std::move(detail)},
    };
    if (!path.empty()) {
        issue["path"] = path.generic_string();
    }
    array.push_back(std::move(issue));
}

size_t countCookValidationSeverity(const std::vector<const nlohmann::json*>& arrays, std::string_view severity) {
    size_t count = 0;
    for (const nlohmann::json* array : arrays) {
        if (array == nullptr || !array->is_array()) {
            continue;
        }
        for (const nlohmann::json& item : *array) {
            if (item.is_object() && item.value("severity", std::string{}) == severity) {
                ++count;
            }
        }
    }
    return count;
}

bool supportedCookCoordinateConversion(std::string_view value) {
    return value == "None" || value == "glTF Y-Up to Engine" || value == "Z-Up to Engine";
}

bool supportedCookMaterialImportMode(std::string_view value) {
    return value == "ImportMaterials" || value == "MetadataOnly" || value == "SkipMaterials";
}

bool supportedCookTextureImportMode(std::string_view value) {
    return value == "ImportTextures" || value == "MetadataOnly" || value == "SkipTextures";
}

bool supportedCookTextureCompression(std::string_view value) {
    return value == "PreserveSource";
}

bool cookPackageNativePayloadCandidate(const std::filesystem::path& path) {
    const rtv::NativeAssetKind kind = rtv::nativeAssetKindFromExtension(path);
    return kind != rtv::NativeAssetKind::Unknown && kind != rtv::NativeAssetKind::Package;
}

struct CookGeneratedTexturePackageVariant {
    std::filesystem::path sourcePath;
    std::filesystem::path outputPath;
    std::string packagePath;
    std::string sourceKind = "native_payload";
    std::string guid;
    std::string targetSet;
    std::string role;
    std::string sourceFormat;
    std::string emittedFormat;
    std::string status = "skipped";
    std::string reason;
    uint64_t bytes = 0;
    bool generated = false;
};

nlohmann::json cookGeneratedTexturePackageVariantJson(
    const CookGeneratedTexturePackageVariant& variant,
    const std::filesystem::path& cookRoot) {
    return {
        {"source", relativePathString(variant.sourcePath, cookRoot)},
        {"output", relativePathString(variant.outputPath, cookRoot)},
        {"packagePath", variant.packagePath},
        {"sourceKind", variant.sourceKind},
        {"guid", variant.guid},
        {"targetSet", variant.targetSet},
        {"role", variant.role},
        {"sourceFormat", variant.sourceFormat},
        {"emittedFormat", variant.emittedFormat},
        {"status", variant.status},
        {"reason", variant.reason},
        {"bytes", variant.bytes},
        {"generated", variant.generated},
    };
}

std::string cookTextureVariantSuffix(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
        return ".bc7";
    case VK_FORMAT_BC5_UNORM_BLOCK:
        return ".bc5";
    case VK_FORMAT_BC4_UNORM_BLOCK:
        return ".bc4";
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return ".rgba16f";
    default:
        return {};
    }
}

struct CookTextureSourceMetadata {
    std::filesystem::path sourcePath;
    std::string displayName;
};

rtv::NativeTextureColorSpace cookTextureColorSpaceForLoadedTexture(const rtv::NativeRuntimeLoadedAsset& loaded) {
    switch (loaded.textureRole) {
    case rtv::NativeTextureRole::BaseColor:
    case rtv::NativeTextureRole::Emissive:
    case rtv::NativeTextureRole::SpecularColor:
    case rtv::NativeTextureRole::SheenColor:
        return rtv::NativeTextureColorSpace::Srgb;
    case rtv::NativeTextureRole::EnvironmentHdr:
        return rtv::NativeTextureColorSpace::HdrLinear;
    case rtv::NativeTextureRole::Normal:
    case rtv::NativeTextureRole::MetallicRoughness:
    case rtv::NativeTextureRole::Metallic:
    case rtv::NativeTextureRole::Roughness:
    case rtv::NativeTextureRole::Occlusion:
    case rtv::NativeTextureRole::Opacity:
    case rtv::NativeTextureRole::Height:
    case rtv::NativeTextureRole::Thickness:
    case rtv::NativeTextureRole::Data:
    case rtv::NativeTextureRole::Specular:
    case rtv::NativeTextureRole::Transmission:
    case rtv::NativeTextureRole::Clearcoat:
    case rtv::NativeTextureRole::ClearcoatRoughness:
    case rtv::NativeTextureRole::ClearcoatNormal:
    case rtv::NativeTextureRole::Sheen:
    case rtv::NativeTextureRole::SheenRoughness:
    case rtv::NativeTextureRole::Iridescence:
    case rtv::NativeTextureRole::IridescenceThickness:
    case rtv::NativeTextureRole::Anisotropy:
        return rtv::NativeTextureColorSpace::Linear;
    case rtv::NativeTextureRole::Unknown:
    default:
        return loaded.texture.srgb ? rtv::NativeTextureColorSpace::Srgb : rtv::NativeTextureColorSpace::Linear;
    }
}

rtv::TextureAsset cookTextureAssetFromData(
    const rtv::TextureData& data,
    const std::filesystem::path& sourcePath,
    std::string displayName,
    rtv::NativeTextureColorSpace colorSpace) {
    rtv::TextureAsset texture;
    texture.name = std::move(displayName);
    texture.sourcePath = sourcePath;
    texture.width = static_cast<uint32_t>(std::max(1, data.width));
    texture.height = static_cast<uint32_t>(std::max(1, data.height));
    texture.channels = 4;
    texture.sourceArrayLayers = data.sourceArrayLayers;
    texture.sourceDepth = data.sourceDepth;
    texture.sourceFaceCount = data.sourceFaceCount;
    texture.sourceIsCubemap = data.sourceIsCubemap;
    texture.mipLevels = std::max(1, data.mipLevels);
    texture.srgb = colorSpace == rtv::NativeTextureColorSpace::Srgb && !data.linearColorSpace;
    texture.linearColorSpace = colorSpace == rtv::NativeTextureColorSpace::HdrLinear || data.linearColorSpace;
    texture.isCompressed = data.isCompressed;
    texture.format = data.format;
    texture.compressedFormat = data.compressedFormat != VK_FORMAT_UNDEFINED
        ? data.compressedFormat
        : (data.isCompressed ? data.format : VK_FORMAT_UNDEFINED);
    texture.sourceContainerKind = data.sourceContainerKind;
    texture.nativePayloadSource = data.nativePayloadSource;
    texture.sourceContainerPreserved = data.sourceContainerPreserved;
    texture.sourceContainerTranscoded = data.sourceContainerTranscoded;
    texture.rgba8 = data.pixels;
    texture.mipData = data.mipData;
    return texture;
}

CookGeneratedTexturePackageVariant cookGeneratedTexturePackageVariant(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& cookRoot,
    const rtv::NativeTextureFormatSupport& targetSet,
    std::unordered_set<std::string>& emittedVariantKeys) {
    CookGeneratedTexturePackageVariant variant;
    variant.sourcePath = sourcePath;
    variant.targetSet = targetSet.platformName;

    rtv::NativeAssetRuntimeLoader loader;
    rtv::NativeRuntimeLoadOptions loadOptions;
    loadOptions.rejectUnsupportedTextureFormats = false;
    loadOptions.textureFormatSupport = targetSet;
    const rtv::NativeRuntimeLoadedAsset loaded = loader.loadStandalone(sourcePath, loadOptions);
    if (!loaded.errors.empty() || loaded.kind != rtv::NativeAssetKind::Texture) {
        variant.status = "skipped_invalid_texture";
        variant.reason = loaded.errors.empty() ? "Native payload is not a texture." : loaded.errors.front().message;
        return variant;
    }

    variant.guid = loaded.guid;
    variant.sourceFormat = rtv::nativeTextureFormatName(loaded.texturePayloadFormat);
    const bool decodedRgbaSource = loaded.texturePayloadFormat == VK_FORMAT_R8G8B8A8_SRGB || loaded.texturePayloadFormat == VK_FORMAT_R8G8B8A8_UNORM;
    const bool decodedHdrSource = loaded.texturePayloadFormat == VK_FORMAT_R32G32B32A32_SFLOAT || loaded.texturePayloadFormat == VK_FORMAT_R16G16B16A16_SFLOAT;
    if (loaded.texture.isCompressed || (!decodedRgbaSource && !decodedHdrSource)) {
        variant.status = "skipped_not_decoded_rgba_or_hdr";
        variant.reason = "Only decoded RGBA8 or HDR native texture payloads are eligible for automatic package sidecar emission.";
        return variant;
    }
    if (loaded.texture.rgba8.empty() || loaded.texture.width == 0 || loaded.texture.height == 0) {
        variant.status = "skipped_empty_payload";
        variant.reason = "Texture payload has no decoded RGBA data.";
        return variant;
    }
    if (loaded.guid.empty()) {
        variant.status = "skipped_missing_guid";
        variant.reason = "Texture payload has no native asset GUID.";
        return variant;
    }

    const rtv::NativeTextureColorSpace colorSpace = loaded.texture.srgb ? rtv::NativeTextureColorSpace::Srgb : rtv::NativeTextureColorSpace::Linear;
    const rtv::NativeTextureFormatSelection formatSelection = rtv::selectNativeTextureFormat(loaded.textureRole, colorSpace, targetSet);
    const std::string variantSuffix = cookTextureVariantSuffix(formatSelection.selectedFormat);
    variant.role = rtv::nativeTextureRoleName(loaded.textureRole);
    variant.emittedFormat = rtv::nativeTextureFormatName(formatSelection.selectedFormat);
    if (variantSuffix.empty() || (!formatSelection.blockCompressed && formatSelection.selectedFormat != VK_FORMAT_R16G16B16A16_SFLOAT)) {
        variant.status = "skipped_no_package_target";
        variant.reason = "Texture role does not select a supported BC4/BC5/BC7/RGBA16F package sidecar target.";
        return variant;
    }
    const std::string emittedVariantKey = loaded.guid + "|" + rtv::nativeTextureFormatName(formatSelection.selectedFormat);
    if (emittedVariantKeys.find(emittedVariantKey) != emittedVariantKeys.end()) {
        variant.status = "skipped_duplicate_target_format";
        variant.reason = "Another package texture target set already emitted the same GUID and payload format.";
        return variant;
    }

    const std::filesystem::path sourceRelative = std::filesystem::path(relativePathString(sourcePath, cookRoot));
    const std::filesystem::path variantRelative = std::filesystem::path("NativePackageVariants") /
        sourceRelative.parent_path() /
        (sourcePath.stem().string() + variantSuffix + sourcePath.extension().string());
    variant.outputPath = cookRoot / variantRelative;
    variant.packagePath = variantRelative.generic_string();

    std::vector<std::byte> sourceBytes;
    if (!readBinaryFileForCook(sourcePath, sourceBytes)) {
        variant.status = "failed_read_source";
        variant.reason = "Could not read copied native texture payload for variant source hashing.";
        return variant;
    }

    rtv::NativeAssetCookInput input;
    input.guid = loaded.guid;
    input.outputPath = variant.outputPath;
    input.sourcePath = sourcePath;
    input.displayName = sourcePath.stem().string() + variantSuffix + "_package_variant";
    input.sourceHash = rtv::nativeHashHex(rtv::nativeHashBytes(sourceBytes));
    input.importSettingsHash = rtv::nativeHashHex(rtv::nativeHashText(
        "rtpkg-auto-texture-sidecar-v4|" + variant.targetSet + "|" + variant.role + "|" + rtv::nativeTextureFormatName(formatSelection.selectedFormat) + "|" + variant.sourceFormat));

    const rtv::NativeAssetCooker cooker(targetSet);
    const rtv::NativeAssetCookResult result = cooker.cookTexture(input, loaded.texture, variant.role);
    variant.emittedFormat = result.emittedVkFormat;
    if (!result.success) {
        variant.status = "failed_cook";
        variant.reason = result.errors.empty() ? "Native texture package sidecar cook failed." : result.errors.front();
        return variant;
    }
    if (result.emittedVkFormat != rtv::nativeTextureFormatName(formatSelection.selectedFormat)) {
        variant.status = "skipped_no_selected_output";
        variant.reason = "Automatic sidecar cook did not realize the selected package texture payload.";
        return variant;
    }

    std::error_code ec;
    const uintmax_t fileBytes = std::filesystem::file_size(variant.outputPath, ec);
    variant.bytes = ec ? 0ull : static_cast<uint64_t>(fileBytes);
    variant.generated = true;
    variant.status = "generated";
    variant.reason = "Decoded texture was cooked into a same-GUID package texture sidecar.";
    emittedVariantKeys.insert(emittedVariantKey);
    return variant;
}

CookGeneratedTexturePackageVariant cookGeneratedKtx2TexturePackageVariant(
    const std::filesystem::path& nativeTexturePath,
    const CookTextureSourceMetadata& sourceMetadata,
    const std::filesystem::path& cookRoot,
    const rtv::NativeTextureFormatSupport& targetSet,
    std::unordered_set<std::string>& emittedVariantKeys) {
    CookGeneratedTexturePackageVariant variant;
    variant.sourcePath = sourceMetadata.sourcePath;
    variant.sourceKind = "ktx2_source";
    variant.targetSet = targetSet.platformName;

    if (sourceMetadata.sourcePath.empty() || rtv::detectCompressedTextureKind(sourceMetadata.sourcePath.string()) != rtv::CompressedTextureKind::Ktx2) {
        variant.status = "skipped_not_ktx2_source";
        variant.reason = "Texture asset source is not a KTX2 container.";
        return variant;
    }

    rtv::NativeAssetRuntimeLoader loader;
    rtv::NativeRuntimeLoadOptions loadOptions;
    loadOptions.rejectUnsupportedTextureFormats = false;
    loadOptions.textureFormatSupport = targetSet;
    const rtv::NativeRuntimeLoadedAsset loaded = loader.loadStandalone(nativeTexturePath, loadOptions);
    if (!loaded.errors.empty() || loaded.kind != rtv::NativeAssetKind::Texture) {
        variant.status = "skipped_invalid_texture";
        variant.reason = loaded.errors.empty() ? "Native payload is not a texture." : loaded.errors.front().message;
        return variant;
    }

    variant.guid = loaded.guid;
    variant.role = rtv::nativeTextureRoleName(loaded.textureRole);
    variant.sourceFormat = rtv::nativeTextureFormatName(loaded.texturePayloadFormat);
    if (loaded.guid.empty()) {
        variant.status = "skipped_missing_guid";
        variant.reason = "Texture payload has no native asset GUID.";
        return variant;
    }

    const rtv::NativeTextureColorSpace colorSpace = cookTextureColorSpaceForLoadedTexture(loaded);
    const rtv::NativeTextureFormatSelection formatSelection = rtv::selectNativeTextureFormat(loaded.textureRole, colorSpace, targetSet);
    const std::string variantSuffix = cookTextureVariantSuffix(formatSelection.selectedFormat);
    variant.emittedFormat = rtv::nativeTextureFormatName(formatSelection.selectedFormat);
    if (variantSuffix.empty() || (!formatSelection.blockCompressed && formatSelection.selectedFormat != VK_FORMAT_R16G16B16A16_SFLOAT)) {
        variant.status = "skipped_no_package_target";
        variant.reason = "Texture role does not select a supported BC4/BC5/BC7/RGBA16F package sidecar target.";
        return variant;
    }
    const std::string emittedVariantKey = loaded.guid + "|" + rtv::nativeTextureFormatName(formatSelection.selectedFormat);
    if (emittedVariantKeys.find(emittedVariantKey) != emittedVariantKeys.end()) {
        variant.status = "skipped_duplicate_target_format";
        variant.reason = "Another package texture target set already emitted the same GUID and payload format.";
        return variant;
    }

    rtv::TextureData textureData;
    try {
        textureData = rtv::TextureLoader::loadKtx2(
            sourceMetadata.sourcePath.string(),
            targetSet,
            loaded.textureRole,
            colorSpace);
    } catch (const std::exception& error) {
        variant.status = "failed_source_transcode";
        variant.reason = std::string("KTX2 source load/transcode failed: ") + error.what();
        return variant;
    }
    if (textureData.pixels.empty() || textureData.width <= 0 || textureData.height <= 0) {
        variant.status = "skipped_empty_payload";
        variant.reason = "KTX2 source produced no texture payload for the target set.";
        return variant;
    }
    if ((formatSelection.selectedFormat == VK_FORMAT_BC7_SRGB_BLOCK || formatSelection.selectedFormat == VK_FORMAT_BC7_UNORM_BLOCK) &&
        (textureData.format == VK_FORMAT_BC7_SRGB_BLOCK || textureData.format == VK_FORMAT_BC7_UNORM_BLOCK)) {
        textureData.format = formatSelection.selectedFormat;
        textureData.compressedFormat = formatSelection.selectedFormat;
    }

    const std::filesystem::path nativeRelative = std::filesystem::path(relativePathString(nativeTexturePath, cookRoot));
    const std::filesystem::path variantRelative = std::filesystem::path("NativePackageVariants") /
        nativeRelative.parent_path() /
        (nativeTexturePath.stem().string() + variantSuffix + nativeTexturePath.extension().string());
    variant.outputPath = cookRoot / variantRelative;
    variant.packagePath = variantRelative.generic_string();

    std::vector<std::byte> sourceBytes;
    if (!readBinaryFileForCook(sourceMetadata.sourcePath, sourceBytes)) {
        variant.status = "failed_read_source";
        variant.reason = "Could not read KTX2 source payload for variant source hashing.";
        return variant;
    }

    rtv::NativeAssetCookInput input;
    input.guid = loaded.guid;
    input.outputPath = variant.outputPath;
    input.sourcePath = sourceMetadata.sourcePath;
    input.displayName = (sourceMetadata.displayName.empty() ? nativeTexturePath.stem().string() : sourceMetadata.displayName) + variantSuffix + "_package_ktx2_variant";
    input.sourceHash = rtv::nativeHashHex(rtv::nativeHashBytes(sourceBytes));
    input.importSettingsHash = rtv::nativeHashHex(rtv::nativeHashText(
        "rtpkg-auto-ktx2-texture-sidecar-v1|" + variant.targetSet + "|" + variant.role + "|" + rtv::nativeTextureFormatName(formatSelection.selectedFormat) + "|" + rtv::nativeTextureFormatName(textureData.format)));

    rtv::TextureAsset texture = cookTextureAssetFromData(textureData, sourceMetadata.sourcePath, sourceMetadata.displayName, colorSpace);
    const rtv::NativeAssetCooker cooker(targetSet);
    const rtv::NativeAssetCookResult result = cooker.cookTexture(input, texture, variant.role);
    variant.emittedFormat = result.emittedVkFormat;
    if (!result.success) {
        variant.status = "failed_cook";
        variant.reason = result.errors.empty() ? "KTX2 native texture package sidecar cook failed." : result.errors.front();
        return variant;
    }
    if (result.emittedVkFormat != rtv::nativeTextureFormatName(formatSelection.selectedFormat)) {
        variant.status = "skipped_no_selected_output";
        variant.reason = "KTX2 sidecar cook did not realize the selected package texture payload.";
        return variant;
    }

    std::error_code ec;
    const uintmax_t fileBytes = std::filesystem::file_size(variant.outputPath, ec);
    variant.bytes = ec ? 0ull : static_cast<uint64_t>(fileBytes);
    variant.generated = true;
    variant.status = "generated";
    variant.reason = "KTX2 source was cooked into a same-GUID package texture sidecar for the target set.";
    emittedVariantKeys.insert(emittedVariantKey);
    return variant;
}

std::string cookPackageBaseName(const rtv::ProjectContext& project) {
    std::string name = project.name.empty() ? project.projectFile.stem().string() : project.name;
    for (char& ch : name) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    return name.empty() ? std::string("Project") : name;
}

std::vector<rtv::NativeTextureFormatSupport> activeNativePackageTextureTargetSets(
    const std::vector<rtv::NativeTextureFormatSupport>& requestedTargetSets) {
    if (!requestedTargetSets.empty()) {
        return requestedTargetSets;
    }
    return {rtv::nativeTextureAllBcFormatSupportForAudit()};
}

int cookProjectCommand(
    const std::filesystem::path& projectFile,
    std::filesystem::path outputDir,
    std::filesystem::path manifestPath,
    const rtv::NativeTextureFormatSupport& textureFormatSupport,
    const std::vector<rtv::NativeTextureFormatSupport>& requestedPackageTextureTargetSets) {
    const std::vector<rtv::NativeTextureFormatSupport> packageTextureTargetSets = activeNativePackageTextureTargetSets(requestedPackageTextureTargetSets);
    rtv::ProjectContext project;
    std::string projectError;
    if (!rtv::loadProjectFile(projectFile, project, &projectError)) {
        if (outputDir.empty()) {
            outputDir = projectFile.parent_path() / "Build" / "Cooked";
        }
        if (manifestPath.empty()) {
            manifestPath = outputDir / "cook_manifest.json";
        }
        const std::filesystem::path validationReportPath = outputDir / "asset_validation_report.json";
        const std::vector<std::string> errors = {"Project load failed: " + projectError};
        const nlohmann::json validationReport = {
            {"version", 1},
            {"kind", "CookAssetValidationReport"},
            {"project", {{"projectFile", genericPathString(projectFile)}}},
            {"assetCount", 0},
            {"errorCount", errors.size()},
            {"warningCount", 0},
            {"validationErrorCount", 1},
            {"validationWarningCount", 0},
            {"cookErrors", errors},
            {"cookWarnings", nlohmann::json::array()},
            {"nativeTextureFormatSupport", nativeTextureFormatSupportJson(textureFormatSupport)},
            {"nativePackageTextureTargetSets", nativeTextureFormatSupportListJson(packageTextureTargetSets)},
            {"status", "failed"},
            {"policy", "Project file could not be loaded, so cook validation stopped before asset scanning."},
        };
        const nlohmann::json manifest = {
            {"schema", "TransparentCookManifestV1"},
            {"project", {{"projectFile", genericPathString(projectFile)}}},
            {"outputRoot", genericPathString(outputDir)},
            {"assetCount", 0},
            {"assets", nlohmann::json::array()},
            {"plannedFiles", nlohmann::json::array()},
            {"copiedFiles", nlohmann::json::array()},
            {"warnings", nlohmann::json::array()},
            {"errors", errors},
            {"validationReport", relativePathString(validationReportPath, outputDir)},
            {"validationErrorCount", 1},
            {"validationWarningCount", 0},
            {"nativeTextureFormatSupport", nativeTextureFormatSupportJson(textureFormatSupport)},
            {"nativePackageTextureTargetSets", nativeTextureFormatSupportListJson(packageTextureTargetSets)},
            {"status", "failed"},
        };
        std::string writeError;
        if (!writeJsonFile(validationReportPath, validationReport, &writeError)) {
            std::cerr << "Cook failed: could not write validation report: " << writeError << '\n';
        }
        if (!writeJsonFile(manifestPath, manifest, &writeError)) {
            std::cerr << "Cook failed: could not write manifest: " << writeError << '\n';
        }
        std::cerr << "Cook failed: could not load project: " << projectError << '\n';
        return 1;
    }
    if (outputDir.empty()) {
        outputDir = project.buildRoot / "Cooked";
    }
    if (manifestPath.empty()) {
        manifestPath = outputDir / "cook_manifest.json";
    }

    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    rtv::AssetRegistry registry;
    std::string registryError;
    if (!registry.load(project.assetRegistryPath, &registryError)) {
        errors.push_back("Asset registry load failed: " + registryError);
    } else {
        (void)registry.refreshRecordHealth(project.projectRoot, false);
    }

    std::unordered_set<rtv::AssetGuid> registryGuids;
    for (const rtv::AssetRecord& record : registry.records()) {
        if (!record.guid.empty()) {
            registryGuids.insert(record.guid);
        }
    }

    nlohmann::json copiedFiles = nlohmann::json::array();
    std::vector<CookFileCopyPlan> copyPlans;
    std::unordered_map<std::string, std::filesystem::path> plannedDestinationBySource;
    std::unordered_map<std::string, CookTextureSourceMetadata> textureSourceMetadataByPayloadDestination;
    nlohmann::json assets = nlohmann::json::array();
    nlohmann::json missingSources = nlohmann::json::array();
    nlohmann::json missingImportedMetadata = nlohmann::json::array();
    nlohmann::json missingCookedPayloads = nlohmann::json::array();
    nlohmann::json missingDependencies = nlohmann::json::array();
    nlohmann::json staleAssets = nlohmann::json::array();
    nlohmann::json failedAssets = nlohmann::json::array();
    nlohmann::json unsupportedImportSettings = nlohmann::json::array();
    nlohmann::json requiresReimport = nlohmann::json::array();
    nlohmann::json projectWarnings = nlohmann::json::array();
    nlohmann::json sublevelSceneReferences = nlohmann::json::array();
    nlohmann::json invalidSavedProjectReferences = nlohmann::json::array();
    nlohmann::json savedProjectReferenceParseErrors = nlohmann::json::array();
    nlohmann::json savedProjectReferenceScanRoots = nlohmann::json::array();
    size_t externalIndex = 0;

    auto planCookFile = [&](const std::filesystem::path& source, std::string_view role, std::string_view ownerGuid) -> std::optional<std::filesystem::path> {
        if (source.empty()) {
            return std::nullopt;
        }
        const std::string sourceKey = cookCopySourceKey(source);
        const auto existing = plannedDestinationBySource.find(sourceKey);
        if (existing != plannedDestinationBySource.end()) {
            return existing->second;
        }
        const std::filesystem::path destination = cookDestinationForPath(source, project.projectRoot, outputDir, externalIndex++);
        copyPlans.push_back(CookFileCopyPlan{
            source,
            destination,
            std::string(role),
            std::string(ownerGuid),
        });
        plannedDestinationBySource.emplace(sourceKey, destination);
        return destination;
    };
    auto copyProjectFile = [&](const std::filesystem::path& source, std::string_view role) {
        (void)planCookFile(source, role, "project");
    };
    std::vector<std::filesystem::path> sceneReferenceQueue;
    auto queueSceneReference = [&](const std::filesystem::path& path) {
        if (!path.empty()) {
            sceneReferenceQueue.push_back(canonicalForCookCompare(path));
        }
    };
    copyProjectFile(project.projectFile, "project_file");
    copyProjectFile(project.assetRegistryPath, "asset_registry");
    if (std::filesystem::exists(project.startupScene)) {
        copyProjectFile(project.startupScene, "startup_scene");
        queueSceneReference(project.startupScene);
    } else {
        warnings.push_back("Startup scene is missing: " + project.startupScene.string());
        projectWarnings.push_back({
            {"severity", "warning"},
            {"kind", "MissingStartupScene"},
            {"path", project.startupScene.generic_string()},
            {"detail", "Project startup scene is missing."},
        });
    }

    if (std::filesystem::exists(project.scenesRoot)) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(project.scenesRoot, ec)) {
            if (ec) {
                warnings.push_back("Scene directory scan warning: " + ec.message());
                break;
            }
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".rtlevel") {
                continue;
            }
            if (entry.path() == project.startupScene) {
                continue;
            }
            copyProjectFile(entry.path(), "scene");
            queueSceneReference(entry.path());
        }
    }

    std::unordered_set<std::string> scannedSublevelScenes;
    for (size_t sceneIndex = 0; sceneIndex < sceneReferenceQueue.size(); ++sceneIndex) {
        const std::filesystem::path sceneFile = sceneReferenceQueue[sceneIndex];
        const std::string sceneKey = cookCopySourceKey(sceneFile);
        if (!scannedSublevelScenes.insert(sceneKey).second) {
            continue;
        }
        const std::optional<nlohmann::json> sceneJson = readCookJsonFile(sceneFile);
        if (!sceneJson.has_value()) {
            continue;
        }
        nlohmann::json references = collectCookSceneSublevelReferences(*sceneJson, sceneFile, project.projectRoot);
        for (nlohmann::json& reference : references) {
            const std::filesystem::path referencedPath = reference.value("resolvedPath", std::string{});
            std::error_code ec;
            if (referencedPath.empty() || !std::filesystem::is_regular_file(referencedPath, ec)) {
                errors.push_back("Referenced sublevel scene is missing: " + referencedPath.string());
                projectWarnings.push_back({
                    {"severity", "error"},
                    {"kind", "MissingReferencedSublevel"},
                    {"ownerScene", relativePathString(sceneFile, project.projectRoot)},
                    {"sceneGuid", reference.value("sceneGuid", std::string{})},
                    {"scenePath", reference.value("scenePath", std::string{})},
                    {"resolvedPath", referencedPath.generic_string()},
                    {"detail", "A saved level instance or sublevel entry references a scene that is not available for cook."},
                });
                sublevelSceneReferences.push_back(reference);
                continue;
            }
            if (std::optional<std::filesystem::path> destination = planCookFile(referencedPath, "sublevel_scene", reference.value("sceneGuid", std::string{}))) {
                reference["ownerScene"] = relativePathString(sceneFile, project.projectRoot);
                reference["output"] = relativePathString(*destination, outputDir);
            }
            sublevelSceneReferences.push_back(reference);
            queueSceneReference(referencedPath);
        }
    }

    for (const rtv::AssetRecord& record : registry.records()) {
        nlohmann::json asset = {
            {"guid", record.guid},
            {"type", rtv::assetTypeName(record.type)},
            {"displayName", record.displayName},
            {"status", rtv::assetImportStatusName(record.status)},
            {"sourceMissing", record.sourceMissing},
            {"importedMetadataMissing", record.importedMetadataMissing},
            {"cookedPayloadMissing", record.cookedPayloadMissing},
            {"dependenciesMissing", record.dependenciesMissing},
            {"stale", record.stale},
        };

        const std::filesystem::path sourcePath = resolveProjectPath(project.projectRoot, record.sourcePath);
        const std::filesystem::path importedPath = resolveProjectPath(project.projectRoot, record.importedPath);
        const std::filesystem::path cachePath = resolveProjectPath(project.projectRoot, record.cachePath);
        const std::filesystem::path thumbnailPath = resolveProjectPath(project.projectRoot, record.thumbnailPath);

        if (record.sourceMissing) {
            warnings.push_back("Source missing but cook can proceed from metadata/payload for " + record.guid + ": " + sourcePath.string());
            appendCookValidationIssue(missingSources, "warning", "MissingSource", record, "Raw import source is missing, but transparent cook can proceed when metadata and cooked payloads exist.", record.sourcePath);
        }
        if (record.stale || record.status == rtv::AssetImportStatus::Stale) {
            warnings.push_back("Asset is stale and should be reimported before shipping: " + record.guid);
            appendCookValidationIssue(staleAssets, "warning", "StaleAsset", record, "Source is newer than imported metadata or cooked payload; reimport before shipping.");
            appendCookValidationIssue(requiresReimport, "warning", "RequiresReimport", record, "Asset is stale and should be reimported before cooking or packaging.");
        }
        if (record.status == rtv::AssetImportStatus::Failed) {
            errors.push_back("Asset import previously failed: " + record.guid);
            appendCookValidationIssue(failedAssets, "error", "FailedImport", record, "Asset import previously failed.");
            appendCookValidationIssue(requiresReimport, "error", "RequiresReimport", record, "Asset import failed and must be repaired or reimported before cooking or packaging.");
        }
        if (record.importedMetadataMissing || record.importedPath.empty()) {
            errors.push_back("Imported metadata missing for asset: " + record.guid);
            appendCookValidationIssue(missingImportedMetadata, "error", "MissingImportedMetadata", record, "Imported asset metadata is missing.", record.importedPath);
            appendCookValidationIssue(requiresReimport, "error", "RequiresReimport", record, "Imported metadata is missing and must be regenerated before cooking or packaging.");
        }
        if (record.cookedPayloadMissing || record.cachePath.empty()) {
            errors.push_back("Cooked payload missing for asset: " + record.guid);
            appendCookValidationIssue(missingCookedPayloads, "error", "MissingCookedPayload", record, "Cooked/runtime payload is missing.", record.cachePath);
            appendCookValidationIssue(requiresReimport, "error", "RequiresReimport", record, "Cooked/runtime payload is missing and must be regenerated before cooking or packaging.");
        }
        for (const rtv::AssetDependency& dependency : record.dependencies) {
            if (!dependency.guid.empty() && registryGuids.find(dependency.guid) == registryGuids.end()) {
                errors.push_back("Asset " + record.guid + " references missing dependency " + dependency.guid + " (" + dependency.kind + ")");
                missingDependencies.push_back({
                    {"severity", "error"},
                    {"kind", "MissingDependencyGuid"},
                    {"ownerGuid", record.guid},
                    {"ownerDisplayName", record.displayName},
                    {"ownerAssetType", rtv::assetTypeName(record.type)},
                    {"dependencyGuid", dependency.guid},
                    {"dependencyKind", dependency.kind},
                    {"detail", "Dependency GUID is not present in the asset registry."},
                });
            }
        }
        if (record.importSettings.unitScale <= 0.0f) {
            errors.push_back("Invalid import unit scale for asset: " + record.guid);
            appendCookValidationIssue(unsupportedImportSettings, "error", "InvalidUnitScale", record, "Import unit scale must be greater than zero.");
        }
        if (!supportedCookCoordinateConversion(record.importSettings.coordinateConversion)) {
            appendCookValidationIssue(unsupportedImportSettings, "warning", "UnsupportedCoordinateConversion", record, "Import coordinate conversion is not recognized: " + record.importSettings.coordinateConversion);
        }
        if (!supportedCookMaterialImportMode(record.importSettings.materialImportMode)) {
            appendCookValidationIssue(unsupportedImportSettings, "warning", "UnsupportedMaterialImportMode", record, "Material import mode is not recognized: " + record.importSettings.materialImportMode);
        }
        if (!supportedCookTextureImportMode(record.importSettings.textureImportMode)) {
            appendCookValidationIssue(unsupportedImportSettings, "warning", "UnsupportedTextureImportMode", record, "Texture import mode is not recognized: " + record.importSettings.textureImportMode);
        }
        if (!supportedCookTextureCompression(record.importSettings.textureCompression)) {
            appendCookValidationIssue(unsupportedImportSettings, "warning", "UnsupportedTextureCompression", record, "Texture compression/transcode mode is not available in this pipeline stage: " + record.importSettings.textureCompression);
        }

        if (!record.importedPath.empty()) {
            if (std::optional<std::filesystem::path> destination = planCookFile(importedPath, "asset_metadata", record.guid)) {
                asset["cookedMetadataOutput"] = relativePathString(*destination, outputDir);
            }
        }
        if (!record.cachePath.empty()) {
            if (std::optional<std::filesystem::path> destination = planCookFile(cachePath, "asset_payload", record.guid)) {
                asset["cookedPayloadOutput"] = relativePathString(*destination, outputDir);
                if (record.type == rtv::AssetType::Texture || record.type == rtv::AssetType::HDRI) {
                    textureSourceMetadataByPayloadDestination[cookCopySourceKey(*destination)] = CookTextureSourceMetadata{
                        sourcePath,
                        record.displayName,
                    };
                }
            }
        }
        if (!record.thumbnailPath.empty() && std::filesystem::exists(thumbnailPath)) {
            if (std::optional<std::filesystem::path> destination = planCookFile(thumbnailPath, "asset_thumbnail", record.guid)) {
                asset["thumbnailOutput"] = relativePathString(*destination, outputDir);
            }
        }
        assets.push_back(std::move(asset));
    }

    std::vector<std::filesystem::path> referenceScanFiles;
    collectCookProjectReferenceScanFiles(project, savedProjectReferenceScanRoots, referenceScanFiles);
    for (const std::filesystem::path& path : referenceScanFiles) {
        std::optional<nlohmann::json> json = readCookJsonFile(path);
        if (!json.has_value()) {
            savedProjectReferenceParseErrors.push_back({
                {"severity", "warning"},
                {"kind", "SavedProjectReferenceParseError"},
                {"file", path.generic_string()},
                {"detail", "File matched the project reference validation set but could not be parsed as JSON."},
            });
            continue;
        }
        appendInvalidCookSavedGuidReferences(*json, registryGuids, path, "$", {}, invalidSavedProjectReferences);
    }
    if (!invalidSavedProjectReferences.empty()) {
        errors.push_back("Saved project metadata contains asset GUID references missing from the asset registry.");
    }
    if (!savedProjectReferenceParseErrors.empty()) {
        warnings.push_back("Some saved project metadata files could not be parsed during cook reference validation.");
    }

    std::sort(errors.begin(), errors.end());
    errors.erase(std::unique(errors.begin(), errors.end()), errors.end());
    std::sort(warnings.begin(), warnings.end());
    warnings.erase(std::unique(warnings.begin(), warnings.end()), warnings.end());

    const std::vector<const nlohmann::json*> validationIssueArrays = {
        &missingSources,
        &missingImportedMetadata,
        &missingCookedPayloads,
        &missingDependencies,
        &staleAssets,
        &failedAssets,
        &unsupportedImportSettings,
        &invalidSavedProjectReferences,
        &savedProjectReferenceParseErrors,
        &requiresReimport,
        &projectWarnings,
    };
    const size_t validationErrorCount = countCookValidationSeverity(validationIssueArrays, "error");
    const size_t validationWarningCount = countCookValidationSeverity(validationIssueArrays, "warning");
    const std::filesystem::path validationReportPath = outputDir / "asset_validation_report.json";
    const std::filesystem::path packagePath = outputDir / (cookPackageBaseName(project) + ".rtpkg");
    const std::filesystem::path packageInspectionPath = outputDir / (cookPackageBaseName(project) + ".rtpkg.inspection.json");
    const std::filesystem::path packageValidationPath = outputDir / (cookPackageBaseName(project) + ".rtpkg.validation.json");
    std::string packageStatus = "pending";
    std::string packageError;
    size_t packageInputCount = 0;
    uint64_t packageBytes = 0;
    nlohmann::json nativeTexturePackageVariants = nlohmann::json::array();
    auto nativeTexturePackageVariantGeneratedCount = [&]() {
        size_t count = 0;
        for (const nlohmann::json& item : nativeTexturePackageVariants) {
            if (item.is_object() && item.value("generated", false)) {
                ++count;
            }
        }
        return count;
    };
    auto plannedFilesJson = [&]() {
        nlohmann::json planned = nlohmann::json::array();
        for (const CookFileCopyPlan& plan : copyPlans) {
            planned.push_back({
                {"role", plan.role},
                {"ownerGuid", plan.ownerGuid},
                {"source", relativePathString(plan.source, project.projectRoot)},
                {"output", relativePathString(plan.destination, outputDir)},
            });
        }
        return planned;
    };
    auto makeValidationReport = [&](std::string_view status) {
        return nlohmann::json{
            {"version", 1},
            {"kind", "CookAssetValidationReport"},
            {"project", {
                {"guid", project.projectGuid},
                {"name", project.name},
                {"projectFile", relativePathString(project.projectFile, project.projectRoot)},
                {"startupScene", relativePathString(project.startupScene, project.projectRoot)},
                {"assetRegistry", relativePathString(project.assetRegistryPath, project.projectRoot)},
            }},
            {"assetCount", registry.records().size()},
            {"errorCount", errors.size()},
            {"warningCount", warnings.size()},
            {"validationErrorCount", validationErrorCount},
            {"validationWarningCount", validationWarningCount},
            {"copyPlanCount", copyPlans.size()},
            {"copiedFileCount", copiedFiles.size()},
            {"missingSources", missingSources},
            {"missingImportedMetadata", missingImportedMetadata},
            {"missingCookedPayloads", missingCookedPayloads},
            {"missingDependencies", missingDependencies},
            {"staleAssets", staleAssets},
            {"failedAssets", failedAssets},
            {"unsupportedImportSettings", unsupportedImportSettings},
            {"invalidSavedProjectReferences", invalidSavedProjectReferences},
            {"savedProjectReferenceParseErrors", savedProjectReferenceParseErrors},
            {"savedProjectReferenceScanRoots", savedProjectReferenceScanRoots},
            {"savedProjectReferenceScannedFileCount", referenceScanFiles.size()},
            {"sublevelSceneReferences", sublevelSceneReferences},
            {"requiresReimport", requiresReimport},
            {"projectWarnings", projectWarnings},
            {"cookErrors", errors},
            {"cookWarnings", warnings},
            {"status", status},
            {"package", {
                {"path", relativePathString(packagePath, outputDir)},
                {"inspection", relativePathString(packageInspectionPath, outputDir)},
                {"validation", relativePathString(packageValidationPath, outputDir)},
                {"status", packageStatus},
                {"inputCount", packageInputCount},
                {"bytes", packageBytes},
                {"error", packageError},
                {"nativeTextureVariantCount", nativeTexturePackageVariants.size()},
                {"nativeTextureVariantGeneratedCount", nativeTexturePackageVariantGeneratedCount()},
                {"nativeTextureVariants", nativeTexturePackageVariants},
            }},
            {"nativeTextureFormatSupport", nativeTextureFormatSupportJson(textureFormatSupport)},
            {"nativePackageTextureTargetSets", nativeTextureFormatSupportListJson(packageTextureTargetSets)},
            {"policy", "This report is generated by --cook-project before package emission. Missing raw sources are warnings when imported metadata and cooked payloads are available; missing metadata, missing cooked payloads, failed imports, and missing dependency GUIDs block the transparent cook."},
        };
    };
    auto makeManifest = [&](std::string_view status) {
        nlohmann::json manifest;
        manifest["schema"] = "TransparentCookManifestV1";
        manifest["project"] = {
            {"guid", project.projectGuid},
            {"name", project.name},
            {"projectFile", relativePathString(project.projectFile, project.projectRoot)},
            {"startupScene", relativePathString(project.startupScene, project.projectRoot)},
            {"assetRegistry", relativePathString(project.assetRegistryPath, project.projectRoot)},
        };
        manifest["outputRoot"] = genericPathString(outputDir);
        manifest["assetCount"] = registry.records().size();
        manifest["assets"] = assets;
        manifest["plannedFiles"] = plannedFilesJson();
        manifest["copiedFiles"] = copiedFiles;
        manifest["sublevelSceneReferences"] = sublevelSceneReferences;
        manifest["warnings"] = warnings;
        manifest["errors"] = errors;
        manifest["validationReport"] = relativePathString(validationReportPath, outputDir);
        manifest["validationErrorCount"] = validationErrorCount;
        manifest["validationWarningCount"] = validationWarningCount;
        manifest["nativePackage"] = {
            {"path", relativePathString(packagePath, outputDir)},
            {"inspection", relativePathString(packageInspectionPath, outputDir)},
            {"validation", relativePathString(packageValidationPath, outputDir)},
            {"status", packageStatus},
            {"inputCount", packageInputCount},
            {"bytes", packageBytes},
            {"error", packageError},
            {"nativeTextureVariantCount", nativeTexturePackageVariants.size()},
            {"nativeTextureVariantGeneratedCount", nativeTexturePackageVariantGeneratedCount()},
            {"nativeTextureVariants", nativeTexturePackageVariants},
        };
        manifest["nativeTextureFormatSupport"] = nativeTextureFormatSupportJson(textureFormatSupport);
        manifest["nativePackageTextureTargetSets"] = nativeTextureFormatSupportListJson(packageTextureTargetSets);
        manifest["status"] = status;
        manifest["futurePackageCompatibility"] = {
            {"packageObjectModel", "metadata_and_payload_chunks"},
            {"opaquePackageExtension", ".rtpkg"},
            {"transparentLayoutPreserved", true},
            {"nativePackageEmission", true},
        };
        return manifest;
    };
    auto writeCookArtifacts = [&](std::string_view status) {
        std::string writeError;
        if (!writeJsonFile(validationReportPath, makeValidationReport(status == std::string_view("success") ? "pass" : status), &writeError)) {
            std::cerr << "Cook failed: could not write validation report: " << writeError << '\n';
            return false;
        }
        if (!writeJsonFile(manifestPath, makeManifest(status), &writeError)) {
            std::cerr << "Cook failed: could not write manifest: " << writeError << '\n';
            return false;
        }
        return true;
    };

    if (!errors.empty()) {
        packageStatus = "failed_validation";
        packageError = "Cook validation failed before native package emission.";
    }
    if (!writeCookArtifacts(errors.empty() ? "copying" : "failed")) {
        return 1;
    }
    if (!errors.empty()) {
        std::cout << "Cook validation report: " << validationReportPath.string() << '\n';
        std::cout << "Cook manifest: " << manifestPath.string() << '\n';
        std::cout << "Cook validation failed before payload copy; warnings=" << warnings.size()
                  << " errors=" << errors.size() << '\n';
        return 1;
    }

    for (const CookFileCopyPlan& plan : copyPlans) {
        (void)copyCookFile(
            plan.source,
            plan.destination,
            copiedFiles,
            errors,
            project.projectRoot,
            outputDir,
            plan.role,
            plan.ownerGuid);
    }

    std::sort(errors.begin(), errors.end());
    errors.erase(std::unique(errors.begin(), errors.end()), errors.end());

    std::vector<rtv::RtpkgAssetInput> packageInputs;
    if (errors.empty()) {
        packageStatus = "collecting_inputs";
        std::unordered_set<std::string> emittedTextureVariantKeys;
        for (const CookFileCopyPlan& plan : copyPlans) {
            if (plan.role != "asset_payload" || !cookPackageNativePayloadCandidate(plan.destination)) {
                continue;
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(plan.destination, ec)) {
                continue;
            }
            rtv::RtpkgAssetInput input;
            input.path = plan.destination;
            input.packagePath = relativePathString(plan.destination, outputDir);
            packageInputs.push_back(std::move(input));

            if (rtv::nativeAssetKindFromExtension(plan.destination) == rtv::NativeAssetKind::Texture) {
                const auto textureSourceIt = textureSourceMetadataByPayloadDestination.find(cookCopySourceKey(plan.destination));
                const bool hasKtx2Source = textureSourceIt != textureSourceMetadataByPayloadDestination.end() &&
                    !textureSourceIt->second.sourcePath.empty() &&
                    rtv::detectCompressedTextureKind(textureSourceIt->second.sourcePath.string()) == rtv::CompressedTextureKind::Ktx2;
                for (const rtv::NativeTextureFormatSupport& targetSet : packageTextureTargetSets) {
                    const CookGeneratedTexturePackageVariant variant = hasKtx2Source
                        ? cookGeneratedKtx2TexturePackageVariant(plan.destination, textureSourceIt->second, outputDir, targetSet, emittedTextureVariantKeys)
                        : cookGeneratedTexturePackageVariant(plan.destination, outputDir, targetSet, emittedTextureVariantKeys);
                    nativeTexturePackageVariants.push_back(cookGeneratedTexturePackageVariantJson(variant, outputDir));
                    if (variant.generated) {
                        rtv::RtpkgAssetInput variantInput;
                        variantInput.path = variant.outputPath;
                        variantInput.packagePath = variant.packagePath;
                        packageInputs.push_back(std::move(variantInput));
                    } else if (variant.status.rfind("failed", 0) == 0) {
                        warnings.push_back("Native texture package variant generation skipped for " + plan.destination.string() + ": " + variant.reason);
                    }
                }
            }
        }
        std::sort(packageInputs.begin(), packageInputs.end(), [](const rtv::RtpkgAssetInput& a, const rtv::RtpkgAssetInput& b) {
            return a.packagePath < b.packagePath;
        });
        packageInputCount = packageInputs.size();
        if (packageInputs.empty()) {
            packageStatus = "skipped_no_native_inputs";
            packageError = "Cook copied no standalone native payloads eligible for .rtpkg emission.";
            warnings.push_back(packageError);
        } else {
            packageStatus = "writing";
            rtv::RtpkgWriteDesc packageDesc;
            packageDesc.debugName = project.name.empty() ? project.projectFile.stem().string() : project.name;
            packageDesc.root = outputDir;
            packageDesc.assets = packageInputs;
            rtv::NativeBinaryError packageWriteError;
            rtv::RtpkgWriter packageWriter;
            if (!packageWriter.write(packagePath, packageDesc, &packageWriteError)) {
                packageStatus = "failed";
                packageError = packageWriteError.message.empty() ? std::string("Native package writer failed.") : packageWriteError.message;
                errors.push_back("Native package emission failed: " + packageError);
            } else {
                std::error_code ec;
                const uintmax_t bytes = std::filesystem::file_size(packagePath, ec);
                packageBytes = ec ? 0ull : static_cast<uint64_t>(bytes);
                rtv::RtpkgReader packageReader;
                const rtv::RtpkgInspection packageInspection = packageReader.inspect(packagePath, true);
                const nlohmann::json packageInspectionJson = rtv::rtpkgInspectionToJson(packageInspection, packagePath);
                const nlohmann::json packageValidationJson = rtv::validateRtpkgInspectionToJson(packageInspection, packagePath);
                std::string writeError;
                if (!writeJsonFile(packageInspectionPath, packageInspectionJson, &writeError)) {
                    packageStatus = "failed_inspection_write";
                    packageError = writeError;
                    errors.push_back("Native package inspection report write failed: " + packageError);
                } else if (!writeJsonFile(packageValidationPath, packageValidationJson, &writeError)) {
                    packageStatus = "failed_validation_write";
                    packageError = writeError;
                    errors.push_back("Native package validation report write failed: " + packageError);
                } else if (!packageInspection.native.ok) {
                    packageStatus = "failed_inspection";
                    packageError = packageInspection.native.errors.empty() ? std::string("Native package inspection failed.") : packageInspection.native.errors.front().message;
                    errors.push_back("Native package inspection failed: " + packageError);
                } else if (!packageValidationJson.value("ok", false)) {
                    packageStatus = "failed_validation";
                    packageError = "Native package strict validation failed.";
                    errors.push_back("Native package strict validation failed: " + relativePathString(packageValidationPath, outputDir));
                } else {
                    packageStatus = "success";
                    packageError.clear();
                }
            }
        }
    }

    const bool success = errors.empty();
    if (!success && packageStatus == "pending") {
        packageStatus = "failed";
    }
    if (!writeCookArtifacts(success ? "success" : "failed")) {
        return 1;
    }

    std::cout << "Cook validation report: " << validationReportPath.string() << '\n';
    std::cout << "Cook manifest: " << manifestPath.string() << '\n';
    if (packageStatus == "success") {
        std::cout << "Cook native package: " << packagePath.string() << " (" << packageInputCount << " assets)\n";
        std::cout << "Cook native package inspection: " << packageInspectionPath.string() << '\n';
        std::cout << "Cook native package validation: " << packageValidationPath.string() << '\n';
    } else {
        std::cout << "Cook native package status: " << packageStatus << " " << packageError << '\n';
    }
    std::cout << "Cook copied " << copiedFiles.size() << " files for "
              << registry.records().size() << " assets; warnings=" << warnings.size()
              << " errors=" << errors.size() << '\n';
    return success ? 0 : 1;
}

int stageImportCommand(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& jsonOut,
    const rtv::NativeTextureFormatSupport& textureFormatSupport,
    float emissiveScale = 1.0f,
    bool buildBlasCache = true) {
    const std::filesystem::path root = workspaceRoot.empty()
        ? (std::filesystem::current_path() / "out" / "stage_import_workspace")
        : workspaceRoot;
    rtv::AssetImportWorkspace workspace;
    workspace.root = root;
    workspace.contentRoot = root / "Content";
    workspace.sourceAssetsRoot = root / "SourceAssets";
    workspace.cacheRoot = root / "Cache";
    workspace.registryPath = root / "Content" / "AssetRegistry.json";
    workspace.nativeTextureFormatSupport = textureFormatSupport;

    std::error_code ec;
    std::filesystem::create_directories(workspace.contentRoot, ec);
    std::filesystem::create_directories(workspace.sourceAssetsRoot, ec);
    std::filesystem::create_directories(workspace.cacheRoot, ec);
    if (ec) {
        std::cerr << "Could not create staged import workspace: " << ec.message() << '\n';
        return 1;
    }

    rtv::AssetImportRequest request;
    request.sourcePath = sourcePath;
    request.destinationFolder = "Models";
    request.mode = "ImportAsset";
    request.settings.copySourceIntoProject = false;
    request.settings.buildCookedPayloadsNow = true;
    request.settings.buildBlasCache = buildBlasCache;
    request.settings.generateThumbnails = false;
    request.settings.emissiveScale = emissiveScale;
    const rtv::StagedAssetImportResult result = rtv::stagePlaceholderAssetImport(request, workspace);

    nlohmann::json records = nlohmann::json::array();
    for (const rtv::AssetRecord& record : result.records) {
        records.push_back({
            {"guid", record.guid},
            {"type", rtv::assetTypeName(record.type)},
            {"displayName", record.displayName},
            {"importedPath", record.importedPath},
            {"cachePath", record.cachePath},
            {"dependencyCount", record.dependencies.size()},
        });
    }
    nlohmann::json generated = nlohmann::json::array();
    for (const std::filesystem::path& path : result.generatedFiles) {
        generated.push_back(path.generic_string());
    }
    const nlohmann::json summary = {
        {"success", result.success},
        {"workspaceRoot", root.generic_string()},
        {"sourcePath", sourcePath.generic_string()},
        {"nativeTextureFormatSupport", nativeTextureFormatSupportJson(textureFormatSupport)},
        {"recordCount", result.records.size()},
        {"generatedFileCount", result.generatedFiles.size()},
        {"generatedFiles", generated},
        {"records", records},
        {"importReportPath", result.importReportPath.generic_string()},
        {"warnings", result.warnings},
        {"errors", result.errors},
    };
    if (!jsonOut.empty()) {
        std::filesystem::create_directories(jsonOut.parent_path(), ec);
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write staged import JSON: " << jsonOut << '\n';
            return 1;
        }
        file << summary.dump(2);
    } else {
        std::cout << summary.dump(2) << '\n';
    }
    return result.success ? 0 : 1;
}

int cookAnimationControllerCommand(
    const std::filesystem::path& sourcePath,
    std::filesystem::path outputPath,
    const std::filesystem::path& jsonOut) {
    if (sourcePath.empty()) {
        std::cerr << "--cook-animation-controller requires a transparent .rtanimcontroller.json source path\n";
        return 1;
    }
    if (outputPath.empty()) {
        outputPath = sourcePath;
        const std::string filename = outputPath.filename().string();
        if (filename.ends_with(".json")) {
            outputPath.replace_filename(filename.substr(0, filename.size() - 5u));
        } else {
            outputPath.replace_extension(".rtanimcontroller");
        }
    }

    std::vector<std::string> warnings;
    const rtv::AnimationController controller = rtv::AnimationController::loadJson(sourcePath, &warnings);
    if (!controller.valid()) {
        std::cerr << "Animation controller cook failed: source did not parse into a valid controller: " << sourcePath << '\n';
        const nlohmann::json report = {
            {"schema", "AnimationControllerCookReportV1"},
            {"success", false},
            {"sourcePath", genericPathString(sourcePath)},
            {"outputPath", genericPathString(outputPath)},
            {"warnings", warnings},
            {"errors", nlohmann::json::array({"source controller is invalid"})},
        };
        if (!jsonOut.empty()) {
            std::string writeError;
            (void)writeJsonFile(jsonOut, report, &writeError);
        }
        return 1;
    }

    std::vector<std::byte> sourceBytes;
    if (!readBinaryFileForCook(sourcePath, sourceBytes)) {
        std::cerr << "Animation controller cook failed: could not read source bytes: " << sourcePath << '\n';
        return 1;
    }

    const std::string sourceHash = rtv::nativeHashHex(rtv::nativeHashBytes(sourceBytes));
    const std::string settingsHash = rtv::nativeHashHex(rtv::nativeHashText("rtanimcontroller-compact-cook-v1"));
    rtv::NativeAssetCookInput input;
    input.guid = sourceHash.substr(0, 8) + "-" + sourceHash.substr(8, 4) + "-" + sourceHash.substr(12, 4) + "-" + sourceHash.substr(16, 4) + "-" + sourceHash.substr(20, 12);
    input.outputPath = outputPath;
    input.sourcePath = sourcePath;
    input.displayName = controller.name().empty() ? sourcePath.stem().string() : controller.name();
    input.sourceHash = sourceHash;
    input.importSettingsHash = settingsHash;

    const rtv::NativeAssetCooker cooker;
    rtv::NativeAssetCookResult result = cooker.cookAnimationController(input, controller);
    for (const std::string& warning : warnings) {
        result.warnings.push_back(warning);
    }

    nlohmann::json nativeInspection = nullptr;
    if (result.success) {
        rtv::NativeAssetReader reader;
        nativeInspection = rtv::nativeAssetInspectionToJson(reader.inspect(result.path, true), result.path);
    }
    const nlohmann::json report = {
        {"schema", "AnimationControllerCookReportV1"},
        {"success", result.success},
        {"sourcePath", genericPathString(sourcePath)},
        {"outputPath", genericPathString(result.path)},
        {"assetGuid", input.guid},
        {"sourceHash", sourceHash},
        {"importSettingsHash", settingsHash},
        {"payloadHash", result.payloadHash},
        {"payloadBytes", result.payloadBytes},
        {"parameterCount", controller.parameters().size()},
        {"stateCount", controller.states().size()},
        {"layerCount", controller.layers().size()},
        {"avatarMaskCount", controller.avatarMasks().size()},
        {"warnings", result.warnings},
        {"errors", result.errors},
        {"nativeInspection", nativeInspection},
    };
    if (!jsonOut.empty()) {
        std::string writeError;
        if (!writeJsonFile(jsonOut, report, &writeError)) {
            std::cerr << "Animation controller cook failed: could not write report: " << writeError << '\n';
            return 1;
        }
    }
    if (!result.success) {
        std::cerr << "Animation controller cook failed: " << (result.errors.empty() ? std::string("unknown error") : result.errors.front()) << '\n';
        return 1;
    }
    std::cout << "Cooked animation controller: " << result.path.string() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        uint32_t maxFrames = 0;
        rtv::RendererDebugView debugView = rtv::RendererDebugView::Beauty;
        bool debugViewProvided = false;
        std::optional<std::filesystem::path> gltfPath;
        std::optional<std::filesystem::path> hdrPath;
        std::optional<std::filesystem::path> scenePath;
        std::optional<bool> denoiserOverride;
        std::optional<rtv::DenoiserBackend> denoiserBackendOverride;
        std::optional<rtv::TemporalUpscaler> temporalUpscalerOverride;
        std::optional<bool> dlssFrameGenerationOverride;
        std::optional<bool> dlssRayReconstructionOverride;
        std::optional<bool> streamlineReflexOverride;
        std::optional<float> dlssSharpeningOverride;
        std::optional<rtv::RestirMode> restirModeOverride;
        std::optional<rtv::RenderPreset> renderPresetOverride;
        std::optional<bool> restirGiOverride;
        std::optional<bool> restirGiFinalStabilizationOverride;
        std::optional<bool> opacityMicromapOverride;
        std::optional<uint32_t> opacityMicromapSubdivisionOverride;
        std::optional<bool> wavefrontQueuesOverride;
        std::optional<bool> wavefrontPrimaryGenerateOverride;
        std::optional<bool> wavefrontTraceOverride;
        std::optional<bool> wavefrontShadeOverride;
        std::optional<bool> wavefrontShadowTraceOverride;
        std::optional<bool> wavefrontCompactOverride;
        std::optional<bool> wavefrontSortOverride;
        std::optional<bool> wavefrontFinalOutputOverride;
        std::optional<bool> shaderExecutionReorderingOverride;
        std::optional<float> dofApertureRadiusOverride;
        std::optional<float> dofFocusDistanceOverride;
        std::optional<uint32_t> dofBladeCountOverride;
        std::optional<float> dofBokehRotationOverride;
        std::optional<bool> motionBlurOverride;
        std::optional<float> motionBlurShutterOpenOverride;
        std::optional<float> motionBlurShutterCloseOverride;
        std::optional<bool> homogeneousVolumeOverride;
        std::optional<float> homogeneousVolumeScatteringOverride;
        std::optional<float> homogeneousVolumeAbsorptionOverride;
        std::optional<float> homogeneousVolumeAnisotropyOverride;
        std::optional<bool> mneeCausticsOverride;
        bool wavefrontValidationMode = false;
        std::optional<float> taaMotionFeedbackOverride;
        std::optional<float> taaReactiveFeedbackOverride;
        std::optional<uint32_t> samplesPerPixelOverride;
        std::optional<bool> sppLimiterOverride;
        bool validationCameraMotion = false;
        bool validationObjectMotion = false;

        rtv::HeadlessDiagnosticsConfig diagConfig;
        bool dumpRenderGraphDot = false;
        std::optional<std::filesystem::path> dotOutputPath;
        std::optional<std::filesystem::path> compareProfileOldPath;
        std::optional<std::filesystem::path> compareProfileNewPath;
        std::optional<std::filesystem::path> compareImageBaselinePath;
        std::optional<std::filesystem::path> compareImageCurrentPath;
        std::optional<std::filesystem::path> compareImageSequenceBaselinePath;
        std::optional<std::filesystem::path> compareImageSequenceCurrentPath;
        std::optional<std::filesystem::path> compareImageOutputPath;
        bool updateBaseline = false;
        bool checkBaseline = false;
        std::filesystem::path baselineRoot = "baselines";
        std::optional<std::filesystem::path> dumpMemoryPath;
        std::optional<std::filesystem::path> dumpFrameTimelinePath;
        std::optional<std::filesystem::path> dumpResourceLifetimesPath;
        std::optional<std::filesystem::path> dumpShaderReportPath;
        std::optional<std::filesystem::path> dumpBindingsPath;
        std::optional<std::filesystem::path> crashDumpPackageDir;
        std::optional<std::filesystem::path> checkBudgetPath;
        std::optional<std::filesystem::path> checkStreamingBudgetPath;
        std::optional<std::filesystem::path> descriptorLifetimeStressPath;
        std::optional<std::filesystem::path> nativePackageScenePath;
        rtv::NativePackageAnimationSelection nativePackageAnimationSelection;
        std::optional<std::filesystem::path> cookProjectPath;
        std::optional<std::filesystem::path> cookAnimationControllerPath;
        std::optional<std::filesystem::path> exportAnimationControllerPath;
        std::optional<std::filesystem::path> mutateAnimationControllerPath;
        std::filesystem::path controllerMutationJsonPath;
        std::filesystem::path controllerOutputPath;
        bool controllerMutationDryRun = false;
        bool controllerForceOverwrite = false;
        std::filesystem::path nativeOutputPath;
        std::optional<std::filesystem::path> stageImportPath;
        std::filesystem::path stageImportWorkspaceRoot;
        std::filesystem::path stageImportJsonPath;
        float stageImportEmissiveScale = 1.0f;
        bool stageImportBuildBlasCache = true;
        std::optional<std::filesystem::path> inspectNativeAssetPath;
        std::optional<std::filesystem::path> inspectAnimationControllerPath;
        std::optional<std::filesystem::path> inspectRuntimeSkeletonPath;
        std::optional<std::filesystem::path> inspectionJsonPath;
        std::optional<std::filesystem::path> emitNativeFixturePath;
        std::optional<std::filesystem::path> emitBasisuKtx2FixturePath;
        rtv::NativeAssetKind emitNativeFixtureKind = rtv::NativeAssetKind::Unknown;
        std::string emitNativeFixtureGuid;
        std::string emitNativeFixtureMaterialTextureGuid;
        std::string emitNativeFixtureTextureRole;
        uint32_t emitNativeFixtureTextureFormat = 0;
        std::optional<std::filesystem::path> migrateNativeAssetPath;
        std::optional<std::filesystem::path> migratePackagePath;
        std::filesystem::path migrationReportPath;
        bool migrationDryRun = false;
        bool inspectNativeStore = false;
        std::vector<std::filesystem::path> nativeStorePackages;
        std::vector<std::filesystem::path> nativeStoreRoots;
        std::vector<std::string> nativeStoreQueries;
        std::vector<std::string> nativeStoreRetains;
        std::vector<std::string> nativeStoreReleases;
        std::vector<std::filesystem::path> nativeStoreUnmountPackages;
        std::optional<std::filesystem::path> loadNativeRuntimeAssetsPath;
        std::optional<std::filesystem::path> writeRtpkgPath;
        std::optional<std::filesystem::path> inspectRtpkgPath;
        std::optional<std::filesystem::path> validateRtpkgPath;
        std::optional<std::filesystem::path> planRtpkgPatchBasePath;
        std::optional<std::filesystem::path> planRtpkgPatchUpdatedPath;
        std::optional<std::filesystem::path> simulateRtpkgCompressionPath;
        std::optional<std::filesystem::path> simulateRtpkgStreamingIoPath;
        std::string rtpkgCompressionProfile = "zstd";
        std::filesystem::path rtpkgRoot;
        std::vector<std::filesystem::path> rtpkgInputs;
        std::filesystem::path cookOutputDir;
        std::filesystem::path cookManifestPath;
        rtv::NativeTextureFormatSupport nativeTextureFormatSupport = rtv::nativeTextureOfflineFallbackFormatSupport();
        std::vector<rtv::NativeTextureFormatSupport> nativePackageTextureTargetSets;
        GpuUploadTicketSimulationArgs gpuUploadSimulation;
        MainThreadApplySimulationArgs mainThreadApplySimulation;
        TopologyRebuildSimulationArgs topologyRebuildSimulation;
        NativeTextureFormatPolicySimulationArgs nativeTextureFormatPolicySimulation;
        StreamingSchedulerSimulationArgs streamingSchedulerSimulation;
        NativeGpuAssetCacheSimulationArgs nativeGpuAssetCacheSimulation;
        GpuSceneStreamingStateSimulationArgs gpuSceneStreamingStateSimulation;
        IncrementalGpuSceneUpdateSimulationArgs incrementalGpuSceneUpdateSimulation;
        StreamingGpuWorkQueueSimulationArgs streamingGpuWorkQueueSimulation;
        uint32_t descriptorLifetimeStressCycles = 12;
        uint32_t descriptorLifetimeStressFrames = 2;
        bool validateGpuLabels = false;
        bool shaderHotReloadReport = false;
        rtv::StreamingRuntimeOptions streamingOptions;
        std::optional<std::filesystem::path> dumpStreamingPath;
        std::optional<std::filesystem::path> streamingValidationScenePath;
        std::optional<std::filesystem::path> validateNativeCatalogPath;
        std::filesystem::path streamingCatalogRoot;
        std::filesystem::path streamingCatalogJsonPath;
        std::optional<std::filesystem::path> simulateStreamingIoPath;
        bool simulateStreamingIoBatch = false;
        uint32_t streamingIoBatchRequests = 16;
        uint64_t streamingIoBatchChunkBytes = 256ull * 1024ull;
        std::filesystem::path streamingIoJsonPath;
        std::optional<std::string> cameraName;
        std::optional<uint32_t> frameIndex;
        std::vector<std::string> disabledPasses;
        std::vector<std::string> sequenceViewNames;

        auto splitCsv = [](std::string_view value) {
            std::vector<std::string> result;
            std::stringstream stream{std::string(value)};
            std::string item;
            while (std::getline(stream, item, ',')) {
                item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
                item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }).base(), item.end());
                if (!item.empty()) {
                    result.push_back(item);
                }
            }
            return result;
        };

        for (int i = 1; i < argc; ++i) {
            std::string_view arg(argv[i]);

            if (arg == "--check-streaming-budget" && i + 1 < argc) {
                checkStreamingBudgetPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--simulate-rtpkg-compression" && i + 1 < argc) {
                simulateRtpkgCompressionPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--simulate-rtpkg-streaming-io" && i + 1 < argc) {
                simulateRtpkgStreamingIoPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--validate-rtpkg" && i + 1 < argc) {
                validateRtpkgPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--plan-rtpkg-patch" && i + 2 < argc) {
                planRtpkgPatchBasePath = std::filesystem::path(argv[++i]);
                planRtpkgPatchUpdatedPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--rtpkg-compression-profile" && i + 1 < argc) {
                rtpkgCompressionProfile = argv[++i];
                continue;
            }
            if (arg == "--mutate-animation-controller" && i + 1 < argc) {
                mutateAnimationControllerPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--stage-import-emissive-scale" && i + 1 < argc) {
                stageImportEmissiveScale = std::strtof(argv[++i], nullptr);
                continue;
            }
            if (arg == "--stage-import-no-blas-cache") {
                stageImportBuildBlasCache = false;
                continue;
            }
            if (arg == "--controller-mutation-json" && i + 1 < argc) {
                controllerMutationJsonPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--controller-dry-run" || arg == "--controller-mutation-dry-run") {
                controllerMutationDryRun = true;
                continue;
            }
            if (arg == "--controller-force-overwrite") {
                controllerForceOverwrite = true;
                continue;
            }
            if (arg == "--native-fixture-guid" && i + 1 < argc) {
                emitNativeFixtureGuid = argv[++i];
                continue;
            }
            if (arg == "--native-fixture-texture-format" && i + 1 < argc) {
                emitNativeFixtureTextureFormat = parseNativeFixtureTextureFormat(argv[++i]);
                continue;
            }
            if (arg == "--native-fixture-texture-role" && i + 1 < argc) {
                emitNativeFixtureTextureRole = argv[++i];
                continue;
            }
            if (arg == "--native-fixture-material-texture-guid" && i + 1 < argc) {
                emitNativeFixtureMaterialTextureGuid = argv[++i];
                continue;
            }
            if (arg == "--native-package-texture-target-set-json" && i + 1 < argc) {
                try {
                    nativePackageTextureTargetSets = nativeTextureFormatSupportListFromJson(nlohmann::json::parse(argv[++i]));
                } catch (const std::exception& error) {
                    throw std::runtime_error(std::string("Invalid --native-package-texture-target-set-json payload: ") + error.what());
                }
                continue;
            }
            if (arg == "--validate-native-catalog" && i + 1 < argc) {
                validateNativeCatalogPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--streaming-catalog-root" && i + 1 < argc) {
                streamingCatalogRoot = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--streaming-catalog-json" && i + 1 < argc) {
                streamingCatalogJsonPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--simulate-streaming-io" && i + 1 < argc) {
                simulateStreamingIoPath = std::filesystem::path(argv[++i]);
                continue;
            }
            if (arg == "--simulate-streaming-io-batch" && i + 1 < argc) {
                simulateStreamingIoPath = std::filesystem::path(argv[++i]);
                simulateStreamingIoBatch = true;
                continue;
            }
            if (arg == "--streaming-io-batch-requests" && i + 1 < argc) {
                streamingIoBatchRequests = static_cast<uint32_t>(std::stoul(argv[++i]));
                continue;
            }
            if (arg == "--streaming-io-batch-chunk-kb" && i + 1 < argc) {
                streamingIoBatchChunkBytes = static_cast<uint64_t>(std::stoull(argv[++i])) * 1024ull;
                continue;
            }
            if (arg == "--streaming-io-json" && i + 1 < argc) {
                streamingIoJsonPath = std::filesystem::path(argv[++i]);
                continue;
            }

            if (parseGpuUploadTicketSimulationArg(arg, argc, argv, i, gpuUploadSimulation)) {
                continue;
            }
            if (parseMainThreadApplySimulationArg(arg, argc, argv, i, mainThreadApplySimulation)) {
                continue;
            }
            if (parseTopologyRebuildSimulationArg(arg, argc, argv, i, topologyRebuildSimulation)) {
                continue;
            }
            if (parseNativeTextureFormatPolicySimulationArg(arg, argc, argv, i, nativeTextureFormatPolicySimulation)) {
                continue;
            }
            if (parseStreamingSchedulerSimulationArg(arg, argc, argv, i, streamingSchedulerSimulation)) {
                continue;
            }
            if (parseNativeGpuAssetCacheSimulationArg(arg, argc, argv, i, nativeGpuAssetCacheSimulation)) {
                continue;
            }
            if (parseGpuSceneStreamingStateSimulationArg(arg, argc, argv, i, gpuSceneStreamingStateSimulation)) {
                continue;
            }
            if (parseIncrementalGpuSceneUpdateSimulationArg(arg, argc, argv, i, incrementalGpuSceneUpdateSimulation)) {
                continue;
            }
            if (parseStreamingGpuWorkQueueSimulationArg(arg, argc, argv, i, streamingGpuWorkQueueSimulation)) {
                continue;
            }
            if (parseNativePackageAnimationSelectionArg(arg, argc, argv, i, nativePackageAnimationSelection)) {
                continue;
            }
            if (parseStreamingRuntimeArg(arg, argc, argv, i, streamingOptions, dumpStreamingPath, streamingValidationScenePath)) {
                continue;
            }
            if ((arg == "--reflex" || arg == "--streamline-reflex") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                streamlineReflexOverride = !(value == "off" || value == "false" || value == "0");
                continue;
            }

            if (arg == "--frames" && i + 1 < argc) {
                maxFrames = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--compare-profile" && i + 2 < argc) {
                compareProfileOldPath = std::filesystem::path(argv[++i]);
                compareProfileNewPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--compare-image" && i + 2 < argc) {
                compareImageBaselinePath = std::filesystem::path(argv[++i]);
                compareImageCurrentPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--compare-image-sequence" && i + 2 < argc) {
                compareImageSequenceBaselinePath = std::filesystem::path(argv[++i]);
                compareImageSequenceCurrentPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--out" && i + 1 < argc) {
                compareImageOutputPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--debug-view" && i + 1 < argc) {
                debugView = rtv::parseRendererDebugView(argv[++i]);
                debugViewProvided = true;
            } else if (arg == "--gltf" && i + 1 < argc) {
                gltfPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--hdr" && i + 1 < argc) {
                hdrPath = std::filesystem::path(argv[++i]);
            } else if ((arg == "--scene" || arg == "--rtlevel") && i + 1 < argc) {
                scenePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--native-package-scene" && i + 1 < argc) {
                nativePackageScenePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--denoiser" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                denoiserOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--denoiser-backend" || arg == "--denoiser-mode") && i + 1 < argc) {
                denoiserBackendOverride = rtv::parseDenoiserBackend(argv[++i]);
            } else if (arg == "--nrd" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                denoiserBackendOverride = (value == "off" || value == "false" || value == "0")
                    ? rtv::DenoiserBackend::Engine
                    : rtv::DenoiserBackend::Nrd;
            } else if ((arg == "--temporal-upscaler" || arg == "--upscaler") && i + 1 < argc) {
                temporalUpscalerOverride = rtv::parseTemporalUpscaler(argv[++i]);
            } else if (arg == "--dlss" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                temporalUpscalerOverride = (value == "off" || value == "false" || value == "0")
                    ? rtv::TemporalUpscaler::TaaTsr
                    : rtv::TemporalUpscaler::Dlss;
            } else if ((arg == "--dlss-fg" || arg == "--dlss-frame-generation" || arg == "--frame-generation") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                dlssFrameGenerationOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--dlss-rr" || arg == "--dlss-ray-reconstruction" || arg == "--ray-reconstruction") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                dlssRayReconstructionOverride = !(value == "off" || value == "false" || value == "0");
                if (*dlssRayReconstructionOverride) {
                    temporalUpscalerOverride = rtv::TemporalUpscaler::Dlss;
                }
            } else if ((arg == "--dlss-sharpening" || arg == "--dlss-sharpness") && i + 1 < argc) {
                dlssSharpeningOverride = std::stof(argv[++i]);
            } else if (arg == "--restir" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                if (value == "classic" || value == "off" || value == "nee") {
                    restirModeOverride = rtv::RestirMode::ClassicNee;
                } else if (value == "restir" || value == "on" || value == "only") {
                    restirModeOverride = rtv::RestirMode::RestirOnly;
                } else if (value == "hybrid" || value == "compare") {
                    restirModeOverride = rtv::RestirMode::HybridCompare;
                } else {
                    throw std::runtime_error("Unknown ReSTIR mode: " + std::string(value));
                }
            } else if (arg == "--render-preset" && i + 1 < argc) {
                renderPresetOverride = rtv::parseRenderPreset(argv[++i]);
            } else if (arg == "--restir-gi" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                restirGiOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--restir-gi-final-stabilization" || arg == "--restir-gi-stabilization") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                restirGiFinalStabilizationOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--opacity-micromaps" || arg == "--omm") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                opacityMicromapOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--omm-subdivision" || arg == "--opacity-micromap-subdivision") && i + 1 < argc) {
                opacityMicromapSubdivisionOverride = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--wavefront-queues" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontQueuesOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-primary-generate" || arg == "--wavefront-generate") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontPrimaryGenerateOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-trace" || arg == "--wavefront-trace-wrapper") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontTraceOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-shade" || arg == "--wavefront-shade-compute") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontShadeOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-shadow-trace" || arg == "--wavefront-shadow") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontShadowTraceOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-compact" || arg == "--wavefront-queue-compact") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontCompactOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-sort" || arg == "--wavefront-ray-sort") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontSortOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-final-output" || arg == "--wavefront-renderer") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontFinalOutputOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--ser" || arg == "--shader-execution-reordering") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                shaderExecutionReorderingOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--dof-aperture-radius" && i + 1 < argc) {
                dofApertureRadiusOverride = std::stof(argv[++i]);
            } else if ((arg == "--dof-focus-distance" || arg == "--focus-distance") && i + 1 < argc) {
                dofFocusDistanceOverride = std::stof(argv[++i]);
            } else if ((arg == "--dof-blades" || arg == "--dof-blade-count") && i + 1 < argc) {
                dofBladeCountOverride = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if ((arg == "--dof-bokeh-rotation" || arg == "--bokeh-rotation") && i + 1 < argc) {
                dofBokehRotationOverride = std::stof(argv[++i]);
            } else if ((arg == "--motion-blur" || arg == "--rt-motion-blur") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                motionBlurOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--motion-blur-shutter-open" && i + 1 < argc) {
                motionBlurShutterOpenOverride = std::stof(argv[++i]);
            } else if (arg == "--motion-blur-shutter-close" && i + 1 < argc) {
                motionBlurShutterCloseOverride = std::stof(argv[++i]);
            } else if ((arg == "--homogeneous-volume" || arg == "--volume") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                homogeneousVolumeOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--volume-scattering" || arg == "--homogeneous-volume-scattering") && i + 1 < argc) {
                homogeneousVolumeScatteringOverride = std::stof(argv[++i]);
            } else if ((arg == "--volume-absorption" || arg == "--homogeneous-volume-absorption") && i + 1 < argc) {
                homogeneousVolumeAbsorptionOverride = std::stof(argv[++i]);
            } else if ((arg == "--volume-anisotropy" || arg == "--homogeneous-volume-anisotropy") && i + 1 < argc) {
                homogeneousVolumeAnisotropyOverride = std::stof(argv[++i]);
            } else if ((arg == "--mnee-caustics" || arg == "--caustics") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                mneeCausticsOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--wavefront-validation") {
                wavefrontValidationMode = true;
            } else if (arg == "--taa-motion-feedback" && i + 1 < argc) {
                taaMotionFeedbackOverride = std::stof(argv[++i]);
            } else if (arg == "--taa-reactive-feedback" && i + 1 < argc) {
                taaReactiveFeedbackOverride = std::stof(argv[++i]);
            } else if ((arg == "--spp" || arg == "--samples-per-pixel") && i + 1 < argc) {
                samplesPerPixelOverride = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if ((arg == "--spp-limit" || arg == "--limit-spp") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                sppLimiterOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--validation-camera-motion") {
                validationCameraMotion = true;
            } else if (arg == "--validation-object-motion") {
                validationObjectMotion = true;
            } else if (arg == "--headless") {
                diagConfig.headless = true;
            } else if (arg == "--warmup-frames" && i + 1 < argc) {
                diagConfig.warmupFrames = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--fixed-seed" && i + 1 < argc) {
                diagConfig.fixedSeed = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--profile") {
                diagConfig.profile = true;
            } else if (arg == "--profile-json" && i + 1 < argc) {
                diagConfig.profileJsonPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-rendergraph" && i + 1 < argc) {
                diagConfig.dumpRenderGraphPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-rendergraph-dot" && i + 1 < argc) {
                dumpRenderGraphDot = true;
                dotOutputPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--save-debug-views" && i + 1 < argc) {
                diagConfig.saveDebugViewsDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--save-frame-sequence" && i + 1 < argc) {
                diagConfig.saveFrameSequenceDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--sequence-views" && i + 1 < argc) {
                sequenceViewNames = splitCsv(argv[++i]);
            } else if (arg == "--sequence-start-frame" && i + 1 < argc) {
                diagConfig.sequenceStartFrame = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--sequence-frame-count" && i + 1 < argc) {
                diagConfig.sequenceFrameCount = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--sequence-step" && i + 1 < argc) {
                diagConfig.sequenceStep = std::max(1u, static_cast<uint32_t>(std::stoul(argv[++i])));
            } else if (arg == "--capture-renderdoc" && i + 1 < argc) {
                diagConfig.captureRenderDocPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--capture-frame" && i + 1 < argc) {
                diagConfig.captureFrame = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--renderdoc-dll" && i + 1 < argc) {
#ifdef RTV_HAS_RENDERDOC
                rdocDllPathOverride = std::filesystem::path(argv[++i]);
#else
                ++i;
                std::cerr << "Warning: --renderdoc-dll ignored because this build was not configured with RENDERDOC_SDK_DIR.\n";
#endif
            } else if (arg == "--make-debug-package" && i + 1 < argc) {
                diagConfig.makeDebugPackageDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--disable-async-compute") {
                diagConfig.disableAsyncCompute = true;
            } else if (arg == "--single-queue-fallback") {
                diagConfig.singleQueueFallback = true;
            } else if (arg == "--disable-resource-aliasing") {
                diagConfig.disableResourceAliasing = true;
            } else if (arg == "--run-validation-suite") {
                diagConfig.runValidationSuite = true;
            } else if (arg == "--validation-output" && i + 1 < argc) {
                diagConfig.validationOutputDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--update-baseline") {
                updateBaseline = true;
            } else if (arg == "--check-baseline") {
                checkBaseline = true;
            } else if (arg == "--baseline-dir" && i + 1 < argc) {
                baselineRoot = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-memory" && i + 1 < argc) {
                dumpMemoryPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-frame-timeline" && i + 1 < argc) {
                dumpFrameTimelinePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-resource-lifetimes" && i + 1 < argc) {
                dumpResourceLifetimesPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-shader-report" && i + 1 < argc) {
                dumpShaderReportPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-bindings" && i + 1 < argc) {
                dumpBindingsPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--crash-dump-package" && i + 1 < argc) {
                crashDumpPackageDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--validate-gpu-labels") {
                validateGpuLabels = true;
            } else if (arg == "--check-budget" && i + 1 < argc) {
                checkBudgetPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--descriptor-lifetime-stress" && i + 1 < argc) {
                descriptorLifetimeStressPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--descriptor-lifetime-stress-cycles" && i + 1 < argc) {
                descriptorLifetimeStressCycles = std::max(1u, static_cast<uint32_t>(std::stoul(argv[++i])));
            } else if (arg == "--descriptor-lifetime-stress-frames" && i + 1 < argc) {
                descriptorLifetimeStressFrames = std::max(1u, static_cast<uint32_t>(std::stoul(argv[++i])));
            } else if (arg == "--cook-project" && i + 1 < argc) {
                cookProjectPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--cook-output" && i + 1 < argc) {
                cookOutputDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--cook-manifest" && i + 1 < argc) {
                cookManifestPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--cook-animation-controller" && i + 1 < argc) {
                cookAnimationControllerPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--export-animation-controller" && i + 1 < argc) {
                exportAnimationControllerPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--controller-output" && i + 1 < argc) {
                controllerOutputPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--native-output" && i + 1 < argc) {
                nativeOutputPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--native-texture-format-support-json" && i + 1 < argc) {
                try {
                    nativeTextureFormatSupport = nativeTextureFormatSupportFromJson(nlohmann::json::parse(argv[++i]));
                } catch (const std::exception& error) {
                    throw std::runtime_error(std::string("Invalid --native-texture-format-support-json payload: ") + error.what());
                }
            } else if (arg == "--stage-import" && i + 1 < argc) {
                stageImportPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--stage-import-workspace" && i + 1 < argc) {
                stageImportWorkspaceRoot = std::filesystem::path(argv[++i]);
            } else if (arg == "--stage-import-json" && i + 1 < argc) {
                stageImportJsonPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--inspect-native-asset" && i + 1 < argc) {
                inspectNativeAssetPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--inspect-animation-controller" && i + 1 < argc) {
                inspectAnimationControllerPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--inspect-runtime-skeleton" && i + 1 < argc) {
                inspectRuntimeSkeletonPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--inspection-json" && i + 1 < argc) {
                inspectionJsonPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--emit-native-fixture" && i + 1 < argc) {
                emitNativeFixturePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--emit-basisu-ktx2-fixture" && i + 1 < argc) {
                emitBasisuKtx2FixturePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--migrate-native-asset" && i + 1 < argc) {
                migrateNativeAssetPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--migrate-package" && i + 1 < argc) {
                migratePackagePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--migration-report" && i + 1 < argc) {
                migrationReportPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dry-run") {
                migrationDryRun = true;
            } else if (arg == "--inspect-native-store") {
                inspectNativeStore = true;
            } else if (arg.rfind("--native-store-", 0) == 0 && i + 1 < argc) {
                const std::string value(argv[++i]);
                if (arg == "--native-store-package") {
                    nativeStorePackages.push_back(std::filesystem::path(value));
                } else if (arg == "--native-store-root") {
                    nativeStoreRoots.push_back(std::filesystem::path(value));
                } else if (arg == "--native-store-query") {
                    nativeStoreQueries.push_back(value);
                } else if (arg == "--native-store-retain") {
                    nativeStoreRetains.push_back(value);
                } else if (arg == "--native-store-release") {
                    nativeStoreReleases.push_back(value);
                } else if (arg == "--native-store-unmount-package") {
                    nativeStoreUnmountPackages.push_back(std::filesystem::path(value));
                }
            } else if (arg == "--load-native-runtime-assets" && i + 1 < argc) {
                loadNativeRuntimeAssetsPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--write-rtpkg" && i + 1 < argc) {
                writeRtpkgPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--package-input" && i + 1 < argc) {
                rtpkgInputs.push_back(std::filesystem::path(argv[++i]));
            } else if (arg == "--package-root" && i + 1 < argc) {
                rtpkgRoot = std::filesystem::path(argv[++i]);
            } else if (arg == "--inspect-package" && i + 1 < argc) {
                inspectRtpkgPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--native-asset-kind" && i + 1 < argc) {
                emitNativeFixtureKind = parseNativeAssetKindName(argv[++i]);
            } else if (arg == "--shader-hot-reload-report") {
                shaderHotReloadReport = true;
            } else if (arg == "--disable-pass" && i + 1 < argc) {
                disabledPasses.push_back(argv[++i]);
            } else if (arg == "--camera" && i + 1 < argc) {
                cameraName = std::string(argv[++i]);
            } else if (arg == "--frame-index" && i + 1 < argc) {
                frameIndex = static_cast<uint32_t>(std::stoul(argv[++i]));
            }
        }

        if (compareProfileOldPath.has_value() || compareProfileNewPath.has_value()) {
            if (!compareProfileOldPath.has_value() || !compareProfileNewPath.has_value()) {
                throw std::runtime_error("--compare-profile requires old.json and new.json");
            }
            return rtv::compareProfileCommand(*compareProfileOldPath, *compareProfileNewPath);
        }
        if (compareImageBaselinePath.has_value() || compareImageCurrentPath.has_value()) {
            if (!compareImageBaselinePath.has_value() || !compareImageCurrentPath.has_value()) {
                throw std::runtime_error("--compare-image requires baseline.png and current.png");
            }
            return rtv::compareImageCommand(*compareImageBaselinePath, *compareImageCurrentPath, compareImageOutputPath);
        }
        if (compareImageSequenceBaselinePath.has_value() || compareImageSequenceCurrentPath.has_value()) {
            if (!compareImageSequenceBaselinePath.has_value() || !compareImageSequenceCurrentPath.has_value()) {
                throw std::runtime_error("--compare-image-sequence requires baseline_dir and current_dir");
            }
            return rtv::compareImageSequenceCommand(
                *compareImageSequenceBaselinePath,
                *compareImageSequenceCurrentPath,
                compareImageOutputPath,
                sequenceViewNames);
        }
        if (gpuUploadSimulation.enabled) {
            if (gpuUploadSimulation.cancelBeforeSubmit && gpuUploadSimulation.cancelAfterSubmit) {
                throw std::runtime_error("--upload-cancel-before-submit and --upload-cancel-after-submit are mutually exclusive");
            }
            return simulateGpuUploadTicketCommand(
                inspectionJsonPath.value_or(std::filesystem::path{}),
                gpuUploadSimulation.totalBytes,
                gpuUploadSimulation.chunkBytes,
                gpuUploadSimulation.frameByteLimit,
                gpuUploadSimulation.cancelBeforeSubmit,
                gpuUploadSimulation.cancelAfterSubmit);
        }
        if (mainThreadApplySimulation.enabled) {
            return simulateMainThreadApplyCommand(
                inspectionJsonPath.value_or(std::filesystem::path{}),
                mainThreadApplySimulation.operationCount,
                mainThreadApplySimulation.operationCostMs,
                mainThreadApplySimulation.frameBudgetMs,
                mainThreadApplySimulation.cancelAfterFrame);
        }
        if (topologyRebuildSimulation.enabled) {
            return simulateTopologyRebuildCommand(
                inspectionJsonPath.value_or(std::filesystem::path{}),
                topologyRebuildSimulation.stageCostMs,
                topologyRebuildSimulation.frameBudgetMs,
                topologyRebuildSimulation.newerEditFrame);
        }
        if (nativeTextureFormatPolicySimulation.enabled) {
            return simulateNativeTextureFormatPolicyCommand(inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (streamingSchedulerSimulation.enabled) {
            return rtv::simulateStreamingSchedulerCommand(
                streamingSchedulerSimulation.taskCount,
                streamingSchedulerSimulation.budget,
                streamingSchedulerSimulation.cancelAfterFrame,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (nativeGpuAssetCacheSimulation.enabled) {
            return rtv::simulateNativeGpuAssetCacheCommand(
                nativeGpuAssetCacheSimulation.assetCount,
                nativeGpuAssetCacheSimulation.budget,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (gpuSceneStreamingStateSimulation.enabled) {
            return rtv::simulateGpuSceneStreamingStateCommand(
                gpuSceneStreamingStateSimulation.instanceCount,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (incrementalGpuSceneUpdateSimulation.enabled) {
            return rtv::simulateIncrementalGpuSceneUpdateQueueCommand(
                incrementalGpuSceneUpdateSimulation.instanceCount,
                incrementalGpuSceneUpdateSimulation.budget,
                incrementalGpuSceneUpdateSimulation.cancelAfterFrame,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (streamingGpuWorkQueueSimulation.enabled) {
            return rtv::simulateStreamingGpuWorkQueueCommand(
                streamingGpuWorkQueueSimulation.ticketCount,
                streamingGpuWorkQueueSimulation.budget,
                streamingGpuWorkQueueSimulation.completeLagFrames,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (validateNativeCatalogPath.has_value()) {
            return rtv::validateNativeAssetCatalogCommand(
                *validateNativeCatalogPath,
                streamingCatalogRoot,
                streamingCatalogJsonPath.empty() ? inspectionJsonPath.value_or(std::filesystem::path{}) : streamingCatalogJsonPath);
        }
        if (simulateStreamingIoPath.has_value()) {
            if (simulateStreamingIoBatch) {
                return rtv::simulateStreamingIoBatchCommand(
                    *simulateStreamingIoPath,
                    streamingOptions,
                    streamingIoBatchRequests,
                    streamingIoBatchChunkBytes,
                    streamingIoJsonPath.empty() ? inspectionJsonPath.value_or(std::filesystem::path{}) : streamingIoJsonPath);
            }
            return rtv::simulateStreamingIoBackendCommand(
                *simulateStreamingIoPath,
                streamingOptions,
                streamingIoJsonPath.empty() ? inspectionJsonPath.value_or(std::filesystem::path{}) : streamingIoJsonPath);
        }
        if (emitBasisuKtx2FixturePath.has_value()) {
            return emitBasisuKtx2FixtureCommand(*emitBasisuKtx2FixturePath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (cookProjectPath.has_value()) {
            return cookProjectCommand(*cookProjectPath, cookOutputDir, cookManifestPath, nativeTextureFormatSupport, nativePackageTextureTargetSets);
        }
        if (cookAnimationControllerPath.has_value()) {
            return cookAnimationControllerCommand(*cookAnimationControllerPath, nativeOutputPath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (exportAnimationControllerPath.has_value()) {
            return rtv::exportAnimationControllerCommand(*exportAnimationControllerPath, controllerOutputPath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (mutateAnimationControllerPath.has_value()) {
            return rtv::mutateAnimationControllerCommand(
                *mutateAnimationControllerPath,
                controllerMutationJsonPath,
                controllerOutputPath,
                inspectionJsonPath.value_or(std::filesystem::path{}),
                controllerMutationDryRun,
                controllerForceOverwrite);
        }
        if (stageImportPath.has_value()) {
            return stageImportCommand(*stageImportPath, stageImportWorkspaceRoot, stageImportJsonPath, nativeTextureFormatSupport, stageImportEmissiveScale, stageImportBuildBlasCache);
        }
        if (emitNativeFixturePath.has_value()) {
            return rtv::emitNativeAssetFixtureCommand(*emitNativeFixturePath, emitNativeFixtureKind, emitNativeFixtureGuid, emitNativeFixtureTextureFormat, emitNativeFixtureMaterialTextureGuid, emitNativeFixtureTextureRole);
        }
        if (inspectNativeAssetPath.has_value()) {
            return rtv::inspectNativeAssetCommand(*inspectNativeAssetPath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (inspectAnimationControllerPath.has_value()) {
            return rtv::inspectAnimationControllerCommand(*inspectAnimationControllerPath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (inspectRuntimeSkeletonPath.has_value()) {
            return rtv::inspectRuntimeSkeletonCommand(*inspectRuntimeSkeletonPath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (migrateNativeAssetPath.has_value()) {
            return rtv::migrateNativeAssetCommand(*migrateNativeAssetPath, migrationReportPath, migrationDryRun);
        }
        if (migratePackagePath.has_value()) {
            return rtv::migratePackageCommand(*migratePackagePath, migrationReportPath, migrationDryRun);
        }
        if (inspectNativeStore) {
            return rtv::inspectNativeAssetStoreCommand(
                nativeStorePackages,
                nativeStoreRoots,
                nativeStoreQueries,
                nativeStoreRetains,
                nativeStoreReleases,
                nativeStoreUnmountPackages,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (loadNativeRuntimeAssetsPath.has_value()) {
            return rtv::loadNativeRuntimeAssetsCommand(*loadNativeRuntimeAssetsPath, inspectionJsonPath.value_or(std::filesystem::path{}), nativeTextureFormatSupport);
        }
        if (writeRtpkgPath.has_value()) {
            return rtv::writeRtpkgCommand(*writeRtpkgPath, rtpkgInputs, rtpkgRoot);
        }
        if (inspectRtpkgPath.has_value()) {
            return rtv::inspectRtpkgCommand(*inspectRtpkgPath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (validateRtpkgPath.has_value()) {
            return rtv::validateRtpkgCommand(*validateRtpkgPath, inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (planRtpkgPatchBasePath.has_value() || planRtpkgPatchUpdatedPath.has_value()) {
            if (!planRtpkgPatchBasePath.has_value() || !planRtpkgPatchUpdatedPath.has_value()) {
                throw std::runtime_error("--plan-rtpkg-patch requires base.rtpkg and updated.rtpkg");
            }
            return rtv::planRtpkgPatchCommand(
                *planRtpkgPatchBasePath,
                *planRtpkgPatchUpdatedPath,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (simulateRtpkgCompressionPath.has_value()) {
            return rtv::simulateRtpkgCompressionCommand(
                *simulateRtpkgCompressionPath,
                rtpkgCompressionProfile,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }
        if (simulateRtpkgStreamingIoPath.has_value()) {
            return rtv::simulateRtpkgStreamingIoCommand(
                *simulateRtpkgStreamingIoPath,
                streamingOptions,
                inspectionJsonPath.value_or(std::filesystem::path{}));
        }

        for (const std::string& viewName : sequenceViewNames) {
            diagConfig.sequenceViews.push_back(rtv::parseRendererDebugView(viewName));
        }

        if (diagConfig.headless && maxFrames == 0) {
            maxFrames = diagConfig.totalFrames;
        }
        if (streamingValidationScenePath.has_value() && !scenePath.has_value() && !gltfPath.has_value() && !nativePackageScenePath.has_value()) {
            scenePath = *streamingValidationScenePath;
        }
        if (maxFrames != 0) {
            diagConfig.totalFrames = maxFrames;
        }
        diagConfig.wavefrontValidationMode = wavefrontValidationMode;

        if (diagConfig.runValidationSuite) {
            rtv::HeadlessDiagnostics diag(diagConfig);
            const auto summary = diag.runValidationSuite();
            std::cout << "Validation suite: " << summary.totalPass << " passed, "
                      << summary.totalFail << " failed\n";
            return summary.totalFail > 0 ? 1 : 0;
        }

        const bool baselineMode = updateBaseline || checkBaseline;
        const bool needsProfile =
            diagConfig.profile ||
            diagConfig.profileJsonPath.has_value() ||
            baselineMode ||
            dumpMemoryPath.has_value() ||
            dumpFrameTimelinePath.has_value() ||
            checkBudgetPath.has_value() ||
            checkStreamingBudgetPath.has_value() ||
            crashDumpPackageDir.has_value() ||
            wavefrontValidationMode;
        const bool needsRenderGraph =
            baselineMode ||
            dumpFrameTimelinePath.has_value() ||
            dumpResourceLifetimesPath.has_value() ||
            dumpBindingsPath.has_value() ||
            validateGpuLabels ||
            crashDumpPackageDir.has_value();
        const bool needsDebugViews = baselineMode || crashDumpPackageDir.has_value();
        if (needsProfile) {
            diagConfig.profile = true;
        }
        std::filesystem::path diagnosticSourcePath = "scene";
        if (scenePath.has_value()) {
            diagnosticSourcePath = *scenePath;
        } else if (gltfPath.has_value()) {
            diagnosticSourcePath = *gltfPath;
        } else if (nativePackageScenePath.has_value()) {
            diagnosticSourcePath = *nativePackageScenePath;
        }
        const std::filesystem::path artifactBase =
            rtv::defaultDiagnosticArtifactDir(diagnosticSourcePath, "current");
        if (needsProfile && !diagConfig.profileJsonPath.has_value()) {
            diagConfig.profileJsonPath = artifactBase / "profile.json";
        }
        if (needsRenderGraph && !diagConfig.dumpRenderGraphPath.has_value()) {
            diagConfig.dumpRenderGraphPath = artifactBase / "rendergraph.json";
        }
        if (needsDebugViews && !diagConfig.saveDebugViewsDir.has_value()) {
            diagConfig.saveDebugViewsDir = artifactBase / "debug_views";
        }
        if (shaderHotReloadReport && !dumpShaderReportPath.has_value()) {
            dumpShaderReportPath = artifactBase / "shader_hot_reload_report.json";
        }
        if (checkBaseline) {
            const rtv::BaselinePaths paths = rtv::baselinePathsFor(diagnosticSourcePath, baselineRoot);
            if (std::filesystem::exists(paths.frameSequence) && !diagConfig.saveFrameSequenceDir.has_value()) {
                diagConfig.saveFrameSequenceDir = artifactBase / "frame_sequence";
            }
        }
        if (frameIndex.has_value() && !diagConfig.fixedSeed.has_value()) {
            diagConfig.fixedSeed = *frameIndex;
        }

        if (nativePackageScenePath.has_value() && (scenePath.has_value() || gltfPath.has_value())) {
            throw std::runtime_error("--native-package-scene is mutually exclusive with --scene and --gltf");
        }
        const bool nativePackageAnimationSelectionRequested = !nativePackageAnimationSelection.controllerGuid.empty() ||
            !nativePackageAnimationSelection.controllerPath.empty() ||
            !nativePackageAnimationSelection.entityName.empty() ||
            nativePackageAnimationSelection.entityUuid != 0;
        if (nativePackageAnimationSelectionRequested && !nativePackageScenePath.has_value()) {
            throw std::runtime_error("Native package animation selection flags require --native-package-scene <path>");
        }
        if (diagConfig.headless && !scenePath.has_value() && !gltfPath.has_value() && !nativePackageScenePath.has_value()) {
            throw std::runtime_error("--headless requires --scene <path>, --gltf <path>, or --native-package-scene <path>");
        }
        if (diagConfig.saveFrameSequenceDir.has_value() && !diagConfig.headless) {
            throw std::runtime_error("--save-frame-sequence requires --headless");
        }
        if (descriptorLifetimeStressPath.has_value() && !diagConfig.headless) {
            throw std::runtime_error("--descriptor-lifetime-stress requires --headless");
        }

#ifdef RTV_HAS_RENDERDOC
        if (diagConfig.captureRenderDocPath.has_value()) {
            rdocCaptureRequested = true;
            rdocCapturePath = *diagConfig.captureRenderDocPath;
            rdocCaptureFrame = std::max(1u, diagConfig.captureFrame);
            initRenderDoc();
        }
#else
        if (diagConfig.captureRenderDocPath.has_value()) {
            std::cerr << "Warning: RenderDoc capture requested, but this build was not configured with RENDERDOC_SDK_DIR.\n";
        }
#endif

        rtv::Application app(debugView, gltfPath, hdrPath, scenePath, nativePackageScenePath,
            nativePackageAnimationSelection,
            denoiserOverride, restirModeOverride, renderPresetOverride, restirGiOverride,
            opacityMicromapOverride,
            opacityMicromapSubdivisionOverride,
            debugViewProvided, validationCameraMotion, validationObjectMotion,
            diagConfig.headless,
            diagConfig.disableAsyncCompute,
            diagConfig.singleQueueFallback,
            diagConfig.disableResourceAliasing,
            streamingOptions);

        if (auto* renderer = app.pathTracer()) {
            renderer->setRayTracingDiagnosticCountersEnabled(diagConfig.profile);
            auto lower = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                return value;
            };
            if (diagConfig.fixedSeed.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.fixedSeed = diagConfig.fixedSeed;
                renderer->applySettings(settings);
            }
            if (restirGiFinalStabilizationOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.restirGiFinalStabilizationEnabled = *restirGiFinalStabilizationOverride;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (denoiserBackendOverride.has_value() ||
                temporalUpscalerOverride.has_value() ||
                dlssFrameGenerationOverride.has_value() ||
                dlssRayReconstructionOverride.has_value() ||
                streamlineReflexOverride.has_value() ||
                dlssSharpeningOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (denoiserBackendOverride.has_value()) {
                    settings.denoiserBackend = *denoiserBackendOverride;
                }
                if (temporalUpscalerOverride.has_value()) {
                    settings.temporalUpscaler = *temporalUpscalerOverride;
                }
                if (dlssFrameGenerationOverride.has_value()) {
                    settings.dlssFrameGenerationEnabled = *dlssFrameGenerationOverride;
                }
                if (dlssRayReconstructionOverride.has_value()) {
                    settings.dlssRayReconstructionEnabled = *dlssRayReconstructionOverride;
                }
                if (streamlineReflexOverride.has_value()) {
                    settings.streamlineReflexEnabled = *streamlineReflexOverride;
                }
                if (dlssSharpeningOverride.has_value()) {
                    settings.dlssSharpeningStrength = std::clamp(*dlssSharpeningOverride, 0.0f, 1.0f);
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
                const auto nvidiaStatus = renderer->nvidiaIntegrationStatus();
                if (settings.denoiserBackend == rtv::DenoiserBackend::Nrd && !nvidiaStatus.nrdAvailable) {
                    std::cerr << "Warning: NRD denoiser requested, but unavailable: "
                              << nvidiaStatus.nrdUnavailableReason << ". Using engine denoiser.\n";
                }
                if (settings.temporalUpscaler == rtv::TemporalUpscaler::Dlss && !nvidiaStatus.dlssAvailable) {
                    std::cerr << "Warning: DLSS requested, but unavailable: "
                              << nvidiaStatus.dlssUnavailableReason << ". Using TAA/TSR.\n";
                }
                if (settings.dlssRayReconstructionEnabled && !nvidiaStatus.dlssRayReconstructionAvailable) {
                    std::cerr << "Warning: DLSS Ray Reconstruction requested, but unavailable: "
                              << nvidiaStatus.dlssRayReconstructionUnavailableReason << ".\n";
                }
                if (settings.streamlineReflexEnabled && !nvidiaStatus.streamlineReflex.requestable) {
                    std::cerr << "Warning: Streamline Reflex requested, but unavailable: "
                              << nvidiaStatus.streamlineReflex.unavailableReason << ".\n";
                }
                if (settings.dlssFrameGenerationEnabled && !nvidiaStatus.dlssFrameGenerationAvailable) {
                    std::cerr << "Warning: DLSS Frame Generation requested, but unavailable: "
                              << nvidiaStatus.dlssFrameGenerationUnavailableReason << ".\n";
                }
            }
            if (taaMotionFeedbackOverride.has_value() || taaReactiveFeedbackOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (taaMotionFeedbackOverride.has_value()) {
                    settings.taaMotionFeedback = *taaMotionFeedbackOverride;
                }
                if (taaReactiveFeedbackOverride.has_value()) {
                    settings.taaReactiveFeedback = *taaReactiveFeedbackOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (samplesPerPixelOverride.has_value() || sppLimiterOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (samplesPerPixelOverride.has_value()) {
                    settings.samplesPerPixel = *samplesPerPixelOverride;
                }
                if (sppLimiterOverride.has_value()) {
                    settings.limitSamplesPerPixel = *sppLimiterOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontQueuesOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontQueuesEnabled = *wavefrontQueuesOverride;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontPrimaryGenerateOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontPrimaryGenerateEnabled = *wavefrontPrimaryGenerateOverride;
                if (*wavefrontPrimaryGenerateOverride) {
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontTraceOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontTraceEnabled = *wavefrontTraceOverride;
                if (*wavefrontTraceOverride) {
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontShadeOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontShadeEnabled = *wavefrontShadeOverride;
                if (*wavefrontShadeOverride) {
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontShadowTraceOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontShadowTraceEnabled = *wavefrontShadowTraceOverride;
                if (*wavefrontShadowTraceOverride) {
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontCompactOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontCompactEnabled = *wavefrontCompactOverride;
                if (*wavefrontCompactOverride) {
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontSortOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontSortEnabled = *wavefrontSortOverride;
                if (*wavefrontSortOverride) {
                    settings.wavefrontCompactEnabled = true;
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontFinalOutputOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontFinalOutputEnabled = *wavefrontFinalOutputOverride;
                if (*wavefrontFinalOutputOverride) {
                    settings.wavefrontShadowTraceEnabled = true;
                    settings.wavefrontCompactEnabled = true;
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (shaderExecutionReorderingOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.shaderExecutionReorderingEnabled = *shaderExecutionReorderingOverride;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (dofApertureRadiusOverride.has_value() || dofFocusDistanceOverride.has_value() ||
                dofBladeCountOverride.has_value() || dofBokehRotationOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (dofApertureRadiusOverride.has_value()) {
                    settings.dofApertureRadius = *dofApertureRadiusOverride;
                }
                if (dofFocusDistanceOverride.has_value()) {
                    settings.dofFocusDistance = *dofFocusDistanceOverride;
                }
                if (dofBladeCountOverride.has_value()) {
                    settings.dofBladeCount = *dofBladeCountOverride;
                }
                if (dofBokehRotationOverride.has_value()) {
                    settings.dofBokehRotation = *dofBokehRotationOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (motionBlurOverride.has_value() || motionBlurShutterOpenOverride.has_value() || motionBlurShutterCloseOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (motionBlurOverride.has_value()) {
                    settings.motionBlurEnabled = *motionBlurOverride;
                }
                if (motionBlurShutterOpenOverride.has_value()) {
                    settings.motionBlurShutterOpen = *motionBlurShutterOpenOverride;
                }
                if (motionBlurShutterCloseOverride.has_value()) {
                    settings.motionBlurShutterClose = *motionBlurShutterCloseOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (homogeneousVolumeOverride.has_value() ||
                homogeneousVolumeScatteringOverride.has_value() ||
                homogeneousVolumeAbsorptionOverride.has_value() ||
                homogeneousVolumeAnisotropyOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (homogeneousVolumeOverride.has_value()) {
                    settings.homogeneousVolumeEnabled = *homogeneousVolumeOverride;
                } else {
                    settings.homogeneousVolumeEnabled = true;
                }
                if (homogeneousVolumeScatteringOverride.has_value()) {
                    settings.homogeneousVolumeScattering = *homogeneousVolumeScatteringOverride;
                }
                if (homogeneousVolumeAbsorptionOverride.has_value()) {
                    settings.homogeneousVolumeAbsorption = *homogeneousVolumeAbsorptionOverride;
                }
                if (homogeneousVolumeAnisotropyOverride.has_value()) {
                    settings.homogeneousVolumeAnisotropy = *homogeneousVolumeAnisotropyOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (mneeCausticsOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.mneeCausticsEnabled = *mneeCausticsOverride;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontValidationMode) {
                rtv::RendererSettings settings = renderer->settings();
                settings.restirMode = rtv::RestirMode::ClassicNee;
                settings.restirGiEnabled = false;
                settings.wavefrontQueuesEnabled = true;
                settings.wavefrontPrimaryGenerateEnabled = true;
                settings.wavefrontTraceEnabled = true;
                settings.wavefrontShadeEnabled = true;
                settings.wavefrontShadowTraceEnabled = true;
                settings.wavefrontCompactEnabled = true;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (!disabledPasses.empty()) {
                rtv::RendererSettings settings = renderer->settings();
                for (const std::string& pass : disabledPasses) {
                    const std::string name = lower(pass);
                    if (name == "denoiser" || name == "temporaldenoiser") {
                        settings.denoiserEnabled = false;
                    } else if (name == "taa" || name == "tsr") {
                        settings.taaEnabled = false;
                    } else if (name == "restir") {
                        settings.restirMode = rtv::RestirMode::ClassicNee;
                        settings.restirGiEnabled = false;
                    } else if (name == "restirdi" || name == "restirspatial") {
                        settings.restirMode = rtv::RestirMode::ClassicNee;
                    } else if (name == "restirgi") {
                        settings.restirGiEnabled = false;
                    } else if (name == "autoexposure") {
                        settings.autoExposureEnabled = false;
                    } else {
                        std::cerr << "Warning: unknown --disable-pass value: " << pass << "\n";
                    }
                }
                renderer->applySettings(settings);
            }
            if (cameraName.has_value()) {
                if (!app.applyNamedCamera(*cameraName)) {
                    std::cerr << "Warning: --camera " << *cameraName
                              << " did not match a scene camera or built-in diagnostic camera; using the active scene camera.\n";
                }
            }
            if (diagConfig.dumpRenderGraphPath.has_value()) {
                renderer->setDumpRenderGraphPath(diagConfig.dumpRenderGraphPath);
            }
            if (dumpRenderGraphDot && dotOutputPath.has_value()) {
                renderer->setDumpRenderGraphDotPath(dotOutputPath);
            }
            if (diagConfig.disableAsyncCompute || diagConfig.singleQueueFallback) {
                rtv::RendererSettings settings = renderer->settings();
                settings.adaptiveQualityMode = rtv::AdaptiveQualityMode::Off;
                renderer->applySettings(settings);
            }
        }

        rtv::HeadlessDiagnostics diag(diagConfig);
        if (diagConfig.makeDebugPackageDir.has_value() || crashDumpPackageDir.has_value()) {
            diag.captureStdout();
        }

#ifdef RTV_HAS_RENDERDOC
        bool rdocCaptureStarted = false;
        bool rdocCaptureFinished = false;
        if (rdocCaptureRequested && rdocApi != nullptr) {
            const std::filesystem::path absoluteCapturePath = std::filesystem::absolute(rdocCapturePath);
            const auto captureDir = absoluteCapturePath.parent_path();
            if (!captureDir.empty()) {
                std::filesystem::create_directories(captureDir);
            }
            rdocCaptureTemplate = absoluteCapturePath.string();
            rdocApi->SetCaptureFilePathTemplate(rdocCaptureTemplate.c_str());
            app.setFrameCaptureCallbacks(
                [&](uint32_t frameNumber) {
                    if (!rdocCaptureStarted && frameNumber == rdocCaptureFrame) {
                        rdocApi->StartFrameCapture(nullptr, nullptr);
                        rdocCaptureStarted = true;
                        std::cout << "RenderDoc capture started at frame " << frameNumber << "\n";
                    }
                },
                [&](uint32_t frameNumber) {
                    if (rdocCaptureStarted && !rdocCaptureFinished && frameNumber == rdocCaptureFrame) {
                        const uint32_t captureSaved = rdocApi->EndFrameCapture(nullptr, nullptr);
                        rdocCaptureFinished = true;
                        if (captureSaved != 0u) {
                            std::cout << "RenderDoc capture saved to template: " << rdocCaptureTemplate << "\n";
                        } else {
                            std::cerr << "Warning: RenderDoc capture ended but was not saved.\n";
                        }
                    }
                });
        }
#endif

        if (diagConfig.headless) {
            app.runHeadless(diagConfig.warmupFrames, maxFrames);
        } else {
            app.run(maxFrames);
        }

        bool descriptorLifetimeStressPassed = true;
        if (descriptorLifetimeStressPath.has_value()) {
            descriptorLifetimeStressPassed = app.runDescriptorLifetimeStress(
                *descriptorLifetimeStressPath,
                descriptorLifetimeStressCycles,
                descriptorLifetimeStressFrames);
        }

#ifdef RTV_HAS_RENDERDOC
        if (rdocCaptureRequested && rdocApi != nullptr && !rdocCaptureFinished) {
            std::cerr << "Warning: RenderDoc capture frame " << rdocCaptureFrame
                      << " was not reached before shutdown.\n";
        }
#endif

        if (diagConfig.profile || diagConfig.saveDebugViewsDir.has_value() ||
            diagConfig.saveFrameSequenceDir.has_value() ||
            diagConfig.dumpRenderGraphPath.has_value() || diagConfig.makeDebugPackageDir.has_value() ||
            dumpMemoryPath.has_value() || dumpFrameTimelinePath.has_value() ||
            dumpResourceLifetimesPath.has_value() || dumpBindingsPath.has_value() ||
            dumpShaderReportPath.has_value() || crashDumpPackageDir.has_value() ||
            baselineMode || validateGpuLabels || checkBudgetPath.has_value() || checkStreamingBudgetPath.has_value()) {
            diag.run(app);

            if (diagConfig.profileJsonPath.has_value()) {
                diag.writeProfileJson(*diagConfig.profileJsonPath);
            }
            if (diagConfig.saveDebugViewsDir.has_value()) {
                diag.exportDebugViews(app, *diagConfig.saveDebugViewsDir);
            }
            if (diagConfig.saveFrameSequenceDir.has_value()) {
                diag.exportFrameSequence(app, *diagConfig.saveFrameSequenceDir);
            }
            if (diagConfig.makeDebugPackageDir.has_value()) {
                diag.makeDebugPackage(app, *diagConfig.makeDebugPackageDir, diagnosticSourcePath);
            }
            if (crashDumpPackageDir.has_value()) {
                diag.makeDebugPackage(app, *crashDumpPackageDir, diagnosticSourcePath);
            }
        }

        std::string capturedLog;
        if (crashDumpPackageDir.has_value()) {
            capturedLog = diag.releaseStdout();
        }

        int finalExitCode = 0;
        if (!descriptorLifetimeStressPassed) {
            finalExitCode = 1;
        }
        if (dumpMemoryPath.has_value()) {
            rtv::writeMemoryReport(*dumpMemoryPath, diag.profileReport(), diagConfig.dumpRenderGraphPath);
        }
        if (dumpFrameTimelinePath.has_value()) {
            rtv::writeFrameTimeline(*dumpFrameTimelinePath, diag.profileReport(), diagConfig.dumpRenderGraphPath);
        }
        if (dumpResourceLifetimesPath.has_value()) {
            rtv::writeResourceLifetimes(*dumpResourceLifetimesPath, diagConfig.dumpRenderGraphPath);
        }
        if (dumpShaderReportPath.has_value()) {
            const auto shaderDir = std::filesystem::current_path() / "shaders";
            const auto shaderOutDir = std::filesystem::current_path() / "build" / "shaders";
            rtv::writeShaderReport(*dumpShaderReportPath, shaderDir, shaderOutDir);
        }
        if (dumpBindingsPath.has_value()) {
            rtv::writeBindingsReport(*dumpBindingsPath, diagConfig.dumpRenderGraphPath);
        }
        std::optional<nlohmann::json> streamingBudgetReport;
        if (dumpStreamingPath.has_value() || checkStreamingBudgetPath.has_value()) {
            std::string writeError;
            nlohmann::json report = app.streamingRuntimeReport();
            report["diagnostic_source"] = diagnosticSourcePath.generic_string();
            report["streaming_validation_scene"] = streamingValidationScenePath.has_value()
                ? streamingValidationScenePath->generic_string()
                : std::string{};
            streamingBudgetReport = report;
            if (dumpStreamingPath.has_value() && !writeJsonFile(*dumpStreamingPath, report, &writeError)) {
                std::cerr << "Could not write streaming JSON: " << writeError << '\n';
                finalExitCode = std::max(finalExitCode, 1);
            }
        }
        if (validateGpuLabels) {
            finalExitCode = std::max(finalExitCode, rtv::validateGpuLabels(diagConfig.dumpRenderGraphPath));
        }
        if (checkBudgetPath.has_value()) {
            finalExitCode = std::max(finalExitCode, rtv::checkBudget(*checkBudgetPath, diag.profileReport()));
        }
        if (checkStreamingBudgetPath.has_value()) {
            finalExitCode = std::max(
                finalExitCode,
                rtv::checkStreamingBudget(*checkStreamingBudgetPath, diag.profileReport(), *streamingBudgetReport));
        }
        if (baselineMode) {
            const rtv::BaselinePaths paths = rtv::baselinePathsFor(diagnosticSourcePath, baselineRoot);
            if (!diagConfig.profileJsonPath.has_value() ||
                !diagConfig.dumpRenderGraphPath.has_value() ||
                !diagConfig.saveDebugViewsDir.has_value()) {
                throw std::runtime_error("Baseline mode requires profile, render graph, and debug view artifacts");
            }
            if (updateBaseline) {
                rtv::updateBaseline(paths, *diagConfig.profileJsonPath, *diagConfig.dumpRenderGraphPath,
                    *diagConfig.saveDebugViewsDir, diagConfig.saveFrameSequenceDir);
            }
            if (checkBaseline) {
                finalExitCode = std::max(finalExitCode,
                    rtv::checkBaseline(paths, *diagConfig.profileJsonPath, *diagConfig.dumpRenderGraphPath, *diagConfig.saveDebugViewsDir));
            }
        }
        if (crashDumpPackageDir.has_value()) {
            rtv::writeCrashDumpPackage(
                *crashDumpPackageDir,
                diagnosticSourcePath,
                diagConfig.profileJsonPath,
                diagConfig.dumpRenderGraphPath,
                diagConfig.saveDebugViewsDir,
                capturedLog);
        }

        return finalExitCode;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
