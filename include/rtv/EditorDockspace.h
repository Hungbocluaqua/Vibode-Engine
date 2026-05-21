#pragma once

#include "rtv/EditorPanels.h"

#include <filesystem>

namespace rtv {

class EditorDockspace {
public:
    void begin(EditorPanelVisibility& visibility, EditorRequests& requests);
    void end();
    void setProfilePath(const std::filesystem::path& scenePath);
    void saveLayout() const;
    void requestResetLayout();

private:
    void buildDefaultLayout();
    void loadLayout();
    void drawMainMenu(EditorPanelVisibility& visibility, EditorRequests& requests);
    void drawHelpWindows();

    std::filesystem::path profilePath_;
    bool layoutResetRequested_ = true;
    bool showControls_ = false;
    bool showRendererInfo_ = false;
};

} // namespace rtv
