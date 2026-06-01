#pragma once

#include "rtv/EditorPanels.h"
#include "rtv/EditorUiStyle.h"

#include <Volk/volk.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

class AssetBrowserPanel {
public:
    void draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);
    void invalidateThumbnails();

private:
    struct CpuThumbnail {
        bool attempted = false;
        bool available = false;
        int width = 0;
        int height = 0;
        int columns = 0;
        int rows = 0;
        std::vector<uint32_t> colors;
    };

    struct ImportOperation {
        uint64_t id = 0;
        std::string label;
        std::string mode;
        std::filesystem::path sourcePath;
        std::filesystem::path destinationFolder;
        AssetGuid assetGuid;
        float progress = 0.0f;
        std::string state = "Queued";
        bool completed = false;
        bool failed = false;
    };

    struct SourcePreview {
        bool attempted = false;
        bool available = false;
        bool loadedFromDiskCache = false;
        EditorGlyphIcon icon = EditorGlyphIcon::File;
        std::string title;
        std::string kind;
        std::vector<std::string> lines;
    };

    void loadFromPath(const std::filesystem::path& path, EditorRequests& requests);
    void prepareImportDialog(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationFolder, int mode);
    void syncBrowserRoot(const EditorRuntimeState& state);
    void navigateTo(const std::filesystem::path& path, bool addHistory = true);
    void drawFolderTree(const std::filesystem::path& path, EditorRequests& requests);
    void drawPathList(const EditorRuntimeState& state, EditorRequests& requests);
    void drawPathContextMenu(const std::filesystem::path& path, bool isDirectory, EditorRequests& requests);
    void drawRegistryTable(const EditorRuntimeState& state, EditorRequests& requests);
    void drawDetails(const EditorRuntimeState& state, EditorRequests& requests);
    void drawImportSettingsDialog(EditorRequests& requests);
    void recordImportOperation(
        const std::string& label,
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& destinationFolder,
        const std::string& mode,
        const AssetGuid& assetGuid = {});
    void refreshImportOperations(const EditorRuntimeState& state);
    void drawImportOperations();
    [[nodiscard]] bool drawGpuSceneTextureThumbnail(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max);
    [[nodiscard]] bool drawStandaloneGpuAssetPreview(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected);
    [[nodiscard]] CpuThumbnail& thumbnailForPath(const std::filesystem::path& path);
    [[nodiscard]] bool drawRasterThumbnail(const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected);
    [[nodiscard]] SourcePreview& sourcePreviewForPath(const std::filesystem::path& path);
    [[nodiscard]] std::filesystem::path generatedPreviewCachePath(const std::filesystem::path& path) const;
    [[nodiscard]] bool loadGeneratedPreviewDiskCache(const std::filesystem::path& path, SourcePreview& preview) const;
    void saveGeneratedPreviewDiskCache(const std::filesystem::path& path, const SourcePreview& preview) const;
    [[nodiscard]] bool drawGeneratedSourcePreview(const std::filesystem::path& path, ImVec2 min, ImVec2 max);
    [[nodiscard]] bool shouldShowPath(const std::filesystem::path& path) const;
    [[nodiscard]] std::string relativeContentPath(const std::filesystem::path& path) const;
    [[nodiscard]] std::string relativeImportDestination(const std::filesystem::path& path) const;

    std::array<char, 512> importSourcePath_{};
    std::array<char, 256> importDestinationFolder_{};
    std::array<char, 128> search_{};
    std::string status_;
    std::filesystem::path browserRoot_;
    std::filesystem::path contentRoot_;
    std::filesystem::path scenesRoot_;
    std::filesystem::path savedRoot_;
    std::filesystem::path cacheRoot_;
    std::filesystem::path currentPath_;
    std::filesystem::path selectedPath_;
    std::string selectedRecordGuid_;
    std::vector<std::filesystem::path> backStack_;
    std::vector<std::filesystem::path> forwardStack_;
    bool gridView_ = false;
    bool showDetails_ = true;
    bool compatibilityMode_ = true;
    bool openImportSettings_ = false;
    int importMode_ = 0;
    AssetImportSettings importSettings_{};
    std::unordered_map<std::string, CpuThumbnail> thumbnailCache_;
    std::unordered_map<std::string, SourcePreview> sourcePreviewCache_;
    std::vector<ImportOperation> importOperations_;
    uint64_t nextImportOperationId_ = 1;
};

} // namespace rtv
