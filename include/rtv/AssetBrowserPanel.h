#pragma once

#include "rtv/EditorPanels.h"

#include <Volk/volk.h>

#include <array>
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
    void loadFromPath(const std::filesystem::path& path, EditorRequests& requests);
    void syncBrowserRoot(const EditorRuntimeState& state);
    void navigateTo(const std::filesystem::path& path, bool addHistory = true);
    void drawFolderTree(const std::filesystem::path& path);
    void drawPathList(const EditorRuntimeState& state, EditorRequests& requests);
    void drawRegistryTable(const EditorRuntimeState& state, EditorRequests& requests);
    void drawDetails(const EditorRuntimeState& state);
    void drawImportSettingsDialog(EditorRequests& requests);
    [[nodiscard]] bool shouldShowPath(const std::filesystem::path& path) const;
    [[nodiscard]] std::string relativeContentPath(const std::filesystem::path& path) const;

    std::array<char, 512> gltfPath_{};
    std::array<char, 512> hdrPath_{};
    std::array<char, 512> scenePath_{};
    std::array<char, 512> importSourcePath_{};
    std::array<char, 256> importDestinationFolder_{};
    std::array<char, 128> search_{};
    std::string status_;
    std::filesystem::path browserRoot_;
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
    std::unordered_map<uint32_t, VkDescriptorSet> thumbnailCache_;
};

} // namespace rtv
