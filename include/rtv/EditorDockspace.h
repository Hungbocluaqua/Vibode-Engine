#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class EditorDockspace {
public:
    void begin(EditorPanelVisibility& visibility, EditorRequests& requests);
    void end();
    void requestResetLayout();

private:
    void buildDefaultLayout();
    void drawMainMenu(EditorPanelVisibility& visibility, EditorRequests& requests);
    void drawHelpWindows();

    bool layoutResetRequested_ = true;
    bool showControls_ = false;
    bool showRendererInfo_ = false;
};

} // namespace rtv
