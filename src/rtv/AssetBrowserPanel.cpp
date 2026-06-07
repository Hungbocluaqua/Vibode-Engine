#include "rtv/AssetBrowserPanel.h"

#include "rtv/AssetImport.h"
#include "rtv/AssetManager.h"
#include "rtv/EditorPreferences.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/FileDialog.h"
#include "rtv/GpuScene.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <nlohmann/json.hpp>

#include <stb_image.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#endif

#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rtv {

namespace {

template <size_t N>
void setTextBuffer(std::array<char, N>& buffer, const std::string& value) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::memcpy(buffer.data(), value.data(), std::min(value.size(), buffer.size() - 1));
}

template <size_t N>
void setPathBuffer(std::array<char, N>& buffer, const std::filesystem::path& path) {
    setTextBuffer(buffer, path.string());
}

std::filesystem::path canonicalForCompare(const std::filesystem::path& path);

std::string lowerString(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string trimString(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

std::string quoteCommandPath(const std::filesystem::path& path) {
    std::string value = path.string();
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string readCommandOutput(const std::string& command) {
    std::string output;
#ifdef _WIN32
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &securityAttributes, 0)) {
        return output;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdOutput = writePipe;
    startupInfo.hStdError = writePipe;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION processInfo{};
    HANDLE jobHandle = CreateJobObjectA(nullptr, nullptr);
    if (jobHandle != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo))) {
            CloseHandle(jobHandle);
            jobHandle = nullptr;
        }
    }
    std::string commandLine = "cmd.exe /C " + command;
    const DWORD creationFlags = CREATE_NO_WINDOW | (jobHandle != nullptr ? CREATE_SUSPENDED : 0u);
    if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, creationFlags, nullptr, nullptr, &startupInfo, &processInfo)) {
        if (jobHandle != nullptr) {
            CloseHandle(jobHandle);
        }
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return output;
    }
    if (jobHandle != nullptr) {
        if (!AssignProcessToJobObject(jobHandle, processInfo.hProcess)) {
            CloseHandle(jobHandle);
            jobHandle = nullptr;
        }
        ResumeThread(processInfo.hThread);
    }
    CloseHandle(writePipe);

    constexpr DWORD kProbeTimeoutMs = 1500;
    constexpr size_t kMaxOutputBytes = 64u * 1024u;
    const auto started = std::chrono::steady_clock::now();
    std::array<char, 512> buffer{};
    bool running = true;
    while (running) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD bytesRead = 0;
            const DWORD bytesToRead = static_cast<DWORD>(std::min<size_t>(buffer.size() - 1u, available));
            if (ReadFile(readPipe, buffer.data(), bytesToRead, &bytesRead, nullptr) && bytesRead > 0) {
                output.append(buffer.data(), bytesRead);
                if (output.size() >= kMaxOutputBytes) {
                    output.resize(kMaxOutputBytes);
                    TerminateProcess(processInfo.hProcess, 1);
                    break;
                }
            }
        }
        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 10);
        running = waitResult == WAIT_TIMEOUT;
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (running && elapsedMs > kProbeTimeoutMs) {
            TerminateProcess(processInfo.hProcess, 1);
            break;
        }
    }
    while (output.size() < kMaxOutputBytes) {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            break;
        }
        DWORD bytesRead = 0;
        const DWORD bytesToRead = static_cast<DWORD>(std::min<size_t>(buffer.size() - 1u, available));
        if (!ReadFile(readPipe, buffer.data(), bytesToRead, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        output.append(buffer.data(), std::min<size_t>(bytesRead, kMaxOutputBytes - output.size()));
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    if (jobHandle != nullptr) {
        CloseHandle(jobHandle);
    }
    CloseHandle(readPipe);
    return output;
#else
    const std::string boundedCommand = "timeout 2s " + command;
    FILE* pipe = popen(boundedCommand.c_str(), "r");
    if (pipe == nullptr) {
        return output;
    }
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
#endif
    return output;
}

std::string currentSourceControlUserName() {
#ifdef _WIN32
    std::array<char, 256> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (GetUserNameA(buffer.data(), &size) != 0 && size > 0) {
        return std::string(buffer.data());
    }
    return {};
#else
    const char* user = std::getenv("USER");
    return user != nullptr ? std::string(user) : std::string{};
#endif
}

struct GitStatusSnapshot {
    bool loaded = false;
    std::unordered_map<std::string, std::string> exactStatusByPath;
    std::vector<std::pair<std::string, std::string>> statusEntries;
};

std::unordered_map<std::string, GitStatusSnapshot>& gitStatusSnapshots() {
    static std::unordered_map<std::string, GitStatusSnapshot> snapshots;
    return snapshots;
}

void clearGitStatusSnapshots() {
    gitStatusSnapshots().clear();
}

std::string normalizeGitStatusPath(std::string value) {
    value = trimString(std::move(value));
    std::replace(value.begin(), value.end(), '\\', '/');
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return lowerString(std::move(value));
}

std::string sourceControlStatusFromPorcelainCode(std::string code) {
    code = trimString(std::move(code));
    if (code == "!!") return "Ignored";
    if (code == "??") return "Untracked";
    if (code.find('U') != std::string::npos) return "Conflict";
    if (code.find('D') != std::string::npos) return "Deleted";
    if (code.find('A') != std::string::npos) return "Added";
    if (code.find('M') != std::string::npos) return "Modified";
    if (code.find('R') != std::string::npos) return "Renamed";
    if (code.find('C') != std::string::npos) return "Copied";
    return code.empty() ? "Clean" : "Changed";
}

const GitStatusSnapshot& gitStatusSnapshotForRoot(const std::filesystem::path& gitRoot) {
    const std::string rootKey = canonicalForCompare(gitRoot).string();
    GitStatusSnapshot& snapshot = gitStatusSnapshots()[rootKey];
    if (snapshot.loaded) {
        return snapshot;
    }
    snapshot.loaded = true;

#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string command = "git -C " + quoteCommandPath(gitRoot) + " status --porcelain=v1 --ignored --untracked-files=all" + stderrRedirect;
    const std::string output = readCommandOutput(command);
    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.size() < 4) {
            continue;
        }
        const std::string status = sourceControlStatusFromPorcelainCode(line.substr(0, 2));
        std::string path = line.substr(3);
        const size_t renameSeparator = path.find(" -> ");
        if (renameSeparator != std::string::npos) {
            path = path.substr(renameSeparator + 4);
        }
        const std::string relativeKey = normalizeGitStatusPath(std::move(path));
        if (relativeKey.empty()) {
            continue;
        }
        snapshot.exactStatusByPath.emplace(relativeKey, status);
        snapshot.statusEntries.emplace_back(relativeKey, status);
    }
    return snapshot;
}

std::string lookupGitStatusSnapshot(const GitStatusSnapshot& snapshot, const std::filesystem::path& relativePath) {
    const std::string relativeKey = normalizeGitStatusPath(relativePath.generic_string());
    if (relativeKey.empty()) {
        return "Unavailable";
    }
    const auto exactIt = snapshot.exactStatusByPath.find(relativeKey);
    if (exactIt != snapshot.exactStatusByPath.end()) {
        return exactIt->second;
    }

    const std::string relativePrefix = relativeKey + "/";
    for (const auto& [entryPath, status] : snapshot.statusEntries) {
        if (entryPath.rfind(relativePrefix, 0) == 0 || relativeKey.rfind(entryPath + "/", 0) == 0) {
            return status;
        }
    }
    return "Clean";
}

void setPreferenceSaveStatus(bool saved, std::string& status, std::string successMessage, std::string failureDetail) {
    status = saved ? std::move(successMessage) : "Preference save failed: " + std::move(failureDetail);
}

std::vector<std::string> parseTagList(const std::string& value) {
    std::vector<std::string> tags;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trimString(std::move(item));
        if (!item.empty()) {
            tags.push_back(std::move(item));
        }
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

std::string joinTagList(const std::vector<std::string>& tags) {
    std::ostringstream out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << tags[i];
    }
    return out.str();
}

bool recordHasTagMatch(const AssetRecord& record, const std::string& filter) {
    const std::string lowerFilter = lowerString(trimString(filter));
    if (lowerFilter.empty()) {
        return true;
    }
    for (const std::string& tag : record.tags) {
        if (lowerString(tag).find(lowerFilter) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> collectRegistryTags(const AssetRegistry* registry) {
    std::vector<std::string> tags;
    if (registry == nullptr) {
        return tags;
    }
    for (const AssetRecord& record : registry->records()) {
        for (const std::string& tag : record.tags) {
            const std::string trimmed = trimString(tag);
            if (!trimmed.empty()) {
                tags.push_back(trimmed);
            }
        }
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

std::vector<std::string> mergedTagSuggestions(std::vector<std::string> tags, const EditorPreferences* prefs) {
    if (prefs != nullptr) {
        tags.insert(tags.end(), prefs->assetTagPresets.begin(), prefs->assetTagPresets.end());
    }
    for (std::string& tag : tags) {
        tag = trimString(std::move(tag));
    }
    tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& tag) { return tag.empty(); }), tags.end());
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

bool tagListContains(const std::vector<std::string>& tags, const std::string& tag) {
    const std::string target = lowerString(trimString(tag));
    if (target.empty()) {
        return false;
    }
    return std::any_of(tags.begin(), tags.end(), [&](const std::string& value) {
        return lowerString(trimString(value)) == target;
    });
}

bool collectionContainsAsset(const EditorAssetCollection& collection, const AssetGuid& guid) {
    return std::find(collection.assetGuids.begin(), collection.assetGuids.end(), guid) != collection.assetGuids.end();
}

bool assetGuidListContains(const std::vector<std::string>& guids, const AssetGuid& guid) {
    return !guid.empty() && std::find(guids.begin(), guids.end(), guid) != guids.end();
}

bool preferencePathListContains(const std::vector<std::string>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    const std::filesystem::path target = canonicalForCompare(path);
    return std::any_of(paths.begin(), paths.end(), [&](const std::string& value) {
        return !value.empty() && canonicalForCompare(std::filesystem::path(value)) == target;
    });
}

std::string matchingPreferencePathValue(const std::vector<std::string>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    const std::filesystem::path target = canonicalForCompare(path);
    for (const std::string& value : paths) {
        if (!value.empty() && canonicalForCompare(std::filesystem::path(value)) == target) {
            return value;
        }
    }
    return {};
}

const EditorAssetCollection* selectedCollection(const EditorPreferences* prefs, int filterIndex) {
    if (prefs == nullptr || filterIndex <= 0) {
        return nullptr;
    }
    const size_t collectionIndex = static_cast<size_t>(filterIndex - 1);
    if (collectionIndex >= prefs->assetCollections.size()) {
        return nullptr;
    }
    return &prefs->assetCollections[collectionIndex];
}

bool isModelAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

bool isPlaceablePrefabSourcePath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".gltf" || ext == ".glb";
}

bool isTextureAssetPath(const std::filesystem::path& path) {
    return editorIsTexturePath(path);
}

bool isEnvironmentAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".hdr" || ext == ".exr";
}

bool isImportableSourceAssetPath(const std::filesystem::path& path) {
    return isModelAssetPath(path) || lowerString(path.extension().string()) == ".mtl" || isTextureAssetPath(path) || isEnvironmentAssetPath(path);
}

bool isRasterThumbnailPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr";
}

bool isSceneAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".rtlevel" || ext == ".mscene";
}

bool isProjectAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".vproject";
}

bool isMaterialAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".mtl";
}

bool isMtlTextureMapLine(const std::string& lowerLine) {
    return lowerLine.rfind("map_", 0) == 0 ||
        lowerLine.rfind("bump ", 0) == 0 ||
        lowerLine.rfind("disp ", 0) == 0 ||
        lowerLine.rfind("decal ", 0) == 0;
}

bool isIesAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".ies";
}

bool isVolumeAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".vdb";
}

std::string contentKindLabel(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) {
        return "Folder";
    }
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".rtlevel") return "Scene";
    if (ext == ".mscene") return "Minitech Scene";
    if (ext == ".vproject") return "Project";
    if (ext == ".gltf" || ext == ".glb") return "Model";
    if (ext == ".obj") return "OBJ Model";
    if (ext == ".mtl") return "Material";
    if (isTextureAssetPath(path)) return "Texture";
    if (ext == ".hdr" || ext == ".exr") return "Environment";
    if (ext == ".ies") return "IES Profile";
    if (ext == ".vdb") return "Volume";
    if (ext == ".glsl" || ext == ".hlsl" || ext == ".spv") return "Shader";
    if (ext == ".json") return "JSON";
    return ext.empty() ? "File" : ext.substr(1);
}

std::string compactPathLabel(const std::filesystem::path& path, const char* fallback) {
    if (!path.empty()) {
        const std::filesystem::path name = path.filename();
        if (!name.empty()) {
            return name.string();
        }
        const std::filesystem::path root = path.root_name();
        if (!root.empty()) {
            return root.string();
        }
    }
    return fallback != nullptr ? fallback : "";
}

bool supportedContentPath(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) return true;
    const std::string ext = lowerString(path.extension().string());
    return ext == ".rtlevel" || ext == ".mscene" || ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".mtl" ||
        ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".exr" ||
        ext == ".ies" || ext == ".vdb" || ext == ".vproject";
}

bool canOpenOrApplyPath(const std::filesystem::path& path) {
    return isSceneAssetPath(path) || isModelAssetPath(path) || isEnvironmentAssetPath(path);
}

void copyPathToClipboard(const std::filesystem::path& path) {
    const std::string text = path.string();
    ImGui::SetClipboardText(text.c_str());
}

void revealPathInFileBrowser(const std::filesystem::path& path) {
#ifdef _WIN32
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path target = ec ? path : absolute;
    if (std::filesystem::is_directory(target, ec)) {
        const std::string directory = target.string();
        ShellExecuteA(nullptr, "open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        const std::string args = "/select,\"" + target.string() + "\"";
        ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
#else
    (void)path;
#endif
}

std::filesystem::path canonicalForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec);
}

bool pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty() || root.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(canonicalForCompare(path), canonicalForCompare(root), ec);
    if (ec) {
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> findGitRoot(std::filesystem::path start) {
    if (start.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(start, ec)) {
        start = start.parent_path();
    }
    start = canonicalForCompare(start);
    while (!start.empty()) {
        if (std::filesystem::exists(start / ".git", ec)) {
            return start;
        }
        const std::filesystem::path parent = start.parent_path();
        if (parent == start || parent.empty()) {
            break;
        }
        start = parent;
    }
    return std::nullopt;
}

std::string gitStatusLabelForPath(const std::filesystem::path& workspaceRoot, const std::filesystem::path& path) {
    if (path.empty()) {
        return "Unavailable";
    }
    std::optional<std::filesystem::path> gitRoot = findGitRoot(path);
    if (!gitRoot.has_value() && !workspaceRoot.empty()) {
        gitRoot = findGitRoot(workspaceRoot);
    }
    if (!gitRoot.has_value()) {
        return "Not in Git";
    }
    if (!pathIsWithin(path, *gitRoot)) {
        return "External";
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(canonicalForCompare(path), *gitRoot, ec);
    if (ec) {
        return "Unavailable";
    }
    return lookupGitStatusSnapshot(gitStatusSnapshotForRoot(*gitRoot), relative);
}

float assetImportProgress(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Imported && !record.missing && !record.stale) {
        return 1.0f;
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return 0.65f;
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        return 0.20f;
    }
    if (record.status == AssetImportStatus::Failed) {
        return 0.0f;
    }
    return 0.35f;
}

const char* assetImportProgressLabel(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Imported && !record.missing && !record.stale) {
        return record.sourceMissing ? "Ready from cooked payload" : "Ready";
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return "Needs reimport";
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        if (record.cookedPayloadMissing) return "Cooked payload missing";
        if (record.importedMetadataMissing) return "Metadata missing";
        if (record.dependenciesMissing) return "Dependency missing";
        return "Broken reference";
    }
    if (record.status == AssetImportStatus::Failed) {
        return "Failed";
    }
    return "Pending metadata";
}

const char* selectedAssetStateLabel(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Failed) {
        return "Failed";
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        if (record.cookedPayloadMissing) return "Cooked payload missing";
        if (record.importedMetadataMissing) return "Metadata missing";
        if (record.dependenciesMissing) return "Dependency missing";
        return "Broken reference";
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return "Stale / needs reimport";
    }
    if (record.status == AssetImportStatus::Imported) {
        return record.sourceMissing ? "Ready from cooked payload" : "Ready";
    }
    return "Pending metadata";
}

ImVec4 selectedAssetStateColor(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Failed || record.status == AssetImportStatus::Missing || record.missing) {
        return ImVec4(0.95f, 0.36f, 0.32f, 1.0f);
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return ImVec4(0.95f, 0.68f, 0.28f, 1.0f);
    }
    if (record.status == AssetImportStatus::Imported) {
        return ImVec4(0.54f, 0.82f, 0.60f, 1.0f);
    }
    return ImVec4(0.65f, 0.70f, 0.78f, 1.0f);
}

bool assetPlacementBlocked(const AssetRecord& record) {
    return record.missing || record.status == AssetImportStatus::Missing || record.status == AssetImportStatus::Failed ||
        record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing;
}

const char* assetPlacementBlockReason(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Failed) return "Import failed; repair or reimport before placement.";
    if (record.importedMetadataMissing) return "Imported metadata is missing; repair or reimport before placement.";
    if (record.cookedPayloadMissing) return "Cooked/runtime payload is missing; rebuild or repair before placement.";
    if (record.dependenciesMissing) return "Dependency records are missing; repair references before placement.";
    if (record.missing || record.status == AssetImportStatus::Missing) return "Asset is marked missing; repair before placement.";
    return "Asset is ready for placement.";
}

const char* selectedPathOriginLabel(const EditorRuntimeState& state, const std::filesystem::path& path) {
    if (state.project != nullptr) {
        if (pathIsWithin(path, state.project->contentRoot)) {
            return "Project content";
        }
        if (pathIsWithin(path, state.project->projectRoot / "SourceAssets")) {
            return "Project source asset";
        }
    }
    return "External or workspace file";
}

ImU32 contentIconColor(const std::filesystem::path& path) {
    const EditorGlyphIcon icon = editorGlyphForPath(path);
    switch (icon) {
    case EditorGlyphIcon::Folder:
        return IM_COL32(188, 194, 204, 255);
    case EditorGlyphIcon::Texture:
    case EditorGlyphIcon::Environment:
        return IM_COL32(184, 196, 211, 255);
    case EditorGlyphIcon::Model:
    case EditorGlyphIcon::SceneFile:
        return IM_COL32(188, 199, 216, 255);
    case EditorGlyphIcon::Material:
        return IM_COL32(198, 190, 212, 255);
    case EditorGlyphIcon::IesProfile:
        return IM_COL32(210, 198, 168, 255);
    case EditorGlyphIcon::VolumeFile:
        return IM_COL32(178, 204, 198, 255);
    case EditorGlyphIcon::ShaderFile:
    case EditorGlyphIcon::ConfigFile:
        return IM_COL32(178, 188, 204, 255);
    default:
        return IM_COL32(158, 166, 178, 255);
    }
}

EditorGlyphIcon editorGlyphForAssetType(AssetType type) {
    switch (type) {
    case AssetType::Mesh: return EditorGlyphIcon::Model;
    case AssetType::Material: return EditorGlyphIcon::Material;
    case AssetType::Texture: return EditorGlyphIcon::Texture;
    case AssetType::HDRI: return EditorGlyphIcon::Environment;
    case AssetType::Scene: return EditorGlyphIcon::SceneFile;
    case AssetType::Prefab: return EditorGlyphIcon::Model;
    case AssetType::Animation: return EditorGlyphIcon::TimelineKey;
    case AssetType::Skeleton: return EditorGlyphIcon::Model;
    case AssetType::Unknown:
    default: return EditorGlyphIcon::File;
    }
}

ImU32 assetTypeIconColor(AssetType type) {
    switch (type) {
    case AssetType::Mesh:
    case AssetType::Prefab:
    case AssetType::Scene:
        return IM_COL32(188, 199, 216, 255);
    case AssetType::Material:
        return IM_COL32(198, 190, 212, 255);
    case AssetType::Texture:
    case AssetType::HDRI:
        return IM_COL32(184, 196, 211, 255);
    case AssetType::Animation:
    case AssetType::Skeleton:
        return IM_COL32(190, 202, 184, 255);
    case AssetType::Unknown:
    default:
        return IM_COL32(158, 166, 178, 255);
    }
}

void drawAssetTypeGlyph(AssetType type, ImVec2 min, ImVec2 max) {
    editorDrawIconGlyph(editorGlyphForAssetType(type), min, max, assetTypeIconColor(type));
}

void drawContentGlyph(const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    editorDrawIconGlyph(editorGlyphForPath(path), min, max, contentIconColor(path));
}

void setPathDragDropPayload(const char* payloadType, const std::filesystem::path& path, const char* labelPrefix) {
    const std::string payload = path.string();
    ImGui::SetDragDropPayload(payloadType, payload.c_str(), payload.size() + 1);
    ImGui::Text("%s %s", labelPrefix, path.filename().string().c_str());
}

bool drawLevelPathDragDropSource(const std::filesystem::path& path) {
    if (!isSceneAssetPath(path) || !ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        return false;
    }
    setPathDragDropPayload("LEVEL_PATH", path, "Level");
    ImGui::EndDragDropSource();
    return true;
}

bool contentActionButton(const char* id, EditorGlyphIcon icon, const char* label, const char* tooltip) {
    const bool clicked = editorIconTextButton(id, icon, label);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

uint8_t toneMapHdrChannel(float value) {
    const float mapped = value <= 0.0f ? 0.0f : value / (1.0f + value);
    return static_cast<uint8_t>(std::clamp(std::pow(mapped, 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f);
}

std::filesystem::path resolveAssetRecordPath(const EditorRuntimeState& state, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    if (path.is_absolute()) {
        return path;
    }
    if (state.project != nullptr) {
        return state.project->projectRoot / path;
    }
    if (state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty()) {
        return state.assetRegistry->state().path.parent_path() / path;
    }
    return path;
}

std::filesystem::path firstResolvedExistingRecordPath(const EditorRuntimeState& state, std::initializer_list<std::string> values) {
    std::filesystem::path firstResolved;
    for (const std::string& value : values) {
        const std::filesystem::path resolved = resolveAssetRecordPath(state, value);
        if (resolved.empty()) {
            continue;
        }
        if (firstResolved.empty()) {
            firstResolved = resolved;
        }
        std::error_code ec;
        if (std::filesystem::exists(resolved, ec)) {
            return resolved;
        }
    }
    return firstResolved;
}

std::filesystem::path recordPreviewPath(const EditorRuntimeState& state, const AssetRecord& record) {
    if (record.type == AssetType::Texture || record.type == AssetType::HDRI) {
        return firstResolvedExistingRecordPath(state, {record.thumbnailPath, record.cachePath, record.sourcePath, record.importedPath});
    }
    return firstResolvedExistingRecordPath(state, {record.thumbnailPath, record.sourcePath, record.importedPath, record.cachePath});
}

std::string fileSizeLabel(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::is_regular_file(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    if (ec) {
        return "Size unavailable";
    }
    const double bytes = static_cast<double>(size);
    std::ostringstream out;
    if (bytes >= 1024.0 * 1024.0) {
        out << "Size " << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024.0) {
        out << "Size " << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
    } else {
        out << "Size " << size << " B";
    }
    return out.str();
}

std::optional<nlohmann::json> readJsonFile(const std::filesystem::path& path) {
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

size_t jsonArraySize(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_array()) {
        return 0;
    }
    return json[key].size();
}

std::string countLabel(const char* label, size_t value) {
    return std::string(label) + " " + std::to_string(value);
}

bool samePathForOperation(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    return canonicalForCompare(lhs) == canonicalForCompare(rhs);
}

bool sceneLoadStatusIsSuccessfulCompletion(const std::string& status) {
    const std::string lower = lowerString(status);
    return lower.find("completed") != std::string::npos &&
        lower.find("failed") == std::string::npos &&
        lower.find("cancelled") == std::string::npos &&
        lower.find("error") == std::string::npos;
}

bool textureAssetMatchesPath(const TextureAsset& texture, const std::filesystem::path& path) {
    if (path.empty() || texture.sourcePath.empty()) {
        return false;
    }
    if (canonicalForCompare(texture.sourcePath) == canonicalForCompare(path)) {
        return true;
    }
    return lowerString(texture.sourcePath.filename().string()) == lowerString(path.filename().string());
}

std::optional<uint32_t> materialTextureSlotForPath(const EditorRuntimeState& state, const std::filesystem::path& path) {
    if (state.importedScene == nullptr || state.assets == nullptr || !isTextureAssetPath(path)) {
        return std::nullopt;
    }
    const SceneAsset& scene = *state.importedScene;
    for (uint32_t slot = 0; slot < scene.textures.size(); ++slot) {
        const TextureAsset* texture = state.assets->texture(scene.textures[slot]);
        if (texture != nullptr && textureAssetMatchesPath(*texture, path)) {
            return slot;
        }
    }
    return std::nullopt;
}

uint64_t fnv1a64(const std::string& text) {
    uint64_t value = 14695981039346656037ull;
    for (unsigned char ch : text) {
        value ^= static_cast<uint64_t>(ch);
        value *= 1099511628211ull;
    }
    return value;
}

std::string hex64(uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

int64_t pathWriteStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return stamp.time_since_epoch().count();
}

uintmax_t pathSizeForCache(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return 0;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

bool isGeneratedPreviewDiskCacheCandidate(const std::filesystem::path& path) {
    std::error_code ec;
    return isModelAssetPath(path) || isSceneAssetPath(path) || isProjectAssetPath(path) || isMaterialAssetPath(path) ||
        isIesAssetPath(path) || isVolumeAssetPath(path) || std::filesystem::is_directory(path, ec);
}

std::optional<uint32_t> loadedMaterialIndexForRecord(const EditorRuntimeState& state, const AssetRecord& record) {
    if (state.importedScene == nullptr || record.type != AssetType::Material || record.sourceHash.empty() || record.importSettingsHash.empty()) {
        return std::nullopt;
    }
    const auto& materials = state.importedScene->materials;
    for (size_t i = 0; i < materials.size(); ++i) {
        if (importedAssetGuidFor(record.sourceHash, record.importSettingsHash, "Material", i) == record.guid && materials[i].valid()) {
            return materials[i].index;
        }
    }
    return std::nullopt;
}

struct AssetUsageSummary {
    size_t registryReferences = 0;
    size_t sceneReferences = 0;

    [[nodiscard]] bool referenced() const {
        return registryReferences > 0 || sceneReferences > 0;
    }
};

AssetUsageSummary assetUsageSummaryForRecord(const EditorRuntimeState& state, const AssetRecord& record) {
    AssetUsageSummary summary;
    if (state.assetRegistry != nullptr) {
        for (const AssetRecord& candidate : state.assetRegistry->records()) {
            if (candidate.guid == record.guid) {
                continue;
            }
            for (const AssetDependency& dependency : candidate.dependencies) {
                if (dependency.guid == record.guid) {
                    ++summary.registryReferences;
                }
            }
            for (const AssetGuid& reference : candidate.references) {
                if (reference == record.guid) {
                    ++summary.registryReferences;
                }
            }
        }
    }
    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            if (renderer.meshGuid == record.guid) {
                ++summary.sceneReferences;
            }
            for (const MaterialSlot& slot : renderer.materialSlots) {
                if (slot.materialGuid == record.guid) {
                    ++summary.sceneReferences;
                }
                if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == record.guid) {
                    ++summary.sceneReferences;
                }
            }
        }
        for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
            if (instance.prefabGuid == record.guid) {
                ++summary.sceneReferences;
            }
        }
    }
    return summary;
}

bool regularFileExists(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path assetValidationReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    if (state.project != nullptr && !state.project->savedRoot.empty()) {
        return state.project->savedRoot / "Reports" / "asset_validation_report.json";
    }
    if (state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty()) {
        return state.assetRegistry->state().path.parent_path() / "Reports" / "asset_validation_report.json";
    }
    return browserRoot / "Saved" / "Reports" / "asset_validation_report.json";
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

std::filesystem::path selectedAssetValidationReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_validation_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetRelationshipReportPath(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& guid,
    const char* relationshipKind) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename(std::string("asset_") + relationshipKind + "_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path assetDependencyGraphReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_dependency_graph.json");
    return path;
}

std::filesystem::path assetDependencyGraphDotPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    std::filesystem::path path = assetDependencyGraphReportPath(state, browserRoot);
    path.replace_extension(".dot");
    return path;
}

std::filesystem::path assetDependencyGraphHtmlPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    std::filesystem::path path = assetDependencyGraphReportPath(state, browserRoot);
    path.replace_extension(".html");
    return path;
}

std::filesystem::path assetProjectReferenceIndexReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_project_reference_index.json");
    return path;
}

std::filesystem::path assetProjectReferenceIndexPersistentPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    if (state.project != nullptr && !state.project->savedRoot.empty()) {
        return state.project->savedRoot / "AssetReferenceIndex.json";
    }
    if (state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty()) {
        return state.assetRegistry->state().path.parent_path() / "Saved" / "AssetReferenceIndex.json";
    }
    return browserRoot / "Saved" / "AssetReferenceIndex.json";
}

std::filesystem::path assetDuplicateReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_duplicate_report.json");
    return path;
}

std::filesystem::path selectedAssetDeleteReadinessReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_delete_readiness_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetProjectReferenceReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_project_references_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetBrokenPlaceholderReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_broken_placeholder_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetPackageInspectionReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_package_inspection_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetThumbnailReadinessReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_thumbnail_readiness_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetImporterReadinessReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_importer_readiness_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetOverwriteRiskReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_overwrite_risk_" + safeReportName(guid) + ".json");
    return path;
}

std::filesystem::path selectedAssetExternalReloadReadinessReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& guid) {
    std::filesystem::path path = assetValidationReportPath(state, browserRoot);
    path.replace_filename("asset_external_reload_readiness_" + safeReportName(guid) + ".json");
    return path;
}

struct AssetOverwriteRisk {
    std::string label;
    std::filesystem::path path;
    std::string status;
};

struct AssetSourceControlSummary {
    std::string label;
    std::string primaryStatus;
    std::string tooltip;
    bool hasSourceChange = false;
    bool hasGeneratedOverwriteRisk = false;
    std::vector<AssetOverwriteRisk> generatedRisks;
};

std::filesystem::path sourceControlDiffReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const std::filesystem::path& path) {
    std::filesystem::path reportPath = assetValidationReportPath(state, browserRoot);
    const std::string name = path.filename().empty() ? std::string("path") : path.filename().string();
    const std::string key = canonicalForCompare(path).string();
    reportPath.replace_filename("source_control_diff_" + safeReportName(name) + "_" + hex64(fnv1a64(key)) + ".patch");
    return reportPath;
}

std::filesystem::path sourceControlStatusReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const std::filesystem::path& path) {
    std::filesystem::path reportPath = assetValidationReportPath(state, browserRoot);
    const std::string name = path.filename().empty() ? std::string("path") : path.filename().string();
    const std::string key = canonicalForCompare(path).string();
    reportPath.replace_filename("source_control_status_" + safeReportName(name) + "_" + hex64(fnv1a64(key)) + ".json");
    return reportPath;
}

std::filesystem::path sourceControlActionPlanReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const std::filesystem::path& path, const std::string& action) {
    std::filesystem::path reportPath = assetValidationReportPath(state, browserRoot);
    const std::string name = path.filename().empty() ? std::string("path") : path.filename().string();
    const std::string key = canonicalForCompare(path).string() + ":" + action;
    reportPath.replace_filename("source_control_action_plan_" + safeReportName(action) + "_" + safeReportName(name) + "_" + hex64(fnv1a64(key)) + ".json");
    return reportPath;
}

std::filesystem::path sourceControlProviderReadinessReportPath(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const std::filesystem::path& path) {
    std::filesystem::path reportPath = assetValidationReportPath(state, browserRoot);
    const std::string name = path.filename().empty() ? std::string("path") : path.filename().string();
    const std::string key = canonicalForCompare(path).string() + ":provider-readiness";
    reportPath.replace_filename("source_control_provider_readiness_" + safeReportName(name) + "_" + hex64(fnv1a64(key)) + ".json");
    return reportPath;
}

bool sourceControlDiffReportAvailable(const std::string& status) {
    return status == "Modified" || status == "Added" || status == "Deleted" || status == "Renamed" || status == "Copied" ||
        status == "Conflict" || status == "Changed" || status == "Untracked";
}

bool sourceControlStatusReportAvailable(const std::string& status) {
    return status != "Unavailable" && status != "Not in Git" && status != "External";
}

bool sourceControlOverwriteRiskStatus(const std::string& status) {
    return sourceControlDiffReportAvailable(status);
}

std::vector<AssetOverwriteRisk> collectAssetOverwriteRisks(
    const EditorRuntimeState& state,
    const AssetRecord& record,
    const std::function<std::string(const std::filesystem::path&)>& statusForPath) {
    std::vector<AssetOverwriteRisk> risks;
    std::unordered_set<std::string> seen;
    auto addPath = [&](std::string label, const std::string& value) {
        const std::filesystem::path path = resolveAssetRecordPath(state, value);
        if (path.empty()) {
            return;
        }
        const std::string key = canonicalForCompare(path).string();
        if (!seen.insert(key).second) {
            return;
        }
        const std::string status = statusForPath(path);
        if (sourceControlOverwriteRiskStatus(status)) {
            risks.push_back(AssetOverwriteRisk{std::move(label), path, status});
        }
    };

    addPath("Imported metadata", record.importedPath);
    addPath("Cooked/runtime payload", record.cachePath);
    addPath("Thumbnail", record.thumbnailPath);
    return risks;
}

AssetSourceControlSummary summarizeAssetSourceControlState(
    const EditorRuntimeState& state,
    const AssetRecord& record,
    const std::function<std::string(const std::filesystem::path&)>& statusForPath) {
    AssetSourceControlSummary summary;
    summary.primaryStatus = "Unavailable";
    summary.generatedRisks = collectAssetOverwriteRisks(state, record, statusForPath);
    summary.hasGeneratedOverwriteRisk = !summary.generatedRisks.empty();

    std::string fallbackStatus;
    auto statusForRecordPath = [&](const std::string& value) {
        const std::filesystem::path path = resolveAssetRecordPath(state, value);
        if (path.empty()) {
            return std::string{};
        }
        return statusForPath(path);
    };

    const std::string sourceStatus = statusForRecordPath(record.sourcePath);
    if (!sourceStatus.empty()) {
        fallbackStatus = sourceStatus;
        summary.hasSourceChange = sourceControlDiffReportAvailable(sourceStatus);
    }
    if (fallbackStatus.empty()) {
        for (const std::string* value : {&record.importedPath, &record.cachePath, &record.thumbnailPath}) {
            const std::string status = statusForRecordPath(*value);
            if (!status.empty()) {
                fallbackStatus = status;
                break;
            }
        }
    }

    if (summary.hasGeneratedOverwriteRisk) {
        summary.primaryStatus = summary.generatedRisks.front().status;
        summary.label = summary.generatedRisks.size() == 1
            ? std::string("Generated: ") + summary.generatedRisks.front().status
            : std::string("Generated: ") + std::to_string(summary.generatedRisks.size()) + " changed";
        summary.tooltip = "Generated asset files have Git changes that reimport, rebuild, or delete workflows may overwrite.";
        for (const AssetOverwriteRisk& risk : summary.generatedRisks) {
            summary.tooltip += "\n" + risk.label + ": " + risk.status + "\n" + risk.path.string();
        }
        return summary;
    }

    if (summary.hasSourceChange) {
        summary.primaryStatus = sourceStatus;
        summary.label = "Src: " + sourceStatus;
        summary.tooltip = "Raw source file has Git changes; generated asset files do not currently report overwrite-risk changes.";
        return summary;
    }

    summary.primaryStatus = fallbackStatus.empty() ? std::string("Unavailable") : fallbackStatus;
    summary.label = summary.primaryStatus;
    summary.tooltip = "Git status summary for the raw source and generated metadata/payload/thumbnail paths.";
    return summary;
}

ImVec4 sourceControlStatusTextColor(const std::string& status) {
    if (status == "Clean") return ImVec4(0.54f, 0.82f, 0.60f, 1.0f);
    if (status == "Modified" || status == "Added" || status == "Renamed" || status == "Copied") return ImVec4(0.95f, 0.68f, 0.28f, 1.0f);
    if (status == "Deleted" || status == "Conflict") return ImVec4(0.95f, 0.36f, 0.32f, 1.0f);
    if (status == "Untracked") return ImVec4(0.55f, 0.72f, 0.95f, 1.0f);
    if (status == "Ignored") return ImVec4(0.48f, 0.52f, 0.58f, 1.0f);
    return ImVec4(0.65f, 0.70f, 0.78f, 1.0f);
}

bool writeSourceControlDiffReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& path,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (path.empty()) {
        outError = "No source-control path selected.";
        return false;
    }
    std::optional<std::filesystem::path> gitRoot = findGitRoot(path);
    if (!gitRoot.has_value() && !workspaceRoot.empty()) {
        gitRoot = findGitRoot(workspaceRoot);
    }
    if (!gitRoot.has_value()) {
        outError = "Path is not inside a Git repository.";
        return false;
    }
    if (!pathIsWithin(path, *gitRoot)) {
        outError = "Path is outside the resolved Git repository.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(canonicalForCompare(path), *gitRoot, ec);
    if (ec) {
        outError = "Could not resolve repository-relative path: " + ec.message();
        return false;
    }

#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string rootArg = quoteCommandPath(*gitRoot);
    const std::string pathArg = quoteCommandPath(relative);
    const std::string statusText = readCommandOutput("git -C " + rootArg + " status --short -- " + pathArg + stderrRedirect);
    const std::string unstagedDiff = readCommandOutput("git -C " + rootArg + " diff -- " + pathArg + stderrRedirect);
    const std::string stagedDiff = readCommandOutput("git -C " + rootArg + " diff --cached -- " + pathArg + stderrRedirect);

    outPath = sourceControlDiffReportPath(state, browserRoot, path);
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create source-control report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write source-control diff report: " + outPath.string();
        return false;
    }

    file << "# Source Control Diff Report\n";
    file << "Repository: " << gitRoot->generic_string() << "\n";
    file << "Path: " << relative.generic_string() << "\n";
    file << "Status:\n" << (trimString(statusText).empty() ? std::string("  Clean\n") : statusText) << "\n";
    file << "## Unstaged Diff\n";
    file << (unstagedDiff.empty() ? std::string("(none)\n") : unstagedDiff);
    file << "\n## Staged Diff\n";
    file << (stagedDiff.empty() ? std::string("(none)\n") : stagedDiff);
    if (unstagedDiff.empty() && stagedDiff.empty() && trimString(statusText).rfind("??", 0) == 0) {
        file << "\n## Note\nUntracked files have no Git diff until they are added to the index.\n";
    }
    return true;
}

bool writeSourceControlStatusReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& path,
    const std::string& statusLabel,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (path.empty()) {
        outError = "No source-control path selected.";
        return false;
    }
    std::optional<std::filesystem::path> gitRoot = findGitRoot(path);
    if (!gitRoot.has_value() && !workspaceRoot.empty()) {
        gitRoot = findGitRoot(workspaceRoot);
    }
    if (!gitRoot.has_value()) {
        outError = "Path is not inside a Git repository.";
        return false;
    }
    if (!pathIsWithin(path, *gitRoot)) {
        outError = "Path is outside the resolved Git repository.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path canonicalPath = canonicalForCompare(path);
    const std::filesystem::path relative = std::filesystem::relative(canonicalPath, *gitRoot, ec);
    if (ec) {
        outError = "Could not resolve repository-relative path: " + ec.message();
        return false;
    }

#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string rootArg = quoteCommandPath(*gitRoot);
    const std::string pathArg = quoteCommandPath(relative);
    const std::string focusedStatus = readCommandOutput("git -C " + rootArg + " status --short --ignored -- " + pathArg + stderrRedirect);
    const std::string repositoryStatus = readCommandOutput("git -C " + rootArg + " status --short --ignored --branch" + stderrRedirect);

    outPath = sourceControlStatusReportPath(state, browserRoot, path);
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create source-control report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write source-control status report: " + outPath.string();
        return false;
    }

    const bool exists = std::filesystem::exists(canonicalPath, ec);
    const bool isDirectory = exists && std::filesystem::is_directory(canonicalPath, ec);
    const nlohmann::json report = {
        {"schema", "ContentBrowserSourceControlStatusReportV1"},
        {"provider", "git"},
        {"readOnly", true},
        {"repositoryRoot", gitRoot->generic_string()},
        {"path", canonicalPath.generic_string()},
        {"repositoryRelativePath", relative.generic_string()},
        {"pathExists", exists},
        {"pathIsDirectory", isDirectory},
        {"statusLabel", statusLabel},
        {"focusedStatus", trimString(focusedStatus).empty() ? std::string("Clean") : focusedStatus},
        {"repositoryStatus", trimString(repositoryStatus).empty() ? std::string("Clean") : repositoryStatus},
        {"policy", {
            {"description", "This report is a read-only source-control snapshot for review before asset or level workflow actions."},
            {"performedActions", nlohmann::json::array()},
            {"unsupportedActions", {"revert", "checkout", "lock", "submit", "perforce-provider-actions"}}
        }}
    };
    file << report.dump(2);
    return true;
}

bool writeSourceControlActionPlanReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& path,
    const std::string& action,
    const std::string& statusLabel,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (path.empty()) {
        outError = "No source-control path selected.";
        return false;
    }
    std::optional<std::filesystem::path> gitRoot = findGitRoot(path);
    if (!gitRoot.has_value() && !workspaceRoot.empty()) {
        gitRoot = findGitRoot(workspaceRoot);
    }
    if (!gitRoot.has_value()) {
        outError = "Path is not inside a Git repository.";
        return false;
    }
    if (!pathIsWithin(path, *gitRoot)) {
        outError = "Path is outside the resolved Git repository.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path canonicalPath = canonicalForCompare(path);
    const std::filesystem::path relative = std::filesystem::relative(canonicalPath, *gitRoot, ec);
    if (ec) {
        outError = "Could not resolve repository-relative path: " + ec.message();
        return false;
    }

#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string rootArg = quoteCommandPath(*gitRoot);
    const std::string pathArg = quoteCommandPath(relative);
    const std::string focusedStatus = readCommandOutput("git -C " + rootArg + " status --short --ignored -- " + pathArg + stderrRedirect);
    const std::string repositoryBranch = readCommandOutput("git -C " + rootArg + " status --short --branch" + stderrRedirect);

    nlohmann::json plannedCommands = nlohmann::json::array();
    nlohmann::json blockers = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    const std::string normalizedAction = lowerString(action);
    if (normalizedAction == "revert") {
        plannedCommands.push_back({
            {"description", "Restore tracked file contents from HEAD."},
            {"command", "git -C " + rootArg + " checkout -- " + pathArg},
        });
        plannedCommands.push_back({
            {"description", "If the path is untracked, remove it after separate user confirmation."},
            {"command", "git -C " + rootArg + " clean -f -- " + pathArg},
            {"requiresUntrackedPath", true},
        });
        warnings.push_back("Revert is destructive. This editor action writes a dry-run plan only and does not execute checkout or clean.");
    } else if (normalizedAction == "checkout") {
        plannedCommands.push_back({
            {"description", "Restore the selected tracked path from HEAD."},
            {"command", "git -C " + rootArg + " checkout -- " + pathArg},
        });
        warnings.push_back("Checkout mutation is not executed by the editor in this slice; review the plan before running commands externally.");
    } else if (normalizedAction == "lock") {
        blockers.push_back("Git has no native asset lock provider in this editor integration.");
        plannedCommands.push_back({
            {"description", "Future Perforce provider lock command placeholder."},
            {"command", "p4 edit " + pathArg},
            {"provider", "perforce"},
            {"available", false},
        });
    } else if (normalizedAction == "submit") {
        plannedCommands.push_back({
            {"description", "Stage the selected path."},
            {"command", "git -C " + rootArg + " add -- " + pathArg},
        });
        plannedCommands.push_back({
            {"description", "Commit staged changes after user review."},
            {"command", "git -C " + rootArg + " commit"},
        });
        plannedCommands.push_back({
            {"description", "Optional push after commit and policy review."},
            {"command", "git -C " + rootArg + " push"},
            {"optional", true},
        });
        warnings.push_back("Submit is represented as a Git stage/commit/push plan only. The editor does not execute provider mutations in this slice.");
    } else {
        blockers.push_back("Unsupported source-control action plan type: " + action);
    }

    outPath = sourceControlActionPlanReportPath(state, browserRoot, path, normalizedAction.empty() ? action : normalizedAction);
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create source-control action plan folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write source-control action plan: " + outPath.string();
        return false;
    }

    const nlohmann::json report = {
        {"schema", "ContentBrowserSourceControlActionPlanV1"},
        {"provider", "git"},
        {"action", normalizedAction},
        {"dryRun", true},
        {"mutationExecuted", false},
        {"repositoryRoot", gitRoot->generic_string()},
        {"path", canonicalPath.generic_string()},
        {"repositoryRelativePath", relative.generic_string()},
        {"statusLabel", statusLabel},
        {"focusedStatus", trimString(focusedStatus).empty() ? std::string("Clean") : focusedStatus},
        {"repositoryBranchStatus", trimString(repositoryBranch).empty() ? std::string("Clean") : repositoryBranch},
        {"plannedCommands", plannedCommands},
        {"blockers", blockers},
        {"warnings", warnings},
        {"policy", {
            {"description", "This report is a dry-run source-control action plan. It records intended commands and provider gaps without mutating the repository."},
            {"requiresUserConfirmationOutsideEditor", true},
            {"performedActions", nlohmann::json::array()},
            {"unsupportedActions", {"editor-executed-revert", "editor-executed-checkout", "provider-lock", "provider-submit", "perforce-provider-actions"}}
        }},
    };
    file << report.dump(2);
    return true;
}

bool writeSourceControlProviderReadinessReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& path,
    const std::string& statusLabel,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (path.empty()) {
        outError = "No source-control path selected.";
        return false;
    }
    std::optional<std::filesystem::path> gitRoot = findGitRoot(path);
    if (!gitRoot.has_value() && !workspaceRoot.empty()) {
        gitRoot = findGitRoot(workspaceRoot);
    }
    if (!gitRoot.has_value()) {
        outError = "Path is not inside a Git repository.";
        return false;
    }
    if (!pathIsWithin(path, *gitRoot)) {
        outError = "Path is outside the resolved Git repository.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path canonicalPath = canonicalForCompare(path);
    const std::filesystem::path relative = std::filesystem::relative(canonicalPath, *gitRoot, ec);
    if (ec) {
        outError = "Could not resolve repository-relative path: " + ec.message();
        return false;
    }

#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string rootArg = quoteCommandPath(*gitRoot);
    const std::string pathArg = quoteCommandPath(relative);
    const std::string focusedStatus = readCommandOutput("git -C " + rootArg + " status --short --ignored -- " + pathArg + stderrRedirect);
    const std::string branchStatus = readCommandOutput("git -C " + rootArg + " status --short --branch" + stderrRedirect);
    const std::string gitLfsLocks = readCommandOutput("git -C " + rootArg + " lfs locks --path " + pathArg + stderrRedirect);
    const std::string p4Info = readCommandOutput("p4 info" + std::string(stderrRedirect));
    const std::string p4Opened = readCommandOutput("p4 opened " + pathArg + std::string(stderrRedirect));
    const bool p4Detected = !trimString(p4Info).empty();
    const bool lfsLockInfoAvailable = !trimString(gitLfsLocks).empty();
    const std::string currentUser = currentSourceControlUserName();

    outPath = sourceControlProviderReadinessReportPath(state, browserRoot, path);
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create source-control provider readiness folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write source-control provider readiness report: " + outPath.string();
        return false;
    }

    const nlohmann::json report = {
        {"schema", "ContentBrowserSourceControlProviderReadinessV1"},
        {"provider", "git"},
        {"repositoryRoot", gitRoot->generic_string()},
        {"path", canonicalPath.generic_string()},
        {"repositoryRelativePath", relative.generic_string()},
        {"statusLabel", statusLabel},
        {"focusedStatus", trimString(focusedStatus).empty() ? std::string("Clean") : focusedStatus},
        {"repositoryBranchStatus", trimString(branchStatus).empty() ? std::string("Clean") : branchStatus},
        {"currentUser", currentUser},
        {"gitProvider", {
            {"available", true},
            {"supportsDiffStatus", true},
            {"supportsEditorMutations", false},
            {"supportsNativeLocks", false},
            {"lfsLockQueryAttempted", true},
            {"lfsLockInfoAvailable", lfsLockInfoAvailable},
            {"lfsLocksRaw", trimString(gitLfsLocks)},
        }},
        {"perforceProvider", {
            {"detected", p4Detected},
            {"providerImplemented", false},
            {"supportsCheckoutLockSubmitWhenImplemented", true},
            {"infoRaw", trimString(p4Info)},
            {"openedRaw", trimString(p4Opened)},
            {"plannedCommands", nlohmann::json::array({
                nlohmann::json{{"action", "checkout"}, {"command", "p4 edit " + pathArg}, {"available", false}},
                nlohmann::json{{"action", "lock"}, {"command", "p4 lock " + pathArg}, {"available", false}},
                nlohmann::json{{"action", "submit"}, {"command", "p4 submit " + pathArg}, {"available", false}},
                nlohmann::json{{"action", "revert"}, {"command", "p4 revert " + pathArg}, {"available", false}}
            })},
        }},
        {"lockOwnership", {
            {"assetLockProviderImplemented", false},
            {"ownershipStateKnown", p4Detected || lfsLockInfoAvailable},
            {"currentOwner", "unknown"},
            {"currentLockState", (p4Detected || lfsLockInfoAvailable) ? "provider-output-needs-parser" : "unknown-no-provider-lock-state"},
            {"conflictHandlingImplemented", false},
            {"safeExternalReloadPromptImplemented", false},
        }},
        {"policy", {
            {"description", "This report inspects source-control provider readiness, lock/ownership visibility, and remaining provider gaps without mutating the repository."},
            {"dryRun", true},
            {"mutationExecuted", false},
            {"performedActions", nlohmann::json::array()},
            {"supportedNow", nlohmann::json::array({"git-status", "git-diff", "git-action-plan", "provider-readiness-report"})},
            {"unsupportedActions", nlohmann::json::array({"editor-executed-revert", "editor-executed-checkout", "provider-lock", "provider-submit", "perforce-provider-mutations", "asset-lock-ownership-enforcement", "provider-conflict-resolution", "automatic-external-change-reload-prompt"})},
        }},
    };
    file << report.dump(2);
    return true;
}

nlohmann::json assetRecordSummaryJson(const EditorRuntimeState& state, const AssetRecord& record) {
    const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
    const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
    const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
    const std::filesystem::path thumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);
    return {
        {"guid", record.guid},
        {"displayName", record.displayName},
        {"assetType", assetTypeName(record.type)},
        {"status", assetImportStatusName(record.status)},
        {"sourcePath", record.sourcePath},
        {"importedPath", record.importedPath},
        {"cachePath", record.cachePath},
        {"thumbnailPath", record.thumbnailPath},
        {"tags", record.tags},
        {"resolvedSourcePath", sourcePath.empty() ? std::string{} : sourcePath.generic_string()},
        {"resolvedImportedPath", importedPath.empty() ? std::string{} : importedPath.generic_string()},
        {"resolvedCachePath", cachePath.empty() ? std::string{} : cachePath.generic_string()},
        {"resolvedThumbnailPath", thumbnailPath.empty() ? std::string{} : thumbnailPath.generic_string()},
        {"sourceMissing", record.sourceMissing},
        {"importedMetadataMissing", record.importedMetadataMissing},
        {"cookedPayloadMissing", record.cookedPayloadMissing},
        {"dependenciesMissing", record.dependenciesMissing},
        {"stale", record.stale},
        {"missing", record.missing},
    };
}

bool writeAssetOverwriteRiskReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    const std::vector<AssetOverwriteRisk>& risks,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetOverwriteRiskReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create overwrite-risk report folder: " + ec.message();
        return false;
    }

    nlohmann::json riskArray = nlohmann::json::array();
    for (const AssetOverwriteRisk& risk : risks) {
        riskArray.push_back({
            {"label", risk.label},
            {"path", risk.path.empty() ? std::string{} : risk.path.generic_string()},
            {"sourceControlStatus", risk.status},
            {"recommendedAction", "Review the diff or commit/stash the external change before reimporting or rebuilding payloads."},
        });
    }

    const nlohmann::json report = {
        {"schema", "TransparentAssetOverwriteRiskReportV1"},
        {"asset", assetRecordSummaryJson(state, record)},
        {"riskCount", risks.size()},
        {"overwriteRisks", riskArray},
        {"policy", {
            {"warning", "Reimport and Rebuild Payload may overwrite generated asset metadata, cooked payloads, or thumbnails that have external source-control changes."},
            {"affectedActions", {"Reimport", "Rebuild Payload"}},
            {"reloadPolicy", "This report does not reload changed files. Refresh/reimport remains an explicit user action."},
        }},
    };

    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write overwrite-risk report: " + outPath.string();
        return false;
    }
    file << report.dump(2);
    return true;
}

bool writeAssetExternalReloadReadinessReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    const std::function<std::string(const std::filesystem::path&)>& statusForPath,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetExternalReloadReadinessReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create external reload readiness report folder: " + ec.message();
        return false;
    }

    nlohmann::json fileStates = nlohmann::json::array();
    size_t changedPathCount = 0;
    size_t generatedChangedPathCount = 0;
    auto appendPathState = [&](const char* role, const std::string& storedPath, bool generated, const char* recommendedAction) {
        const std::filesystem::path resolvedPath = resolveAssetRecordPath(state, storedPath);
        if (resolvedPath.empty()) {
            return;
        }
        const std::string status = statusForPath(resolvedPath);
        const bool sourceControlChanged = sourceControlDiffReportAvailable(status);
        if (sourceControlChanged) {
            ++changedPathCount;
            if (generated) {
                ++generatedChangedPathCount;
            }
        }
        fileStates.push_back({
            {"role", role},
            {"storedPath", storedPath},
            {"resolvedPath", resolvedPath.generic_string()},
            {"generatedAssetFile", generated},
            {"exists", std::filesystem::exists(resolvedPath, ec)},
            {"sourceControlStatus", status},
            {"sourceControlChanged", sourceControlChanged},
            {"recommendedAction", recommendedAction},
        });
    };
    appendPathState("Source", record.sourcePath, false, "Review source changes, then use Repair Asset or Reimport to regenerate metadata and cooked payloads explicitly.");
    appendPathState("ImportedMetadata", record.importedPath, true, "Review the generated metadata diff before reimporting; automatic metadata hot-reload is not performed.");
    appendPathState("CookedRuntimePayload", record.cachePath, true, "Review payload ownership/status before Rebuild Payload or Repair Asset; automatic runtime payload reload is not performed.");
    appendPathState("Thumbnail", record.thumbnailPath, true, "Thumbnail changes can be refreshed by reimport/regeneration; fallback previews remain safe until then.");

    const nlohmann::json report = {
        {"schema", "TransparentAssetExternalReloadReadinessV1"},
        {"asset", assetRecordSummaryJson(state, record)},
        {"sceneDirty", state.sceneDirty},
        {"projectSettingsDirty", state.projectSettingsDirty},
        {"changedPathCount", changedPathCount},
        {"generatedChangedPathCount", generatedChangedPathCount},
        {"fileStates", fileStates},
        {"promptReadiness", {
            {"shouldPrompt", changedPathCount > 0},
            {"sourceChanged", std::any_of(fileStates.begin(), fileStates.end(), [](const nlohmann::json& item) {
                return item.value("role", std::string{}) == "Source" && item.value("sourceControlChanged", false);
            })},
            {"generatedFilesChanged", generatedChangedPathCount > 0},
            {"recommendedPrompt", changedPathCount > 0
                ? "External source-control changes are present. Review diffs before explicit reimport, repair, rebuild, or thumbnail regeneration."
                : "No source-control-changed asset paths were detected for this selected asset."}
        }},
        {"policy", {
            {"description", "This report is a safe external-change reload readiness review for the selected asset."},
            {"automaticReloadPromptImplemented", false},
            {"mutationExecuted", false},
            {"performedActions", nlohmann::json::array()},
            {"supportedFollowUpActions", {"Git Diff", "Git Status", "Repair Asset", "Reimport", "Rebuild Payload", "Reveal Source", "Reveal Metadata", "Reveal Payload"}},
            {"unsupportedActions", {"automatic-source-reload", "automatic-generated-metadata-reload", "automatic-runtime-payload-reload", "provider-level-conflict-resolution"}}
        }},
    };

    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write external reload readiness report: " + outPath.string();
        return false;
    }
    file << report.dump(2);
    return true;
}

const AssetRecord* findAssetRecordByGuid(const AssetRegistry& registry, const AssetGuid& guid) {
    for (const AssetRecord& record : registry.records()) {
        if (record.guid == guid) {
            return &record;
        }
    }
    return nullptr;
}

void appendValidationIssue(
    nlohmann::json& array,
    const char* severity,
    const char* kind,
    const AssetRecord& record,
    std::string detail,
    std::string path = {}) {
    array.push_back({
        {"severity", severity},
        {"kind", kind},
        {"guid", record.guid},
        {"displayName", record.displayName},
        {"assetType", assetTypeName(record.type)},
        {"detail", std::move(detail)},
        {"path", std::move(path)},
    });
}

void appendComponentReferenceIssue(
    nlohmann::json& array,
    const Entity& entity,
    const char* component,
    const char* field,
    const AssetGuid& guid) {
    array.push_back({
        {"severity", "error"},
        {"kind", "InvalidComponentReference"},
        {"entity", entity.name},
        {"entityUuid", entity.uuid},
        {"component", component},
        {"field", field},
        {"guid", guid},
        {"detail", "Component references an asset GUID that is not present in the asset registry."},
    });
}

bool supportedCoordinateConversion(std::string_view value) {
    return value == "None" || value == "glTF Y-Up to Engine" || value == "Z-Up to Engine";
}

bool supportedMaterialImportMode(std::string_view value) {
    return value == "ImportMaterials" || value == "MetadataOnly" || value == "SkipMaterials";
}

bool supportedTextureImportMode(std::string_view value) {
    return value == "ImportTextures" || value == "MetadataOnly" || value == "SkipTextures";
}

bool supportedTextureCompression(std::string_view value) {
    return value == "PreserveSource";
}

bool projectReferenceScanFileCandidate(const std::filesystem::path& path) {
    const std::string filename = lowerString(path.filename().string());
    const std::string ext = lowerString(path.extension().string());
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
        endsWith(".rthdri.json");
}

void appendUniqueScanRoot(std::vector<std::filesystem::path>& roots, const std::filesystem::path& root) {
    if (root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }
    const std::filesystem::path canonical = canonicalForCompare(root);
    for (const std::filesystem::path& existing : roots) {
        if (canonicalForCompare(existing) == canonical) {
            return;
        }
    }
    roots.push_back(canonical);
}

std::string jsonPathChild(std::string parent, const std::string& child) {
    if (parent.empty()) {
        parent = "$";
    }
    return parent + "/" + child;
}

void appendGuidOccurrences(const nlohmann::json& value, const AssetGuid& targetGuid, const std::string& jsonPath, nlohmann::json& occurrences) {
    if (value.is_string()) {
        if (value.get<std::string>() == targetGuid) {
            occurrences.push_back({{"jsonPath", jsonPath.empty() ? "$" : jsonPath}});
        }
        return;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            appendGuidOccurrences(it.value(), targetGuid, jsonPathChild(jsonPath, it.key()), occurrences);
        }
        return;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            appendGuidOccurrences(value[i], targetGuid, jsonPathChild(jsonPath, std::to_string(i)), occurrences);
        }
    }
}

void collectProjectReferenceScanFiles(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    nlohmann::json& checkedRoots,
    std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> roots;
    if (state.project != nullptr) {
        appendUniqueScanRoot(roots, state.project->contentRoot);
        appendUniqueScanRoot(roots, state.project->scenesRoot);
    } else {
        appendUniqueScanRoot(roots, browserRoot);
    }

    for (const std::filesystem::path& root : roots) {
        checkedRoots.push_back(root.generic_string());
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            std::error_code entryError;
            if (entry.is_regular_file(entryError) && projectReferenceScanFileCandidate(entry.path())) {
                files.push_back(canonicalForCompare(entry.path()));
            }
        }
    }
    if (state.project != nullptr && !state.project->projectFile.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(state.project->projectFile, ec) && projectReferenceScanFileCandidate(state.project->projectFile)) {
            files.push_back(canonicalForCompare(state.project->projectFile));
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
}

void appendInvalidSavedGuidReferences(
    const nlohmann::json& value,
    const std::unordered_set<AssetGuid>& registryGuids,
    const std::filesystem::path& filePath,
    const std::string& jsonPath,
    std::string objectKey,
    nlohmann::json& invalidReferences) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            appendInvalidSavedGuidReferences(it.value(), registryGuids, filePath, jsonPathChild(jsonPath, it.key()), it.key(), invalidReferences);
        }
        return;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            appendInvalidSavedGuidReferences(value[i], registryGuids, filePath, jsonPathChild(jsonPath, std::to_string(i)), objectKey, invalidReferences);
        }
        return;
    }
    if (!value.is_string()) {
        return;
    }

    const std::string keyLower = lowerString(std::move(objectKey));
    if (keyLower.find("guid") == std::string::npos) {
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
        {"detail", "Saved project metadata contains a GUID field whose value is not present in the loaded asset registry."},
    });
}

void appendProjectReferenceIndexEntries(
    const EditorRuntimeState& state,
    const nlohmann::json& value,
    const std::unordered_set<AssetGuid>& registryGuids,
    const std::filesystem::path& filePath,
    const std::string& jsonPath,
    std::string objectKey,
    nlohmann::json& references,
    nlohmann::json& unknownGuidFields) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            appendProjectReferenceIndexEntries(state, it.value(), registryGuids, filePath, jsonPathChild(jsonPath, it.key()), it.key(), references, unknownGuidFields);
        }
        return;
    }
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            appendProjectReferenceIndexEntries(state, value[i], registryGuids, filePath, jsonPathChild(jsonPath, std::to_string(i)), objectKey, references, unknownGuidFields);
        }
        return;
    }
    if (!value.is_string()) {
        return;
    }

    const std::string keyLower = lowerString(std::move(objectKey));
    if (keyLower.find("guid") == std::string::npos) {
        return;
    }
    const AssetGuid guid = value.get<std::string>();
    if (guid.empty()) {
        return;
    }
    const AssetRecord* asset = state.assetRegistry != nullptr ? findAssetRecordByGuid(*state.assetRegistry, guid) : nullptr;
    if (registryGuids.find(guid) != registryGuids.end()) {
        references.push_back({
            {"file", filePath.generic_string()},
            {"jsonPath", jsonPath.empty() ? "$" : jsonPath},
            {"field", keyLower},
            {"guid", guid},
            {"asset", asset != nullptr ? assetRecordSummaryJson(state, *asset) : nlohmann::json::object()},
        });
    } else {
        unknownGuidFields.push_back({
            {"severity", "warning"},
            {"kind", "UnknownSavedGuidField"},
            {"file", filePath.generic_string()},
            {"jsonPath", jsonPath.empty() ? "$" : jsonPath},
            {"field", keyLower},
            {"guid", guid},
            {"detail", "Saved project metadata contains a GUID field whose value is not present in the loaded asset registry."},
        });
    }
}

nlohmann::json sourceControlPolicyReportJson(size_t copiedSourceAssetCount) {
    return {
        {"schema", "TransparentAssetMetadataV1"},
        {"commitImportedMetadata", true},
        {"commitCopiedSourceAssets", copiedSourceAssetCount > 0},
        {"copiedSourceAssetCount", copiedSourceAssetCount},
        {"commitCookedPayloads", false},
        {"commitThumbnails", false},
        {"regenerateCookedPayloadsWhenMissing", true},
        {"policy", "Commit deterministic Content metadata and copied SourceAssets when import settings internalize source files. Treat Cache payloads and thumbnails as generated unless a project-specific source-control policy says otherwise."},
    };
}

size_t countValidationSeverity(const std::vector<const nlohmann::json*>& arrays, std::string_view severity) {
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

nlohmann::json buildAssetValidationReport(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& targetGuid = {}) {
    const bool scopedToAsset = !targetGuid.empty();
    nlohmann::json missingSources = nlohmann::json::array();
    nlohmann::json missingImportedMetadata = nlohmann::json::array();
    nlohmann::json missingCookedPayloads = nlohmann::json::array();
    nlohmann::json missingDependencies = nlohmann::json::array();
    nlohmann::json staleAssets = nlohmann::json::array();
    nlohmann::json unsupportedImportSettings = nlohmann::json::array();
    nlohmann::json invalidComponentReferences = nlohmann::json::array();
    nlohmann::json invalidSavedProjectReferences = nlohmann::json::array();
    nlohmann::json savedProjectReferenceParseErrors = nlohmann::json::array();
    nlohmann::json savedProjectReferenceScanRoots = nlohmann::json::array();
    nlohmann::json requiresReimport = nlohmann::json::array();
    nlohmann::json missingThumbnails = nlohmann::json::array();
    nlohmann::json reverseAssetReferences = nlohmann::json::array();
    nlohmann::json currentSceneReferences = nlohmann::json::array();
    size_t copiedSourceAssetCount = 0;
    size_t validatedAssetCount = 0;
    nlohmann::json selectedAsset = nlohmann::json::object();

    std::unordered_set<AssetGuid> registryGuids;
    if (state.assetRegistry != nullptr) {
        registryGuids.reserve(state.assetRegistry->records().size());
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (!record.guid.empty()) {
                registryGuids.insert(record.guid);
            }
        }

        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.importSettings.copySourceIntoProject) {
                ++copiedSourceAssetCount;
            }
            if (scopedToAsset && record.guid != targetGuid) {
                for (const AssetDependency& dependency : record.dependencies) {
                    if (dependency.guid == targetGuid) {
                        reverseAssetReferences.push_back({
                            {"ownerGuid", record.guid},
                            {"ownerDisplayName", record.displayName},
                            {"ownerAssetType", assetTypeName(record.type)},
                            {"role", dependency.kind.empty() ? "dependency" : dependency.kind},
                            {"source", "Dependency"},
                        });
                    }
                }
                for (const AssetGuid& reference : record.references) {
                    if (reference == targetGuid) {
                        reverseAssetReferences.push_back({
                            {"ownerGuid", record.guid},
                            {"ownerDisplayName", record.displayName},
                            {"ownerAssetType", assetTypeName(record.type)},
                            {"role", "reference"},
                            {"source", "Reference"},
                        });
                    }
                }
                continue;
            }
            ++validatedAssetCount;
            if (scopedToAsset) {
                selectedAsset = {
                    {"guid", record.guid},
                    {"displayName", record.displayName},
                    {"assetType", assetTypeName(record.type)},
                    {"status", assetImportStatusName(record.status)},
                    {"sourcePath", record.sourcePath},
                    {"importedPath", record.importedPath},
                    {"cachePath", record.cachePath},
                    {"thumbnailPath", record.thumbnailPath},
                    {"tags", record.tags},
                };
            }
            const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
            const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
            const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
            const std::filesystem::path thumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);
            const bool sourceMissing = !record.sourcePath.empty() && !regularFileExists(sourcePath);
            const bool importedMissing = !record.importedPath.empty() && !regularFileExists(importedPath);
            const bool cookedMissing = !record.cachePath.empty() && !regularFileExists(cachePath);
            const bool thumbnailMissing = !record.thumbnailPath.empty() && !regularFileExists(thumbnailPath);

            if (sourceMissing || record.sourceMissing) {
                appendValidationIssue(missingSources, "warning", "MissingSource", record, "Raw import source is missing.", record.sourcePath);
            }
            if (importedMissing || record.importedMetadataMissing) {
                appendValidationIssue(missingImportedMetadata, "error", "MissingImportedMetadata", record, "Imported asset metadata file is missing.", record.importedPath);
            }
            if (cookedMissing || record.cookedPayloadMissing) {
                appendValidationIssue(missingCookedPayloads, "error", "MissingCookedPayload", record, "Cooked/runtime payload is missing.", record.cachePath);
            }
            if (thumbnailMissing) {
                appendValidationIssue(missingThumbnails, "warning", "MissingThumbnail", record, "Thumbnail preview path is missing; Content Browser will use a type fallback icon.", record.thumbnailPath);
            }
            for (const AssetDependency& dependency : record.dependencies) {
                if (!dependency.guid.empty() && registryGuids.find(dependency.guid) == registryGuids.end()) {
                    missingDependencies.push_back({
                        {"severity", "error"},
                        {"kind", "MissingDependencyGuid"},
                        {"ownerGuid", record.guid},
                        {"ownerDisplayName", record.displayName},
                        {"ownerAssetType", assetTypeName(record.type)},
                        {"dependencyGuid", dependency.guid},
                        {"dependencyKind", dependency.kind},
                        {"detail", "Dependency GUID is not present in the asset registry."},
                    });
                }
            }
            if (record.stale || record.status == AssetImportStatus::Stale) {
                appendValidationIssue(staleAssets, "warning", "StaleAsset", record, "Source is newer than imported metadata or cooked payload.");
            }
            if (record.status == AssetImportStatus::Failed || record.stale || importedMissing || cookedMissing) {
                appendValidationIssue(requiresReimport, record.status == AssetImportStatus::Failed ? "error" : "warning", "RequiresReimport", record, "Asset should be reimported or repaired before cooking/packaging.");
            }
            if (record.importSettings.unitScale <= 0.0f) {
                appendValidationIssue(unsupportedImportSettings, "error", "InvalidUnitScale", record, "Import unit scale must be greater than zero.");
            }
            if (!supportedCoordinateConversion(record.importSettings.coordinateConversion)) {
                appendValidationIssue(unsupportedImportSettings, "warning", "UnsupportedCoordinateConversion", record, "Import coordinate conversion is not recognized: " + record.importSettings.coordinateConversion);
            }
            if (!supportedMaterialImportMode(record.importSettings.materialImportMode)) {
                appendValidationIssue(unsupportedImportSettings, "warning", "UnsupportedMaterialImportMode", record, "Material import mode is not recognized: " + record.importSettings.materialImportMode);
            }
            if (!supportedTextureImportMode(record.importSettings.textureImportMode)) {
                appendValidationIssue(unsupportedImportSettings, "warning", "UnsupportedTextureImportMode", record, "Texture import mode is not recognized: " + record.importSettings.textureImportMode);
            }
            if (!supportedTextureCompression(record.importSettings.textureCompression)) {
                appendValidationIssue(unsupportedImportSettings, "warning", "UnsupportedTextureCompression", record, "Texture compression/transcode mode is not available in this pipeline stage: " + record.importSettings.textureCompression);
            }
        }
    }

    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            if (scopedToAsset && renderer.meshGuid == targetGuid) {
                currentSceneReferences.push_back({
                    {"entity", entity->name},
                    {"entityUuid", entity->uuid},
                    {"component", "MeshRenderer"},
                    {"field", "meshGuid"},
                    {"guid", renderer.meshGuid},
                });
            } else if (!scopedToAsset && !renderer.meshGuid.empty() && registryGuids.find(renderer.meshGuid) == registryGuids.end()) {
                appendComponentReferenceIssue(invalidComponentReferences, *entity, "MeshRenderer", "meshGuid", renderer.meshGuid);
            }
            for (const MaterialSlot& slot : renderer.materialSlots) {
                if (scopedToAsset && slot.materialGuid == targetGuid) {
                    currentSceneReferences.push_back({
                        {"entity", entity->name},
                        {"entityUuid", entity->uuid},
                        {"component", "MeshRenderer"},
                        {"field", "materialGuid"},
                        {"guid", slot.materialGuid},
                    });
                } else if (!scopedToAsset && !slot.materialGuid.empty() && registryGuids.find(slot.materialGuid) == registryGuids.end()) {
                    appendComponentReferenceIssue(invalidComponentReferences, *entity, "MeshRenderer", "materialGuid", slot.materialGuid);
                }
                if (scopedToAsset && slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == targetGuid) {
                    currentSceneReferences.push_back({
                        {"entity", entity->name},
                        {"entityUuid", entity->uuid},
                        {"component", "MeshRenderer"},
                        {"field", "overrideMaterialGuid"},
                        {"guid", *slot.overrideMaterialGuid},
                    });
                } else if (!scopedToAsset && slot.overrideMaterialGuid.has_value() && !slot.overrideMaterialGuid->empty() && registryGuids.find(*slot.overrideMaterialGuid) == registryGuids.end()) {
                    appendComponentReferenceIssue(invalidComponentReferences, *entity, "MeshRenderer", "overrideMaterialGuid", *slot.overrideMaterialGuid);
                }
            }
        }
        for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
            if (scopedToAsset && instance.prefabGuid == targetGuid) {
                currentSceneReferences.push_back({
                    {"entity", "Prefab Instance"},
                    {"entityUuid", instance.instanceRoot.index},
                    {"component", "PrefabInstance"},
                    {"field", "prefabGuid"},
                    {"guid", instance.prefabGuid},
                });
            } else if (!scopedToAsset && !instance.prefabGuid.empty() && registryGuids.find(instance.prefabGuid) == registryGuids.end()) {
                invalidComponentReferences.push_back({
                    {"severity", "error"},
                    {"kind", "InvalidPrefabInstanceReference"},
                    {"prefabGuid", instance.prefabGuid},
                    {"instanceRoot", instance.instanceRoot.index},
                    {"detail", "Prefab instance references an asset GUID that is not present in the asset registry."},
                });
            }
        }
    }

    size_t savedProjectReferenceScannedFileCount = 0;
    if (!scopedToAsset) {
        std::vector<std::filesystem::path> files;
        collectProjectReferenceScanFiles(state, browserRoot, savedProjectReferenceScanRoots, files);
        savedProjectReferenceScannedFileCount = files.size();
        for (const std::filesystem::path& path : files) {
            std::optional<nlohmann::json> json = readJsonFile(path);
            if (!json.has_value()) {
                savedProjectReferenceParseErrors.push_back({
                    {"severity", "warning"},
                    {"kind", "SavedProjectReferenceParseError"},
                    {"file", path.generic_string()},
                    {"detail", "File matched the project reference validation set but could not be parsed as JSON."},
                });
                continue;
            }
            appendInvalidSavedGuidReferences(*json, registryGuids, path, "$", {}, invalidSavedProjectReferences);
        }
    }

    const std::vector<const nlohmann::json*> issueArrays = {
        &missingSources,
        &missingImportedMetadata,
        &missingCookedPayloads,
        &missingThumbnails,
        &missingDependencies,
        &staleAssets,
        &unsupportedImportSettings,
        &invalidComponentReferences,
        &invalidSavedProjectReferences,
        &savedProjectReferenceParseErrors,
        &requiresReimport,
    };
    const size_t errorCount = countValidationSeverity(issueArrays, "error");
    const size_t warningCount = countValidationSeverity(issueArrays, "warning");
    return {
        {"version", 1},
        {"kind", scopedToAsset ? "SelectedAssetValidationReport" : "AssetValidationReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"assetCount", scopedToAsset ? validatedAssetCount : state.assetRegistry != nullptr ? state.assetRegistry->records().size() : 0},
        {"errorCount", errorCount},
        {"warningCount", warningCount},
        {"sourceControlPolicy", sourceControlPolicyReportJson(copiedSourceAssetCount)},
        {"missingSources", missingSources},
        {"missingImportedMetadata", missingImportedMetadata},
        {"missingCookedPayloads", missingCookedPayloads},
        {"missingThumbnails", missingThumbnails},
        {"missingDependencies", missingDependencies},
        {"staleAssets", staleAssets},
        {"unsupportedImportSettings", unsupportedImportSettings},
        {"invalidComponentReferences", invalidComponentReferences},
        {"invalidSavedProjectReferences", invalidSavedProjectReferences},
        {"savedProjectReferenceParseErrors", savedProjectReferenceParseErrors},
        {"savedProjectReferenceScanRoots", savedProjectReferenceScanRoots},
        {"savedProjectReferenceScannedFileCount", savedProjectReferenceScannedFileCount},
        {"requiresReimport", requiresReimport},
        {"reverseAssetReferences", reverseAssetReferences},
        {"currentSceneReferences", currentSceneReferences},
    };
}

nlohmann::json expectedDependencyAssetTypes(const std::string& role) {
    const std::string lowerRole = lowerString(role);
    nlohmann::json types = nlohmann::json::array();
    auto addType = [&](AssetType type) {
        types.push_back(assetTypeName(type));
    };
    if (lowerRole.find("material") != std::string::npos) {
        addType(AssetType::Material);
    } else if (lowerRole.find("texture") != std::string::npos || lowerRole.find("image") != std::string::npos || lowerRole.find("hdr") != std::string::npos || lowerRole.find("environment") != std::string::npos) {
        addType(AssetType::Texture);
        addType(AssetType::HDRI);
    } else if (lowerRole.find("mesh") != std::string::npos) {
        addType(AssetType::Mesh);
    } else if (lowerRole.find("prefab") != std::string::npos || lowerRole.find("model") != std::string::npos) {
        addType(AssetType::Prefab);
    } else if (lowerRole.find("anim") != std::string::npos) {
        addType(AssetType::Animation);
    } else if (lowerRole.find("skeleton") != std::string::npos || lowerRole.find("skin") != std::string::npos) {
        addType(AssetType::Skeleton);
    }
    return types;
}

bool dependencyRoleMatchesType(const std::string& role, AssetType type) {
    const nlohmann::json expectedTypes = expectedDependencyAssetTypes(role);
    if (expectedTypes.empty()) {
        return true;
    }
    const std::string typeName = assetTypeName(type);
    for (const nlohmann::json& expected : expectedTypes) {
        if (expected.is_string() && expected.get<std::string>() == typeName) {
            return true;
        }
    }
    return false;
}

nlohmann::json dependencyRepairCandidatesJson(
    const EditorRuntimeState& state,
    const AssetRecord& owner,
    const AssetDependency& dependency) {
    nlohmann::json candidates = nlohmann::json::array();
    if (state.assetRegistry == nullptr) {
        return candidates;
    }

    std::vector<std::pair<int, const AssetRecord*>> scored;
    for (const AssetRecord& candidate : state.assetRegistry->records()) {
        if (candidate.guid.empty() || candidate.guid == owner.guid || candidate.guid == dependency.guid) {
            continue;
        }
        if (!dependencyRoleMatchesType(dependency.kind, candidate.type)) {
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
        if (score == 0 && !expectedDependencyAssetTypes(dependency.kind).empty()) {
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

    const size_t maxCandidates = std::min<size_t>(scored.size(), 16u);
    for (size_t i = 0; i < maxCandidates; ++i) {
        const AssetRecord* candidate = scored[i].second;
        if (candidate == nullptr) {
            continue;
        }
        candidates.push_back({
            {"score", scored[i].first},
            {"asset", assetRecordSummaryJson(state, *candidate)},
            {"reason", "Role-compatible asset in the loaded registry, prioritized by shared import group/root metadata."},
        });
    }
    return candidates;
}

nlohmann::json buildAssetDependencyReport(const EditorRuntimeState& state, const AssetGuid& targetGuid) {
    nlohmann::json dependencies = nlohmann::json::array();
    nlohmann::json storedReferences = nlohmann::json::array();
    nlohmann::json missingDependencyRepairPlan = nlohmann::json::array();
    nlohmann::json selectedAsset = nlohmann::json::object();
    if (state.assetRegistry == nullptr) {
        return {
            {"version", 1},
            {"kind", "SelectedAssetDependencyReport"},
            {"targetGuid", targetGuid},
            {"selectedAsset", selectedAsset},
            {"dependencies", dependencies},
            {"storedReferences", storedReferences},
        };
    }

    const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid);
    if (target != nullptr) {
        selectedAsset = assetRecordSummaryJson(state, *target);
        for (const AssetDependency& dependency : target->dependencies) {
            const AssetRecord* linked = findAssetRecordByGuid(*state.assetRegistry, dependency.guid);
            nlohmann::json repairCandidates = linked == nullptr ? dependencyRepairCandidatesJson(state, *target, dependency) : nlohmann::json::array();
            nlohmann::json dependencyEntry = {
                {"guid", dependency.guid},
                {"role", dependency.kind.empty() ? "dependency" : dependency.kind},
                {"expectedAssetTypes", expectedDependencyAssetTypes(dependency.kind)},
                {"found", linked != nullptr},
                {"asset", linked != nullptr ? assetRecordSummaryJson(state, *linked) : nlohmann::json::object()},
                {"repairCandidateCount", repairCandidates.size()},
                {"unambiguousRepairCandidate", linked == nullptr && repairCandidates.size() == 1u},
                {"repairCandidates", repairCandidates},
            };
            if (linked == nullptr) {
                missingDependencyRepairPlan.push_back({
                    {"missingGuid", dependency.guid},
                    {"role", dependency.kind.empty() ? "dependency" : dependency.kind},
                    {"expectedAssetTypes", expectedDependencyAssetTypes(dependency.kind)},
                    {"candidateCount", repairCandidates.size()},
                    {"unambiguousRepairCandidate", repairCandidates.size() == 1u},
                    {"candidates", repairCandidates},
                });
            }
            dependencies.push_back(std::move(dependencyEntry));
        }
        for (const AssetGuid& reference : target->references) {
            const AssetRecord* linked = findAssetRecordByGuid(*state.assetRegistry, reference);
            storedReferences.push_back({
                {"guid", reference},
                {"role", "reference"},
                {"found", linked != nullptr},
                {"asset", linked != nullptr ? assetRecordSummaryJson(state, *linked) : nlohmann::json::object()},
            });
        }
    }

    size_t unambiguousMissingDependencyRepairCount = 0;
    for (const nlohmann::json& item : missingDependencyRepairPlan) {
        if (item.value("unambiguousRepairCandidate", false)) {
            ++unambiguousMissingDependencyRepairCount;
        }
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetDependencyReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"dependencyCount", dependencies.size()},
        {"storedReferenceCount", storedReferences.size()},
        {"missingDependencyCount", missingDependencyRepairPlan.size()},
        {"unambiguousMissingDependencyRepairCount", unambiguousMissingDependencyRepairCount},
        {"dependencies", dependencies},
        {"storedReferences", storedReferences},
        {"missingDependencyRepairPlan", missingDependencyRepairPlan},
        {"repairPolicy", "This report is non-destructive. Missing dependencies can be repaired safely only when a replacement candidate is unambiguous and the user explicitly rewrites references or reimports the source asset."},
    };
}

nlohmann::json buildAssetReferenceReport(const EditorRuntimeState& state, const AssetGuid& targetGuid) {
    nlohmann::json registryReferences = nlohmann::json::array();
    nlohmann::json currentSceneReferences = nlohmann::json::array();
    nlohmann::json selectedAsset = nlohmann::json::object();
    if (state.assetRegistry != nullptr) {
        const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid);
        if (target != nullptr) {
            selectedAsset = assetRecordSummaryJson(state, *target);
        }
        for (const AssetRecord& owner : state.assetRegistry->records()) {
            if (owner.guid == targetGuid) {
                continue;
            }
            for (const AssetDependency& dependency : owner.dependencies) {
                if (dependency.guid == targetGuid) {
                    registryReferences.push_back({
                        {"source", "Dependency"},
                        {"role", dependency.kind.empty() ? "dependency" : dependency.kind},
                        {"owner", assetRecordSummaryJson(state, owner)},
                    });
                }
            }
            for (const AssetGuid& reference : owner.references) {
                if (reference == targetGuid) {
                    registryReferences.push_back({
                        {"source", "Reference"},
                        {"role", "reference"},
                        {"owner", assetRecordSummaryJson(state, owner)},
                    });
                }
            }
        }
    }

    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            auto appendSceneReference = [&](const char* field) {
                currentSceneReferences.push_back({
                    {"entity", entity->name},
                    {"entityUuid", entity->uuid},
                    {"component", "MeshRenderer"},
                    {"field", field},
                });
            };
            if (renderer.meshGuid == targetGuid) {
                appendSceneReference("meshGuid");
            }
            for (const MaterialSlot& slot : renderer.materialSlots) {
                if (slot.materialGuid == targetGuid) {
                    appendSceneReference("materialGuid");
                }
                if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == targetGuid) {
                    appendSceneReference("overrideMaterialGuid");
                }
            }
        }
        for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
            if (instance.prefabGuid == targetGuid) {
                currentSceneReferences.push_back({
                    {"entity", "Prefab Instance"},
                    {"entityUuid", instance.instanceRoot.index},
                    {"component", "PrefabInstance"},
                    {"field", "prefabGuid"},
                });
            }
        }
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetReferenceReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"registryReferenceCount", registryReferences.size()},
        {"currentSceneReferenceCount", currentSceneReferences.size()},
        {"registryReferences", registryReferences},
        {"currentSceneReferences", currentSceneReferences},
    };
}

nlohmann::json buildAssetDependencyGraphReport(const EditorRuntimeState& state) {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json edges = nlohmann::json::array();
    nlohmann::json missingTargets = nlohmann::json::array();
    nlohmann::json currentSceneUsages = nlohmann::json::array();
    if (state.assetRegistry == nullptr) {
        return {
            {"version", 1},
            {"kind", "AssetDependencyGraphReport"},
            {"assetCount", 0},
            {"edgeCount", 0},
            {"missingTargetCount", 0},
            {"currentSceneUsageCount", 0},
            {"nodes", nodes},
            {"edges", edges},
            {"missingTargets", missingTargets},
            {"currentSceneUsages", currentSceneUsages},
        };
    }

    std::unordered_set<AssetGuid> registryGuids;
    std::unordered_map<AssetGuid, size_t> incomingRegistryEdgeCount;
    std::unordered_map<AssetGuid, size_t> currentSceneUsageCount;
    registryGuids.reserve(state.assetRegistry->records().size());
    for (const AssetRecord& record : state.assetRegistry->records()) {
        if (!record.guid.empty()) {
            registryGuids.insert(record.guid);
            incomingRegistryEdgeCount.emplace(record.guid, 0);
            currentSceneUsageCount.emplace(record.guid, 0);
        }
    }

    auto appendEdge = [&](const AssetRecord& owner, const AssetGuid& targetGuid, const char* relation, const std::string& role) {
        const bool targetFound = !targetGuid.empty() && registryGuids.find(targetGuid) != registryGuids.end();
        edges.push_back({
            {"sourceGuid", owner.guid},
            {"sourceDisplayName", owner.displayName},
            {"targetGuid", targetGuid},
            {"relation", relation},
            {"role", role.empty() ? relation : role},
            {"targetFound", targetFound},
        });
        if (targetFound) {
            ++incomingRegistryEdgeCount[targetGuid];
        } else {
            missingTargets.push_back({
                {"sourceGuid", owner.guid},
                {"sourceDisplayName", owner.displayName},
                {"targetGuid", targetGuid},
                {"relation", relation},
                {"role", role.empty() ? relation : role},
            });
        }
    };

    for (const AssetRecord& record : state.assetRegistry->records()) {
        for (const AssetDependency& dependency : record.dependencies) {
            appendEdge(record, dependency.guid, "dependency", dependency.kind);
        }
        for (const AssetGuid& reference : record.references) {
            appendEdge(record, reference, "reference", "reference");
        }
    }

    auto appendSceneUsage = [&](const AssetGuid& guid, const Entity* entity, const char* component, const char* field) {
        if (guid.empty()) {
            return;
        }
        const AssetRecord* asset = findAssetRecordByGuid(*state.assetRegistry, guid);
        currentSceneUsages.push_back({
            {"guid", guid},
            {"assetFound", asset != nullptr},
            {"asset", asset != nullptr ? assetRecordSummaryJson(state, *asset) : nlohmann::json::object()},
            {"entity", entity != nullptr ? entity->name : std::string{}},
            {"entityUuid", entity != nullptr ? nlohmann::json(entity->uuid) : nlohmann::json(nullptr)},
            {"component", component},
            {"field", field},
        });
        if (asset != nullptr) {
            ++currentSceneUsageCount[guid];
        }
    };

    if (state.sceneDocument != nullptr) {
        for (const Entity* entity : state.sceneDocument->registry().entities()) {
            if (entity == nullptr || !entity->meshRenderer.has_value()) {
                continue;
            }
            const MeshRenderer& renderer = *entity->meshRenderer;
            appendSceneUsage(renderer.meshGuid, entity, "MeshRenderer", "meshGuid");
            for (const MaterialSlot& slot : renderer.materialSlots) {
                appendSceneUsage(slot.materialGuid, entity, "MeshRenderer", "materialGuid");
                if (slot.overrideMaterialGuid.has_value()) {
                    appendSceneUsage(*slot.overrideMaterialGuid, entity, "MeshRenderer", "overrideMaterialGuid");
                }
            }
        }
        for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
            if (instance.prefabGuid.empty()) {
                continue;
            }
            const AssetRecord* asset = findAssetRecordByGuid(*state.assetRegistry, instance.prefabGuid);
            currentSceneUsages.push_back({
                {"guid", instance.prefabGuid},
                {"assetFound", asset != nullptr},
                {"asset", asset != nullptr ? assetRecordSummaryJson(state, *asset) : nlohmann::json::object()},
                {"entity", "Prefab Instance"},
                {"entityUuid", instance.instanceRoot.index},
                {"component", "PrefabInstance"},
                {"field", "prefabGuid"},
            });
            if (asset != nullptr) {
                ++currentSceneUsageCount[instance.prefabGuid];
            }
        }
    }

    for (const AssetRecord& record : state.assetRegistry->records()) {
        nlohmann::json node = assetRecordSummaryJson(state, record);
        node["outgoingDependencyCount"] = record.dependencies.size();
        node["storedReferenceCount"] = record.references.size();
        node["incomingRegistryEdgeCount"] = incomingRegistryEdgeCount[record.guid];
        node["currentSceneUsageCount"] = currentSceneUsageCount[record.guid];
        nodes.push_back(std::move(node));
    }

    return {
        {"version", 1},
        {"kind", "AssetDependencyGraphReport"},
        {"registryPath", state.assetRegistry->state().path.empty() ? std::string{} : state.assetRegistry->state().path.generic_string()},
        {"assetCount", nodes.size()},
        {"edgeCount", edges.size()},
        {"missingTargetCount", missingTargets.size()},
        {"currentSceneUsageCount", currentSceneUsages.size()},
        {"nodes", nodes},
        {"edges", edges},
        {"missingTargets", missingTargets},
        {"currentSceneUsages", currentSceneUsages},
        {"checkedScopes", nlohmann::json::array({"LoadedAssetRegistry", "CurrentScene"})},
        {"limitation", "This graph uses the loaded asset registry plus current scene references. It does not rewrite references or scan saved project files; use Delete Readiness or Validate Project for saved metadata scans."},
    };
}

std::string dotEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': break;
        case '\t': escaped.push_back(' '); break;
        default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

std::string dotString(const std::string& value) {
    return std::string("\"") + dotEscape(value) + "\"";
}

std::string buildAssetDependencyGraphDot(const nlohmann::json& graph) {
    std::ostringstream dot;
    std::unordered_set<std::string> emittedMissingTargets;

    auto emitMissingTarget = [&](const std::string& guid) {
        const std::string key = guid.empty() ? std::string("<empty>") : guid;
        if (!emittedMissingTargets.insert(key).second) {
            return;
        }
        dot << "  " << dotString("missing:" + key)
            << " [label=" << dotString(std::string("Missing Asset\n") + key)
            << ", shape=octagon, color=\"#c44\", fontcolor=\"#8a1f1f\", fillcolor=\"#ffe8e8\", style=\"filled\"];\n";
    };

    dot << "digraph AssetDependencyGraph {\n";
    dot << "  graph [rankdir=LR, labelloc=\"t\", label="
        << dotString("Asset Dependency Graph\nassets=" + std::to_string(graph.value("assetCount", 0u))
            + " edges=" + std::to_string(graph.value("edgeCount", 0u))
            + " currentSceneUsages=" + std::to_string(graph.value("currentSceneUsageCount", 0u)))
        << "];\n";
    dot << "  node [shape=box, fontname=\"Consolas\", fontsize=10, style=\"rounded,filled\", fillcolor=\"#f8fbff\", color=\"#7a8aa0\"];\n";
    dot << "  edge [fontname=\"Consolas\", fontsize=9, color=\"#546a7b\"];\n\n";

    if (graph.contains("nodes") && graph["nodes"].is_array()) {
        dot << "  subgraph cluster_assets {\n";
        dot << "    label=\"Loaded Asset Registry\";\n";
        for (const nlohmann::json& node : graph["nodes"]) {
            const std::string guid = node.value("guid", std::string{});
            if (guid.empty()) {
                continue;
            }
            std::string name = node.value("displayName", std::string{});
            if (name.empty()) {
                name = guid;
            }
            const std::string type = node.value("assetType", std::string("Asset"));
            const bool missing = node.value("missing", false) || node.value("dependenciesMissing", false);
            dot << "    " << dotString("asset:" + guid)
                << " [label=" << dotString(name + "\n" + type + "\n" + guid)
                << ", tooltip=" << dotString(guid)
                << ", fillcolor=" << dotString(missing ? "#fff0df" : "#f8fbff")
                << "];\n";
        }
        dot << "  }\n\n";
    }

    if (graph.contains("edges") && graph["edges"].is_array()) {
        for (const nlohmann::json& edge : graph["edges"]) {
            const std::string sourceGuid = edge.value("sourceGuid", std::string{});
            const std::string targetGuid = edge.value("targetGuid", std::string{});
            if (sourceGuid.empty()) {
                continue;
            }
            const bool targetFound = edge.value("targetFound", false) && !targetGuid.empty();
            if (!targetFound) {
                emitMissingTarget(targetGuid);
            }
            const std::string relation = edge.value("relation", std::string("reference"));
            const std::string role = edge.value("role", relation);
            dot << "  " << dotString("asset:" + sourceGuid)
                << " -> " << dotString(targetFound ? "asset:" + targetGuid : "missing:" + (targetGuid.empty() ? std::string("<empty>") : targetGuid))
                << " [label=" << dotString(role)
                << ", style=" << dotString(targetFound ? (relation == "dependency" ? "solid" : "dashed") : "dashed")
                << ", color=" << dotString(targetFound ? (relation == "dependency" ? "#336699" : "#777777") : "#c44")
                << "];\n";
        }
        dot << "\n";
    }

    if (graph.contains("currentSceneUsages") && graph["currentSceneUsages"].is_array()) {
        dot << "  subgraph cluster_current_scene {\n";
        dot << "    label=\"Current Scene Usages\";\n";
        size_t index = 0;
        for (const nlohmann::json& usage : graph["currentSceneUsages"]) {
            const std::string guid = usage.value("guid", std::string{});
            const std::string sceneNodeId = "scene:" + std::to_string(index++);
            const std::string entity = usage.value("entity", std::string("Scene"));
            const std::string component = usage.value("component", std::string("Component"));
            const std::string field = usage.value("field", std::string("guid"));
            const bool assetFound = usage.value("assetFound", false) && !guid.empty();
            if (!assetFound) {
                emitMissingTarget(guid);
            }
            dot << "    " << dotString(sceneNodeId)
                << " [label=" << dotString(entity + "\n" + component + "." + field)
                << ", shape=note, fillcolor=\"#eef8ee\", color=\"#6a9a6a\"];\n";
            dot << "    " << dotString(sceneNodeId)
                << " -> " << dotString(assetFound ? "asset:" + guid : "missing:" + (guid.empty() ? std::string("<empty>") : guid))
                << " [label=\"uses\", style=\"dotted\", color=\"#4f8a4f\"];\n";
        }
        dot << "  }\n";
    }

    dot << "}\n";
    return dot.str();
}

std::string htmlEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

std::string buildAssetDependencyGraphHtml(const nlohmann::json& graph) {
    std::ostringstream html;
    html << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
    html << "<title>Asset Dependency Graph</title>\n";
    html << "<style>"
            "body{margin:0;font:13px Segoe UI,Arial,sans-serif;background:#111820;color:#dce5ef;}"
            "header{display:flex;gap:12px;align-items:center;padding:10px 14px;background:#1b2633;border-bottom:1px solid #314052;}"
            "header h1{font-size:16px;margin:0 12px 0 0;font-weight:600;}"
            "input,select{background:#0f151d;color:#dce5ef;border:1px solid #405267;border-radius:3px;padding:5px 7px;}"
            "button{background:#25364a;color:#e6edf6;border:1px solid #47617b;border-radius:3px;padding:5px 8px;cursor:pointer;}"
            "#wrap{display:grid;grid-template-columns:minmax(0,1fr) 330px;height:calc(100vh - 47px);}"
            "#graph{position:relative;overflow:auto;background:#f4f7fb;}svg{min-width:1200px;min-height:720px;}"
            ".node rect{fill:#ffffff;stroke:#60758d;stroke-width:1.2px;rx:4px}.node.missing rect{fill:#ffe8e8;stroke:#ba4545}.node.scene rect{fill:#e9f7ea;stroke:#5f985f}.node.selected rect{stroke:#1f78d1;stroke-width:3px}"
            ".edge{stroke:#5f7287;stroke-width:1.4px;fill:none}.edge.reference{stroke-dasharray:5 4}.edge.missing{stroke:#c44;stroke-dasharray:6 4}.edge.scene{stroke:#3f8d4a;stroke-dasharray:2 4}.edge.hidden,.node.hidden{display:none}"
            ".label{font-size:11px;fill:#17212c;pointer-events:none}.meta{font-size:10px;fill:#536171;pointer-events:none}.edgeLabel{font-size:10px;fill:#26384c;background:#fff}"
            "aside{overflow:auto;background:#121b25;border-left:1px solid #314052;padding:12px;}pre{white-space:pre-wrap;word-break:break-word;background:#0b1118;padding:8px;border:1px solid #263647;border-radius:3px;}"
            ".stat{color:#9db1c8}.warn{color:#ffb36b}"
            "</style>\n</head>\n<body>\n";
    html << "<header><h1>Asset Dependency Graph</h1>"
            "<span class=\"stat\" id=\"stats\"></span>"
            "<input id=\"filter\" placeholder=\"Filter name, GUID, type, role\" size=\"36\">"
            "<select id=\"edgeMode\"><option value=\"all\">All edges</option><option value=\"dependency\">Dependencies</option><option value=\"reference\">References</option><option value=\"missing\">Missing only</option><option value=\"scene\">Scene usages</option></select>"
            "<button id=\"reset\">Reset</button></header>\n";
    html << "<div id=\"wrap\"><main id=\"graph\"><svg id=\"svg\" viewBox=\"0 0 1200 720\"></svg></main><aside><h2>Selection</h2><div id=\"details\" class=\"stat\">Select a node or edge.</div><h2>Policy</h2><p>This is an interactive HTML sidecar generated from the loaded asset registry and current scene usage graph.</p><p class=\"warn\">It does not mutate references, watch files continuously, or inspect package/cache internals.</p></aside></div>\n";
    html << "<script id=\"graph-data\" type=\"application/json\">" << htmlEscape(graph.dump()) << "</script>\n";
    html << R"SCRIPT(<script>
const graph = JSON.parse(document.getElementById('graph-data').textContent);
const svg = document.getElementById('svg');
const details = document.getElementById('details');
const filter = document.getElementById('filter');
const edgeMode = document.getElementById('edgeMode');
const stats = document.getElementById('stats');
const nodeById = new Map();
const edges = [];
function el(name, attrs = {}) { const e = document.createElementNS('http://www.w3.org/2000/svg', name); for (const [k,v] of Object.entries(attrs)) e.setAttribute(k, v); return e; }
function textValue(v) { return (v ?? '').toString(); }
function assetLabel(asset) { return textValue(asset.displayName || asset.guid || '(asset)'); }
function show(obj) { details.innerHTML = '<pre>' + JSON.stringify(obj, null, 2).replace(/[&<>]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c])) + '</pre>'; }
function addNode(id, label, meta, kind, payload, x, y) {
  const g = el('g', {class:'node ' + kind, transform:`translate(${x},${y})`, 'data-search':`${label} ${meta} ${id}`.toLowerCase()});
  g.appendChild(el('rect', {width:170, height:54}));
  const t = el('text', {x:8, y:20, class:'label'}); t.textContent = label.slice(0, 26); g.appendChild(t);
  const m = el('text', {x:8, y:39, class:'meta'}); m.textContent = meta.slice(0, 30); g.appendChild(m);
  g.addEventListener('click', () => { document.querySelectorAll('.node.selected').forEach(n => n.classList.remove('selected')); g.classList.add('selected'); show(payload); });
  svg.appendChild(g); nodeById.set(id, {id, label, meta, kind, payload, x, y, element:g}); return nodeById.get(id);
}
function addEdge(sourceId, targetId, relation, role, payload) {
  const s = nodeById.get(sourceId); const t = nodeById.get(targetId); if (!s || !t) return;
  const sx = s.x + 170, sy = s.y + 27, tx = t.x, ty = t.y + 27;
  const path = el('path', {class:`edge ${relation}`, d:`M ${sx} ${sy} C ${sx+60} ${sy}, ${tx-60} ${ty}, ${tx} ${ty}`, 'data-search':`${relation} ${role} ${s.label} ${t.label}`.toLowerCase()});
  path.addEventListener('click', () => show(payload)); svg.insertBefore(path, svg.firstChild);
  const mid = el('text', {x:(sx+tx)/2 - 20, y:(sy+ty)/2 - 4, class:'edgeLabel'}); mid.textContent = role; svg.appendChild(mid);
  edges.push({sourceId, targetId, relation, role, payload, element:path, label:mid});
}
function build() {
  svg.innerHTML = ''; nodeById.clear(); edges.length = 0;
  const nodes = graph.nodes || []; const cols = Math.max(1, Math.ceil(Math.sqrt(Math.max(1, nodes.length))));
  nodes.forEach((n, i) => addNode('asset:' + n.guid, assetLabel(n), `${n.assetType || 'Asset'}  in:${n.incomingRegistryEdgeCount || 0} out:${n.outgoingDependencyCount || 0}`, n.missing || n.dependenciesMissing ? 'missing' : 'asset', n, 40 + (i % cols) * 230, 40 + Math.floor(i / cols) * 95));
  const missing = new Set();
  (graph.edges || []).forEach(e => { if (!e.targetFound) missing.add(e.targetGuid || '<empty>'); });
  (graph.currentSceneUsages || []).forEach(u => { if (!u.assetFound) missing.add(u.guid || '<empty>'); });
  let rowBase = Math.ceil(nodes.length / cols) * 95 + 80; let missIndex = 0;
  missing.forEach(guid => addNode('missing:' + guid, 'Missing Asset', guid, 'missing', {missingGuid:guid}, 40 + (missIndex++ % cols) * 230, rowBase + Math.floor((missIndex - 1) / cols) * 95));
  (graph.currentSceneUsages || []).forEach((u, i) => addNode('scene:' + i, u.entity || 'Scene usage', `${u.component || 'Component'}.${u.field || 'guid'}`, 'scene', u, 40 + (i % cols) * 230, rowBase + 120 + Math.floor(i / cols) * 95));
  (graph.edges || []).forEach(e => addEdge('asset:' + e.sourceGuid, e.targetFound ? 'asset:' + e.targetGuid : 'missing:' + (e.targetGuid || '<empty>'), e.targetFound ? e.relation : 'missing', e.role || e.relation, e));
  (graph.currentSceneUsages || []).forEach((u, i) => addEdge('scene:' + i, u.assetFound ? 'asset:' + u.guid : 'missing:' + (u.guid || '<empty>'), 'scene', 'uses', u));
  stats.textContent = `assets=${graph.assetCount || 0} edges=${graph.edgeCount || 0} missing=${graph.missingTargetCount || 0} scene=${graph.currentSceneUsageCount || 0}`;
}
function applyFilters() {
  const q = filter.value.toLowerCase(); const mode = edgeMode.value;
  nodeById.forEach(n => n.element.classList.toggle('hidden', q && !n.element.dataset.search.includes(q)));
  edges.forEach(e => { const modeHide = mode !== 'all' && e.relation !== mode; const textHide = q && !e.element.dataset.search.includes(q) && !(nodeById.get(e.sourceId)?.element.dataset.search.includes(q)) && !(nodeById.get(e.targetId)?.element.dataset.search.includes(q)); e.element.classList.toggle('hidden', modeHide || textHide); e.label.classList.toggle('hidden', modeHide || textHide); });
}
filter.addEventListener('input', applyFilters); edgeMode.addEventListener('change', applyFilters); document.getElementById('reset').addEventListener('click', () => { filter.value=''; edgeMode.value='all'; applyFilters(); });
build();
</script>)SCRIPT";
    html << "\n</body>\n</html>\n";
    return html.str();
}

nlohmann::json buildAssetProjectReferenceIndexReport(const EditorRuntimeState& state, const std::filesystem::path& browserRoot) {
    nlohmann::json checkedRoots = nlohmann::json::array();
    nlohmann::json scannedFiles = nlohmann::json::array();
    nlohmann::json references = nlohmann::json::array();
    nlohmann::json unknownGuidFields = nlohmann::json::array();
    nlohmann::json parseErrors = nlohmann::json::array();
    nlohmann::json assets = nlohmann::json::array();
    std::unordered_set<AssetGuid> registryGuids;
    std::unordered_map<AssetGuid, size_t> savedReferenceCounts;

    if (state.assetRegistry != nullptr) {
        registryGuids.reserve(state.assetRegistry->records().size());
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (!record.guid.empty()) {
                registryGuids.insert(record.guid);
                savedReferenceCounts.emplace(record.guid, 0);
            }
        }
    }

    std::vector<std::filesystem::path> files;
    collectProjectReferenceScanFiles(state, browserRoot, checkedRoots, files);
    for (const std::filesystem::path& path : files) {
        scannedFiles.push_back(path.generic_string());
        std::optional<nlohmann::json> json = readJsonFile(path);
        if (!json.has_value()) {
            parseErrors.push_back({
                {"severity", "warning"},
                {"kind", "SavedProjectReferenceParseError"},
                {"file", path.generic_string()},
                {"detail", "File matched the project reference index set but could not be parsed as JSON."},
            });
            continue;
        }
        const size_t before = references.size();
        appendProjectReferenceIndexEntries(state, *json, registryGuids, path, "$", {}, references, unknownGuidFields);
        for (size_t i = before; i < references.size(); ++i) {
            const AssetGuid guid = references[i].value("guid", std::string{});
            if (!guid.empty()) {
                ++savedReferenceCounts[guid];
            }
        }
    }

    if (state.assetRegistry != nullptr) {
        for (const AssetRecord& record : state.assetRegistry->records()) {
            nlohmann::json asset = assetRecordSummaryJson(state, record);
            asset["savedProjectReferenceCount"] = savedReferenceCounts[record.guid];
            assets.push_back(std::move(asset));
        }
    }

    return {
        {"version", 1},
        {"kind", "AssetProjectReferenceIndexReport"},
        {"registryPath", state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty() ? state.assetRegistry->state().path.generic_string() : std::string{}},
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
        {"checkedFileTypes", nlohmann::json::array({".rtlevel", ".mscene", ".vproject", ".rtprefab.json", ".rtmesh.json", ".rtmaterial.json", ".rttexture.json", ".rthdri.json"})},
        {"persistence", "The latest generated index is also written to Saved/AssetReferenceIndex.json when the Project References action runs."},
        {"limitation", "This is a regenerated-on-demand saved-file JSON index for project content and scene roots. It is persisted as the latest index artifact, but it is not continuously updated in the background and does not rewrite references or inspect generated cache payload internals."},
    };
}

void appendDuplicateAssetGroups(
    const EditorRuntimeState& state,
    const std::unordered_map<std::string, std::vector<const AssetRecord*>>& groupsByKey,
    const char* reason,
    const char* confidence,
    nlohmann::json& duplicateGroups) {
    std::vector<std::pair<std::string, std::vector<const AssetRecord*>>> groups;
    groups.reserve(groupsByKey.size());
    for (const auto& [key, records] : groupsByKey) {
        if (records.size() > 1) {
            groups.push_back({key, records});
        }
    }
    std::sort(groups.begin(), groups.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.size() != rhs.second.size()) return lhs.second.size() > rhs.second.size();
        return lhs.first < rhs.first;
    });
    for (const auto& [key, records] : groups) {
        nlohmann::json recordArray = nlohmann::json::array();
        for (const AssetRecord* record : records) {
            if (record != nullptr) {
                recordArray.push_back(assetRecordSummaryJson(state, *record));
            }
        }
        duplicateGroups.push_back({
            {"reason", reason},
            {"confidence", confidence},
            {"key", key},
            {"recordCount", recordArray.size()},
            {"records", recordArray},
        });
    }
}

nlohmann::json buildAssetDuplicateReport(const EditorRuntimeState& state) {
    nlohmann::json duplicateGroups = nlohmann::json::array();
    if (state.assetRegistry == nullptr) {
        return {
            {"version", 1},
            {"kind", "AssetDuplicateReport"},
            {"assetCount", 0},
            {"duplicateGroupCount", 0},
            {"duplicateGroups", duplicateGroups},
        };
    }

    std::unordered_map<std::string, std::vector<const AssetRecord*>> byImportIdentity;
    std::unordered_map<std::string, std::vector<const AssetRecord*>> bySourceHash;
    std::unordered_map<std::string, std::vector<const AssetRecord*>> bySourcePath;
    std::unordered_map<std::string, std::vector<const AssetRecord*>> byImportedPath;
    std::unordered_map<std::string, std::vector<const AssetRecord*>> byCachePath;
    std::unordered_map<std::string, std::vector<const AssetRecord*>> byDisplayName;

    auto appendPathGroup = [&](std::unordered_map<std::string, std::vector<const AssetRecord*>>& groups, const std::string& value, const AssetRecord& record) {
        const std::filesystem::path path = resolveAssetRecordPath(state, value);
        if (path.empty()) {
            return;
        }
        groups[lowerString(canonicalForCompare(path).generic_string())].push_back(&record);
    };

    for (const AssetRecord& record : state.assetRegistry->records()) {
        const std::string typeName = assetTypeName(record.type);
        if (!record.sourceHash.empty()) {
            bySourceHash[typeName + ":" + record.sourceHash].push_back(&record);
        }
        if (!record.sourceHash.empty() && !record.importSettingsHash.empty()) {
            byImportIdentity[typeName + ":" + record.sourceHash + ":" + record.importSettingsHash].push_back(&record);
        }
        appendPathGroup(bySourcePath, record.sourcePath, record);
        appendPathGroup(byImportedPath, record.importedPath, record);
        appendPathGroup(byCachePath, record.cachePath, record);
        const std::string displayName = lowerString(trimString(record.displayName));
        if (!displayName.empty()) {
            byDisplayName[typeName + ":" + displayName].push_back(&record);
        }
    }

    appendDuplicateAssetGroups(state, byImportIdentity, "SameSourceAndImportSettingsHash", "high", duplicateGroups);
    appendDuplicateAssetGroups(state, bySourceHash, "SameSourceHash", "medium", duplicateGroups);
    appendDuplicateAssetGroups(state, bySourcePath, "SameResolvedSourcePath", "medium", duplicateGroups);
    appendDuplicateAssetGroups(state, byImportedPath, "SameResolvedImportedMetadataPath", "high", duplicateGroups);
    appendDuplicateAssetGroups(state, byCachePath, "SameResolvedCookedPayloadPath", "high", duplicateGroups);
    appendDuplicateAssetGroups(state, byDisplayName, "SameTypeAndDisplayName", "low", duplicateGroups);

    return {
        {"version", 1},
        {"kind", "AssetDuplicateReport"},
        {"registryPath", state.assetRegistry->state().path.empty() ? std::string{} : state.assetRegistry->state().path.generic_string()},
        {"assetCount", state.assetRegistry->records().size()},
        {"duplicateGroupCount", duplicateGroups.size()},
        {"duplicateGroups", duplicateGroups},
        {"checkedSignals", nlohmann::json::array({"sourceHash+importSettingsHash+type", "sourceHash+type", "resolved source path", "resolved imported metadata path", "resolved cooked payload path", "type+displayName"})},
        {"limitation", "This report is non-destructive duplicate visibility for the loaded registry. It does not merge assets, delete files, rewrite references, or inspect saved project/package references."},
    };
}

nlohmann::json buildAssetProjectReferenceScanReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    const std::unordered_set<std::string>& excludedFileKeys = {});

nlohmann::json buildAssetDeleteReadinessReport(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, const AssetGuid& targetGuid) {
    nlohmann::json referenceReport = buildAssetReferenceReport(state, targetGuid);
    std::unordered_set<std::string> excludedReferenceFiles;
    if (state.assetRegistry != nullptr) {
        if (const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid)) {
            const std::filesystem::path targetMetadataPath = resolveAssetRecordPath(state, target->importedPath);
            if (!targetMetadataPath.empty()) {
                excludedReferenceFiles.insert(canonicalForCompare(targetMetadataPath).string());
            }
        }
    }
    nlohmann::json projectReferenceScan = buildAssetProjectReferenceScanReport(state, browserRoot, targetGuid, excludedReferenceFiles);
    const size_t registryReferenceCount = referenceReport.value("registryReferenceCount", 0u);
    const size_t currentSceneReferenceCount = referenceReport.value("currentSceneReferenceCount", 0u);
    const size_t savedProjectReferenceCount = projectReferenceScan.value("referenceOccurrenceCount", 0u);
    const size_t savedProjectReferencingFileCount = projectReferenceScan.value("referencingFileCount", 0u);
    const size_t savedProjectParseErrorCount = projectReferenceScan.value("parseErrorCount", 0u);
    const bool blockedByCheckedData = registryReferenceCount > 0 || currentSceneReferenceCount > 0 || savedProjectReferenceCount > 0 || savedProjectParseErrorCount > 0;
    nlohmann::json blockers = nlohmann::json::array();

    if (registryReferenceCount > 0) {
        blockers.push_back({
            {"scope", "LoadedAssetRegistry"},
            {"severity", "warning"},
            {"count", registryReferenceCount},
            {"detail", "Loaded registry records still depend on or reference this asset GUID."},
        });
    }
    if (currentSceneReferenceCount > 0) {
        blockers.push_back({
            {"scope", "CurrentScene"},
            {"severity", "warning"},
            {"count", currentSceneReferenceCount},
            {"detail", "The current scene still contains component or prefab references to this asset GUID."},
        });
    }
    if (savedProjectReferenceCount > 0) {
        blockers.push_back({
            {"scope", "SavedProjectMetadata"},
            {"severity", "warning"},
            {"count", savedProjectReferenceCount},
            {"fileCount", savedProjectReferencingFileCount},
            {"detail", "Saved project metadata files contain this asset GUID."},
        });
    }
    if (savedProjectParseErrorCount > 0) {
        blockers.push_back({
            {"scope", "SavedProjectMetadata"},
            {"severity", "warning"},
            {"count", savedProjectParseErrorCount},
            {"detail", "One or more saved project metadata files could not be parsed, so saved-file readiness is not fully verified."},
        });
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetDeleteReadinessReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", referenceReport.value("selectedAsset", nlohmann::json::object())},
        {"deleteReadyForLoadedData", registryReferenceCount == 0 && currentSceneReferenceCount == 0},
        {"deleteReadyForSavedProjectFiles", savedProjectReferenceCount == 0 && savedProjectParseErrorCount == 0},
        {"deleteReadyForCheckedScopes", !blockedByCheckedData},
        {"registryReferenceCount", registryReferenceCount},
        {"currentSceneReferenceCount", currentSceneReferenceCount},
        {"savedProjectReferenceCount", savedProjectReferenceCount},
        {"savedProjectReferencingFileCount", savedProjectReferencingFileCount},
        {"savedProjectReferenceParseErrorCount", savedProjectParseErrorCount},
        {"blockers", blockers},
        {"registryReferences", referenceReport.value("registryReferences", nlohmann::json::array())},
        {"currentSceneReferences", referenceReport.value("currentSceneReferences", nlohmann::json::array())},
        {"savedProjectReferenceScan", projectReferenceScan},
        {"checkedScopes", nlohmann::json::array({"LoadedAssetRegistry", "CurrentScene", "SavedProjectMetadata"})},
        {"uncheckedScopes", nlohmann::json::array({"GeneratedCachePayloadInternals", "ExternalProjectFiles", "OpaquePackages"})},
        {"recommendation", blockedByCheckedData
            ? "Replace or remove loaded-registry, current-scene, and saved-project metadata references, and resolve saved metadata parse errors, before deleting this asset."
            : "No references were found in the loaded registry, current scene, or saved project metadata scan. Destructive deletion still remains disabled until cross-file rewrite and package/cache validation workflows are implemented."},
        {"destructiveActionEnabled", false},
        {"limitation", "This report checks loaded data plus saved project JSON metadata. It does not rewrite references or inspect generated cache payload internals, external project files, or opaque packages."},
    };
}

nlohmann::json buildAssetProjectReferenceScanReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    const std::unordered_set<std::string>& excludedFileKeys) {
    nlohmann::json selectedAsset = nlohmann::json::object();
    if (state.assetRegistry != nullptr) {
        if (const AssetRecord* target = findAssetRecordByGuid(*state.assetRegistry, targetGuid)) {
            selectedAsset = assetRecordSummaryJson(state, *target);
        }
    }

    nlohmann::json checkedRoots = nlohmann::json::array();
    std::vector<std::filesystem::path> files;
    collectProjectReferenceScanFiles(state, browserRoot, checkedRoots, files);

    nlohmann::json scannedFiles = nlohmann::json::array();
    nlohmann::json filesWithReferences = nlohmann::json::array();
    nlohmann::json excludedFilesWithReferences = nlohmann::json::array();
    nlohmann::json parseErrors = nlohmann::json::array();
    size_t occurrenceCount = 0;
    size_t excludedOccurrenceCount = 0;
    for (const std::filesystem::path& path : files) {
        scannedFiles.push_back(path.generic_string());
        std::optional<nlohmann::json> json = readJsonFile(path);
        if (!json.has_value()) {
            parseErrors.push_back({
                {"path", path.generic_string()},
                {"detail", "File matched the project reference scan set but could not be parsed as JSON."},
            });
            continue;
        }
        nlohmann::json occurrences = nlohmann::json::array();
        appendGuidOccurrences(*json, targetGuid, "$", occurrences);
        if (!occurrences.empty()) {
            const std::string fileKey = canonicalForCompare(path).string();
            if (excludedFileKeys.find(fileKey) != excludedFileKeys.end()) {
                excludedOccurrenceCount += occurrences.size();
                excludedFilesWithReferences.push_back({
                    {"path", path.generic_string()},
                    {"referenceCount", occurrences.size()},
                    {"occurrences", occurrences},
                    {"reason", "SelectedAssetMetadata"},
                });
                continue;
            }
            occurrenceCount += occurrences.size();
            filesWithReferences.push_back({
                {"path", path.generic_string()},
                {"referenceCount", occurrences.size()},
                {"occurrences", occurrences},
            });
        }
    }

    return {
        {"version", 1},
        {"kind", "SelectedAssetProjectReferenceScanReport"},
        {"targetGuid", targetGuid},
        {"selectedAsset", selectedAsset},
        {"checkedRoots", checkedRoots},
        {"scannedFileCount", scannedFiles.size()},
        {"referencingFileCount", filesWithReferences.size()},
        {"referenceOccurrenceCount", occurrenceCount},
        {"excludedReferenceOccurrenceCount", excludedOccurrenceCount},
        {"excludedFilesWithReferences", excludedFilesWithReferences},
        {"scannedFiles", scannedFiles},
        {"filesWithReferences", filesWithReferences},
        {"parseErrorCount", parseErrors.size()},
        {"parseErrors", parseErrors},
        {"checkedFileTypes", nlohmann::json::array({".rtlevel", ".mscene", ".vproject", ".rtprefab.json", ".rtmesh.json", ".rtmaterial.json", ".rttexture.json", ".rthdri.json"})},
        {"limitation", "This is a saved-file JSON scan for project content and scene roots. It does not rewrite references or inspect generated cache payload internals."},
    };
}

nlohmann::json buildAssetBrokenPlaceholderReport(const EditorRuntimeState& state, const AssetRecord& record) {
    const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
    const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
    const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
    const bool sourceMissing = !record.sourcePath.empty() && !regularFileExists(sourcePath);
    const bool importedMissing = !record.importedPath.empty() && !regularFileExists(importedPath);
    const bool payloadMissing = !record.cachePath.empty() && !regularFileExists(cachePath);
    const bool broken = record.missing || record.status == AssetImportStatus::Missing || record.sourceMissing || record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing || sourceMissing || importedMissing || payloadMissing;

    nlohmann::json missingReasons = nlohmann::json::array();
    auto appendReason = [&](const char* kind, const char* severity, const char* detail, const std::string& storedPath, const std::filesystem::path& resolvedPath) {
        missingReasons.push_back({
            {"kind", kind},
            {"severity", severity},
            {"detail", detail},
            {"path", storedPath},
            {"resolvedPath", resolvedPath.empty() ? std::string{} : resolvedPath.generic_string()},
        });
    };
    if (record.sourceMissing || sourceMissing) {
        appendReason("MissingSource", "warning", "Raw source is unavailable. Existing imported metadata and cooked payload may still be usable.", record.sourcePath, sourcePath);
    }
    if (record.importedMetadataMissing || importedMissing) {
        appendReason("MissingImportedMetadata", "error", "Imported transparent asset metadata is missing.", record.importedPath, importedPath);
    }
    if (record.cookedPayloadMissing || payloadMissing) {
        appendReason("MissingCookedPayload", "error", "Cooked/runtime payload is missing.", record.cachePath, cachePath);
    }
    if (record.dependenciesMissing) {
        appendReason("MissingDependencyRecord", "error", "One or more dependency GUID records are missing from the loaded asset registry.", {}, {});
    }
    if (record.missing || record.status == AssetImportStatus::Missing) {
        appendReason("MissingRegistryAsset", "error", "Registry status marks this asset as missing or broken.", {}, {});
    }

    nlohmann::json availableActions = nlohmann::json::array();
    availableActions.push_back({
        {"action", "RevealMetadata"},
        {"available", !importedPath.empty() && regularFileExists(importedPath)},
        {"detail", "Reveal imported metadata when it exists."},
    });
    availableActions.push_back({
        {"action", "RevealPayload"},
        {"available", !cachePath.empty() && regularFileExists(cachePath)},
        {"detail", "Reveal cooked/runtime payload when it exists."},
    });
    availableActions.push_back({
        {"action", "RelinkSource"},
        {"available", true},
        {"detail", "Choose a replacement raw source path for this registry record."},
    });
    availableActions.push_back({
        {"action", "RebuildPayload"},
        {"available", !record.sourcePath.empty() && regularFileExists(sourcePath)},
        {"detail", "Queue reimport to regenerate missing metadata or cooked/runtime payloads when source is available."},
    });

    return {
        {"version", 1},
        {"kind", "SelectedAssetBrokenPlaceholderReport"},
        {"targetGuid", record.guid},
        {"selectedAsset", assetRecordSummaryJson(state, record)},
        {"placeholderRequired", broken},
        {"placeholderKind", broken ? "BrokenAsset" : "None"},
        {"missingReasons", missingReasons},
        {"availableActions", availableActions},
        {"placementPolicy", broken
            ? "Placement should show a broken asset placeholder or block placement until metadata/payload references are repaired."
            : "No broken placeholder is required for the currently loaded registry health state."},
        {"limitation", "This report describes loaded registry health and filesystem availability for the selected record. It does not create a runtime placeholder mesh or repair files automatically."},
    };
}

nlohmann::json pathInspectionJson(const std::filesystem::path& path, const char* role) {
    nlohmann::json info = {
        {"role", role},
        {"path", path.empty() ? std::string{} : path.generic_string()},
        {"exists", false},
        {"isRegularFile", false},
        {"isDirectory", false},
        {"sizeBytes", 0},
        {"writeStamp", 0},
    };
    if (path.empty()) {
        return info;
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    info["exists"] = exists && !ec;
    if (!exists || ec) {
        if (ec) {
            info["error"] = ec.message();
        }
        return info;
    }
    const bool regular = std::filesystem::is_regular_file(path, ec);
    info["isRegularFile"] = regular && !ec;
    const bool directory = std::filesystem::is_directory(path, ec);
    info["isDirectory"] = directory && !ec;
    if (regular) {
        info["sizeBytes"] = static_cast<uint64_t>(pathSizeForCache(path));
        info["writeStamp"] = pathWriteStamp(path);
    }
    return info;
}

nlohmann::json metadataFileInspectionJson(const std::filesystem::path& path, const char* role) {
    nlohmann::json info = pathInspectionJson(path, role);
    if (!info.value("isRegularFile", false)) {
        return info;
    }
    std::optional<nlohmann::json> json = readJsonFile(path);
    if (!json.has_value()) {
        info["parseStatus"] = "unreadable_or_invalid_json";
        return info;
    }
    info["parseStatus"] = "ok";
    info["topLevelType"] = json->is_object() ? "object" : json->is_array() ? "array" : json->is_string() ? "string" : json->is_number() ? "number" : json->is_boolean() ? "boolean" : "null";
    if (json->is_object()) {
        nlohmann::json keys = nlohmann::json::array();
        for (auto it = json->begin(); it != json->end(); ++it) {
            keys.push_back(it.key());
        }
        info["topLevelKeys"] = keys;
        if (json->contains("schema")) info["schema"] = (*json)["schema"];
        if (json->contains("kind")) info["kind"] = (*json)["kind"];
        if (json->contains("version")) info["version"] = (*json)["version"];
        if (json->contains("runtimePayload")) info["runtimePayload"] = (*json)["runtimePayload"];
        if (json->contains("payloads")) info["payloadCount"] = jsonArraySize(*json, "payloads");
        if (json->contains("dependencies")) info["dependencyCount"] = jsonArraySize(*json, "dependencies");
        if (json->contains("references")) info["referenceCount"] = jsonArraySize(*json, "references");
        if (json->contains("assets")) info["assetCount"] = jsonArraySize(*json, "assets");
    }
    return info;
}

std::string nativeRuntimeExtensionForAssetType(AssetType type) {
    switch (type) {
    case AssetType::Mesh: return ".rtmesh";
    case AssetType::Material: return ".rtmaterial";
    case AssetType::Texture:
    case AssetType::HDRI: return ".rttexture";
    case AssetType::Prefab: return ".rtprefab";
    case AssetType::Scene: return ".rtlevel";
    case AssetType::Animation: return ".rtanim";
    case AssetType::Skeleton: return ".rtskeleton";
    case AssetType::Unknown: break;
    }
    return {};
}

nlohmann::json nativeArtifactCandidateJson(
    const AssetRecord& record,
    const std::filesystem::path& importedPath,
    const std::filesystem::path& cachePath) {
    const std::string nativeExtension = nativeRuntimeExtensionForAssetType(record.type);
    nlohmann::json candidates = nlohmann::json::array();
    auto appendCandidate = [&](std::filesystem::path path, const char* basis) {
        if (path.empty() || nativeExtension.empty()) {
            return;
        }
        path.replace_extension(nativeExtension);
        candidates.push_back(pathInspectionJson(path, basis));
    };
    appendCandidate(importedPath, "nativeStandaloneFromImportedMetadata");
    appendCandidate(cachePath, "nativeStandaloneFromCookedPayload");

    return {
        {"assetType", assetTypeName(record.type)},
        {"expectedStandaloneExtension", nativeExtension},
        {"nativeStandaloneRuntimeFormats", nlohmann::json::array({".rtmesh", ".rtmaterial", ".rttexture"})},
        {"candidateCount", candidates.size()},
        {"candidates", candidates},
        {"status", nativeExtension.empty() ? "unsupported-asset-type" : "not-emitted-by-current-transparent-pipeline"},
        {"policy", "Current imports use transparent JSON metadata plus cooked/cache payload paths. Native standalone runtime files are expected future artifacts and are reported here for debug readiness only."},
    };
}

nlohmann::json opaquePackageCandidateJson(const std::filesystem::path& importedPath, const std::filesystem::path& cachePath) {
    nlohmann::json candidates = nlohmann::json::array();
    auto appendCandidate = [&](std::filesystem::path path, const char* basis) {
        if (path.empty()) {
            return;
        }
        path.replace_extension(".rtpkg");
        candidates.push_back(pathInspectionJson(path, basis));
    };
    appendCandidate(importedPath, "packageFromImportedMetadata");
    appendCandidate(cachePath, "packageFromCookedPayload");

    return {
        {"expectedExtension", ".rtpkg"},
        {"candidateCount", candidates.size()},
        {"candidates", candidates},
        {"status", "not-emitted-by-current-transparent-pipeline"},
        {"policy", "Opaque package emission/loading, migration/versioning, and binary package debug inspection remain future production package-system work."},
    };
}

nlohmann::json packageVersioningReadinessJson(const AssetRecord& record) {
    return {
        {"schema", "RtpkgVersioningReadinessV1"},
        {"assetGuid", record.guid},
        {"assetType", assetTypeName(record.type)},
        {"expectedPackageExtension", ".rtpkg"},
        {"currentTransparentMetadataVersion", 1},
        {"plannedPackageHeader", {
            {"magic", "RTPKG"},
            {"endianness", "little"},
            {"headerVersion", 1},
            {"packageGuid", record.guid},
            {"assetType", assetTypeName(record.type)},
            {"contentVersion", 1},
            {"dependencyTableOffset", "planned"},
            {"objectTableOffset", "planned"},
            {"chunkTableOffset", "planned"},
            {"debugDirectoryOffset", "planned"},
        }},
        {"plannedObjectTables", nlohmann::json::array({
            "AssetSummary",
            "DependencyTable",
            "PayloadChunkTable",
            "ImportSettingsSnapshot",
            "SourceControlPolicy",
            "DebugDirectory"
        })},
        {"migrationPolicy", {
            {"migrationTableImplemented", false},
            {"minimumReadableVersion", 1},
            {"currentWriterVersion", 1},
            {"migrationStrategy", "Future package loader should migrate package header/object/chunk table versions before binding runtime payloads."},
            {"backupPolicy", "Future package migration should write side-by-side backups before in-place package rewrites."},
        }},
        {"debugInspectionPolicy", {
            {"binaryInspectorImplemented", false},
            {"plannedInspectionFields", nlohmann::json::array({"header", "assetSummary", "dependencies", "objectTable", "chunkTable", "payloadHashes", "sourceControlPolicy", "migrationHistory"})},
            {"currentInspectionFallback", "Use transparent JSON metadata, package candidate paths, and filesystem payload facts from this report."},
        }},
        {"implementationStatus", {
            {"packageEmissionImplemented", false},
            {"packageLoadingImplemented", false},
            {"packageMigrationImplemented", false},
            {"binaryDebugInspectionImplemented", false},
        }},
    };
}

nlohmann::json textureCookedBindingPolicyJson(
    const AssetRecord& record,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& importedPath,
    const std::filesystem::path& cachePath) {
    const bool textureLike = record.type == AssetType::Texture || record.type == AssetType::HDRI;
    const std::string sourceExtension = lowerString(sourcePath.extension().string());
    std::string sourceContainer = "not-applicable";
    std::string colorSpace = "not-applicable";
    std::string runtimeBindingStatus = textureLike ? "loose-cache-payload-bound-by-registry-guid" : "not-a-texture-asset";
    if (textureLike) {
        if (record.type == AssetType::HDRI || sourceExtension == ".hdr" || sourceExtension == ".exr") {
            sourceContainer = "hdr-environment";
            colorSpace = "linear-hdr";
        } else if (sourceExtension == ".dds" || sourceExtension == ".ktx" || sourceExtension == ".ktx2") {
            sourceContainer = "compressed-container";
            colorSpace = "source-defined";
        } else if (sourceExtension == ".basis") {
            sourceContainer = "basis-standalone-unsupported";
            colorSpace = "source-defined";
            runtimeBindingStatus = "unsupported-source-container";
        } else {
            sourceContainer = "decoded-ldr-image";
            colorSpace = "role-dependent-srgb-or-linear";
        }
    }

    std::filesystem::path nativeTextureCandidate = importedPath;
    if (nativeTextureCandidate.empty()) {
        nativeTextureCandidate = cachePath;
    }
    if (!nativeTextureCandidate.empty()) {
        nativeTextureCandidate.replace_extension(".rttexture");
    }

    return {
        {"appliesToTextureLikeAsset", textureLike},
        {"assetGuid", record.guid},
        {"assetType", assetTypeName(record.type)},
        {"sourceExtension", sourceExtension},
        {"sourceContainer", sourceContainer},
        {"intendedColorSpace", colorSpace},
        {"importSettings", {
            {"textureImportMode", record.importSettings.textureImportMode},
            {"textureCompression", record.importSettings.textureCompression},
        }},
        {"currentRuntimeBinding", {
            {"status", runtimeBindingStatus},
            {"bindingKey", "AssetRecord.guid"},
            {"metadataPath", importedPath.empty() ? std::string{} : importedPath.generic_string()},
            {"looseCookedPayloadPath", cachePath.empty() ? std::string{} : cachePath.generic_string()},
            {"nativeTextureCandidatePath", nativeTextureCandidate.empty() ? std::string{} : nativeTextureCandidate.generic_string()},
            {"payloadExists", !cachePath.empty() && regularFileExists(cachePath)},
        }},
        {"compressionTranscodingPolicy", {
            {"requestedPolicy", record.importSettings.textureCompression.empty() ? std::string("PreserveSource") : record.importSettings.textureCompression},
            {"effectiveCookPolicy", textureLike ? "preserve-source-or-decoded-loose-payload" : "not-applicable"},
            {"platformTranscodeImplemented", false},
            {"nativeCookedTextureEmissionImplemented", false},
            {"basisStandaloneImportImplemented", false},
            {"ktx2RuntimeTranscodeNote", "Runtime KTX2/BasisU texture loading may transcode for renderer upload, but the editor asset cook does not yet emit platform-specific native .rttexture payloads."},
            {"supportedSourceContainers", nlohmann::json::array({"png", "jpg", "jpeg", "tga", "bmp", "hdr", "exr", "dds", "ktx", "ktx2"})},
            {"unsupportedSourceContainers", nlohmann::json::array({"basis"})},
        }},
        {"remainingWork", nlohmann::json::array({
            "Native .rttexture emission/loading",
            "Texture/HDR GUID binding through native cooked payloads",
            "Platform compression/transcoding policy",
            "Basis standalone source import",
            "Material/environment rebinding to native texture payloads"
        })},
    };
}

nlohmann::json buildAssetPackageInspectionReport(const EditorRuntimeState& state, const AssetRecord& record) {
    const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
    const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
    const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
    const std::filesystem::path thumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);

    nlohmann::json files = nlohmann::json::array();
    files.push_back(pathInspectionJson(sourcePath, "source"));
    files.push_back(metadataFileInspectionJson(importedPath, "importedMetadata"));
    files.push_back(pathInspectionJson(cachePath, "cookedOrRuntimePayload"));
    files.push_back(pathInspectionJson(thumbnailPath, "thumbnail"));

    nlohmann::json payloadPolicy = {
        {"sourcePath", record.sourcePath},
        {"importedMetadataPath", record.importedPath},
        {"cachePath", record.cachePath},
        {"thumbnailPath", record.thumbnailPath},
        {"sourceHash", record.sourceHash},
        {"importedHash", record.importedHash},
        {"importSettingsHash", record.importSettingsHash},
        {"importGroupId", record.importGroupId},
        {"importGroupName", record.importGroupName},
        {"importRootGuid", record.importRootGuid},
    };

    return {
        {"version", 1},
        {"kind", "SelectedAssetPackageInspectionReport"},
        {"targetGuid", record.guid},
        {"asset", assetRecordSummaryJson(state, record)},
        {"payloadPolicy", payloadPolicy},
        {"nativeRuntimeArtifactReadiness", nativeArtifactCandidateJson(record, importedPath, cachePath)},
        {"opaquePackageReadiness", opaquePackageCandidateJson(importedPath, cachePath)},
        {"packageVersioningReadiness", packageVersioningReadinessJson(record)},
        {"textureCookedBindingPolicy", textureCookedBindingPolicyJson(record, sourcePath, importedPath, cachePath)},
        {"files", files},
        {"registryHealth", {
            {"missing", record.missing},
            {"stale", record.stale},
            {"sourceMissing", record.sourceMissing},
            {"importedMetadataMissing", record.importedMetadataMissing},
            {"cookedPayloadMissing", record.cookedPayloadMissing},
            {"dependenciesMissing", record.dependenciesMissing},
        }},
        {"checkedScopes", nlohmann::json::array({"LoadedAssetRegistry", "ResolvedSourcePath", "ImportedMetadataJson", "CookedRuntimePayloadPath", "ThumbnailPath", "NativeStandaloneArtifactCandidates", "OpaquePackageCandidates", "PackageVersioningReadiness", "TextureCookedBindingPolicy"})},
        {"uncheckedScopes", nlohmann::json::array({"OpaqueRtpkgObjects", "GeneratedCachePayloadInternals", "ExternalProjectFiles", "BinaryPayloadDeepInspection", "PackageMigrationTables", "PlatformTextureTranscodeOutputs"})},
        {"limitation", "This report inspects selected transparent asset metadata and filesystem payload facts plus expected native/package candidate paths, package versioning/debug-readiness policy, and texture/HDR cooked-binding policy. It does not emit native runtime files, transcode platform texture payloads, parse binary cache payload internals, rewrite references, repair missing files, load .rtpkg packages, migrate packages, or inspect package internals."},
    };
}

nlohmann::json buildAssetThumbnailReadinessReport(const EditorRuntimeState& state, const AssetRecord& record) {
    const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
    const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
    const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
    const std::filesystem::path thumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);
    const bool thumbnailMetadataPresent = !record.thumbnailPath.empty();
    const bool thumbnailExists = !thumbnailPath.empty() && regularFileExists(thumbnailPath);
    const bool rasterSourcePreview = !sourcePath.empty() && regularFileExists(sourcePath) && isRasterThumbnailPath(sourcePath);
    const bool metadataPreviewCandidate = !sourcePath.empty() && regularFileExists(sourcePath) && isGeneratedPreviewDiskCacheCandidate(sourcePath);
    std::string currentPreviewMode = "fallback-icon";
    if (thumbnailExists) {
        currentPreviewMode = "metadata-thumbnail-path";
    } else if (rasterSourcePreview) {
        currentPreviewMode = "raster-source-preview";
    } else if (metadataPreviewCandidate) {
        currentPreviewMode = "generated-source-preview";
    }

    return {
        {"schema", "SelectedAssetThumbnailReadinessV1"},
        {"asset", assetRecordSummaryJson(state, record)},
        {"thumbnail", {
            {"storedThumbnailPath", record.thumbnailPath},
            {"resolvedThumbnailPath", thumbnailPath.empty() ? std::string{} : thumbnailPath.generic_string()},
            {"metadataPresent", thumbnailMetadataPresent},
            {"fileExists", thumbnailExists},
            {"currentPreviewMode", currentPreviewMode},
            {"fallbackPreviewAvailable", true},
            {"missingThumbnailIsBlocking", false},
        }},
        {"previewInputs", {
            {"source", pathInspectionJson(sourcePath, "source")},
            {"importedMetadata", metadataFileInspectionJson(importedPath, "importedMetadata")},
            {"cookedRuntimePayload", pathInspectionJson(cachePath, "cookedOrRuntimePayload")},
            {"thumbnail", pathInspectionJson(thumbnailPath, "thumbnail")},
            {"rasterSourcePreviewSupported", rasterSourcePreview},
            {"generatedSourcePreviewSupported", metadataPreviewCandidate},
        }},
        {"asyncRenderedThumbnailPlan", {
            {"schema", "AsyncRenderedThumbnailPlanV1"},
            {"plannedJobTypes", nlohmann::json::array({"TextureThumbnail", "MaterialThumbnail", "PrefabModelThumbnail", "EnvironmentThumbnail", "LevelThumbnail"})},
            {"plannedStages", nlohmann::json::array({"collect-preview-inputs", "load-runtime-preview-assets", "render-offscreen-preview", "write-thumbnail-asset", "update-registry-thumbnail-path", "refresh-content-browser-preview-cache"})},
            {"jobCenterProgressImplemented", false},
            {"cancellationImplemented", false},
            {"asyncRenderedThumbnailGenerationImplemented", false},
            {"currentFallback", "Content Browser uses existing thumbnailPath metadata, raster/source previews, generated source previews, or a type-correct fallback icon."},
        }},
        {"currentProgress", {
            {"jobQueued", false},
            {"jobRunning", false},
            {"stage", "not-started"},
            {"progress", 0.0},
            {"cancelAvailable", false},
        }},
        {"policy", {
            {"description", "This report exposes selected-asset thumbnail readiness and current fallback behavior without claiming that async rendered thumbnail jobs are implemented."},
            {"safeToUseFallbackPreview", true},
            {"mutationExecuted", false},
            {"performedActions", nlohmann::json::array()},
            {"unsupportedActions", nlohmann::json::array({"async-rendered-thumbnail-generation", "thumbnail-job-cancellation", "job-center-thumbnail-progress", "material-prefab-environment-level-offscreen-thumbnail-rendering"})},
        }},
    };
}

nlohmann::json buildAssetImporterReadinessReport(const EditorRuntimeState& state, const AssetRecord& record) {
    const std::filesystem::path sourcePath = resolveAssetRecordPath(state, record.sourcePath);
    const std::filesystem::path importedPath = resolveAssetRecordPath(state, record.importedPath);
    const std::filesystem::path cachePath = resolveAssetRecordPath(state, record.cachePath);
    const std::string fallbackExtension = std::filesystem::path(record.sourcePath).extension().string();
    const std::string sourceExtension = lowerString(sourcePath.extension().string().empty() ? fallbackExtension : sourcePath.extension().string());

    std::string sourceClass = "unknown-or-generated";
    bool currentImportSupported = false;
    bool currentPlaceAfterImportSupported = false;
    if (sourceExtension == ".gltf" || sourceExtension == ".glb") {
        sourceClass = "gltf-model";
        currentImportSupported = true;
        currentPlaceAfterImportSupported = true;
    } else if (sourceExtension == ".obj") {
        sourceClass = "obj-model-partial";
        currentImportSupported = true;
    } else if (sourceExtension == ".mtl") {
        sourceClass = "mtl-material-source-partial";
        currentImportSupported = true;
    } else if (sourceExtension == ".fbx") {
        sourceClass = "fbx-model-unsupported";
    } else if (sourceExtension == ".usd" || sourceExtension == ".usda" || sourceExtension == ".usdc" || sourceExtension == ".usdz") {
        sourceClass = "usd-scene-unsupported";
    } else if (sourceExtension == ".basis") {
        sourceClass = "basis-texture-unsupported";
    } else if (sourceExtension == ".dds" || sourceExtension == ".ktx" || sourceExtension == ".ktx2") {
        sourceClass = "compressed-texture-container-policy-only";
        currentImportSupported = true;
    } else if (sourceExtension == ".png" || sourceExtension == ".jpg" || sourceExtension == ".jpeg" || sourceExtension == ".tga" || sourceExtension == ".bmp" || sourceExtension == ".hdr" || sourceExtension == ".exr") {
        sourceClass = "texture-or-environment";
        currentImportSupported = true;
    }

    auto productionFormatRow = [](const char* format, const char* sourceType, bool importSupported, bool runtimeCookingComplete, bool materialBindingComplete, bool viewportPlacementComplete, const char* note) {
        return nlohmann::json{
            {"format", format},
            {"sourceType", sourceType},
            {"importSupportedNow", importSupported},
            {"runtimeCookingComplete", runtimeCookingComplete},
            {"materialCreationComplete", materialBindingComplete},
            {"textureBindingComplete", materialBindingComplete},
            {"viewportPlacementComplete", viewportPlacementComplete},
            {"note", note},
        };
    };

    nlohmann::json productionMatrix = nlohmann::json::array({
        productionFormatRow("glTF/GLB", "model-scene", true, true, true, true, "Current production baseline for source import, prefab metadata, cooked payload reuse, and import-and-place."),
        productionFormatRow("OBJ/MTL", "model-material", true, false, false, false, "OBJ/MTL source discovery and preview/import routing exist, but full runtime cooking, material creation, texture binding, and viewport placement are not production-complete."),
        productionFormatRow("FBX", "model-animation", false, false, false, false, "Full FBX importer remains future work."),
        productionFormatRow("USD/USDZ", "scene-package", false, false, false, false, "USD/USDZ importer remains future work."),
        productionFormatRow("Basis", "compressed-texture", false, false, false, false, "Standalone Basis source import and platform transcode policy remain future work."),
        productionFormatRow("DDS/KTX/KTX2", "compressed-texture", true, false, false, false, "Container recognition exists through texture paths/policy reports, but native platform cook outputs are not emitted."),
    });

    return {
        {"schema", "SelectedAssetImporterReadinessV1"},
        {"asset", assetRecordSummaryJson(state, record)},
        {"sourceFormat", {
            {"extension", sourceExtension},
            {"class", sourceClass},
            {"currentImportSupported", currentImportSupported},
            {"currentPlaceAfterImportSupported", currentPlaceAfterImportSupported},
            {"sourceFile", pathInspectionJson(sourcePath, "source")},
        }},
        {"currentTransparentPipeline", {
            {"importedMetadata", metadataFileInspectionJson(importedPath, "importedMetadata")},
            {"cookedRuntimePayload", pathInspectionJson(cachePath, "cookedOrRuntimePayload")},
            {"registryGuidBinding", !record.guid.empty()},
            {"hasImportedMetadataPath", !record.importedPath.empty()},
            {"hasCookedPayloadPath", !record.cachePath.empty()},
            {"placementBlocked", assetPlacementBlocked(record)},
            {"placementBlockReason", assetPlacementBlockReason(record)},
        }},
        {"productionImportMatrix", productionMatrix},
        {"objMtlProductionReadiness", {
            {"schema", "ObjMtlRuntimeCookingReadinessV1"},
            {"sourceDiscoveryImplemented", currentImportSupported && (sourceExtension == ".obj" || sourceExtension == ".mtl")},
            {"runtimeMeshCookingImplemented", false},
            {"mtlMaterialCreationImplemented", false},
            {"mtlTextureBindingImplemented", false},
            {"viewportPlacementImplemented", false},
            {"diagnosticsImplemented", false},
            {"remainingStages", nlohmann::json::array({"parse-obj-geometry", "parse-mtl-materials", "resolve-texture-map-paths", "cook-runtime-mesh-payload", "create-material-assets", "bind-texture-assets-by-guid", "place-prefab-or-mesh-in-viewport", "write-import-diagnostics"})},
        }},
        {"compressedTexturePolicy", {
            {"basisStandaloneImportImplemented", false},
            {"rawCompressedTexturePolicyImplemented", false},
            {"platformTranscodeImplemented", false},
            {"nativeRtTextureEmissionImplemented", false},
            {"policyReportAvailableInPackageInspection", true},
            {"supportedRecognitionOnly", nlohmann::json::array({"dds", "ktx", "ktx2"})},
            {"unsupportedStandaloneSources", nlohmann::json::array({"basis"})},
        }},
        {"unsupportedProductionImporters", nlohmann::json::array({"full-fbx-importer", "usd-usdz-importer", "basis-standalone-import", "full-obj-mtl-runtime-cooking", "obj-mtl-material-creation", "obj-mtl-texture-guid-binding", "obj-mtl-viewport-placement", "non-gltf-production-import-parity"})},
        {"policy", {
            {"description", "This report exposes selected-asset importer coverage and production import gaps without mutating the registry or claiming unsupported importers are implemented."},
            {"mutationExecuted", false},
            {"performedActions", nlohmann::json::array()},
            {"supportedNow", nlohmann::json::array({"gltf-glb-import-and-place", "transparent-registry-metadata-inspection", "selected-asset-importer-readiness-report"})},
            {"unsupportedActions", nlohmann::json::array({"fbx-import", "usd-usdz-import", "basis-import", "full-obj-mtl-runtime-cooking", "obj-mtl-material-texture-binding", "obj-mtl-viewport-placement"})},
        }},
    };
}

bool writeAssetRelationshipReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    bool referencesReport,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = selectedAssetRelationshipReportPath(state, browserRoot, targetGuid, referencesReport ? "references" : "dependencies");
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset relationship report: " + outPath.string();
        return false;
    }
    file << (referencesReport ? buildAssetReferenceReport(state, targetGuid) : buildAssetDependencyReport(state, targetGuid)).dump(2);
    return true;
}

bool writeAssetDependencyGraphReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = assetDependencyGraphReportPath(state, browserRoot);
    const std::filesystem::path dotPath = assetDependencyGraphDotPath(state, browserRoot);
    const std::filesystem::path htmlPath = assetDependencyGraphHtmlPath(state, browserRoot);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create dependency graph report folder: " + ec.message();
        return false;
    }
    nlohmann::json graph = buildAssetDependencyGraphReport(state);
    graph["dotPath"] = dotPath.generic_string();
    graph["htmlPath"] = htmlPath.generic_string();
    graph["visualization"] = {
        {"formats", nlohmann::json::array({"json", "graphviz-dot", "interactive-html"})},
        {"dotPath", dotPath.generic_string()},
        {"htmlPath", htmlPath.generic_string()},
        {"description", "JSON, Graphviz DOT, and self-contained interactive HTML sidecars for loaded-registry dependency/reference graph review."},
        {"interactiveHtmlFeatures", nlohmann::json::array({"search", "edge-filter", "clickable-node-details", "clickable-edge-details", "missing-target-highlighting", "current-scene-usage-nodes"})},
        {"limitation", "This is an inspectable sidecar visualization, not a native in-editor graph panel, continuously refreshed project reference graph, or package/cache-internal graph."},
    };
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write dependency graph report: " + outPath.string();
        return false;
    }
    file << graph.dump(2);
    std::ofstream dotFile(dotPath);
    if (!dotFile.is_open()) {
        outError = "Could not write dependency graph DOT report: " + dotPath.string();
        return false;
    }
    dotFile << buildAssetDependencyGraphDot(graph);
    std::ofstream htmlFile(htmlPath);
    if (!htmlFile.is_open()) {
        outError = "Could not write dependency graph HTML report: " + htmlPath.string();
        return false;
    }
    htmlFile << buildAssetDependencyGraphHtml(graph);
    return true;
}

bool writeAssetProjectReferenceIndexReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = assetProjectReferenceIndexReportPath(state, browserRoot);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create project reference index folder: " + ec.message();
        return false;
    }
    nlohmann::json index = buildAssetProjectReferenceIndexReport(state, browserRoot);
    const std::filesystem::path persistentPath = assetProjectReferenceIndexPersistentPath(state, browserRoot);
    index["persistentIndexPath"] = persistentPath.generic_string();
    index["reportPath"] = outPath.generic_string();

    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write project reference index: " + outPath.string();
        return false;
    }
    file << index.dump(2);

    std::filesystem::create_directories(persistentPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create persistent project reference index folder: " + ec.message();
        return false;
    }
    std::ofstream persistentFile(persistentPath);
    if (!persistentFile.is_open()) {
        outError = "Could not write persistent project reference index: " + persistentPath.string();
        return false;
    }
    persistentFile << index.dump(2);
    return true;
}

bool writeAssetDuplicateReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = assetDuplicateReportPath(state, browserRoot);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create duplicate report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write duplicate report: " + outPath.string();
        return false;
    }
    file << buildAssetDuplicateReport(state).dump(2);
    return true;
}

bool writeAssetDeleteReadinessReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    std::filesystem::path& outPath,
    std::string& outError) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = selectedAssetDeleteReadinessReportPath(state, browserRoot, targetGuid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset delete-readiness report: " + outPath.string();
        return false;
    }
    file << buildAssetDeleteReadinessReport(state, browserRoot, targetGuid).dump(2);
    return true;
}

bool writeAssetProjectReferenceScanReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetGuid& targetGuid,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetProjectReferenceReportPath(state, browserRoot, targetGuid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset project reference scan report: " + outPath.string();
        return false;
    }
    file << buildAssetProjectReferenceScanReport(state, browserRoot, targetGuid).dump(2);
    return true;
}

bool writeAssetBrokenPlaceholderReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetBrokenPlaceholderReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset broken-placeholder report: " + outPath.string();
        return false;
    }
    file << buildAssetBrokenPlaceholderReport(state, record).dump(2);
    return true;
}

bool writeAssetPackageInspectionReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetPackageInspectionReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create package inspection report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write package inspection report: " + outPath.string();
        return false;
    }
    file << buildAssetPackageInspectionReport(state, record).dump(2);
    return true;
}

bool writeAssetThumbnailReadinessReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetThumbnailReadinessReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create thumbnail readiness report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write thumbnail readiness report: " + outPath.string();
        return false;
    }
    file << buildAssetThumbnailReadinessReport(state, record).dump(2);
    return true;
}

bool writeAssetImporterReadinessReport(
    const EditorRuntimeState& state,
    const std::filesystem::path& browserRoot,
    const AssetRecord& record,
    std::filesystem::path& outPath,
    std::string& outError) {
    outPath = selectedAssetImporterReadinessReportPath(state, browserRoot, record.guid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create importer readiness report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath, std::ios::trunc);
    if (!file.is_open()) {
        outError = "Could not write importer readiness report: " + outPath.string();
        return false;
    }
    file << buildAssetImporterReadinessReport(state, record).dump(2);
    return true;
}

bool writeAssetValidationReport(const EditorRuntimeState& state, const std::filesystem::path& browserRoot, std::filesystem::path& outPath, std::string& outError, const AssetGuid& targetGuid = {}) {
    if (state.assetRegistry == nullptr) {
        outError = "Asset registry is unavailable.";
        return false;
    }
    outPath = targetGuid.empty() ? assetValidationReportPath(state, browserRoot) : selectedAssetValidationReportPath(state, browserRoot, targetGuid);
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        outError = "Could not create report folder: " + ec.message();
        return false;
    }
    std::ofstream file(outPath);
    if (!file.is_open()) {
        outError = "Could not write asset validation report: " + outPath.string();
        return false;
    }
    file << buildAssetValidationReport(state, browserRoot, targetGuid).dump(2);
    return true;
}

} // namespace

void AssetBrowserPanel::drawDependencyGraphPreview(const EditorRuntimeState& state) {
    if (state.assetRegistry == nullptr) {
        ImGui::TextDisabled("Dependency graph preview requires a loaded asset registry.");
        return;
    }

    const nlohmann::json graph = buildAssetDependencyGraphReport(state);
    ImGui::SeparatorText("Dependency Graph Preview");
    ImGui::TextDisabled(
        "Loaded registry graph: assets=%llu edges=%llu missing=%llu scene=%llu",
        static_cast<unsigned long long>(graph.value("assetCount", 0u)),
        static_cast<unsigned long long>(graph.value("edgeCount", 0u)),
        static_cast<unsigned long long>(graph.value("missingTargetCount", 0u)),
        static_cast<unsigned long long>(graph.value("currentSceneUsageCount", 0u)));
    ImGui::SameLine();
    ImGui::TextDisabled("Inspectable in-editor; not a background project-wide index.");
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##DependencyGraphPreviewFilter", "Filter graph name, GUID, type, role", dependencyGraphFilter_.data(), dependencyGraphFilter_.size());
    const std::string filter = lowerString(trimString(dependencyGraphFilter_.data()));
    auto matches = [&](std::initializer_list<std::string> values) {
        if (filter.empty()) {
            return true;
        }
        for (std::string value : values) {
            if (lowerString(std::move(value)).find(filter) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    if (ImGui::BeginTabBar("DependencyGraphPreviewTabs")) {
        if (ImGui::BeginTabItem("Assets")) {
            if (ImGui::BeginTable("DependencyGraphPreviewAssetNodes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 220.0f))) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 86.0f);
                ImGui::TableSetupColumn("Outgoing", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Incoming", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Scene", ImGuiTableColumnFlags_WidthFixed, 58.0f);
                ImGui::TableSetupColumn("GUID");
                ImGui::TableHeadersRow();
                for (const nlohmann::json& node : graph.value("nodes", nlohmann::json::array())) {
                    const std::string guid = node.value("guid", std::string{});
                    const std::string name = node.value("displayName", guid);
                    const std::string type = node.value("assetType", std::string("Asset"));
                    if (!matches({name, type, guid})) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(name.empty() ? guid.c_str() : name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(node.value("outgoingDependencyCount", 0u) + node.value("storedReferenceCount", 0u)));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%llu", static_cast<unsigned long long>(node.value("incomingRegistryEdgeCount", 0u)));
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%llu", static_cast<unsigned long long>(node.value("currentSceneUsageCount", 0u)));
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(guid.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Edges")) {
            if (ImGui::BeginTable("DependencyGraphPreviewEdges", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 220.0f))) {
                ImGui::TableSetupColumn("Source");
                ImGui::TableSetupColumn("Relation", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                ImGui::TableSetupColumn("Role");
                ImGui::TableSetupColumn("Target");
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();
                for (const nlohmann::json& edge : graph.value("edges", nlohmann::json::array())) {
                    const std::string sourceName = edge.value("sourceDisplayName", std::string{});
                    const std::string sourceGuid = edge.value("sourceGuid", std::string{});
                    const std::string targetGuid = edge.value("targetGuid", std::string{});
                    const std::string relation = edge.value("relation", std::string{});
                    const std::string role = edge.value("role", relation);
                    if (!matches({sourceName, sourceGuid, targetGuid, relation, role})) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(sourceName.empty() ? sourceGuid.c_str() : sourceName.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(relation.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(role.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(targetGuid.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextColored(edge.value("targetFound", false) ? ImVec4(0.54f, 0.82f, 0.60f, 1.0f) : ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "%s", edge.value("targetFound", false) ? "Found" : "Missing");
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Missing")) {
            if (ImGui::BeginTable("DependencyGraphPreviewMissing", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 180.0f))) {
                ImGui::TableSetupColumn("Owner");
                ImGui::TableSetupColumn("Relation", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                ImGui::TableSetupColumn("Role");
                ImGui::TableSetupColumn("Missing GUID");
                ImGui::TableHeadersRow();
                for (const nlohmann::json& missing : graph.value("missingTargets", nlohmann::json::array())) {
                    const std::string sourceName = missing.value("sourceDisplayName", std::string{});
                    const std::string sourceGuid = missing.value("sourceGuid", std::string{});
                    const std::string targetGuid = missing.value("targetGuid", std::string{});
                    const std::string relation = missing.value("relation", std::string{});
                    const std::string role = missing.value("role", relation);
                    if (!matches({sourceName, sourceGuid, targetGuid, relation, role})) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(sourceName.empty() ? sourceGuid.c_str() : sourceName.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(relation.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(role.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "%s", targetGuid.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scene Uses")) {
            if (ImGui::BeginTable("DependencyGraphPreviewSceneUses", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 180.0f))) {
                ImGui::TableSetupColumn("Entity");
                ImGui::TableSetupColumn("Component");
                ImGui::TableSetupColumn("Field");
                ImGui::TableSetupColumn("GUID");
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();
                for (const nlohmann::json& usage : graph.value("currentSceneUsages", nlohmann::json::array())) {
                    const std::string entity = usage.value("entity", std::string{});
                    const std::string component = usage.value("component", std::string{});
                    const std::string field = usage.value("field", std::string{});
                    const std::string guid = usage.value("guid", std::string{});
                    if (!matches({entity, component, field, guid})) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(entity.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(component.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(field.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(guid.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextColored(usage.value("assetFound", false) ? ImVec4(0.54f, 0.82f, 0.60f, 1.0f) : ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "%s", usage.value("assetFound", false) ? "Found" : "Missing");
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void AssetBrowserPanel::drawExternalChangeConfirmPrompt(EditorRequests& requests) {
    if (externalChangePromptOpen_) {
        ImGui::OpenPopup("Confirm External Changes");
        externalChangePromptOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Confirm External Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("Generated asset files for this asset have external source-control changes. Confirm before running %s because the existing reimport path may overwrite generated metadata, payload, or thumbnail files.", pendingExternalChangeAction_.c_str());
    ImGui::SeparatorText("Asset");
    ImGui::Text("%s", pendingExternalChangeDisplayName_.empty() ? pendingExternalChangeGuid_.c_str() : pendingExternalChangeDisplayName_.c_str());
    ImGui::TextDisabled("GUID: %s", pendingExternalChangeGuid_.c_str());
    if (!pendingExternalChangeSourcePath_.empty()) {
        ImGui::TextWrapped("Source: %s", pendingExternalChangeSourcePath_.string().c_str());
    }
    ImGui::SeparatorText("Changed Generated Files");
    if (pendingExternalChangeRiskLines_.empty()) {
        ImGui::TextDisabled("No changed generated files are currently listed.");
    } else {
        for (const std::string& line : pendingExternalChangeRiskLines_) {
            ImGui::BulletText("%s", line.c_str());
        }
    }
    ImGui::Separator();
    ImGui::TextWrapped("This prompt does not reload files automatically or resolve provider conflicts. Use Git Diff, Git Status, Reload Readiness, or Review Overwrite Risk before confirming if you need more context.");

    const std::string confirmLabel = std::string("Confirm ") + pendingExternalChangeAction_;
    if (ImGui::Button(confirmLabel.c_str(), ImVec2(170.0f, 0.0f))) {
        requests.reimportAsset = pendingExternalChangeGuid_;
        const std::string operation = pendingExternalChangeAction_.empty() ? std::string("Reimport Asset") : pendingExternalChangeAction_;
        recordImportOperation(operation, pendingExternalChangeSourcePath_, {}, "Reimport", pendingExternalChangeGuid_);
        status_ = "Queued " + operation + " after external-change confirmation: " + (pendingExternalChangeDisplayName_.empty() ? pendingExternalChangeGuid_ : pendingExternalChangeDisplayName_);
        pendingExternalChangeGuid_.clear();
        pendingExternalChangeAction_.clear();
        pendingExternalChangeDisplayName_.clear();
        pendingExternalChangeSourcePath_.clear();
        pendingExternalChangeRiskLines_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f))) {
        status_ = "External-change prompt canceled";
        pendingExternalChangeGuid_.clear();
        pendingExternalChangeAction_.clear();
        pendingExternalChangeDisplayName_.clear();
        pendingExternalChangeSourcePath_.clear();
        pendingExternalChangeRiskLines_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::invalidateThumbnails() {
    thumbnailCache_.clear();
    sourcePreviewCache_.clear();
    invalidateDirectoryCache();
}

void AssetBrowserPanel::invalidateDirectoryCache() {
    directoryListingCache_.clear();
    ++directoryListingGeneration_;
    if (directoryListingGeneration_ == 0) {
        directoryListingGeneration_ = 1;
    }
}

bool AssetBrowserPanel::openSelectedAsset(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    showDetails_ = true;
    if (!selectedRecordGuid_.empty() && state.assetRegistry != nullptr) {
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.guid != selectedRecordGuid_) {
                continue;
            }
            if (record.type == AssetType::Material) {
                if (std::optional<uint32_t> materialIndex = loadedMaterialIndexForRecord(state, record)) {
                    selection.selectMaterial(*materialIndex);
                    requests.showMaterialEditor = true;
                    status_ = "Opened material: " + (record.displayName.empty() ? record.guid : record.displayName);
                } else {
                    requests.showMaterialEditor = true;
                    status_ = "Opened Material Editor; selected material is not loaded in the current scene.";
                }
                return true;
            }
            if (record.type == AssetType::Prefab || record.type == AssetType::Mesh || record.type == AssetType::Scene) {
                status_ = "Opened asset details: " + (record.displayName.empty() ? record.guid : record.displayName);
                return true;
            }
            status_ = "Opened asset details: " + (record.displayName.empty() ? record.guid : record.displayName);
            return true;
        }
        status_ = "Selected asset record is no longer available.";
        return false;
    }
    if (!selectedPath_.empty()) {
        if (std::filesystem::is_directory(selectedPath_)) {
            navigateTo(selectedPath_);
            status_ = "Opened folder: " + selectedPath_.filename().string();
        } else if (isMaterialAssetPath(selectedPath_)) {
            requests.showMaterialEditor = true;
            status_ = "Opened Material Editor for selected material file.";
        } else {
            status_ = "Opened asset details: " + selectedPath_.filename().string();
        }
        return true;
    }
    status_ = "Select an asset in Content before using Open Asset.";
    return false;
}

AssetBrowserPanel::CpuThumbnail& AssetBrowserPanel::thumbnailForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    CpuThumbnail& thumbnail = thumbnailCache_[key];
    if (thumbnail.attempted) {
        return thumbnail;
    }
    thumbnail.attempted = true;
    thumbnail.columns = 12;
    thumbnail.rows = 7;
    thumbnail.colors.assign(static_cast<size_t>(thumbnail.columns * thumbnail.rows), IM_COL32(32, 38, 46, 255));

    if (!isRasterThumbnailPath(path) || !std::filesystem::exists(path, ec)) {
        return thumbnail;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    const std::string filename = path.string();
    if (lowerString(path.extension().string()) == ".hdr") {
        float* data = stbi_loadf(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (data == nullptr || width <= 0 || height <= 0) {
            if (data != nullptr) {
                stbi_image_free(data);
            }
            return thumbnail;
        }
        thumbnail.width = width;
        thumbnail.height = height;
        thumbnail.available = true;
        for (int row = 0; row < thumbnail.rows; ++row) {
            for (int col = 0; col < thumbnail.columns; ++col) {
                const int sampleX = std::clamp((col * width) / thumbnail.columns + width / (thumbnail.columns * 2), 0, width - 1);
                const int sampleY = std::clamp((row * height) / thumbnail.rows + height / (thumbnail.rows * 2), 0, height - 1);
                const size_t index = (static_cast<size_t>(sampleY) * static_cast<size_t>(width) + static_cast<size_t>(sampleX)) * 4u;
                thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)] = IM_COL32(
                    toneMapHdrChannel(data[index + 0]),
                    toneMapHdrChannel(data[index + 1]),
                    toneMapHdrChannel(data[index + 2]),
                    255);
            }
        }
        stbi_image_free(data);
        return thumbnail;
    }

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (data == nullptr || width <= 0 || height <= 0) {
        if (data != nullptr) {
            stbi_image_free(data);
        }
        return thumbnail;
    }
    thumbnail.width = width;
    thumbnail.height = height;
    thumbnail.available = true;
    for (int row = 0; row < thumbnail.rows; ++row) {
        for (int col = 0; col < thumbnail.columns; ++col) {
            const int sampleX = std::clamp((col * width) / thumbnail.columns + width / (thumbnail.columns * 2), 0, width - 1);
            const int sampleY = std::clamp((row * height) / thumbnail.rows + height / (thumbnail.rows * 2), 0, height - 1);
            const size_t index = (static_cast<size_t>(sampleY) * static_cast<size_t>(width) + static_cast<size_t>(sampleX)) * 4u;
            thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)] = IM_COL32(data[index + 0], data[index + 1], data[index + 2], 255);
        }
    }
    stbi_image_free(data);
    return thumbnail;
}

bool AssetBrowserPanel::drawRasterThumbnail(const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected) {
    if (!isRasterThumbnailPath(path)) {
        return false;
    }
    CpuThumbnail& thumbnail = thumbnailForPath(path);
    if (!thumbnail.available) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 innerMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 innerMax(max.x - 4.0f, max.y - 17.0f);
    const float cellW = (innerMax.x - innerMin.x) / static_cast<float>(thumbnail.columns);
    const float cellH = (innerMax.y - innerMin.y) / static_cast<float>(thumbnail.rows);
    for (int row = 0; row < thumbnail.rows; ++row) {
        for (int col = 0; col < thumbnail.columns; ++col) {
            const ImVec2 cellMin(innerMin.x + static_cast<float>(col) * cellW, innerMin.y + static_cast<float>(row) * cellH);
            const ImVec2 cellMax(innerMin.x + static_cast<float>(col + 1) * cellW + 0.5f, innerMin.y + static_cast<float>(row + 1) * cellH + 0.5f);
            dl->AddRectFilled(cellMin, cellMax, thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)]);
        }
    }
    dl->AddRect(innerMin, innerMax, IM_COL32(255, 255, 255, 42), 1.0f);
    const std::string badge = std::to_string(thumbnail.width) + "x" + std::to_string(thumbnail.height);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(178, 188, 202, 255), badge.c_str());
    dl->AddRect(min, max, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(54, 62, 72, 255), EditorUiMetric::cardRounding);
    return true;
}

bool AssetBrowserPanel::drawGpuSceneTextureThumbnail(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    if (!state.uiTextures.valid()) {
        return false;
    }
    const std::optional<uint32_t> slot = materialTextureSlotForPath(state, path);
    if (!slot.has_value()) {
        return false;
    }
    const std::vector<VkDescriptorImageInfo> descriptors = state.renderer.scene().materialCombinedDescriptors();
    if (*slot >= descriptors.size()) {
        return false;
    }
    const VkDescriptorImageInfo& descriptor = descriptors[*slot];
    const VkDescriptorSet texture = state.uiTextures.texture(descriptor.imageView, descriptor.imageLayout);
    if (texture == VK_NULL_HANDLE) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 imageMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 imageMax(max.x - 4.0f, max.y - 17.0f);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), imageMin, imageMax);
    dl->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 42), 1.0f);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(160, 210, 255, 255), "GPU texture");
    dl->AddRect(min, max, ImGui::GetColorU32(editorActiveRowColor()), EditorUiMetric::cardRounding);
    return true;
}

bool AssetBrowserPanel::drawStandaloneGpuAssetPreview(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected) {
    uint32_t width = 0;
    uint32_t height = 0;
    const VkDescriptorSet texture = state.uiTextures.assetPreviewTexture(path, &width, &height);
    if (texture == VK_NULL_HANDLE) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 imageMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 imageMax(max.x - 4.0f, max.y - 17.0f);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), imageMin, imageMax);
    dl->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 42), 1.0f);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    const std::string badge = width > 0 && height > 0
        ? "GPU preview " + std::to_string(width) + "x" + std::to_string(height)
        : std::string("GPU preview");
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(160, 210, 255, 255), badge.c_str());
    dl->AddRect(min, max, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(56, 66, 82, 210), EditorUiMetric::cardRounding);
    return true;
}

std::filesystem::path AssetBrowserPanel::generatedPreviewCachePath(const std::filesystem::path& path) const {
    if (cacheRoot_.empty() || !isGeneratedPreviewDiskCacheCandidate(path)) {
        return {};
    }
    const std::filesystem::path canonical = canonicalForCompare(path);
    const std::string keyText = canonical.string() + "|" + std::to_string(pathWriteStamp(path)) + "|" + std::to_string(pathSizeForCache(path));
    return cacheRoot_ / "Editor" / "GeneratedPreviews" / (hex64(fnv1a64(keyText)) + ".json");
}

bool AssetBrowserPanel::loadGeneratedPreviewDiskCache(const std::filesystem::path& path, SourcePreview& preview) const {
    const std::filesystem::path cachePath = generatedPreviewCachePath(path);
    if (cachePath.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec)) {
        return false;
    }
    std::optional<nlohmann::json> json = readJsonFile(cachePath);
    if (!json.has_value() || json->value("schema", std::string()) != "vibode.generatedPreview.v1") {
        return false;
    }
    const std::filesystem::path canonical = canonicalForCompare(path);
    if (json->value("sourcePath", std::string()) != canonical.string() || json->value("writeStamp", int64_t{}) != pathWriteStamp(path) ||
        json->value("sourceSize", uint64_t{}) != static_cast<uint64_t>(pathSizeForCache(path))) {
        return false;
    }

    preview.available = json->value("available", false);
    preview.loadedFromDiskCache = true;
    preview.icon = editorGlyphForPath(path);
    preview.title = json->value("title", path.filename().string());
    preview.kind = json->value("kind", contentKindLabel(path));
    preview.lines.clear();
    if (json->contains("lines") && (*json)["lines"].is_array()) {
        for (const nlohmann::json& line : (*json)["lines"]) {
            if (line.is_string()) {
                preview.lines.push_back(line.get<std::string>());
            }
        }
    }
    return preview.available;
}

void AssetBrowserPanel::saveGeneratedPreviewDiskCache(const std::filesystem::path& path, const SourcePreview& preview) const {
    const std::filesystem::path cachePath = generatedPreviewCachePath(path);
    if (cachePath.empty() || !preview.available) {
        return;
    }
    const std::filesystem::path parent = cachePath.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return;
        }
    }

    nlohmann::json json;
    json["schema"] = "vibode.generatedPreview.v1";
    json["sourcePath"] = canonicalForCompare(path).string();
    json["writeStamp"] = pathWriteStamp(path);
    json["sourceSize"] = static_cast<uint64_t>(pathSizeForCache(path));
    json["available"] = preview.available;
    json["title"] = preview.title;
    json["kind"] = preview.kind;
    json["lines"] = preview.lines;

    std::ofstream file(cachePath, std::ios::trunc);
    if (!file) {
        return;
    }
    file << json.dump(2);
}

AssetBrowserPanel::SourcePreview& AssetBrowserPanel::sourcePreviewForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    SourcePreview& preview = sourcePreviewCache_[key];
    if (preview.attempted) {
        return preview;
    }

    preview.attempted = true;
    if (loadGeneratedPreviewDiskCache(path, preview)) {
        return preview;
    }

    auto finishPreview = [&]() -> SourcePreview& {
        saveGeneratedPreviewDiskCache(path, preview);
        return preview;
    };

    preview.available = true;
    preview.icon = editorGlyphForPath(path);
    preview.title = path.filename().string();
    preview.kind = contentKindLabel(path);
    preview.lines.push_back(fileSizeLabel(path));

    if (std::filesystem::is_directory(path, ec)) {
        size_t folders = 0;
        size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec)) {
                ++folders;
            } else if (entry.is_regular_file(ec)) {
                ++files;
            }
        }
        preview.lines.push_back(countLabel("Folders", folders));
        preview.lines.push_back(countLabel("Files", files));
        return finishPreview();
    }

    if (isModelAssetPath(path)) {
        if (lowerString(path.extension().string()) == ".gltf") {
            if (std::optional<nlohmann::json> json = readJsonFile(path)) {
                preview.lines.push_back(countLabel("Nodes", jsonArraySize(*json, "nodes")));
                preview.lines.push_back(countLabel("Meshes", jsonArraySize(*json, "meshes")));
                preview.lines.push_back(countLabel("Materials", jsonArraySize(*json, "materials")));
                preview.lines.push_back(countLabel("Textures", jsonArraySize(*json, "textures")));
                preview.lines.push_back(countLabel("Cameras", jsonArraySize(*json, "cameras")));
            } else {
                preview.lines.push_back("glTF metadata unavailable");
            }
        } else {
            preview.lines.push_back("Binary/external model preview");
            preview.lines.push_back("Import to generate prefab metadata");
        }
        return finishPreview();
    }

    if (isSceneAssetPath(path)) {
        if (std::optional<nlohmann::json> json = readJsonFile(path)) {
            const size_t entities = json->contains("entities") && (*json)["entities"].is_array() ? (*json)["entities"].size() : 0;
            size_t cameras = 0;
            size_t lights = 0;
            size_t meshes = 0;
            if (json->contains("entities") && (*json)["entities"].is_array()) {
                for (const nlohmann::json& entity : (*json)["entities"]) {
                    if (entity.contains("camera")) ++cameras;
                    if (entity.contains("light") || entity.contains("sun")) ++lights;
                    if (entity.contains("meshRenderer")) ++meshes;
                }
            }
            preview.lines.push_back(countLabel("Entities", entities));
            preview.lines.push_back(countLabel("Cameras", cameras));
            preview.lines.push_back(countLabel("Lights", lights));
            preview.lines.push_back(countLabel("Mesh renderers", meshes));
        } else {
            preview.lines.push_back("Scene metadata unavailable");
        }
        return finishPreview();
    }

    if (isProjectAssetPath(path)) {
        if (std::optional<nlohmann::json> json = readJsonFile(path)) {
            preview.title = json->value("name", preview.title);
            preview.lines.push_back("Project file .vproject");
            preview.lines.push_back("Startup " + json->value("startupScene", std::string("(none)")));
            preview.lines.push_back("Content " + json->value("contentRoot", std::string("Content")));
            preview.lines.push_back("Scenes " + json->value("scenesRoot", std::string("Scenes")));
        } else {
            preview.lines.push_back("Project metadata unavailable");
        }
        return finishPreview();
    }

    if (isMaterialAssetPath(path)) {
        std::ifstream file(path);
        size_t materials = 0;
        size_t textureRefs = 0;
        std::string line;
        while (std::getline(file, line)) {
            const std::string lower = lowerString(trimString(line));
            if (lower.rfind("newmtl ", 0) == 0) ++materials;
            if (isMtlTextureMapLine(lower)) ++textureRefs;
        }
        preview.lines.push_back(countLabel("Materials", materials));
        preview.lines.push_back(countLabel("Texture maps", textureRefs));
        preview.lines.push_back("Swatch preview from source metadata");
        return finishPreview();
    }

    if (isIesAssetPath(path)) {
        std::ifstream file(path);
        std::string firstLine;
        std::getline(file, firstLine);
        preview.lines.push_back(firstLine.empty() ? "IES photometric profile" : firstLine.substr(0, 80));
        preview.lines.push_back("Assignable to authored light profile fields");
        return finishPreview();
    }

    if (isVolumeAssetPath(path)) {
        preview.lines.push_back("OpenVDB volume container");
        preview.lines.push_back("Metadata preview; runtime import pending");
        return finishPreview();
    }

    preview.available = supportedContentPath(path);
    if (!preview.available) {
        preview.lines.push_back("Unsupported file type");
    }
    return finishPreview();
}

bool AssetBrowserPanel::drawGeneratedSourcePreview(const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    SourcePreview& preview = sourcePreviewForPath(path);
    if (!preview.available) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent = contentIconColor(path);
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(max.x - 4.0f, min.y + 30.0f), IM_COL32(28, 34, 43, 245), 2.0f);
    editorDrawIconGlyph(preview.icon, ImVec2(min.x + 10.0f, min.y + 8.0f), ImVec2(min.x + 26.0f, min.y + 24.0f), accent);
    dl->AddText(ImVec2(min.x + 32.0f, min.y + 9.0f), IM_COL32(220, 226, 236, 255), preview.kind.c_str());

    const float diagramTop = min.y + 38.0f;
    if (isModelAssetPath(path)) {
        const ImVec2 c(min.x + 42.0f, diagramTop + 32.0f);
        const float s = 28.0f;
        dl->AddRect(ImVec2(c.x - s, c.y - s * 0.55f), ImVec2(c.x + s, c.y + s * 0.55f), accent, 2.0f, 0, 1.5f);
        dl->AddLine(ImVec2(c.x - s, c.y - s * 0.55f), ImVec2(c.x - s * 0.45f, c.y - s), accent, 1.5f);
        dl->AddLine(ImVec2(c.x + s, c.y - s * 0.55f), ImVec2(c.x + s * 0.45f, c.y - s), accent, 1.5f);
        dl->AddLine(ImVec2(c.x - s * 0.45f, c.y - s), ImVec2(c.x + s * 0.45f, c.y - s), accent, 1.5f);
    } else if (isSceneAssetPath(path) || isProjectAssetPath(path)) {
        for (int i = 0; i < 4; ++i) {
            const float x = min.x + 18.0f + static_cast<float>(i % 2) * 42.0f;
            const float y = diagramTop + 8.0f + static_cast<float>(i / 2) * 24.0f;
            dl->AddRect(ImVec2(x, y), ImVec2(x + 28.0f, y + 16.0f), accent, 2.0f, 0, 1.3f);
        }
    } else if (isMaterialAssetPath(path)) {
        dl->AddCircleFilled(ImVec2(min.x + 44.0f, diagramTop + 34.0f), 26.0f, IM_COL32(115, 92, 140, 255));
        dl->AddCircle(ImVec2(min.x + 44.0f, diagramTop + 34.0f), 26.0f, accent, 24, 1.5f);
    } else if (isIesAssetPath(path)) {
        const ImVec2 c(min.x + 48.0f, diagramTop + 42.0f);
        dl->PathLineTo(c);
        for (int i = 0; i <= 12; ++i) {
            const float t = static_cast<float>(i) / 12.0f;
            const float angle = -1.25f + t * 2.5f;
            const float radius = 14.0f + std::sin(t * 3.14159f) * 24.0f;
            dl->PathLineTo(ImVec2(c.x + std::cos(angle) * radius, c.y - std::sin(angle) * radius));
        }
        dl->PathStroke(accent, 0, 1.6f);
    } else if (isVolumeAssetPath(path)) {
        for (int i = 0; i < 4; ++i) {
            dl->AddCircle(ImVec2(min.x + 30.0f + i * 12.0f, diagramTop + 26.0f + (i % 2) * 9.0f), 18.0f, IM_COL32(130, 210, 190, 120), 20, 1.4f);
        }
    }

    float textY = min.y + 38.0f;
    const float textX = min.x + 92.0f;
    const float textMaxX = max.x - 8.0f;
    for (size_t i = 0; i < preview.lines.size() && i < 4; ++i) {
        std::string line = preview.lines[i];
        while (!line.empty() && ImGui::CalcTextSize(line.c_str()).x > textMaxX - textX) {
            line.pop_back();
        }
        if (line.size() < preview.lines[i].size() && line.size() > 3) {
            line.replace(line.size() - 3, 3, "...");
        }
        dl->AddText(ImVec2(textX, textY), i == 0 ? IM_COL32(205, 214, 226, 255) : IM_COL32(142, 151, 164, 255), line.c_str());
        textY += 17.0f;
    }
    dl->AddRect(min, max, IM_COL32(65, 76, 91, 190), EditorUiMetric::cardRounding);
    return true;
}

void AssetBrowserPanel::loadFromPath(const std::filesystem::path& path, EditorRequests& requests) {
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".hdr" || ext == ".exr") {
        requests.loadHdr = path;
        status_ = "Queued HDRI import/apply: " + path.string();
    } else if (ext == ".rtlevel") {
        requests.openScene = path;
        status_ = "Queued scene open: " + path.string();
    } else if (!compatibilityMode_ && isModelAssetPath(path)) {
        requests.importAsset = EditorImportAssetRequest{.sourcePath = path};
        recordImportOperation("Import Asset", path, currentPath_, "ImportAsset");
        status_ = "Queued Import Asset: " + path.string();
    } else {
        requests.importSceneAsNewScene = path;
        status_ = "Queued Import Scene as New Scene: " + path.string();
    }
}

void AssetBrowserPanel::recordImportOperation(
    const std::string& label,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationFolder,
    const std::string& mode,
    const AssetGuid& assetGuid) {
    for (ImportOperation& operation : importOperations_) {
        const bool sameGuid = !assetGuid.empty() && operation.assetGuid == assetGuid;
        const bool sameSource = assetGuid.empty() && samePathForOperation(operation.sourcePath, sourcePath) && operation.mode == mode;
        if ((sameGuid || sameSource) && !operation.completed && !operation.failed) {
            operation.label = label;
            operation.sourcePath = sourcePath;
            operation.destinationFolder = destinationFolder;
            operation.mode = mode;
            operation.progress = std::max(operation.progress, 0.12f);
            operation.state = "Queued";
            return;
        }
    }

    ImportOperation operation;
    operation.id = nextImportOperationId_++;
    operation.label = label;
    operation.sourcePath = sourcePath;
    operation.destinationFolder = destinationFolder;
    operation.mode = mode;
    operation.assetGuid = assetGuid;
    operation.progress = 0.12f;
    operation.state = "Queued";
    importOperations_.insert(importOperations_.begin(), std::move(operation));
    if (importOperations_.size() > 12) {
        importOperations_.resize(12);
    }
}

void AssetBrowserPanel::refreshImportOperations(const EditorRuntimeState& state) {
    if (importOperations_.empty()) {
        return;
    }
    const AssetRegistry* registry = state.assetRegistry;
    for (ImportOperation& operation : importOperations_) {
        if (operation.completed || operation.failed) {
            continue;
        }

        const AssetRecord* matchedRecord = nullptr;
        if (registry != nullptr) {
            for (const AssetRecord& record : registry->records()) {
                if (!operation.assetGuid.empty() && record.guid == operation.assetGuid) {
                    matchedRecord = &record;
                    break;
                }
                const std::filesystem::path recordSource = resolveAssetRecordPath(state, record.sourcePath);
                if (operation.assetGuid.empty() && samePathForOperation(operation.sourcePath, recordSource)) {
                    matchedRecord = &record;
                    break;
                }
            }
        }

        if (matchedRecord != nullptr) {
            operation.progress = assetImportProgress(*matchedRecord);
            operation.state = assetImportProgressLabel(*matchedRecord);
            operation.failed = matchedRecord->status == AssetImportStatus::Failed;
            operation.completed = matchedRecord->status == AssetImportStatus::Imported && !matchedRecord->missing && !matchedRecord->stale;
            if (operation.completed) {
                operation.progress = 1.0f;
                operation.state = "Completed";
            }
        } else {
            operation.progress = std::max(operation.progress, 0.35f);
            operation.state = "Handed off";
        }
    }
}

void AssetBrowserPanel::drawImportOperations() {
    if (importOperations_.empty()) {
        return;
    }

    ImGui::SeparatorText("Import Queue");
    ImGui::TextDisabled("Queued and recent asset import operations");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Completed")) {
        importOperations_.erase(
            std::remove_if(importOperations_.begin(), importOperations_.end(), [](const ImportOperation& operation) {
                return operation.completed || operation.failed;
            }),
            importOperations_.end());
    }

    if (ImGui::BeginTable("ContentImportOperations", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Operation");
        ImGui::TableSetupColumn("Asset");
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::progressColumnWidth);
        ImGui::TableHeadersRow();
        for (const ImportOperation& operation : importOperations_) {
            ImGui::PushID(static_cast<int>(operation.id));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(operation.label.c_str());
            ImGui::TableSetColumnIndex(1);
            const std::string assetName = operation.sourcePath.empty()
                ? operation.assetGuid
                : operation.sourcePath.filename().string();
            ImGui::TextUnformatted(assetName.empty() ? "(asset)" : assetName.c_str());
            if (!operation.destinationFolder.empty()) {
                ImGui::TextDisabled("-> %s", operation.destinationFolder.generic_string().c_str());
            }
            ImGui::TableSetColumnIndex(2);
            if (operation.failed) {
                ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.32f, 1.0f), "%s", operation.state.c_str());
            } else if (operation.completed) {
                ImGui::TextColored(ImVec4(0.48f, 0.82f, 0.55f, 1.0f), "%s", operation.state.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.55f, 0.72f, 0.95f, 1.0f), "%s", operation.state.c_str());
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::ProgressBar(std::clamp(operation.progress, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), operation.completed ? "Done" : operation.state.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void AssetBrowserPanel::prepareImportDialog(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationFolder, int mode) {
    setPathBuffer(importSourcePath_, sourcePath);
    std::string destination = "Models";
    if (!destinationFolder.empty()) {
        destination = relativeImportDestination(destinationFolder);
        if (destination.empty()) {
            destination = ".";
        }
    }
    setTextBuffer(importDestinationFolder_, destination);
    importMode_ = mode;
    openImportSettings_ = true;
}

void AssetBrowserPanel::syncBrowserRoot(const EditorRuntimeState& state) {
    std::filesystem::path root;
    std::filesystem::path defaultPath;
    compatibilityMode_ = state.project == nullptr;
    if (state.project != nullptr) {
        root = state.project->projectRoot;
        defaultPath = state.project->contentRoot;
        contentRoot_ = canonicalForCompare(state.project->contentRoot);
        scenesRoot_ = canonicalForCompare(state.project->scenesRoot);
        savedRoot_ = canonicalForCompare(state.project->savedRoot);
        cacheRoot_ = canonicalForCompare(state.project->cacheRoot);
    } else if (state.scenePath != nullptr && state.scenePath->has_value()) {
        root = state.scenePath->value().parent_path();
        defaultPath = root;
        contentRoot_.clear();
        scenesRoot_.clear();
        savedRoot_.clear();
        cacheRoot_.clear();
    } else {
        root = std::filesystem::current_path();
        defaultPath = root;
        contentRoot_.clear();
        scenesRoot_.clear();
        savedRoot_.clear();
        cacheRoot_.clear();
    }

    if (root.empty()) {
        root = std::filesystem::current_path();
    }
    if (defaultPath.empty()) {
        defaultPath = root;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::filesystem::create_directories(defaultPath, ec);
    root = canonicalForCompare(root);
    defaultPath = canonicalForCompare(defaultPath);
    if (browserRoot_ != root) {
        browserRoot_ = root;
        currentPath_ = pathIsWithin(defaultPath, browserRoot_) ? defaultPath : root;
        selectedPath_.clear();
        selectedRecordGuid_.clear();
        backStack_.clear();
        forwardStack_.clear();
        sourceControlStatusCache_.clear();
        invalidateDirectoryCache();
        clearGitStatusSnapshots();
    } else if (currentPath_.empty()) {
        currentPath_ = root;
    }
}

void AssetBrowserPanel::navigateTo(const std::filesystem::path& path, bool addHistory) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_directory(path, ec)) {
        return;
    }
    const std::filesystem::path next = canonicalForCompare(path);
    if (next == currentPath_) {
        return;
    }
    if (addHistory && !currentPath_.empty()) {
        backStack_.push_back(currentPath_);
        forwardStack_.clear();
    }
    currentPath_ = next;
    selectedPath_.clear();
    selectedRecordGuid_.clear();
}

bool AssetBrowserPanel::shouldShowPath(const std::filesystem::path& path) const {
    const std::string filter = lowerString(search_.data());
    if (filter.empty()) {
        return true;
    }
    return lowerString(path.filename().string()).find(filter) != std::string::npos;
}

const AssetBrowserPanel::DirectoryListingCache& AssetBrowserPanel::directoryListingForPath(const std::filesystem::path& path) {
    const std::string key = canonicalForCompare(path).string();
    DirectoryListingCache& cache = directoryListingCache_[key];
    if (cache.generation == directoryListingGeneration_) {
        return cache;
    }

    cache = DirectoryListingCache{};
    cache.generation = directoryListingGeneration_;
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_directory(path, ec)) {
        return cache;
    }

    for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryError;
        const bool isDirectory = entry.is_directory(entryError);
        if (entryError) {
            continue;
        }
        cache.entries.push_back(PathListEntry{entry.path(), isDirectory});
        if (isDirectory) {
            cache.childDirectories.push_back(entry.path());
        }
    }

    std::sort(cache.childDirectories.begin(), cache.childDirectories.end());
    std::sort(cache.entries.begin(), cache.entries.end(), [](const PathListEntry& a, const PathListEntry& b) {
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory > b.isDirectory;
        }
        return lowerString(a.path.filename().string()) < lowerString(b.path.filename().string());
    });
    return cache;
}

std::string AssetBrowserPanel::relativeContentPath(const std::filesystem::path& path) const {
    if (browserRoot_.empty() || path.empty()) {
        return path.string();
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, browserRoot_, ec);
    return ec ? path.string() : relative.generic_string();
}

std::string AssetBrowserPanel::relativeImportDestination(const std::filesystem::path& path) const {
    if (!contentRoot_.empty() && pathIsWithin(path, contentRoot_)) {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(path, contentRoot_, ec);
        return ec ? path.string() : relative.generic_string();
    }
    return relativeContentPath(path);
}

void AssetBrowserPanel::drawPathContextMenu(const std::filesystem::path& path, bool isDirectory, EditorRequests& requests) {
    if (isDirectory) {
        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Open Folder")) {
            navigateTo(path);
        }
        editorGlyphMenuItem(EditorGlyphIcon::Add, "New Folder", false);
        if (editorGlyphMenuItem(EditorGlyphIcon::Import, "Import Here...", !compatibilityMode_)) {
            if (auto source = openImportAssetFileDialog()) {
                prepareImportDialog(*source, path, 0);
            }
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Import and Place Here...", !compatibilityMode_)) {
            if (auto source = openGltfFileDialog()) {
                prepareImportDialog(*source, path, 1);
            }
        }
        ImGui::Separator();
        if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
            copyPathToClipboard(path);
            status_ = "Copied path: " + path.string();
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer")) {
            revealPathInFileBrowser(path);
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Refresh")) {
            invalidateThumbnails();
            status_ = "Content refreshed";
        }
        return;
    }

    const bool canOpen = canOpenOrApplyPath(path);
    const bool canImport = !compatibilityMode_ && isImportableSourceAssetPath(path);
    const bool canImportAndPlace = !compatibilityMode_ && isPlaceablePrefabSourcePath(path);
    if (editorGlyphMenuItem(editorGlyphForPath(path), "Open / Apply", canOpen)) {
        loadFromPath(path, requests);
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Import, "Import Asset...", canImport)) {
        prepareImportDialog(path, currentPath_, 0);
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Import and Place...", canImportAndPlace)) {
        prepareImportDialog(path, currentPath_, 1);
    }
    editorGlyphMenuItem(EditorGlyphIcon::Details, "Preview", false);
    editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Reimport", false);
    ImGui::Separator();
    if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
        copyPathToClipboard(path);
        status_ = "Copied path: " + path.string();
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer")) {
        revealPathInFileBrowser(path);
    }
    editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete", false);
}

void AssetBrowserPanel::drawFolderTree(const std::filesystem::path& path, EditorRequests& requests) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return;
    }
    const DirectoryListingCache& listing = directoryListingForPath(path);
    const bool selected = canonicalForCompare(path) == canonicalForCompare(currentPath_);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (listing.childDirectories.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    ImGui::PushID(path.string().c_str());
    const std::string treeLabel = editorGlyphLabel(path.filename().empty() ? path.string() : path.filename().string());
    editorDrawPreRowBand(EditorUiMetric::contentRowHeight);
    editorPushRowSelectionStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::contentRowHeight));
    const bool open = ImGui::TreeNodeEx(treeLabel.c_str(), flags);
    ImGui::PopStyleVar();
    editorPopRowSelectionStyle();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const float iconX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + 2.0f;
    const float iconY = rowMin.y + std::max(0.0f, (rowMax.y - rowMin.y - 16.0f) * 0.5f);
    drawContentGlyph(path, ImVec2(iconX, iconY), ImVec2(iconX + 16.0f, iconY + 16.0f));
    if (ImGui::IsItemClicked()) {
        navigateTo(path);
    }
    if (ImGui::BeginPopupContextItem("FolderContext")) {
        selectedPath_ = path;
        selectedRecordGuid_.clear();
        drawPathContextMenu(path, true, requests);
        ImGui::EndPopup();
    }
    if (open) {
        for (const auto& child : listing.childDirectories) {
            drawFolderTree(child, requests);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::drawPathList(const EditorRuntimeState& state, EditorRequests& requests) {
    const DirectoryListingCache& listing = directoryListingForPath(currentPath_);
    std::vector<const PathListEntry*> entries;
    entries.reserve(listing.entries.size());
    for (const PathListEntry& entry : listing.entries) {
        if (shouldShowPath(entry.path)) {
            entries.push_back(&entry);
        }
    }

    if (gridView_) {
        const float cellWidth = EditorUiMetric::contentGridCellWidth;
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth));
        ImGui::Columns(columns, "ContentGrid", false);
        for (const PathListEntry* entry : entries) {
            const std::filesystem::path path = entry->path;
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            const ImVec2 thumbSize(EditorUiMetric::contentGridThumbWidth, EditorUiMetric::contentGridThumbHeight);
            ImGui::InvisibleButton("ContentGridThumb", thumbSize);
            const ImVec2 thumbMin = ImGui::GetItemRectMin();
            const ImVec2 thumbMax = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(thumbMin, thumbMax, selected ? ImGui::GetColorU32(editorSelectedRowColor()) : IM_COL32(24, 27, 32, 255), EditorUiMetric::cardRounding);
            dl->AddRect(thumbMin, thumbMax, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(54, 62, 72, 255), EditorUiMetric::cardRounding);
            if (!drawGpuSceneTextureThumbnail(state, path, thumbMin, thumbMax) &&
                !drawStandaloneGpuAssetPreview(state, path, thumbMin, thumbMax, selected) &&
                !drawRasterThumbnail(path, thumbMin, thumbMax, selected)) {
                drawContentGlyph(
                    path,
                    ImVec2(thumbMin.x + thumbSize.x * 0.34f, thumbMin.y + thumbSize.y * 0.22f),
                    ImVec2(thumbMax.x - thumbSize.x * 0.34f, thumbMax.y - thumbSize.y * 0.22f));
            }
            if (ImGui::IsItemClicked()) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
            }
            (void)drawLevelPathDragDropSource(path);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry->isDirectory) {
                    navigateTo(path);
                } else {
                    loadFromPath(path, requests);
                }
            }
            if (ImGui::BeginPopupContextItem("PathContext")) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                drawPathContextMenu(path, entry->isDirectory, requests);
                ImGui::EndPopup();
            }
            ImGui::TextWrapped("%s%s", selected ? "> " : "", path.filename().string().c_str());
            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 0.0f));
    if (ImGui::BeginTable("ContentPathListCompact", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Asset");
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entries.size()), EditorUiMetric::contentRowHeight);
        while (clipper.Step()) {
        for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex) {
            const PathListEntry* entry = entries[static_cast<size_t>(rowIndex)];
            const std::filesystem::path path = entry->path;
            const bool isDir = entry->isDirectory;
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const ImVec2 nameCursor = ImGui::GetCursorScreenPos();
            const std::string name = editorGlyphLabel(path.filename().string());
            editorPushRowSelectionStyle();
            if (ImGui::Selectable(
                    name.c_str(),
                    selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0.0f, EditorUiMetric::contentRowHeight))) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (isDir) {
                        navigateTo(path);
                    } else {
                        loadFromPath(path, requests);
                    }
                }
            }
            editorPopRowSelectionStyle();
            (void)drawLevelPathDragDropSource(path);
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float iconY = nameCursor.y + std::max(0.0f, (itemMax.y - nameCursor.y - 16.0f) * 0.5f);
            drawContentGlyph(path, ImVec2(nameCursor.x + 2.0f, iconY), ImVec2(nameCursor.x + 18.0f, iconY + 16.0f));
            if (ImGui::BeginPopupContextItem("PathContext")) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                drawPathContextMenu(path, isDir, requests);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void AssetBrowserPanel::drawRegistryTable(const EditorRuntimeState& state, EditorRequests& requests) {
    if (state.assetRegistry == nullptr) {
        return;
    }
    const AssetRegistry& registry = *state.assetRegistry;
    ImGui::SeparatorText("Asset Registry");
    ImGui::Text("Registry: %s%s",
        registry.state().path.empty() ? "(none)" : registry.state().path.string().c_str(),
        registry.dirty() ? " *" : "");
    const auto& records = registry.records();
    if (records.empty()) {
        ImGui::TextDisabled("No registry records yet. Import Asset will populate this in the next milestone.");
        return;
    }
    const std::vector<std::string> registryTags = collectRegistryTags(state.assetRegistry);
    const std::vector<std::string> tagSuggestions = mergedTagSuggestions(registryTags, state.editorPrefs);

    struct RegistryGroupView {
        std::string id;
        std::string name;
        AssetGuid rootGuid;
        std::vector<AssetGuid> guids;
    };
    auto registryGroupIdForRecord = [](const AssetRecord& record) {
        if (!record.importGroupId.empty()) {
            return record.importGroupId;
        }
        if (!record.sourceHash.empty() && !record.importSettingsHash.empty()) {
            return record.sourceHash + ":" + record.importSettingsHash;
        }
        if (!record.sourcePath.empty()) {
            return std::string("source:") + lowerString(std::filesystem::path(record.sourcePath).lexically_normal().generic_string());
        }
        if (!record.importedPath.empty()) {
            return std::string("imported:") + lowerString(std::filesystem::path(record.importedPath).parent_path().lexically_normal().generic_string());
        }
        return std::string("asset:") + record.guid;
    };
    auto registryGroupNameForRecord = [](const AssetRecord& record) {
        if (!record.importGroupName.empty()) {
            return record.importGroupName;
        }
        if (!record.sourcePath.empty()) {
            const std::string stem = std::filesystem::path(record.sourcePath).stem().string();
            if (!stem.empty()) {
                return stem;
            }
        }
        if (!record.importedPath.empty()) {
            const std::string parent = std::filesystem::path(record.importedPath).parent_path().filename().string();
            if (!parent.empty()) {
                return parent;
            }
        }
        return record.displayName.empty() ? std::string("Ungrouped") : record.displayName;
    };
    std::vector<RegistryGroupView> registryGroups;
    for (const AssetRecord& record : records) {
        const std::string groupId = registryGroupIdForRecord(record);
        auto groupIt = std::find_if(registryGroups.begin(), registryGroups.end(), [&](const RegistryGroupView& group) {
            return group.id == groupId;
        });
        if (groupIt == registryGroups.end()) {
            RegistryGroupView group;
            group.id = groupId;
            group.name = registryGroupNameForRecord(record);
            group.rootGuid = record.importRootGuid.empty() ? record.guid : record.importRootGuid;
            group.guids.push_back(record.guid);
            registryGroups.push_back(std::move(group));
        } else {
            groupIt->guids.push_back(record.guid);
            if (groupIt->rootGuid.empty() && !record.importRootGuid.empty()) {
                groupIt->rootGuid = record.importRootGuid;
            }
        }
    }
    std::sort(registryGroups.begin(), registryGroups.end(), [](const RegistryGroupView& lhs, const RegistryGroupView& rhs) {
        if (lowerString(lhs.name) != lowerString(rhs.name)) return lowerString(lhs.name) < lowerString(rhs.name);
        return lhs.id < rhs.id;
    });
    if (!selectedRegistryGroupId_.empty()) {
        const bool selectedGroupExists = std::any_of(registryGroups.begin(), registryGroups.end(), [&](const RegistryGroupView& group) {
            return group.id == selectedRegistryGroupId_;
        });
        if (!selectedGroupExists) {
            selectedRegistryGroupId_.clear();
        }
    }

    const RegistryGroupView* selectedRegistryGroup = nullptr;
    for (const RegistryGroupView& group : registryGroups) {
        if (group.id == selectedRegistryGroupId_) {
            selectedRegistryGroup = &group;
            break;
        }
    }

    ImGui::SeparatorText("Asset Registry Browser");
    ImGui::BeginChild("RegistryFolderTree", ImVec2(300.0f, 132.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextDisabled("Folders");
    auto drawRegistryFolderIcon = [](ImGuiTreeNodeFlags flags) {
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const float iconX = rowMin.x + ((flags & ImGuiTreeNodeFlags_Leaf) ? ImGui::GetTreeNodeToLabelSpacing() : ImGui::GetTreeNodeToLabelSpacing()) + 2.0f;
        const float iconY = rowMin.y + std::max(0.0f, (rowMax.y - rowMin.y - 16.0f) * 0.5f);
        editorDrawIconGlyph(EditorGlyphIcon::Folder, ImVec2(iconX, iconY), ImVec2(iconX + 16.0f, iconY + 16.0f), IM_COL32(185, 202, 224, 255));
    };
    auto drawAllImportsRow = [&]() {
        ImGui::PushID("AllImportsFolder");
        const std::string label = editorGlyphLabel("All Imports (" + std::to_string(records.size()) + ")");
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selectedRegistryGroupId_.empty()) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        editorDrawPreRowBand(EditorUiMetric::contentRowHeight);
        editorPushRowSelectionStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::contentRowHeight));
        ImGui::TreeNodeEx(label.c_str(), flags);
        ImGui::PopStyleVar();
        editorPopRowSelectionStyle();
        drawRegistryFolderIcon(flags);
        if (ImGui::IsItemClicked()) {
            selectedRegistryGroupId_.clear();
        }
        ImGui::PopID();
    };
    drawAllImportsRow();

    ImGui::PushID("ImportedAssetsRoot");
    const std::string rootLabel = editorGlyphLabel("Imported Assets");
    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    editorDrawPreRowBand(EditorUiMetric::contentRowHeight);
    editorPushRowSelectionStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::contentRowHeight));
    const bool rootOpen = ImGui::TreeNodeEx(rootLabel.c_str(), rootFlags);
    ImGui::PopStyleVar();
    editorPopRowSelectionStyle();
    drawRegistryFolderIcon(rootFlags);
    if (rootOpen) {
        for (const RegistryGroupView& group : registryGroups) {
            ImGui::PushID(group.id.c_str());
            const std::string label = editorGlyphLabel(group.name + " (" + std::to_string(group.guids.size()) + ")");
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (selectedRegistryGroupId_ == group.id) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            editorDrawPreRowBand(EditorUiMetric::contentRowHeight);
            editorPushRowSelectionStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::contentRowHeight));
            ImGui::TreeNodeEx(label.c_str(), flags);
            ImGui::PopStyleVar();
            editorPopRowSelectionStyle();
            drawRegistryFolderIcon(flags);
            if (ImGui::IsItemClicked()) {
                selectedRegistryGroupId_ = group.id;
            }
            if (ImGui::BeginPopupContextItem()) {
                if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove Folder From Registry")) {
                    requests.deleteAssets = EditorDeleteAssetRequest{group.guids, false};
                    if (selectedRegistryGroupId_ == group.id) {
                        selectedRegistryGroupId_.clear();
                    }
                    status_ = "Queued registry folder removal: " + group.name;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete Folder Generated Files")) {
                    requests.deleteAssets = EditorDeleteAssetRequest{group.guids, true};
                    if (selectedRegistryGroupId_ == group.id) {
                        selectedRegistryGroupId_.clear();
                    }
                    status_ = "Queued registry folder file delete: " + group.name;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(selectedRegistryGroup != nullptr ? selectedRegistryGroup->name.c_str() : "All Imports");
    ImGui::TextDisabled(
        selectedRegistryGroup != nullptr ? "%zu assets in selected folder" : "%zu assets across %zu folders",
        selectedRegistryGroup != nullptr ? selectedRegistryGroup->guids.size() : records.size(),
        registryGroups.size());
    if (selectedRegistryGroup != nullptr) {
        if (contentActionButton("RegistryFolderRemoveSelected", EditorGlyphIcon::Trash, "Remove Folder", "Remove this virtual folder's records from the registry")) {
            requests.deleteAssets = EditorDeleteAssetRequest{selectedRegistryGroup->guids, false};
            selectedRegistryGroupId_.clear();
            status_ = "Queued registry folder removal: " + selectedRegistryGroup->name;
        }
        ImGui::SameLine();
        if (contentActionButton("RegistryFolderDeleteSelectedFiles", EditorGlyphIcon::Trash, "Delete Files", "Remove this virtual folder and delete generated metadata/cache files inside the project")) {
            requests.deleteAssets = EditorDeleteAssetRequest{selectedRegistryGroup->guids, true};
            selectedRegistryGroupId_.clear();
            status_ = "Queued registry folder file delete: " + selectedRegistryGroup->name;
        }
    } else {
        ImGui::TextDisabled("Select a folder to filter the registry table.");
    }
    ImGui::EndGroup();

    constexpr const char* typeFilters[] = {"All Types", "Mesh", "Material", "Texture", "HDRI", "Scene", "Prefab", "Animation", "Skeleton", "Unknown"};
    constexpr const char* statusFilters[] = {"All Status", "Imported", "Missing", "Stale", "Failed", "Unknown"};
    constexpr const char* healthFilters[] = {
        "All Health",
        "Healthy",
        "Any Issue",
        "Source Missing",
        "Metadata Missing",
        "Payload Missing",
        "Dependency Missing",
        "Has Dependencies",
        "Has References",
        "Used By Loaded Data",
    };
    constexpr const char* favoriteFilters[] = {"All Assets", "Favorite Assets"};
    registryTypeFilter_ = std::clamp(registryTypeFilter_, 0, static_cast<int>(std::size(typeFilters)) - 1);
    registryStatusFilter_ = std::clamp(registryStatusFilter_, 0, static_cast<int>(std::size(statusFilters)) - 1);
    registryHealthFilter_ = std::clamp(registryHealthFilter_, 0, static_cast<int>(std::size(healthFilters)) - 1);
    registryFavoriteFilter_ = std::clamp(registryFavoriteFilter_, 0, static_cast<int>(std::size(favoriteFilters)) - 1);
    const int collectionFilterMax = state.editorPrefs != nullptr ? static_cast<int>(state.editorPrefs->assetCollections.size()) : 0;
    registryCollectionFilter_ = std::clamp(registryCollectionFilter_, 0, collectionFilterMax);

    ImGui::PushID("RegistryFilters");
    ImGui::SetNextItemWidth(126.0f);
    ImGui::Combo("##type", &registryTypeFilter_, typeFilters, static_cast<int>(std::size(typeFilters)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by asset type");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(126.0f);
    ImGui::Combo("##status", &registryStatusFilter_, statusFilters, static_cast<int>(std::size(statusFilters)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by import status");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(164.0f);
    ImGui::Combo("##health", &registryHealthFilter_, healthFilters, static_cast<int>(std::size(healthFilters)));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by health, dependencies, or loaded usage");
    }
    if (state.editorPrefs != nullptr) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(132.0f);
        ImGui::Combo("##favorites", &registryFavoriteFilter_, favoriteFilters, static_cast<int>(std::size(favoriteFilters)));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Filter registry records to favorite assets");
        }
    }
    if (state.editorPrefs != nullptr && !state.editorPrefs->assetCollections.empty()) {
        ImGui::SameLine();
        const char* collectionLabel = "All Collections";
        if (const EditorAssetCollection* collection = selectedCollection(state.editorPrefs, registryCollectionFilter_)) {
            collectionLabel = collection->name.c_str();
        }
        if (ImGui::BeginCombo("##collectionFilter", collectionLabel)) {
            if (ImGui::Selectable("All Collections", registryCollectionFilter_ == 0)) {
                registryCollectionFilter_ = 0;
            }
            for (size_t i = 0; i < state.editorPrefs->assetCollections.size(); ++i) {
                const int index = static_cast<int>(i + 1);
                const EditorAssetCollection& collection = state.editorPrefs->assetCollections[i];
                if (ImGui::Selectable(collection.name.c_str(), registryCollectionFilter_ == index)) {
                    registryCollectionFilter_ = index;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Filter registry records by saved asset collection");
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(132.0f);
    ImGui::InputTextWithHint("##tag", "Tag filter", registryTagFilter_.data(), registryTagFilter_.size());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Filter registry records by asset tag");
    }
    if (!tagSuggestions.empty()) {
        ImGui::SameLine();
        if (ImGui::BeginCombo("##tagPresetFilter", "Tags")) {
            for (const std::string& tag : tagSuggestions) {
                const bool selected = lowerString(trimString(registryTagFilter_.data())) == lowerString(tag);
                if (ImGui::Selectable(tag.c_str(), selected)) {
                    setTextBuffer(registryTagFilter_, tag);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Use an existing asset tag as the filter");
        }
    }
    const bool filtersActive = registryTypeFilter_ != 0 || registryStatusFilter_ != 0 || registryHealthFilter_ != 0 || registryCollectionFilter_ != 0 || registryFavoriteFilter_ != 0 || !selectedRegistryGroupId_.empty() || registryTagFilter_[0] != '\0' || search_[0] != '\0';
    if (filtersActive) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Filters")) {
            registryTypeFilter_ = 0;
            registryStatusFilter_ = 0;
            registryHealthFilter_ = 0;
            registryCollectionFilter_ = 0;
            registryFavoriteFilter_ = 0;
            selectedRegistryGroupId_.clear();
            std::fill(registryTagFilter_.begin(), registryTagFilter_.end(), '\0');
            std::fill(search_.begin(), search_.end(), '\0');
        }
    }
    ImGui::PopID();

    auto typeMatches = [&](AssetType type) {
        switch (registryTypeFilter_) {
        case 1: return type == AssetType::Mesh;
        case 2: return type == AssetType::Material;
        case 3: return type == AssetType::Texture;
        case 4: return type == AssetType::HDRI;
        case 5: return type == AssetType::Scene;
        case 6: return type == AssetType::Prefab;
        case 7: return type == AssetType::Animation;
        case 8: return type == AssetType::Skeleton;
        case 9: return type == AssetType::Unknown;
        default: return true;
        }
    };
    auto statusMatches = [&](AssetImportStatus status) {
        switch (registryStatusFilter_) {
        case 1: return status == AssetImportStatus::Imported;
        case 2: return status == AssetImportStatus::Missing;
        case 3: return status == AssetImportStatus::Stale;
        case 4: return status == AssetImportStatus::Failed;
        case 5: return status == AssetImportStatus::Unknown;
        default: return true;
        }
    };
    auto searchMatches = [&](const AssetRecord& record) {
        const std::string filter = lowerString(search_.data());
        if (filter.empty()) {
            return true;
        }
        return lowerString(record.displayName).find(filter) != std::string::npos ||
            lowerString(record.guid).find(filter) != std::string::npos ||
            lowerString(record.importGroupName).find(filter) != std::string::npos ||
            lowerString(record.sourcePath).find(filter) != std::string::npos ||
            lowerString(record.importedPath).find(filter) != std::string::npos ||
            lowerString(record.cachePath).find(filter) != std::string::npos ||
            lowerString(joinTagList(record.tags)).find(filter) != std::string::npos;
    };
    auto healthMatches = [&](const AssetRecord& record) {
        const AssetUsageSummary usage = registryHealthFilter_ == 9 ? assetUsageSummaryForRecord(state, record) : AssetUsageSummary{};
        switch (registryHealthFilter_) {
        case 1: return !record.missing && !record.stale && record.status == AssetImportStatus::Imported;
        case 2: return record.missing || record.stale || record.sourceMissing || record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing || record.status == AssetImportStatus::Missing || record.status == AssetImportStatus::Stale || record.status == AssetImportStatus::Failed;
        case 3: return record.sourceMissing;
        case 4: return record.importedMetadataMissing;
        case 5: return record.cookedPayloadMissing;
        case 6: return record.dependenciesMissing;
        case 7: return !record.dependencies.empty();
        case 8: return !record.references.empty();
        case 9: return usage.referenced();
        default: return true;
        }
    };
    auto recordMatchesFilters = [&](const AssetRecord& record) {
        const EditorAssetCollection* collection = selectedCollection(state.editorPrefs, registryCollectionFilter_);
        const bool collectionMatches = collection == nullptr || collectionContainsAsset(*collection, record.guid);
        const bool favoriteMatches = registryFavoriteFilter_ == 0 || (state.editorPrefs != nullptr && assetGuidListContains(state.editorPrefs->favoriteAssetGuids, record.guid));
        const bool groupMatches = selectedRegistryGroupId_.empty() || registryGroupIdForRecord(record) == selectedRegistryGroupId_;
        return groupMatches && typeMatches(record.type) && statusMatches(record.status) && healthMatches(record) && collectionMatches && favoriteMatches && recordHasTagMatch(record, registryTagFilter_.data()) && searchMatches(record);
    };

    std::vector<AssetGuid> visibleRecordGuids;
    std::vector<const AssetRecord*> visibleRecords;
    visibleRecordGuids.reserve(records.size());
    visibleRecords.reserve(records.size());
    for (const AssetRecord& record : records) {
        if (recordMatchesFilters(record)) {
            visibleRecordGuids.push_back(record.guid);
            visibleRecords.push_back(&record);
        }
    }
    const size_t visibleRecordCount = visibleRecords.size();
    ImGui::TextDisabled("Showing %zu of %zu registry records", visibleRecordCount, records.size());
    auto savePrefsStatus = [&](std::string successMessage, std::string failureDetail) {
        if (state.editorPrefs == nullptr) {
            return;
        }
        const std::filesystem::path prefsPath = state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath;
        setPreferenceSaveStatus(state.editorPrefs->save(prefsPath), status_, std::move(successMessage), std::move(failureDetail));
    };
    if (ImGui::TreeNodeEx("Registry Bulk Tools")) {
        if (contentActionButton("AssetDependencyGraphReport", EditorGlyphIcon::Details, "Dependency Graph", "Write and open a loaded-registry dependency/reference graph report")) {
            std::filesystem::path reportPath;
            std::string error;
            if (writeAssetDependencyGraphReport(state, browserRoot_, reportPath, error)) {
                requests.openFilePath = reportPath;
                status_ = "Asset dependency graph report: " + reportPath.string()
                    + " (DOT: " + assetDependencyGraphDotPath(state, browserRoot_).string()
                    + "; HTML: " + assetDependencyGraphHtmlPath(state, browserRoot_).string() + ")";
            } else {
                status_ = "Asset dependency graph report failed: " + error;
            }
        }
        ImGui::SameLine();
        if (contentActionButton("AssetProjectReferenceIndexReport", EditorGlyphIcon::Details, "Project References", "Write and open a saved-project asset reference index report")) {
            std::filesystem::path reportPath;
            std::string error;
            if (writeAssetProjectReferenceIndexReport(state, browserRoot_, reportPath, error)) {
                requests.openFilePath = reportPath;
                status_ = "Asset project-reference index: " + reportPath.string();
            } else {
                status_ = "Asset project-reference index failed: " + error;
            }
        }
        ImGui::SameLine();
        if (contentActionButton("AssetDuplicateReport", EditorGlyphIcon::Details, "Duplicate Scan", "Write and open a non-destructive duplicate asset report")) {
            std::filesystem::path reportPath;
            std::string error;
            if (writeAssetDuplicateReport(state, browserRoot_, reportPath, error)) {
                requests.openFilePath = reportPath;
                status_ = "Asset duplicate report: " + reportPath.string();
            } else {
                status_ = "Asset duplicate report failed: " + error;
            }
        }
        drawDependencyGraphPreview(state);
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputTextWithHint("##virtualFolder", "Virtual folder", virtualFolderBuffer_.data(), virtualFolderBuffer_.size());
        const std::string virtualFolder = trimString(virtualFolderBuffer_.data());
        const bool canMoveVisibleToFolder = !virtualFolder.empty() && !visibleRecordGuids.empty();
        ImGui::SameLine();
        if (!canMoveVisibleToFolder) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("MoveVisibleToVirtualFolder", EditorGlyphIcon::Folder, "Move Visible", "Move currently visible registry records into this virtual folder without moving files")) {
            requests.moveAssetsToFolder = EditorMoveAssetsToFolderRequest{visibleRecordGuids, virtualFolder};
            selectedRegistryGroupId_ = std::string("folder:") + lowerString(virtualFolder);
            status_ = "Queued virtual folder move: " + virtualFolder;
        }
        if (!canMoveVisibleToFolder) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (visibleRecordGuids.empty()) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("ClearVisibleVirtualFolder", EditorGlyphIcon::Trash, "Clear Folder", "Clear custom virtual-folder metadata for currently visible registry records")) {
            requests.moveAssetsToFolder = EditorMoveAssetsToFolderRequest{visibleRecordGuids, {}};
            selectedRegistryGroupId_.clear();
            status_ = "Queued virtual folder metadata clear";
        }
        if (visibleRecordGuids.empty()) {
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("Virtual folder moves update registry metadata only; generated files and GUID references are preserved.");
        ImGui::Separator();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputTextWithHint("##collectionName", "Collection", collectionNameBuffer_.data(), collectionNameBuffer_.size());
        if (state.editorPrefs != nullptr && !state.editorPrefs->assetCollections.empty()) {
            ImGui::SameLine();
            if (ImGui::BeginCombo("##collectionPicker", "Collections")) {
                for (const EditorAssetCollection& collection : state.editorPrefs->assetCollections) {
                    if (ImGui::Selectable(collection.name.c_str(), false)) {
                        setTextBuffer(collectionNameBuffer_, collection.name);
                    }
                }
                ImGui::EndCombo();
            }
        }
        const std::string collectionName = trimString(collectionNameBuffer_.data());
        const bool canEditCollection = state.editorPrefs != nullptr && !collectionName.empty();
        ImGui::SameLine();
        if (!canEditCollection || visibleRecordGuids.empty()) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("AddVisibleToCollection", EditorGlyphIcon::Add, "Add Visible", "Add currently visible registry records to this collection")) {
            state.editorPrefs->addAssetsToCollection(collectionName, visibleRecordGuids);
            savePrefsStatus("Added visible assets to collection: " + collectionName, "add visible assets to collection " + collectionName);
        }
        if (!canEditCollection || visibleRecordGuids.empty()) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!canEditCollection || visibleRecordGuids.empty()) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("RemoveVisibleFromCollection", EditorGlyphIcon::Trash, "Remove Visible", "Remove currently visible registry records from this collection")) {
            state.editorPrefs->removeAssetsFromCollection(collectionName, visibleRecordGuids);
            savePrefsStatus("Removed visible assets from collection: " + collectionName, "remove visible assets from collection " + collectionName);
        }
        if (!canEditCollection || visibleRecordGuids.empty()) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!canEditCollection) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("DeleteCollection", EditorGlyphIcon::Trash, "Delete Collection", "Delete this saved asset collection")) {
            const bool deletingActiveCollection = [&] {
                const EditorAssetCollection* selected = selectedCollection(state.editorPrefs, registryCollectionFilter_);
                return selected != nullptr && selected->name == collectionName;
            }();
            state.editorPrefs->removeAssetCollection(collectionName);
            if (deletingActiveCollection) {
                registryCollectionFilter_ = 0;
            }
            savePrefsStatus("Deleted collection: " + collectionName, "delete collection " + collectionName);
        }
        if (!canEditCollection) {
            ImGui::EndDisabled();
        }

        ImGui::SetNextItemWidth(132.0f);
        ImGui::InputTextWithHint("##bulkTag", "Bulk add tag", bulkTagBuffer_.data(), bulkTagBuffer_.size());
        if (!tagSuggestions.empty()) {
            ImGui::SameLine();
            if (ImGui::BeginCombo("##bulkTagPreset", "Tags")) {
                for (const std::string& tag : tagSuggestions) {
                    if (ImGui::Selectable(tag.c_str(), false)) {
                        setTextBuffer(bulkTagBuffer_, tag);
                    }
                }
                ImGui::EndCombo();
            }
        }
        const std::string bulkTag = trimString(bulkTagBuffer_.data());
        const bool bulkTagSaved = state.editorPrefs != nullptr && tagListContains(state.editorPrefs->assetTagPresets, bulkTag);
        ImGui::SameLine();
        if (state.editorPrefs == nullptr || bulkTag.empty() || bulkTagSaved) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("SaveBulkTagPreset", EditorGlyphIcon::Add, "Save Preset", "Save this tag as an editor preset")) {
            state.editorPrefs->addAssetTagPreset(bulkTag);
            savePrefsStatus("Saved tag preset: " + bulkTag, "save tag preset " + bulkTag);
        }
        if (state.editorPrefs == nullptr || bulkTag.empty() || bulkTagSaved) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (state.editorPrefs == nullptr || bulkTag.empty() || !bulkTagSaved) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("RemoveBulkTagPreset", EditorGlyphIcon::Trash, "Remove Preset", "Remove this tag from editor presets")) {
            state.editorPrefs->removeAssetTagPreset(bulkTag);
            savePrefsStatus("Removed tag preset: " + bulkTag, "remove tag preset " + bulkTag);
        }
        if (state.editorPrefs == nullptr || bulkTag.empty() || !bulkTagSaved) {
            ImGui::EndDisabled();
        }
        const bool canBulkTag = !bulkTag.empty() && !visibleRecordGuids.empty();
        ImGui::SameLine();
        if (!canBulkTag) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("BulkAddTagVisible", EditorGlyphIcon::Add, "Tag Visible", "Add this tag to all currently visible registry records")) {
            requests.bulkAddAssetTag = EditorBulkAssetTagRequest{visibleRecordGuids, bulkTag};
            status_ = "Queued bulk tag: " + bulkTag;
        }
        ImGui::SameLine();
        if (contentActionButton("BulkRemoveTagVisible", EditorGlyphIcon::Trash, "Untag Visible", "Remove this tag from all currently visible registry records")) {
            requests.bulkRemoveAssetTag = EditorBulkAssetTagRequest{visibleRecordGuids, bulkTag};
            status_ = "Queued bulk untag: " + bulkTag;
        }
        if (!canBulkTag) {
            ImGui::EndDisabled();
        }
        const bool canDeleteVisible = !visibleRecordGuids.empty();
        ImGui::Separator();
        if (!canDeleteVisible) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("DeleteVisibleRegistryRecords", EditorGlyphIcon::Trash, "Remove Visible", "Remove currently visible records from the asset registry")) {
            requests.deleteAssets = EditorDeleteAssetRequest{visibleRecordGuids, false};
            if (std::find(visibleRecordGuids.begin(), visibleRecordGuids.end(), selectedRecordGuid_) != visibleRecordGuids.end()) {
                selectedRecordGuid_.clear();
            }
            status_ = "Queued visible registry record removal";
        }
        ImGui::SameLine();
        if (contentActionButton("DeleteVisibleGeneratedFiles", EditorGlyphIcon::Trash, "Delete Visible Files", "Remove visible records and delete their generated metadata/cache files inside the project")) {
            requests.deleteAssets = EditorDeleteAssetRequest{visibleRecordGuids, true};
            if (std::find(visibleRecordGuids.begin(), visibleRecordGuids.end(), selectedRecordGuid_) != visibleRecordGuids.end()) {
                selectedRecordGuid_.clear();
            }
            status_ = "Queued visible registry file delete";
        }
        if (!canDeleteVisible) {
            ImGui::EndDisabled();
        }
        ImGui::TreePop();
    }
    const std::filesystem::path sourceControlRoot = state.project != nullptr ? state.project->projectRoot : browserRoot_;
    auto cachedSourceControlStatus = [&](const std::filesystem::path& path) {
        if (path.empty()) {
            return std::string("Unavailable");
        }
        const std::string key = canonicalForCompare(path).string();
        auto it = sourceControlStatusCache_.find(key);
        if (it != sourceControlStatusCache_.end()) {
            return it->second;
        }
        const std::string status = gitStatusLabelForPath(sourceControlRoot, path);
        sourceControlStatusCache_[key] = status;
        return status;
    };
    auto summarizeRecordSourceControl = [&](const AssetRecord& record) {
        return summarizeAssetSourceControlState(state, record, cachedSourceControlStatus);
    };
    auto overwriteRisksForRecord = [&](const AssetRecord& record) {
        return collectAssetOverwriteRisks(state, record, cachedSourceControlStatus);
    };
    const ImVec2 registryTableSize(0.0f, std::max(160.0f, ImGui::GetContentRegionAvail().y - 8.0f));
    if (ImGui::BeginTable("AssetRegistryRecords", 13, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, registryTableSize)) {
        ImGui::TableSetupColumn("Fav", ImGuiTableColumnFlags_WidthFixed, 38.0f);
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("GUID");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Imported");
        ImGui::TableSetupColumn("Git", ImGuiTableColumnFlags_WidthFixed, 118.0f);
        ImGui::TableSetupColumn("Tags");
        ImGui::TableSetupColumn("Deps");
        ImGui::TableSetupColumn("Refs");
        ImGui::TableSetupColumn("Missing/Stale");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::progressColumnWidth);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visibleRecords.size()), EditorUiMetric::contentRowHeight);
        while (clipper.Step()) {
        for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex) {
            const AssetRecord& record = *visibleRecords[static_cast<size_t>(rowIndex)];
            ImGui::PushID(record.guid.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool favoriteAsset = state.editorPrefs != nullptr && assetGuidListContains(state.editorPrefs->favoriteAssetGuids, record.guid);
            ImGui::TextUnformatted(favoriteAsset ? "*" : "");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && favoriteAsset) {
                ImGui::SetTooltip("Favorite asset");
            }
            ImGui::TableSetColumnIndex(1);
            const ImVec2 typeCursor = ImGui::GetCursorScreenPos();
            const float typeIconY = typeCursor.y + std::max(0.0f, (EditorUiMetric::contentRowHeight - 16.0f) * 0.5f);
            drawAssetTypeGlyph(record.type, ImVec2(typeCursor.x, typeIconY), ImVec2(typeCursor.x + 16.0f, typeIconY + 16.0f));
            ImGui::Dummy(ImVec2(20.0f, EditorUiMetric::contentRowHeight));
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextUnformatted(assetTypeName(record.type));
            ImGui::TableSetColumnIndex(2);
            const char* name = record.displayName.empty() ? "(unnamed)" : record.displayName.c_str();
            editorPushRowSelectionStyle();
            if (ImGui::Selectable(name, selectedRecordGuid_ == record.guid, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, EditorUiMetric::contentRowHeight))) {
                selectedRecordGuid_ = record.guid;
                selectedPath_.clear();
            }
            editorPopRowSelectionStyle();
            if ((record.type == AssetType::Prefab || record.type == AssetType::Mesh || record.type == AssetType::Material || record.type == AssetType::HDRI || record.type == AssetType::Scene) &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                if (record.type == AssetType::Scene) {
                    const std::filesystem::path levelPath = firstResolvedExistingRecordPath(state, {record.sourcePath, record.importedPath, record.cachePath});
                    if (!levelPath.empty()) {
                        setPathDragDropPayload("LEVEL_PATH", levelPath, "Level");
                    } else {
                        ImGui::TextDisabled("Level path unavailable");
                    }
                } else {
                    std::array<char, 128> guidPayload{};
                    record.guid.copy(guidPayload.data(), std::min(record.guid.size(), guidPayload.size() - 1));
                    if (record.type == AssetType::Prefab) {
                        if (assetPlacementBlocked(record)) {
                            ImGui::TextDisabled("Placement blocked: %s", assetPlacementBlockReason(record));
                        } else {
                            ImGui::SetDragDropPayload("PREFAB_ASSET", guidPayload.data(), guidPayload.size());
                            ImGui::Text("%s %s", assetTypeName(record.type), name);
                        }
                    } else if (record.type == AssetType::Mesh) {
                        if (assetPlacementBlocked(record)) {
                            ImGui::TextDisabled("Placement blocked: %s", assetPlacementBlockReason(record));
                        } else {
                            ImGui::SetDragDropPayload("MESH_ASSET", guidPayload.data(), guidPayload.size());
                            ImGui::Text("%s %s", assetTypeName(record.type), name);
                        }
                    } else if (record.type == AssetType::Material) {
                        ImGui::SetDragDropPayload("MATERIAL_ASSET", guidPayload.data(), guidPayload.size());
                        ImGui::Text("%s %s", assetTypeName(record.type), name);
                    } else {
                        ImGui::SetDragDropPayload("ENVIRONMENT_ASSET", guidPayload.data(), guidPayload.size());
                        ImGui::Text("%s %s", assetTypeName(record.type), name);
                    }
                }
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (state.editorPrefs != nullptr) {
                    if (favoriteAsset) {
                        if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove Asset Favorite")) {
                            state.editorPrefs->removeFavoriteAsset(record.guid);
                            savePrefsStatus(
                                "Removed asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                                "remove asset favorite " + record.guid);
                        }
                    } else if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Add Asset Favorite")) {
                        state.editorPrefs->addFavoriteAsset(record.guid);
                        savePrefsStatus(
                            "Added asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                            "add asset favorite " + record.guid);
                    }
                    ImGui::Separator();
                }
                const bool canPlacePrefab = record.type == AssetType::Prefab && !assetPlacementBlocked(record);
                if (record.type == AssetType::Prefab && editorGlyphMenuItem(EditorGlyphIcon::Add, "Place Prefab", canPlacePrefab)) {
                    requests.placeAsset = record.guid;
                }
                if (record.type == AssetType::Prefab && !canPlacePrefab) {
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", assetPlacementBlockReason(record));
                    }
                }
                const std::filesystem::path resolvedSourcePath = resolveAssetRecordPath(state, record.sourcePath);
                const bool canReimport = !record.sourcePath.empty() && std::filesystem::exists(resolvedSourcePath);
                const std::vector<AssetOverwriteRisk> overwriteRisks = overwriteRisksForRecord(record);
                if (!overwriteRisks.empty()) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Reimport overwrite warning");
                    for (const AssetOverwriteRisk& risk : overwriteRisks) {
                        ImGui::BulletText("%s: %s", risk.label.c_str(), risk.status.c_str());
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", risk.path.string().c_str());
                        }
                    }
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Reimport", canReimport)) {
                    requests.reimportAsset = record.guid;
                    recordImportOperation("Reimport Asset", resolvedSourcePath, {}, "Reimport", record.guid);
                    status_ = overwriteRisks.empty()
                        ? "Queued reimport: " + record.displayName
                        : "Queued reimport after overwrite warning: " + record.displayName;
                }
                ImGui::Separator();
                if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove From Registry")) {
                    requests.deleteAssets = EditorDeleteAssetRequest{{record.guid}, false};
                    if (selectedRecordGuid_ == record.guid) {
                        selectedRecordGuid_.clear();
                    }
                    status_ = "Queued registry record removal: " + record.displayName;
                }
                if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete Generated Files")) {
                    requests.deleteAssets = EditorDeleteAssetRequest{{record.guid}, true};
                    if (selectedRecordGuid_ == record.guid) {
                        selectedRecordGuid_.clear();
                    }
                    status_ = "Queued generated asset file delete: " + record.displayName;
                }
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(record.guid.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(record.sourcePath.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(record.importedPath.c_str());
            ImGui::TableSetColumnIndex(6);
            const AssetSourceControlSummary scmSummary = summarizeRecordSourceControl(record);
            ImGui::TextColored(sourceControlStatusTextColor(scmSummary.primaryStatus), "%s", scmSummary.label.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", scmSummary.tooltip.c_str());
            }
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(joinTagList(record.tags).c_str());
            ImGui::TableSetColumnIndex(8);
            ImGui::Text("%zu", record.dependencies.size());
            ImGui::TableSetColumnIndex(9);
            ImGui::Text("%zu", record.references.size());
            ImGui::TableSetColumnIndex(10);
            ImGui::Text("%s%s", record.missing ? "missing" : "ok", record.stale ? " / stale" : "");
            ImGui::TableSetColumnIndex(11);
            ImGui::TextUnformatted(assetImportStatusName(record.status));
            ImGui::TableSetColumnIndex(12);
            ImGui::ProgressBar(assetImportProgress(record), ImVec2(-FLT_MIN, 0.0f), assetImportProgressLabel(record));
            ImGui::PopID();
        }
        }
        ImGui::EndTable();
    }
}

void AssetBrowserPanel::drawDetails(const EditorRuntimeState& state, EditorRequests& requests) {
    ImGui::SeparatorText("Details");
    const std::filesystem::path sourceControlRoot = state.project != nullptr ? state.project->projectRoot : browserRoot_;
    auto savePrefsStatus = [&](std::string successMessage, std::string failureDetail) {
        if (state.editorPrefs == nullptr) {
            return;
        }
        const std::filesystem::path prefsPath = state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath;
        setPreferenceSaveStatus(state.editorPrefs->save(prefsPath), status_, std::move(successMessage), std::move(failureDetail));
    };
    auto sourceControlStatus = [&](const std::filesystem::path& path) {
        if (path.empty()) {
            return std::string("Unavailable");
        }
        const std::string key = canonicalForCompare(path).string();
        auto it = sourceControlStatusCache_.find(key);
        if (it != sourceControlStatusCache_.end()) {
            return it->second;
        }
        const std::string status = gitStatusLabelForPath(sourceControlRoot, path);
        sourceControlStatusCache_[key] = status;
        return status;
    };
    auto drawSourceControlStatus = [&](const char* label, const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        const std::string status = sourceControlStatus(path);
        ImGui::TextColored(sourceControlStatusTextColor(status), "%s Source Control: %s", label, status.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", path.string().c_str());
        }
    };
    auto drawSourceControlActions = [&](const char* id, const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        const std::string status = sourceControlStatus(path);
        const bool canOpenExternal = regularFileExists(path);
        const bool canStatusReport = sourceControlStatusReportAvailable(status);
        const bool canDiff = sourceControlDiffReportAvailable(status);
        ImGui::PushID(id);
        if (!canOpenExternal) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("OpenExternal", EditorGlyphIcon::File, "Open External", "Open this file with the OS-associated external tool")) {
            requests.openFilePath = path;
            status_ = "Opening file: " + path.string();
        }
        if (!canOpenExternal) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!canStatusReport) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("GitStatus", EditorGlyphIcon::Details, "Git Status", "Write and open a read-only Git status report for this path")) {
            std::filesystem::path reportPath;
            std::string error;
            if (writeSourceControlStatusReport(state, browserRoot_, sourceControlRoot, path, status, reportPath, error)) {
                requests.openFilePath = reportPath;
                status_ = "Source-control status report: " + reportPath.string();
            } else {
                status_ = "Source-control status report failed: " + error;
            }
        }
        if (!canStatusReport) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!canDiff) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("GitDiff", EditorGlyphIcon::Details, "Git Diff", "Write and open a Git diff report for this path")) {
            std::filesystem::path reportPath;
            std::string error;
            if (writeSourceControlDiffReport(state, browserRoot_, sourceControlRoot, path, reportPath, error)) {
                requests.openFilePath = reportPath;
                status_ = "Source-control diff report: " + reportPath.string();
            } else {
                status_ = "Source-control diff failed: " + error;
            }
        }
        if (!canDiff) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!canStatusReport) {
            ImGui::BeginDisabled();
        }
        if (contentActionButton("ProviderReadiness", EditorGlyphIcon::Details, "Provider Readiness", "Write a Git/Perforce provider readiness and asset lock/ownership report for this path")) {
            std::filesystem::path reportPath;
            std::string error;
            if (writeSourceControlProviderReadinessReport(state, browserRoot_, sourceControlRoot, path, status, reportPath, error)) {
                requests.openFilePath = reportPath;
                status_ = "Source-control provider readiness report: " + reportPath.string();
            } else {
                status_ = "Source-control provider readiness failed: " + error;
            }
        }
        if (!canStatusReport) {
            ImGui::EndDisabled();
        }
        auto drawActionPlanButton = [&](const char* buttonId, const char* label, const char* action, const char* tooltip) {
            ImGui::SameLine();
            if (!canStatusReport) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton(buttonId, EditorGlyphIcon::Details, label, tooltip)) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeSourceControlActionPlanReport(state, browserRoot_, sourceControlRoot, path, action, status, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = std::string("Source-control ") + action + " plan: " + reportPath.string();
                } else {
                    status_ = std::string("Source-control ") + action + " plan failed: " + error;
                }
            }
            if (!canStatusReport) {
                ImGui::EndDisabled();
            }
        };
        drawActionPlanButton("PlanRevert", "Plan Revert", "revert", "Write a dry-run Git revert/clean action plan for this path; no mutation is executed");
        drawActionPlanButton("PlanCheckout", "Plan Checkout", "checkout", "Write a dry-run Git checkout action plan for this path; no mutation is executed");
        drawActionPlanButton("PlanLock", "Plan Lock", "lock", "Write a provider lock action plan and current Git/Perforce limitation report");
        drawActionPlanButton("PlanSubmit", "Plan Submit", "submit", "Write a dry-run Git stage/commit/push action plan for this path; no mutation is executed");
        ImGui::PopID();
    };
    if (ImGui::SmallButton("Refresh Source Control")) {
        sourceControlStatusCache_.clear();
        clearGitStatusSnapshots();
        status_ = "Source control status refreshed";
    }
    if (!selectedPath_.empty()) {
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewWidth = std::min(ImGui::GetContentRegionAvail().x, EditorUiMetric::assetPreviewMaxWidth);
        const ImVec2 previewSize(previewWidth, EditorUiMetric::assetPreviewHeight);
        ImGui::InvisibleButton("AssetPreview", previewSize);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(20, 23, 27, 255), 4.0f);
        drawList->AddRect(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(55, 62, 72, 255), 4.0f);
        const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
        const bool hasGpuPreview = drawGpuSceneTextureThumbnail(state, selectedPath_, previewPos, previewMax);
        const bool hasStandaloneGpuPreview = hasGpuPreview ? false : drawStandaloneGpuAssetPreview(state, selectedPath_, previewPos, previewMax, false);
        const bool hasRasterPreview = (hasGpuPreview || hasStandaloneGpuPreview) ? false : drawRasterThumbnail(selectedPath_, previewPos, previewMax, false);
        if (!hasGpuPreview && !hasStandaloneGpuPreview && !hasRasterPreview) {
            const bool hasGeneratedPreview = drawGeneratedSourcePreview(selectedPath_, previewPos, previewMax);
            if (!hasGeneratedPreview) {
                const ImVec2 previewIconSize(34.0f, 34.0f);
                drawContentGlyph(
                    selectedPath_,
                    ImVec2(previewPos.x + previewSize.x * 0.5f - previewIconSize.x * 0.5f, previewPos.y + 28.0f),
                    ImVec2(previewPos.x + previewSize.x * 0.5f + previewIconSize.x * 0.5f, previewPos.y + 28.0f + previewIconSize.y));
                const std::string previewKind = contentKindLabel(selectedPath_);
                const ImVec2 kindSize = ImGui::CalcTextSize(previewKind.c_str());
                drawList->AddText(
                    ImVec2(previewPos.x + previewSize.x * 0.5f - kindSize.x * 0.5f, previewPos.y + 64.0f),
                    IM_COL32(130, 137, 148, 255),
                    previewKind.c_str());
            }
        }
        ImGui::Text("Selected: %s", selectedPath_.filename().string().c_str());
        ImGui::Text("Kind: %s", contentKindLabel(selectedPath_).c_str());
        ImGui::TextWrapped("Path: %s", relativeContentPath(selectedPath_).c_str());
        ImGui::SeparatorText("Selected Asset State");
        ImGui::Text("Origin: %s", selectedPathOriginLabel(state, selectedPath_));
        drawSourceControlStatus("Selected", selectedPath_);
        drawSourceControlActions("SelectedPathSourceControlActions", selectedPath_);
        if (state.assetRegistry != nullptr && state.assetRegistry->dirty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Registry Metadata: Unsaved changes");
        } else if (state.assetRegistry != nullptr) {
            ImGui::TextColored(ImVec4(0.54f, 0.82f, 0.60f, 1.0f), "Registry Metadata: Saved");
        } else {
            ImGui::TextDisabled("Registry Metadata: unavailable");
        }
        ImGui::TextDisabled("Imported asset state is exposed through registry records.");
        const bool isDirectory = std::filesystem::is_directory(selectedPath_);
        if (isDirectory) {
            if (contentActionButton("OpenFolder", EditorGlyphIcon::Folder, "Open Folder", "Open this folder in Content")) {
                navigateTo(selectedPath_);
            }
            ImGui::SameLine();
            if (compatibilityMode_) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ImportHere", EditorGlyphIcon::Import, "Import Here", "Import a model asset into this folder")) {
                if (auto source = openGltfFileDialog()) {
                    prepareImportDialog(*source, selectedPath_, 0);
                }
            }
            if (compatibilityMode_) {
                ImGui::EndDisabled();
            }
        } else {
            const bool canOpen = canOpenOrApplyPath(selectedPath_);
            if (!canOpen) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("OpenApply", EditorGlyphIcon::File, "Open / Apply", "Open the selected asset or apply it to the scene")) {
                loadFromPath(selectedPath_, requests);
            }
            if (!canOpen) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            const bool canImport = !compatibilityMode_ && isImportableSourceAssetPath(selectedPath_);
            const bool canImportAndPlace = !compatibilityMode_ && isPlaceablePrefabSourcePath(selectedPath_);
            if (!canImport) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ImportAsset", EditorGlyphIcon::Import, "Import Asset", "Import this source asset into the project asset registry")) {
                prepareImportDialog(selectedPath_, currentPath_, 0);
            }
            if (!canImport) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (!canImportAndPlace) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("PlaceAsset", EditorGlyphIcon::Add, "Place", "Import and place this model in the current scene")) {
                prepareImportDialog(selectedPath_, currentPath_, 1);
            }
            if (!canImportAndPlace) {
                ImGui::EndDisabled();
            }
        }
        if (contentActionButton("CopyPath", EditorGlyphIcon::Command, "Copy Path", "Copy the selected asset path to the clipboard")) {
            copyPathToClipboard(selectedPath_);
            status_ = "Copied path: " + selectedPath_.string();
        }
        ImGui::SameLine();
        if (contentActionButton("ShowInExplorer", EditorGlyphIcon::Folder, "Show in Explorer", "Reveal the selected asset in Explorer")) {
            revealPathInFileBrowser(selectedPath_);
        }
        if (!supportedContentPath(selectedPath_)) {
            ImGui::TextDisabled("No supported files selected");
        }
    } else if (selectedRecordGuid_.empty()) {
        ImGui::TextDisabled("No supported files selected");
    }
    const bool hasDetailsSelection = !selectedPath_.empty() || !selectedRecordGuid_.empty();
    if (!selectedRecordGuid_.empty() && state.assetRegistry != nullptr) {
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.guid != selectedRecordGuid_) {
                continue;
            }
            const std::filesystem::path recordPreviewSource = recordPreviewPath(state, record);
            const ImVec2 previewPos = ImGui::GetCursorScreenPos();
            const float previewWidth = std::min(ImGui::GetContentRegionAvail().x, EditorUiMetric::assetPreviewMaxWidth);
            const ImVec2 previewSize(previewWidth, EditorUiMetric::assetPreviewHeight);
            ImGui::InvisibleButton("AssetRecordPreview", previewSize);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(20, 23, 27, 255), 4.0f);
            drawList->AddRect(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(55, 62, 72, 255), 4.0f);
            const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
            const bool previewDrawn = !recordPreviewSource.empty() &&
                (drawGpuSceneTextureThumbnail(state, recordPreviewSource, previewPos, previewMax) ||
                    drawStandaloneGpuAssetPreview(state, recordPreviewSource, previewPos, previewMax, false) ||
                    drawRasterThumbnail(recordPreviewSource, previewPos, previewMax, false) ||
                    drawGeneratedSourcePreview(recordPreviewSource, previewPos, previewMax));
            if (!previewDrawn) {
                const ImVec2 previewIconSize(34.0f, 34.0f);
                drawAssetTypeGlyph(
                    record.type,
                    ImVec2(previewPos.x + previewSize.x * 0.5f - previewIconSize.x * 0.5f, previewPos.y + 28.0f),
                    ImVec2(previewPos.x + previewSize.x * 0.5f + previewIconSize.x * 0.5f, previewPos.y + 28.0f + previewIconSize.y));
                const std::string previewKind = std::string(assetTypeName(record.type)) + " preview unavailable";
                const ImVec2 kindSize = ImGui::CalcTextSize(previewKind.c_str());
                drawList->AddText(
                    ImVec2(previewPos.x + previewSize.x * 0.5f - kindSize.x * 0.5f, previewPos.y + 64.0f),
                    IM_COL32(130, 137, 148, 255),
                    previewKind.c_str());
            }
            ImGui::Text("Asset: %s", record.displayName.empty() ? "(unnamed)" : record.displayName.c_str());
            ImGui::Text("GUID: %s", record.guid.c_str());
            ImGui::Text("Type: %s", assetTypeName(record.type));
            ImGui::SeparatorText("Selected Asset State");
            ImGui::TextColored(selectedAssetStateColor(record), "Asset State: %s", selectedAssetStateLabel(record));
            if (state.assetRegistry->dirty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Registry Metadata: Unsaved changes");
            } else {
                ImGui::TextColored(ImVec4(0.54f, 0.82f, 0.60f, 1.0f), "Registry Metadata: Saved");
            }
            const AssetSourceControlSummary selectedSourceControlSummary = summarizeAssetSourceControlState(state, record, sourceControlStatus);
            ImGui::TextColored(
                sourceControlStatusTextColor(selectedSourceControlSummary.primaryStatus),
                "Source Control Risk: %s",
                selectedSourceControlSummary.label.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", selectedSourceControlSummary.tooltip.c_str());
            }
            if (selectedSourceControlSummary.hasGeneratedOverwriteRisk) {
                ImGui::TextWrapped("Generated asset files have external Git changes. Review the overwrite-risk report before reimporting, rebuilding, or deleting generated files.");
            }
            if (state.editorPrefs != nullptr) {
                const bool favoriteAsset = assetGuidListContains(state.editorPrefs->favoriteAssetGuids, record.guid);
                if (favoriteAsset) {
                    if (contentActionButton("RemoveAssetFavorite", EditorGlyphIcon::Trash, "Remove Favorite", "Remove this asset GUID from editor favorites")) {
                        state.editorPrefs->removeFavoriteAsset(record.guid);
                        savePrefsStatus(
                            "Removed asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                            "remove asset favorite " + record.guid);
                    }
                } else if (contentActionButton("AddAssetFavorite", EditorGlyphIcon::Add, "Add Favorite", "Save this asset GUID as an editor favorite")) {
                    state.editorPrefs->addFavoriteAsset(record.guid);
                    savePrefsStatus(
                        "Added asset favorite: " + (record.displayName.empty() ? record.guid : record.displayName),
                        "add asset favorite " + record.guid);
                }
            }
            if (assetRenameBufferGuid_ != record.guid) {
                setTextBuffer(assetRenameBuffer_, record.displayName.empty() ? record.guid : record.displayName);
                assetRenameBufferGuid_ = record.guid;
            }
            ImGui::SeparatorText("Asset Name");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##assetDisplayName", "Display name", assetRenameBuffer_.data(), assetRenameBuffer_.size());
            const std::string editedAssetName = trimString(assetRenameBuffer_.data());
            const std::string currentAssetName = trimString(record.displayName.empty() ? record.guid : record.displayName);
            const bool canRenameAsset = !editedAssetName.empty() && editedAssetName != currentAssetName;
            if (!canRenameAsset) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("RenameAssetRecord", EditorGlyphIcon::Refresh, "Rename Asset", "Update this asset's registry display name without changing its GUID or generated file paths")) {
                requests.renameAsset = EditorRenameAssetRequest{record.guid, editedAssetName};
                status_ = "Queued asset rename: " + currentAssetName + " -> " + editedAssetName;
            }
            if (!canRenameAsset) {
                ImGui::EndDisabled();
            }
            if (editedAssetName.empty()) {
                ImGui::TextDisabled("Asset display name cannot be empty.");
            } else {
                ImGui::TextDisabled("Rename updates registry metadata only; GUIDs, references, and generated file paths are preserved.");
            }
            if (record.stale || record.status == AssetImportStatus::Stale) {
                ImGui::TextWrapped("The selected asset has stale import metadata; reimport updates its cooked payload and registry record.");
            } else if (record.missing || record.status == AssetImportStatus::Missing) {
                ImGui::TextWrapped("The selected asset has broken registry references. Reimport or repair missing metadata, cooked payload, or dependency records before placing it.");
            } else if (record.status == AssetImportStatus::Failed) {
                ImGui::TextWrapped("The selected asset import failed; reimport or inspect the source path before placing it.");
            } else if (record.sourceMissing) {
                ImGui::TextWrapped("The raw source path is missing, but imported metadata and cooked payload references are still available.");
            }
            if (record.sourceMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Source file missing");
            }
            if (record.importedMetadataMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "Imported metadata missing");
            }
            if (record.cookedPayloadMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "Cooked payload missing");
            }
            if (record.dependenciesMissing) {
                ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "Dependency record missing");
            }
            const bool brokenPlaceholderRequired = record.missing || record.status == AssetImportStatus::Missing || record.sourceMissing || record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing;
            if (brokenPlaceholderRequired) {
                ImGui::TextWrapped("Broken placeholder state: placement and packaging should treat this record as broken until missing metadata, payload, source, or dependency records are repaired.");
            } else {
                ImGui::TextDisabled("Broken placeholder state: not required for the loaded registry health state.");
            }
            if (contentActionButton("BrokenPlaceholderReport", EditorGlyphIcon::Details, "Broken Placeholder", "Write and open a broken-asset placeholder readiness report")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetBrokenPlaceholderReport(state, browserRoot_, record, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset broken-placeholder report: " + reportPath.string();
                } else {
                    status_ = "Asset broken-placeholder report failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("PackageInspectionReport", EditorGlyphIcon::Details, "Inspect Package", "Write and open a transparent metadata/cache inspection report for this asset")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetPackageInspectionReport(state, browserRoot_, record, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset package inspection report: " + reportPath.string();
                } else {
                    status_ = "Asset package inspection report failed: " + error;
                }
            }
            ImGui::TextWrapped("Thumbnail: %s", record.thumbnailPath.empty() ? "(fallback icon)" : record.thumbnailPath.c_str());
            const std::filesystem::path resolvedThumbnailPath = resolveAssetRecordPath(state, record.thumbnailPath);
            if (!record.thumbnailPath.empty() && !regularFileExists(resolvedThumbnailPath)) {
                ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.28f, 1.0f), "Thumbnail state: missing; using fallback preview");
            } else if (!record.thumbnailPath.empty()) {
                ImGui::TextColored(ImVec4(0.54f, 0.82f, 0.60f, 1.0f), "Thumbnail state: available");
            } else {
                ImGui::TextDisabled("Thumbnail state: no generated thumbnail metadata yet");
            }
            if (contentActionButton("ThumbnailReadinessReport", EditorGlyphIcon::Details, "Thumbnail Readiness", "Write and open thumbnail fallback/progress readiness for this asset")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetThumbnailReadinessReport(state, browserRoot_, record, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset thumbnail readiness report: " + reportPath.string();
                } else {
                    status_ = "Asset thumbnail readiness failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("ImporterReadinessReport", EditorGlyphIcon::Details, "Importer Readiness", "Write and open source-format import pipeline readiness for this asset")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetImporterReadinessReport(state, browserRoot_, record, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset importer readiness report: " + reportPath.string();
                } else {
                    status_ = "Asset importer readiness failed: " + error;
                }
            }
            ImGui::TextWrapped("Source: %s", record.sourcePath.c_str());
            drawSourceControlStatus("Source", resolveAssetRecordPath(state, record.sourcePath));
            drawSourceControlActions("RecordSourceSourceControlActions", resolveAssetRecordPath(state, record.sourcePath));
            ImGui::TextWrapped("Imported: %s", record.importedPath.c_str());
            drawSourceControlStatus("Metadata", resolveAssetRecordPath(state, record.importedPath));
            drawSourceControlActions("RecordMetadataSourceControlActions", resolveAssetRecordPath(state, record.importedPath));
            ImGui::TextWrapped("Cache: %s", record.cachePath.c_str());
            drawSourceControlStatus("Payload", resolveAssetRecordPath(state, record.cachePath));
            drawSourceControlActions("RecordPayloadSourceControlActions", resolveAssetRecordPath(state, record.cachePath));
            if (assetTagsBufferGuid_ != record.guid) {
                setTextBuffer(assetTagsBuffer_, joinTagList(record.tags));
                assetTagsBufferGuid_ = record.guid;
            }
            ImGui::SeparatorText("Tags");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##assetTags", "comma-separated tags", assetTagsBuffer_.data(), assetTagsBuffer_.size());
            const std::vector<std::string> editedTags = parseTagList(assetTagsBuffer_.data());
            const bool tagsChanged = editedTags != parseTagList(joinTagList(record.tags));
            if (!tagsChanged) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ApplyTags", EditorGlyphIcon::Refresh, "Apply Tags", "Update this asset's registry tags")) {
                requests.updateAssetTags = EditorAssetTagsRequest{record.guid, editedTags};
                status_ = "Queued asset tag update: " + record.displayName;
            }
            if (!tagsChanged) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (record.tags.empty()) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ClearTags", EditorGlyphIcon::Trash, "Clear Tags", "Remove all tags from this asset")) {
                std::fill(assetTagsBuffer_.begin(), assetTagsBuffer_.end(), '\0');
                requests.updateAssetTags = EditorAssetTagsRequest{record.guid, {}};
                status_ = "Queued asset tag clear: " + record.displayName;
            }
            if (record.tags.empty()) {
                ImGui::EndDisabled();
            }
            const std::vector<std::string> registryTags = mergedTagSuggestions(collectRegistryTags(state.assetRegistry), state.editorPrefs);
            bool hasSuggestedTags = false;
            for (const std::string& tag : registryTags) {
                if (!tagListContains(record.tags, tag)) {
                    hasSuggestedTags = true;
                    break;
                }
            }
            if (hasSuggestedTags) {
                ImGui::TextDisabled("Existing tags");
                for (const std::string& tag : registryTags) {
                    if (tagListContains(record.tags, tag)) {
                        continue;
                    }
                    ImGui::PushID(tag.c_str());
                    if (contentActionButton("AddExistingTag", EditorGlyphIcon::Add, tag.c_str(), "Add this existing registry tag to the selected asset")) {
                        std::vector<std::string> updatedTags = record.tags;
                        updatedTags.push_back(tag);
                        requests.updateAssetTags = EditorAssetTagsRequest{record.guid, updatedTags};
                        setTextBuffer(assetTagsBuffer_, joinTagList(parseTagList(joinTagList(updatedTags))));
                        status_ = "Queued asset tag add: " + tag;
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
            const std::filesystem::path resolvedSourceForReveal = resolveAssetRecordPath(state, record.sourcePath);
            const std::filesystem::path resolvedImportedForReveal = resolveAssetRecordPath(state, record.importedPath);
            const std::filesystem::path resolvedCacheForReveal = resolveAssetRecordPath(state, record.cachePath);
            const std::vector<AssetOverwriteRisk>& overwriteRisks = selectedSourceControlSummary.generatedRisks;
            auto queueReimportWithExternalChangePrompt = [&](const char* actionLabel, const std::filesystem::path& sourcePath) {
                if (overwriteRisks.empty()) {
                    requests.reimportAsset = record.guid;
                    recordImportOperation(actionLabel, sourcePath, {}, "Reimport", record.guid);
                    status_ = std::string("Queued ") + actionLabel + ": " + (record.displayName.empty() ? record.guid : record.displayName);
                    return;
                }
                pendingExternalChangeGuid_ = record.guid;
                pendingExternalChangeAction_ = actionLabel;
                pendingExternalChangeDisplayName_ = record.displayName.empty() ? record.guid : record.displayName;
                pendingExternalChangeSourcePath_ = sourcePath;
                pendingExternalChangeRiskLines_.clear();
                for (const AssetOverwriteRisk& risk : overwriteRisks) {
                    pendingExternalChangeRiskLines_.push_back(risk.label + ": " + risk.status + " - " + risk.path.string());
                }
                externalChangePromptOpen_ = true;
                status_ = std::string("External-change confirmation required for ") + actionLabel + ": " + pendingExternalChangeDisplayName_;
            };
            auto drawRevealAction = [&](const char* id, const char* label, const char* tooltip, const std::filesystem::path& path) {
                const bool canReveal = !path.empty() && std::filesystem::exists(path);
                if (!canReveal) {
                    ImGui::BeginDisabled();
                }
                if (contentActionButton(id, EditorGlyphIcon::Folder, label, tooltip)) {
                    revealPathInFileBrowser(path);
                    status_ = std::string("Revealed: ") + path.string();
                }
                if (!canReveal) {
                    ImGui::EndDisabled();
                }
            };
            ImGui::PushID("RecordRepairActions");
            if (contentActionButton("CopyGuid", EditorGlyphIcon::Command, "Copy GUID", "Copy this asset GUID to the clipboard")) {
                ImGui::SetClipboardText(record.guid.c_str());
                status_ = "Copied asset GUID: " + record.guid;
            }
            if (!record.sourcePath.empty()) {
                ImGui::SameLine();
                if (contentActionButton("CopySource", EditorGlyphIcon::Command, "Copy Source", "Copy the resolved source path to the clipboard")) {
                    copyPathToClipboard(resolvedSourceForReveal.empty() ? std::filesystem::path(record.sourcePath) : resolvedSourceForReveal);
                    status_ = "Copied source path: " + record.sourcePath;
                }
                ImGui::SameLine();
                drawRevealAction("RevealSource", "Reveal Source", "Reveal the source asset in Explorer", resolvedSourceForReveal);
            }
            if (!compatibilityMode_) {
                ImGui::SameLine();
                if (contentActionButton("RelinkSource", EditorGlyphIcon::Refresh, "Relink Source", "Choose a replacement raw source path for this asset record")) {
                    if (auto source = openImportAssetFileDialog()) {
                        requests.relinkAssetSource = EditorAssetRelinkSourceRequest{record.guid, *source};
                        status_ = "Queued source relink: " + record.displayName;
                    }
                }
            }
            if (!record.importedPath.empty()) {
                ImGui::SameLine();
                drawRevealAction("RevealImported", "Reveal Metadata", "Reveal the imported metadata file in Explorer", resolvedImportedForReveal);
            }
            if (!record.cachePath.empty()) {
                ImGui::SameLine();
                drawRevealAction("RevealCache", "Reveal Payload", "Reveal the cooked/runtime payload in Explorer", resolvedCacheForReveal);
            }
            const bool canRepairByReimport = !record.sourcePath.empty() && !record.sourceMissing && std::filesystem::exists(resolvedSourceForReveal);
            const bool repairNeeded = record.stale || record.missing || record.importedMetadataMissing || record.cookedPayloadMissing || record.dependenciesMissing || record.status == AssetImportStatus::Missing || record.status == AssetImportStatus::Stale || record.status == AssetImportStatus::Failed;
            ImGui::SameLine();
            if (!canRepairByReimport || !repairNeeded) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("RepairAsset", EditorGlyphIcon::Refresh, "Repair Asset", "Queue reimport to repair stale, failed, missing-metadata, or missing-payload asset state")) {
                queueReimportWithExternalChangePrompt("Repair Asset", resolvedSourceForReveal);
            }
            if (!canRepairByReimport || !repairNeeded) {
                ImGui::EndDisabled();
            }
            if (repairNeeded && !canRepairByReimport) {
                ImGui::TextDisabled("Repair Asset requires an available source path; restore or relink the source first.");
            } else if (!repairNeeded) {
                ImGui::TextDisabled("Repair Asset: no stale, failed, or missing metadata/payload state is currently reported.");
            }
            if ((!record.sourcePath.empty() && !std::filesystem::exists(resolvedSourceForReveal)) ||
                (!record.importedPath.empty() && !std::filesystem::exists(resolvedImportedForReveal)) ||
                (!record.cachePath.empty() && !std::filesystem::exists(resolvedCacheForReveal))) {
                ImGui::TextDisabled("Missing paths can be repaired by restoring files or reimporting when the source is available.");
            }
            if (!overwriteRisks.empty()) {
                ImGui::SeparatorText("Source Control Overwrite Warning");
                ImGui::TextWrapped("Reimport or Rebuild Payload may overwrite generated asset files that have external source-control changes. Review the changed files before continuing.");
                for (const AssetOverwriteRisk& risk : overwriteRisks) {
                    ImGui::TextColored(sourceControlStatusTextColor(risk.status), "%s: %s", risk.label.c_str(), risk.status.c_str());
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", risk.path.string().c_str());
                    }
                }
                if (contentActionButton("OverwriteRiskReport", EditorGlyphIcon::Details, "Review Overwrite Risk", "Write and open a report of generated files that may be overwritten by reimport/rebuild")) {
                    std::filesystem::path reportPath;
                    std::string error;
                    if (writeAssetOverwriteRiskReport(state, browserRoot_, record, overwriteRisks, reportPath, error)) {
                        requests.openFilePath = reportPath;
                        status_ = "Asset overwrite-risk report: " + reportPath.string();
                    } else {
                        status_ = "Asset overwrite-risk report failed: " + error;
                    }
                }
            }
            if (contentActionButton("ExternalReloadReadinessReport", EditorGlyphIcon::Details, "Reload Readiness", "Write and open a safe external-change reload readiness report for this asset")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetExternalReloadReadinessReport(state, browserRoot_, record, sourceControlStatus, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset external reload readiness report: " + reportPath.string();
                } else {
                    status_ = "Asset external reload readiness failed: " + error;
                }
            }
            if (contentActionButton("ValidateSelectedAsset", EditorGlyphIcon::Details, "Validate Asset", "Write a validation report for this asset and open it")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetValidationReport(state, browserRoot_, reportPath, error, record.guid)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset validation report: " + reportPath.string();
                } else {
                    status_ = "Asset validation failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("ShowDependencies", EditorGlyphIcon::Details, "Show Dependencies", "Write and open a report of assets this record depends on")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetRelationshipReport(state, browserRoot_, record.guid, false, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset dependency report: " + reportPath.string();
                } else {
                    status_ = "Asset dependency report failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("ShowReferences", EditorGlyphIcon::Details, "Show References", "Write and open a report of assets and scene components that reference this record")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetRelationshipReport(state, browserRoot_, record.guid, true, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset reference report: " + reportPath.string();
                } else {
                    status_ = "Asset reference report failed: " + error;
                }
            }
            const AssetUsageSummary usageSummary = assetUsageSummaryForRecord(state, record);
            ImGui::SeparatorText("Move / Delete Guard");
            if (usageSummary.referenced()) {
                ImGui::TextColored(
                    ImVec4(0.95f, 0.68f, 0.28f, 1.0f),
                    "Referenced by %zu registry link%s and %zu current-scene use%s",
                    usageSummary.registryReferences,
                    usageSummary.registryReferences == 1 ? "" : "s",
                    usageSummary.sceneReferences,
                    usageSummary.sceneReferences == 1 ? "" : "s");
                ImGui::TextWrapped("Inspect references or replace them before moving or deleting this asset. This live warning covers the loaded registry and current scene; Delete Readiness also runs the saved project metadata scan.");
            } else {
                ImGui::TextDisabled("No loaded registry or current-scene references found for this asset.");
            }
            if (contentActionButton("DeleteReadinessReport", EditorGlyphIcon::Details, "Delete Readiness", "Write and open a loaded-registry/current-scene/saved-project delete-readiness report")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetDeleteReadinessReport(state, browserRoot_, record.guid, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset delete-readiness report: " + reportPath.string();
                } else {
                    status_ = "Asset delete-readiness report failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("ProjectReferenceScan", EditorGlyphIcon::Details, "Project References", "Scan saved project content and scene metadata for this asset GUID")) {
                std::filesystem::path reportPath;
                std::string error;
                if (writeAssetProjectReferenceScanReport(state, browserRoot_, record.guid, reportPath, error)) {
                    requests.openFilePath = reportPath;
                    status_ = "Asset project reference scan report: " + reportPath.string();
                } else {
                    status_ = "Asset project reference scan failed: " + error;
                }
            }
            ImGui::SameLine();
            if (contentActionButton("RemoveRegistryRecord", EditorGlyphIcon::Trash, "Remove Registry Item", "Remove this asset record from the registry and leave generated files on disk")) {
                requests.deleteAssets = EditorDeleteAssetRequest{{record.guid}, false};
                selectedRecordGuid_.clear();
                status_ = "Queued registry record removal: " + record.displayName;
            }
            ImGui::SameLine();
            if (contentActionButton("DeleteGeneratedAssetFiles", EditorGlyphIcon::Trash, "Delete Generated Files", "Remove this asset record and delete its generated metadata/cache files inside the project")) {
                requests.deleteAssets = EditorDeleteAssetRequest{{record.guid}, true};
                selectedRecordGuid_.clear();
                status_ = "Queued generated asset file delete: " + record.displayName;
            }
            ImGui::SeparatorText("Reference Repair");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##replaceReferenceGuid", "Replacement asset GUID", replaceReferenceGuid_.data(), replaceReferenceGuid_.size());
            const AssetRecord* replacementRecord = nullptr;
            const std::string replacementGuid = replaceReferenceGuid_.data();
            if (!replacementGuid.empty()) {
                for (const AssetRecord& candidate : state.assetRegistry->records()) {
                    if (candidate.guid == replacementGuid) {
                        replacementRecord = &candidate;
                        break;
                    }
                }
            }
            const bool canReplaceReferences = replacementRecord != nullptr && replacementRecord->guid != record.guid && replacementRecord->type == record.type;
            if (!canReplaceReferences) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ReplaceReferences", EditorGlyphIcon::Refresh, "Replace References", "Replace current-scene and loaded-registry references to this asset with the replacement GUID")) {
                requests.replaceAssetReferences = EditorReplaceAssetReferencesRequest{record.guid, replacementRecord->guid, false};
                status_ = "Queued reference replacement: " + record.guid + " -> " + replacementRecord->guid;
            }
            ImGui::SameLine();
            if (contentActionButton("ReplaceProjectReferences", EditorGlyphIcon::Refresh, "Replace Project References", "Replace loaded references and saved project JSON metadata references; writes backups and a report")) {
                requests.replaceAssetReferences = EditorReplaceAssetReferencesRequest{record.guid, replacementRecord->guid, true};
                status_ = "Queued project-wide reference replacement: " + record.guid + " -> " + replacementRecord->guid;
            }
            if (!canReplaceReferences) {
                ImGui::EndDisabled();
            }
            if (!replacementGuid.empty() && replacementRecord == nullptr) {
                ImGui::TextDisabled("Replacement GUID is not in the loaded asset registry.");
            } else if (replacementRecord != nullptr && replacementRecord->type != record.type) {
                ImGui::TextDisabled("Replacement asset type must match the selected asset type.");
            } else if (replacementRecord != nullptr && replacementRecord->guid == record.guid) {
                ImGui::TextDisabled("Replacement GUID must be different from the selected asset.");
            }
            ImGui::PopID();
            ImGui::Text("Dependencies: %zu", record.dependencies.size());
            ImGui::Text("References: %zu", record.references.size());
            auto findRecordByGuid = [&](const AssetGuid& guid) -> const AssetRecord* {
                for (const AssetRecord& candidate : state.assetRegistry->records()) {
                    if (candidate.guid == guid) {
                        return &candidate;
                    }
                }
                return nullptr;
            };
            size_t missingDependencyCount = 0;
            size_t unambiguousMissingDependencyRepairCount = 0;
            for (const AssetDependency& dependency : record.dependencies) {
                if (dependency.guid.empty() || findRecordByGuid(dependency.guid) != nullptr) {
                    continue;
                }
                ++missingDependencyCount;
                const nlohmann::json repairCandidates = dependencyRepairCandidatesJson(state, record, dependency);
                if (repairCandidates.size() == 1u) {
                    ++unambiguousMissingDependencyRepairCount;
                }
            }
            auto drawLinkedAssetTable = [&](const char* label, const std::vector<AssetDependency>* dependencies, const std::vector<AssetGuid>* references) {
                const size_t count = dependencies != nullptr ? dependencies->size() : references != nullptr ? references->size() : 0;
                if (count == 0) {
                    return;
                }
                ImGui::SeparatorText(label);
                const std::string tableId = std::string(label) + "Table";
                if (ImGui::BeginTable(tableId.c_str(), 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Asset");
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                    ImGui::TableHeadersRow();
                    for (size_t index = 0; index < count; ++index) {
                        const AssetGuid guid = dependencies != nullptr ? (*dependencies)[index].guid : (*references)[index];
                        const std::string role = dependencies != nullptr ? (*dependencies)[index].kind : "reference";
                        const AssetRecord* linked = findRecordByGuid(guid);
                        const std::string rowId = std::string(label) + "_" + std::to_string(index);
                        ImGui::PushID(rowId.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(role.empty() ? "asset" : role.c_str());
                        ImGui::TableSetColumnIndex(1);
                        const std::string display = linked != nullptr && !linked->displayName.empty() ? linked->displayName : guid;
                        if (linked != nullptr) {
                            if (ImGui::Selectable(display.c_str())) {
                                selectedRecordGuid_ = linked->guid;
                                selectedPath_.clear();
                                status_ = "Selected linked asset: " + display;
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("%s", linked->guid.c_str());
                            }
                        } else {
                            ImGui::TextColored(ImVec4(0.95f, 0.36f, 0.32f, 1.0f), "%s", guid.c_str());
                        }
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(linked != nullptr ? assetTypeName(linked->type) : "Missing");
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(linked != nullptr ? assetImportStatusName(linked->status) : "Broken");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            };
            drawLinkedAssetTable("Dependencies", &record.dependencies, nullptr);
            drawLinkedAssetTable("References", nullptr, &record.references);
            if (missingDependencyCount > 0) {
                ImGui::SeparatorText("Dependency Repair");
                ImGui::TextColored(
                    unambiguousMissingDependencyRepairCount > 0 ? ImVec4(0.95f, 0.68f, 0.28f, 1.0f) : ImVec4(0.95f, 0.36f, 0.32f, 1.0f),
                    "Missing dependencies: %zu; unambiguous repair candidates: %zu",
                    missingDependencyCount,
                    unambiguousMissingDependencyRepairCount);
                ImGui::TextWrapped("This action rewrites only this loaded registry record's missing dependency GUIDs when the candidate report finds exactly one replacement. Saved project files, source files, and package/cache internals are not rewritten.");
                if (unambiguousMissingDependencyRepairCount == 0) {
                    ImGui::BeginDisabled();
                }
                if (contentActionButton("RepairUnambiguousDependencies", EditorGlyphIcon::Refresh, "Repair Unambiguous Dependencies", "Rewrite this record's missing dependency GUIDs only when each repaired dependency has exactly one candidate")) {
                    requests.repairMissingAssetDependencies = EditorRepairMissingAssetDependenciesRequest{record.guid, true};
                    status_ = "Queued missing dependency repair: " + (record.displayName.empty() ? record.guid : record.displayName);
                }
                if (unambiguousMissingDependencyRepairCount == 0) {
                    ImGui::EndDisabled();
                }
            }
            auto drawReverseRegistryReferences = [&] {
                struct ReverseReferenceRow {
                    const AssetRecord* owner = nullptr;
                    std::string role;
                    std::string source;
                };
                std::vector<ReverseReferenceRow> rows;
                for (const AssetRecord& candidate : state.assetRegistry->records()) {
                    if (candidate.guid == record.guid) {
                        continue;
                    }
                    for (const AssetDependency& dependency : candidate.dependencies) {
                        if (dependency.guid == record.guid) {
                            rows.push_back(ReverseReferenceRow{&candidate, dependency.kind.empty() ? "dependency" : dependency.kind, "Dependency"});
                        }
                    }
                    for (const AssetGuid& reference : candidate.references) {
                        if (reference == record.guid) {
                            rows.push_back(ReverseReferenceRow{&candidate, "reference", "Reference"});
                        }
                    }
                }
                if (rows.empty()) {
                    return;
                }
                ImGui::SeparatorText("Used By Assets");
                if (ImGui::BeginTable("ReverseAssetReferencesTable", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Asset");
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                    ImGui::TableHeadersRow();
                    for (size_t index = 0; index < rows.size(); ++index) {
                        const ReverseReferenceRow& row = rows[index];
                        if (row.owner == nullptr) {
                            continue;
                        }
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(row.role.c_str());
                        ImGui::TableSetColumnIndex(1);
                        const std::string display = row.owner->displayName.empty() ? row.owner->guid : row.owner->displayName;
                        if (ImGui::Selectable(display.c_str())) {
                            selectedRecordGuid_ = row.owner->guid;
                            selectedPath_.clear();
                            status_ = "Selected referring asset: " + display;
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", row.owner->guid.c_str());
                        }
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(assetTypeName(row.owner->type));
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(row.source.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            };
            auto drawSceneReferences = [&] {
                if (state.sceneDocument == nullptr) {
                    return;
                }
                struct SceneReferenceRow {
                    std::string entityName;
                    uint64_t entityUuid = 0;
                    std::string component;
                    std::string field;
                };
                std::vector<SceneReferenceRow> rows;
                for (const Entity* entity : state.sceneDocument->registry().entities()) {
                    if (entity == nullptr || !entity->meshRenderer.has_value()) {
                        continue;
                    }
                    const MeshRenderer& renderer = *entity->meshRenderer;
                    if (renderer.meshGuid == record.guid) {
                        rows.push_back(SceneReferenceRow{entity->name, entity->uuid, "MeshRenderer", "meshGuid"});
                    }
                    for (const MaterialSlot& slot : renderer.materialSlots) {
                        if (slot.materialGuid == record.guid) {
                            rows.push_back(SceneReferenceRow{entity->name, entity->uuid, "MeshRenderer", "materialGuid"});
                        }
                        if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == record.guid) {
                            rows.push_back(SceneReferenceRow{entity->name, entity->uuid, "MeshRenderer", "overrideMaterialGuid"});
                        }
                    }
                }
                for (const PrefabInstance& instance : state.sceneDocument->prefabInstances()) {
                    if (instance.prefabGuid == record.guid) {
                        rows.push_back(SceneReferenceRow{"Prefab Instance", instance.instanceRoot.index, "PrefabInstance", "prefabGuid"});
                    }
                }
                if (rows.empty()) {
                    return;
                }
                ImGui::SeparatorText("Used By Current Scene");
                if (ImGui::BeginTable("SceneAssetReferencesTable", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Entity");
                    ImGui::TableSetupColumn("UUID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthFixed, 112.0f);
                    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 132.0f);
                    ImGui::TableHeadersRow();
                    for (size_t index = 0; index < rows.size(); ++index) {
                        const SceneReferenceRow& row = rows[index];
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(row.entityName.empty() ? "(unnamed)" : row.entityName.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%llu", static_cast<unsigned long long>(row.entityUuid));
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(row.component.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(row.field.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            };
            drawReverseRegistryReferences();
            drawSceneReferences();
            ImGui::Text("Status: %s%s%s%s", assetImportStatusName(record.status), record.missing ? " missing" : "", record.stale ? " stale" : "", record.sourceMissing ? " source-missing" : "");
            ImGui::ProgressBar(assetImportProgress(record), ImVec2(-FLT_MIN, 0.0f), assetImportProgressLabel(record));
            const std::filesystem::path resolvedSourcePath = resolveAssetRecordPath(state, record.sourcePath);
            const bool canReimport = !record.sourcePath.empty() && !record.sourceMissing && std::filesystem::exists(resolvedSourcePath);
            const bool canRebuildPayload = canReimport && record.cookedPayloadMissing;
            if (!canRebuildPayload) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("RebuildPayload", EditorGlyphIcon::Refresh, "Rebuild Payload", "Queue a reimport to regenerate the missing cooked/runtime payload")) {
                queueReimportWithExternalChangePrompt("Rebuild Payload", resolvedSourcePath);
            }
            if (!canRebuildPayload) {
                ImGui::EndDisabled();
                if (record.cookedPayloadMissing && !canReimport) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Source unavailable");
                }
            }
            ImGui::SameLine();
            if (!canReimport) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ReimportRecord", EditorGlyphIcon::Refresh, "Reimport", "Queue this asset for reimport")) {
                queueReimportWithExternalChangePrompt("Reimport Asset", resolvedSourcePath);
            }
            if (!canReimport) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("Source unavailable");
            }
            if (record.type == AssetType::Prefab) {
                ImGui::SameLine();
                const bool canPlacePrefab = !assetPlacementBlocked(record);
                if (!canPlacePrefab) {
                    ImGui::BeginDisabled();
                }
                if (contentActionButton("PlacePrefab", EditorGlyphIcon::Add, "Place Prefab", "Place this prefab in the current scene")) {
                    requests.placeAsset = record.guid;
                }
                if (!canPlacePrefab) {
                    ImGui::EndDisabled();
                    ImGui::TextDisabled("Placement blocked: %s", assetPlacementBlockReason(record));
                }
            }
            break;
        }
    }
    if (hasDetailsSelection) {
        ImGui::SeparatorText("Context");
        if (state.project != nullptr) {
            ImGui::Text("Project: %s", state.project->name.c_str());
            ImGui::TextWrapped("Content Root: %s", state.project->contentRoot.string().c_str());
        } else {
            ImGui::TextDisabled("Project: none");
        }
        ImGui::Text("Current Folder: %s", currentPath_.empty() ? "(none)" : relativeContentPath(currentPath_).c_str());
    }
    drawExternalChangeConfirmPrompt(requests);
}

void AssetBrowserPanel::drawImportSettingsDialog(EditorRequests& requests) {
    if (openImportSettings_) {
        ImGui::OpenPopup("Import Settings");
        openImportSettings_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const char* modes[] = {"Import Asset", "Import and Place"};
    ImGui::Combo("Mode", &importMode_, modes, IM_ARRAYSIZE(modes));
    ImGui::InputText("Source", importSourcePath_.data(), importSourcePath_.size(), ImGuiInputTextFlags_ReadOnly);
    ImGui::InputTextWithHint("Destination Folder", "Models", importDestinationFolder_.data(), importDestinationFolder_.size());
    ImGui::SeparatorText("Source");
    ImGui::Checkbox("Copy source into project", &importSettings_.copySourceIntoProject);
    ImGui::SeparatorText("Asset Output");
    importSettings_.generatePrefabAsset = true;
    ImGui::BeginDisabled(true);
    ImGui::Checkbox("Generate prefab/model asset", &importSettings_.generatePrefabAsset);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Current glTF/GLB imports use a prefab/model root asset as the durable registry entry.");
    }
    ImGui::SeparatorText("Hierarchy");
    ImGui::Checkbox("Preserve hierarchy", &importSettings_.preserveHierarchy);
    const char* materialModes[] = {"Import materials", "Metadata only", "Skip materials"};
    const char* materialModeValues[] = {"ImportMaterials", "MetadataOnly", "SkipMaterials"};
    int materialMode = importSettings_.materialImportMode == "SkipMaterials" ? 2 : importSettings_.materialImportMode == "MetadataOnly" ? 1 : 0;
    if (ImGui::Combo("Material mode", &materialMode, materialModes, IM_ARRAYSIZE(materialModes))) {
        importSettings_.materialImportMode = materialModeValues[materialMode];
    }
    importSettings_.importMaterials = importSettings_.materialImportMode != "SkipMaterials";
    const char* textureModes[] = {"Import textures", "Metadata only", "Skip textures"};
    const char* textureModeValues[] = {"ImportTextures", "MetadataOnly", "SkipTextures"};
    int textureMode = importSettings_.textureImportMode == "SkipTextures" ? 2 : importSettings_.textureImportMode == "MetadataOnly" ? 1 : 0;
    if (ImGui::Combo("Texture mode", &textureMode, textureModes, IM_ARRAYSIZE(textureModes))) {
        importSettings_.textureImportMode = textureModeValues[textureMode];
    }
    importSettings_.importTextures = importSettings_.textureImportMode != "SkipTextures";
    const char* textureCompressionModes[] = {"Preserve source payload"};
    int textureCompressionMode = 0;
    if (ImGui::Combo("Texture compression", &textureCompressionMode, textureCompressionModes, IM_ARRAYSIZE(textureCompressionModes))) {
        importSettings_.textureCompression = "PreserveSource";
    }
    ImGui::Checkbox("Import cameras", &importSettings_.importCameras);
    ImGui::Checkbox("Import lights", &importSettings_.importLights);
    ImGui::SeparatorText("Geometry / Cache");
    ImGui::Checkbox("Generate tangents", &importSettings_.generateTangents);
    ImGui::Checkbox("Build BLAS cache", &importSettings_.buildBlasCache);
    ImGui::Checkbox("Build cooked payloads now", &importSettings_.buildCookedPayloadsNow);
    ImGui::Checkbox("Generate thumbnails", &importSettings_.generateThumbnails);
    ImGui::InputFloat("Unit scale", &importSettings_.unitScale, 0.1f, 1.0f, "%.3f");
    const char* coordinateModes[] = {"None", "glTF Y-Up to Engine", "Z-Up to Engine"};
    int coordinateMode = importSettings_.coordinateConversion == "glTF Y-Up to Engine" ? 1
        : importSettings_.coordinateConversion == "Z-Up to Engine" ? 2
        : 0;
    if (ImGui::Combo("Coordinate conversion", &coordinateMode, coordinateModes, IM_ARRAYSIZE(coordinateModes))) {
        importSettings_.coordinateConversion = coordinateModes[coordinateMode];
    }

    if (ImGui::Button("Import")) {
        EditorImportAssetRequest request;
        request.sourcePath = std::filesystem::path(importSourcePath_.data());
        request.destinationFolder = std::filesystem::path(importDestinationFolder_.data());
        request.mode = importMode_ == 0 ? "ImportAsset" : "ImportAndPlace";
        request.settings = importSettings_;
        if (importMode_ == 0) {
            requests.importAsset = std::move(request);
            recordImportOperation("Import Asset", requests.importAsset->sourcePath, requests.importAsset->destinationFolder, requests.importAsset->mode);
            status_ = "Queued non-mutating Import Asset: " + requests.importAsset->sourcePath.string();
        } else {
            recordImportOperation("Import and Place", request.sourcePath, request.destinationFolder, request.mode);
            const std::filesystem::path sourcePath = request.sourcePath;
            requests.importAndPlace = std::move(request);
            status_ = "Queued Import and Place: " + sourcePath.string();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        status_ = "Import Asset cancelled";
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    (void)selection;
    if (!ImGui::Begin(EditorDockWindowTitle::Content)) {
        ImGui::End();
        return;
    }

    syncBrowserRoot(state);
    refreshImportOperations(state);

    ImGui::BeginGroup();
    if (editorIconButton("ContentAdd", EditorGlyphIcon::Add, false)) {
        ImGui::OpenPopup("ContentAddMenu");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Add or import content");
    }
    if (ImGui::BeginPopup("ContentAddMenu")) {
        ImGui::MenuItem("New Folder", nullptr, false, false);
        if (ImGui::MenuItem("New Scene", nullptr, false, true)) {
            requests.newScene = true;
        }
        ImGui::MenuItem("New Material", nullptr, false, false);
        ImGui::Separator();
        const bool canImportAssets = !compatibilityMode_;
        if (ImGui::MenuItem("Import Asset...", nullptr, false, canImportAssets)) {
            if (auto path = openImportAssetFileDialog()) {
                prepareImportDialog(*path, {}, 0);
            }
        }
        if (ImGui::MenuItem("Import Into Scene...", nullptr, false, canImportAssets)) {
            if (auto path = openGltfFileDialog()) {
                prepareImportDialog(*path, {}, 1);
            }
        }
        if (ImGui::MenuItem("Import Texture...", nullptr, false, canImportAssets)) {
            if (auto path = openTextureFileDialog()) {
                prepareImportDialog(*path, {}, 0);
            }
        }
        if (ImGui::MenuItem("Import HDRI...", nullptr, false, canImportAssets)) {
            if (auto path = openHdrFileDialog()) {
                prepareImportDialog(*path, {}, 0);
            }
        }
        ImGui::MenuItem("Import IES Profile...", nullptr, false, false);
        ImGui::MenuItem("Browse Filesystem...", nullptr, false, false);
        if (compatibilityMode_) {
            ImGui::Separator();
            if (ImGui::MenuItem("Open Project Manager")) {
                requests.showProjectManager = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool canValidateProject = state.assetRegistry != nullptr;
    if (!canValidateProject) {
        ImGui::BeginDisabled();
    }
    if (contentActionButton("ValidateProject", EditorGlyphIcon::Details, "Validate Project", "Write a project asset validation report and open it")) {
        std::filesystem::path reportPath;
        std::string error;
        if (writeAssetValidationReport(state, browserRoot_, reportPath, error)) {
            requests.openFilePath = reportPath;
            status_ = "Project validation report: " + reportPath.string();
        } else {
            status_ = "Project validation failed: " + error;
        }
    }
    if (!canValidateProject) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##contentFilter", "Filter in selected folder...", search_.data(), search_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(backStack_.empty());
    if (editorIconButton("ContentBack", EditorGlyphIcon::Back, false)) {
        forwardStack_.push_back(currentPath_);
        const std::filesystem::path previous = backStack_.back();
        backStack_.pop_back();
        navigateTo(previous, false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Back");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(forwardStack_.empty());
    if (editorIconButton("ContentForward", EditorGlyphIcon::Forward, false)) {
        backStack_.push_back(currentPath_);
        const std::filesystem::path next = forwardStack_.back();
        forwardStack_.pop_back();
        navigateTo(next, false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Forward");
    }
    showDetails_ = true;

    if (!browserRoot_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::PushID("ContentBreadcrumb");
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(currentPath_, browserRoot_, relativeError);
        const std::string rootLabel = compatibilityMode_ ? compactPathLabel(browserRoot_, "Workspace") : "Project";
        if (ImGui::SmallButton(rootLabel.c_str())) {
            navigateTo(browserRoot_);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", browserRoot_.string().c_str());
        }
        std::filesystem::path accum = browserRoot_;
        for (const auto& part : relativeError ? std::filesystem::path{} : relative) {
            const std::string partString = part.string();
            if (partString == "." || partString.empty()) {
                continue;
            }
            accum /= part;
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            ImGui::PushID(partString.c_str());
            if (ImGui::SmallButton(partString.c_str())) {
                navigateTo(accum);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndGroup();
    drawImportSettingsDialog(requests);

    const std::string sceneLoadStatus = state.sceneLoadingStatus != nullptr ? *state.sceneLoadingStatus : std::string{};
    const bool hasSceneLoadStatus = !sceneLoadStatus.empty();
    const bool sceneLoadCompleted = !state.sceneLoadRunning && hasSceneLoadStatus && sceneLoadStatusIsSuccessfulCompletion(sceneLoadStatus);
    const bool showSceneLoadBanner = state.sceneLoadRunning || (hasSceneLoadStatus && !sceneLoadCompleted);
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }
    if (showSceneLoadBanner) {
        const float progress = std::clamp(state.sceneLoadProgress, 0.0f, 1.0f);
        ImGui::ProgressBar(progress, ImVec2(std::min(360.0f, ImGui::GetContentRegionAvail().x), 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.sceneLoadRunning ? "Import / load in progress" : "Last import / load status");
        ImGui::TextWrapped("%s", sceneLoadStatus.c_str());
    }

    const float browserHeight = ImGui::GetContentRegionAvail().y;
    if (browserHeight > ImGui::GetTextLineHeightWithSpacing()) {
        const float browserWidth = ImGui::GetContentRegionAvail().x;
        const float sectionSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float treeWidth = std::clamp(
            browserWidth * EditorUiMetric::contentTreePanelRatio,
            EditorUiMetric::contentTreeMinWidth,
            EditorUiMetric::contentTreeMaxWidth);
        float detailsWidth = showDetails_
            ? std::clamp(
                browserWidth * EditorUiMetric::contentDetailsPanelRatio,
                EditorUiMetric::contentDetailsMinWidth,
                EditorUiMetric::contentDetailsMaxWidth)
            : 0.0f;
        if (showDetails_) {
            const float maxDetailsWidth = browserWidth - treeWidth - EditorUiMetric::contentListMinWidth - (sectionSpacing * 2.0f);
            detailsWidth = std::max(0.0f, std::min(detailsWidth, maxDetailsWidth));
        }
        ImGui::BeginChild("ContentFolders", ImVec2(treeWidth, 0.0f), true);
        if (!browserRoot_.empty()) {
            drawFolderTree(browserRoot_, requests);
        }
        if (state.editorPrefs != nullptr) {
            auto& prefs = *state.editorPrefs;
            auto savePrefsStatus = [&](std::string successMessage, std::string failureDetail) {
                const std::filesystem::path prefsPath = state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath;
                setPreferenceSaveStatus(prefs.save(prefsPath), status_, std::move(successMessage), std::move(failureDetail));
            };
            auto drawStoredPathEntry = [&](const char* listName, size_t index, const std::filesystem::path& path, bool favoriteRow) {
                std::error_code ec;
                const bool exists = std::filesystem::exists(path, ec);
                const bool directory = exists && std::filesystem::is_directory(path, ec);
                const bool selected = !selectedPath_.empty() && canonicalForCompare(selectedPath_) == canonicalForCompare(path);
                const std::string filename = path.filename().empty() ? path.string() : path.filename().string();
                const std::string label = editorGlyphLabel((exists ? filename : filename + " (missing)").c_str());
                ImGui::PushID(listName);
                ImGui::PushID(static_cast<int>(index));
                if (!exists) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                }
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedPath_ = path;
                    selectedRecordGuid_.clear();
                    if (exists && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (directory) {
                            navigateTo(path);
                        } else {
                            loadFromPath(path, requests);
                        }
                    }
                }
                if (!exists) {
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", path.string().c_str());
                }
                if (ImGui::BeginPopupContextItem("StoredPathContext")) {
                    if (directory) {
                        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Open Folder", exists)) {
                            navigateTo(path);
                        }
                    } else if (editorGlyphMenuItem(editorGlyphForPath(path), "Open / Apply", exists && canOpenOrApplyPath(path))) {
                        loadFromPath(path, requests);
                    }
                    if (!favoriteRow && editorGlyphMenuItem(EditorGlyphIcon::Add, "Add to Favorites", exists && !preferencePathListContains(prefs.favoriteFiles, path))) {
                        prefs.addFavorite(path);
                        savePrefsStatus("Added favorite: " + path.string(), "add favorite " + path.string());
                    }
                    if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
                        copyPathToClipboard(path);
                        status_ = "Copied path: " + path.string();
                    }
                    if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer", exists)) {
                        revealPathInFileBrowser(path);
                    }
                    if (favoriteRow && editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove from Favorites")) {
                        prefs.removeFavorite(path.string());
                        savePrefsStatus("Removed favorite: " + path.string(), "remove favorite " + path.string());
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::PopID();
            };
            if (state.assetRegistry != nullptr && !prefs.favoriteAssetGuids.empty() && ImGui::TreeNodeEx("Favorite Assets", ImGuiTreeNodeFlags_DefaultOpen)) {
                std::string assetFavoriteToRemove;
                for (size_t i = 0; i < prefs.favoriteAssetGuids.size(); ++i) {
                    const AssetGuid& guid = prefs.favoriteAssetGuids[i];
                    const AssetRecord* record = findAssetRecordByGuid(*state.assetRegistry, guid);
                    const bool missingRecord = record == nullptr;
                    const std::string displayName = missingRecord
                        ? guid + " (missing)"
                        : (record->displayName.empty() ? record->guid : record->displayName);
                    const std::string label = editorGlyphLabel(displayName);
                    ImGui::PushID("FavoriteAsset");
                    ImGui::PushID(static_cast<int>(i));
                    if (missingRecord) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
                    }
                    if (ImGui::Selectable(label.c_str(), selectedRecordGuid_ == guid)) {
                        selectedRecordGuid_ = guid;
                        selectedPath_.clear();
                    }
                    if (missingRecord) {
                        ImGui::PopStyleColor();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", guid.c_str());
                    }
                    if (ImGui::BeginPopupContextItem("FavoriteAssetContext")) {
                        if (editorGlyphMenuItem(EditorGlyphIcon::Details, "Open Details", !missingRecord)) {
                            selectedRecordGuid_ = guid;
                            selectedPath_.clear();
                        }
                        const bool canPlaceFavoritePrefab = !missingRecord && record->type == AssetType::Prefab && !assetPlacementBlocked(*record);
                        if (!missingRecord && record->type == AssetType::Prefab && editorGlyphMenuItem(EditorGlyphIcon::Add, "Place Prefab", canPlaceFavoritePrefab)) {
                            requests.placeAsset = guid;
                        }
                        if (!missingRecord && record->type == AssetType::Prefab && !canPlaceFavoritePrefab) {
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("%s", assetPlacementBlockReason(*record));
                            }
                        }
                        if (editorGlyphMenuItem(EditorGlyphIcon::Details, "Filter Registry To Favorites")) {
                            registryFavoriteFilter_ = 1;
                        }
                        if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy GUID")) {
                            ImGui::SetClipboardText(guid.c_str());
                            status_ = "Copied asset GUID: " + guid;
                        }
                        if (editorGlyphMenuItem(EditorGlyphIcon::Trash, "Remove Asset Favorite")) {
                            assetFavoriteToRemove = guid;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
                if (!assetFavoriteToRemove.empty()) {
                    prefs.removeFavoriteAsset(assetFavoriteToRemove);
                    savePrefsStatus("Removed asset favorite: " + assetFavoriteToRemove, "remove asset favorite " + assetFavoriteToRemove);
                }
                ImGui::TreePop();
            }
            if (!prefs.favoriteFiles.empty() && ImGui::TreeNodeEx("Favorites", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.favoriteFiles.size(); ++i) {
                    drawStoredPathEntry("Favorite", i, std::filesystem::path(prefs.favoriteFiles[i]), true);
                }
                ImGui::TreePop();
            }
            if (!prefs.recentFiles.empty() && ImGui::TreeNodeEx("Recent", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.recentFiles.size(); ++i) {
                    drawStoredPathEntry("Recent", i, std::filesystem::path(prefs.recentFiles[i]), false);
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("ContentItems", ImVec2(-(detailsWidth + (showDetails_ ? sectionSpacing : 0.0f)), 0.0f), true);
        drawPathList(state, requests);
        drawRegistryTable(state, requests);
        drawImportOperations();
        ImGui::EndChild();
        if (showDetails_) {
            ImGui::SameLine();
            ImGui::BeginChild("ContentDetails", ImVec2(detailsWidth, 0.0f), true);
            drawDetails(state, requests);
            if (state.editorPrefs != nullptr && !selectedPath_.empty()) {
                const std::string storedFavorite = matchingPreferencePathValue(state.editorPrefs->favoriteFiles, selectedPath_);
                if (!storedFavorite.empty()) {
                    if (ImGui::SmallButton("Remove Selected Favorite")) {
                        state.editorPrefs->removeFavorite(storedFavorite);
                        const std::filesystem::path prefsPath = state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath;
                        setPreferenceSaveStatus(
                            state.editorPrefs->save(prefsPath),
                            status_,
                            "Removed favorite: " + selectedPath_.string(),
                            "remove favorite " + selectedPath_.string());
                    }
                } else if (ImGui::SmallButton("Add Selected to Favorites")) {
                    state.editorPrefs->addFavorite(selectedPath_);
                    const std::filesystem::path prefsPath = state.editorPreferencesPath.empty() ? EditorPreferences::defaultPath() : state.editorPreferencesPath;
                    setPreferenceSaveStatus(
                        state.editorPrefs->save(prefsPath),
                        status_,
                        "Added favorite: " + selectedPath_.string(),
                        "add favorite " + selectedPath_.string());
                }
            }
            ImGui::EndChild();
        }
    }

    ImGui::End();
}

} // namespace rtv
