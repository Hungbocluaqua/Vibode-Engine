#pragma once

#include "rtv/EditorPanels.h"

#include <array>
#include <string>

namespace rtv {

class AssetBrowserPanel {
public:
    void draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);

private:
    std::array<char, 512> gltfPath_{};
    std::array<char, 512> hdrPath_{};
    std::array<char, 512> scenePath_{};
    std::string status_;
};

} // namespace rtv
