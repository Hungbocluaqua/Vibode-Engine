#pragma once

#include "rtv/EditorCommands.h"
#include "rtv/EditorPanels.h"

#include <filesystem>

namespace rtv {

class EditorDockspace {
public:
    void begin(EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests);
    void end(EditorPanelVisibility& visibility, EditorRequests& requests);
    void setProfileFile(const std::filesystem::path& layoutPath);
    void setProfilePath(const std::filesystem::path& scenePath);
    void saveLayout() const;
    void requestResetLayout();

private:
    void buildDefaultLayout();
    void loadLayout();
    void executeCommand(EditorCommandId id, EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests);
    void drawMainMenu(EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests);
    void drawDockTabIconOverlays(EditorPanelVisibility& visibility, EditorRequests& requests);
    void drawDockPanelChromeOverlays();
    void drawHelpWindows();

    std::filesystem::path profilePath_;
    bool layoutResetRequested_ = true;
    bool showControls_ = false;
    bool showRendererInfo_ = false;
    bool dockChromeStylePushed_ = false;
};

} // namespace rtv
