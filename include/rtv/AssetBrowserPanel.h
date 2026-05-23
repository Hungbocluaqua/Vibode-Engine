#pragma once

#include "rtv/EditorPanels.h"

#include <Volk/volk.h>

#include <array>
#include <string>
#include <unordered_map>

namespace rtv {

class AssetBrowserPanel {
public:
    void draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);
    void invalidateThumbnails();

private:
    void loadFromPath(const std::filesystem::path& path, EditorRequests& requests);

    std::array<char, 512> gltfPath_{};
    std::array<char, 512> hdrPath_{};
    std::array<char, 512> scenePath_{};
    std::string status_;
    std::unordered_map<uint32_t, VkDescriptorSet> thumbnailCache_;
};

} // namespace rtv
