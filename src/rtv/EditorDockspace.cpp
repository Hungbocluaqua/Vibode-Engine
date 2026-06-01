#include "rtv/EditorDockspace.h"

#include "rtv/EditorCommands.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/FileDialog.h"
#include "rtv/KeyBindings.h"
#include "rtv/UndoStack.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace rtv {

namespace {

std::string activeSceneTitle(const EditorRuntimeState& state) {
    std::filesystem::path path;
    if (state.scenePath != nullptr && state.scenePath->has_value()) {
        path = **state.scenePath;
    } else if (state.gltfPath != nullptr && state.gltfPath->has_value()) {
        path = **state.gltfPath;
    }
    std::string title = path.empty() ? "Untitled Scene" : path.stem().string();
    if (state.sceneDirty) {
        title += "*";
    }
    return title;
}

void menuItemTooltip(const char* description, const char* disabledReason = nullptr) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        return;
    }
    if (disabledReason != nullptr && disabledReason[0] != '\0') {
        ImGui::SetTooltip("%s\nDisabled: %s", description != nullptr ? description : "Not available", disabledReason);
    } else if (description != nullptr && description[0] != '\0') {
        ImGui::SetTooltip("%s", description);
    }
}

EditorGlyphIcon commandGlyph(EditorCommandId id) {
    switch (id) {
    case EditorCommandId::ProjectManager:
    case EditorCommandId::CloseProject:
        return EditorGlyphIcon::ProjectFile;
    case EditorCommandId::NewScene:
    case EditorCommandId::OpenScene:
    case EditorCommandId::SaveScene:
    case EditorCommandId::SaveSceneAs:
        return EditorGlyphIcon::SceneFile;
    case EditorCommandId::ImportAsset:
    case EditorCommandId::ImportAndPlace:
    case EditorCommandId::ImportSceneAsNewScene:
    case EditorCommandId::MergeScene:
    case EditorCommandId::ImportHdri:
        return EditorGlyphIcon::Import;
    case EditorCommandId::CreateCamera:
        return EditorGlyphIcon::Camera;
    case EditorCommandId::CreatePointLight:
    case EditorCommandId::CreateSpotLight:
    case EditorCommandId::CreateAreaLight:
        return EditorGlyphIcon::Light;
    case EditorCommandId::CreatePrimarySun:
        return EditorGlyphIcon::Sun;
    case EditorCommandId::CreateEnvironmentLight:
        return EditorGlyphIcon::Environment;
    case EditorCommandId::CreateSkyAtmosphere:
        return EditorGlyphIcon::Sky;
    case EditorCommandId::CreateHeightFog:
        return EditorGlyphIcon::Fog;
    case EditorCommandId::CreateVolumetricCloud:
        return EditorGlyphIcon::Cloud;
    case EditorCommandId::CreatePostProcessVolume:
        return EditorGlyphIcon::PostProcess;
    case EditorCommandId::ReloadShaders:
    case EditorCommandId::ResetAccumulation:
    case EditorCommandId::ToggleDenoiser:
    case EditorCommandId::CycleDebugView:
    case EditorCommandId::CycleIntermediateView:
    case EditorCommandId::SetDebugBeauty:
    case EditorCommandId::SetDebugDirectLighting:
    case EditorCommandId::SetDebugIndirectLighting:
    case EditorCommandId::SetDebugNormals:
    case EditorCommandId::SetDebugDepth:
    case EditorCommandId::SetDebugMotionVectors:
    case EditorCommandId::SetDebugVariance:
    case EditorCommandId::SetDebugAlbedo:
    case EditorCommandId::RenderCurrentViewport:
    case EditorCommandId::RenderImage:
    case EditorCommandId::RenderSequence:
    case EditorCommandId::StopRender:
    case EditorCommandId::OpenOutputFolder:
        return EditorGlyphIcon::Render;
    case EditorCommandId::CommandPalette:
    case EditorCommandId::ShowControls:
    case EditorCommandId::ShowRendererInfo:
        return EditorGlyphIcon::Command;
    case EditorCommandId::SaveLayout:
    case EditorCommandId::ResetLayout:
        return EditorGlyphIcon::Layout;
    case EditorCommandId::Undo:
        return EditorGlyphIcon::Undo;
    case EditorCommandId::Redo:
        return EditorGlyphIcon::Redo;
    case EditorCommandId::Exit:
        return EditorGlyphIcon::Exit;
    default:
        return EditorGlyphIcon::Entity;
    }
}

std::string menuLabelWithGlyphPadding(const char* label) {
    return editorGlyphLabel(label);
}

void drawMenuItemGlyph(EditorGlyphIcon glyph, bool enabled) {
    if (!ImGui::IsItemVisible()) {
        return;
    }
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float rowHeight = max.y - min.y;
    const float iconSize = rowHeight > 0.0f ? std::min(16.0f, rowHeight - 4.0f) : 14.0f;
    const float y = min.y + (rowHeight - iconSize) * 0.5f;
    if (!enabled) {
        editorDrawDisabledRowChrome(min, max);
    }
    const ImVec4 tint = enabled ? editorIconTint(false) : editorDisabledIconTint();
    editorDrawIconGlyph(glyph, ImVec2(min.x + 8.0f, y), ImVec2(min.x + 8.0f + iconSize, y + iconSize), ImGui::GetColorU32(tint));
}

bool commandMenuItem(EditorCommandId id, const EditorPreferences* preferences, bool enabled = true, const char* disabledReason = nullptr) {
    const EditorCommand* command = editorCommand(id);
    const std::string shortcut = editorCommandShortcutDisplay(id, preferences);
    const std::string label = menuLabelWithGlyphPadding(editorCommandName(id));
    const bool clicked = ImGui::MenuItem(label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), false, enabled);
    drawMenuItemGlyph(commandGlyph(id), enabled);
    const char* reason = !enabled && disabledReason == nullptr ? "Not available in current context" : disabledReason;
    menuItemTooltip(command != nullptr ? command->description.c_str() : nullptr, enabled ? nullptr : reason);
    return clicked;
}

std::string lowerMenuText(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool menuFilterMatches(const char* filter, const char* label) {
    if (filter == nullptr || filter[0] == '\0') {
        return true;
    }
    return lowerMenuText(label).find(lowerMenuText(filter)) != std::string::npos;
}

void drawMenuSearch(const char* id, std::array<char, 96>& filter) {
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint(id, "Search menu...", filter.data(), filter.size());
    ImGui::Separator();
}

void menuSection(const char* label) {
    ImGui::SeparatorText(label);
}

bool filteredCommandMenuItem(EditorCommandId id, const EditorPreferences* preferences, const char* filter, bool enabled = true, const char* disabledReason = nullptr) {
    return menuFilterMatches(filter, editorCommandName(id)) && commandMenuItem(id, preferences, enabled, disabledReason);
}

bool filteredMenuItem(
    const char* label,
    const char* filter,
    const char* shortcut = nullptr,
    bool selected = false,
    bool enabled = true,
    const char* disabledReason = nullptr,
    EditorGlyphIcon glyph = EditorGlyphIcon::File) {
    if (!menuFilterMatches(filter, label)) {
        return false;
    }
    const std::string decorated = menuLabelWithGlyphPadding(label);
    const bool clicked = ImGui::MenuItem(decorated.c_str(), shortcut, selected, enabled);
    drawMenuItemGlyph(glyph, enabled);
    const char* reason = !enabled && disabledReason == nullptr ? "Not available in current context" : disabledReason;
    menuItemTooltip(label, enabled ? nullptr : reason);
    return clicked;
}

void filteredToggleMenuItem(const char* label, const char* filter, bool* value, EditorGlyphIcon glyph = EditorGlyphIcon::Window) {
    if (menuFilterMatches(filter, label)) {
        const std::string decorated = menuLabelWithGlyphPadding(label);
        ImGui::MenuItem(decorated.c_str(), nullptr, value);
        drawMenuItemGlyph(glyph, true);
        menuItemTooltip(label);
    }
}

bool drawSceneTabChrome(const std::string& title) {
    const ImVec2 textSize = ImGui::CalcTextSize(title.c_str());
    const float width = std::clamp(
        textSize.x + EditorUiMetric::sceneTabExtraWidth,
        EditorUiMetric::sceneTabMinWidth,
        EditorUiMetric::sceneTabMaxWidth);
    const ImVec2 size(width, ImGui::GetFrameHeight());
    ImGui::InvisibleButton("EditorSceneTab", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 closeMin(max.x - EditorUiMetric::sceneTabCloseMinOffset, min.y + EditorUiMetric::sceneTabClosePaddingY);
    const ImVec2 closeMax(max.x - EditorUiMetric::sceneTabCloseMaxOffset, max.y - EditorUiMetric::sceneTabClosePaddingY);
    const bool closeHovered = ImGui::IsMouseHoveringRect(closeMin, closeMax);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 separator = ImGui::GetColorU32(editorSceneTabBorderColor());
    dl->AddLine(ImVec2(min.x + 1.0f, min.y + 5.0f), ImVec2(min.x + 1.0f, max.y - 5.0f), separator, 1.0f);
    dl->AddLine(ImVec2(max.x - 1.0f, min.y + 5.0f), ImVec2(max.x - 1.0f, max.y - 5.0f), separator, 1.0f);
    if (hovered) {
        dl->AddRectFilled(
            ImVec2(min.x + 4.0f, min.y + 2.0f),
            ImVec2(max.x - 4.0f, max.y - 2.0f),
            ImGui::GetColorU32(editorSceneTabBgColor(true)),
            EditorUiMetric::compactButtonRounding);
        editorDrawIconGlyph(
            EditorGlyphIcon::SceneFile,
            ImVec2(min.x + EditorUiMetric::sceneTabIconMinX, min.y + EditorUiMetric::sceneTabIconPaddingY),
            ImVec2(min.x + EditorUiMetric::sceneTabIconMaxX, max.y - EditorUiMetric::sceneTabIconPaddingY),
            ImGui::GetColorU32(editorSceneTabIconColor()));
    }
    const float textX = hovered ? min.x + EditorUiMetric::sceneTabIconMaxX + 6.0f : min.x + EditorUiMetric::sceneTabTextX;
    dl->AddText(
        ImVec2(textX, min.y + (size.y - textSize.y) * 0.5f),
        ImGui::GetColorU32(editorSceneTabTextColor()),
        title.c_str());
    if (closeHovered) {
        dl->AddRectFilled(closeMin, closeMax, ImGui::GetColorU32(editorSceneTabCloseHoverColor()), EditorUiMetric::compactButtonRounding);
    }
    if (hovered || closeHovered) {
        editorDrawIconGlyph(
            EditorGlyphIcon::Exit,
            ImVec2(closeMin.x + EditorUiMetric::sceneTabCloseIconPaddingX, closeMin.y + EditorUiMetric::sceneTabCloseIconPaddingY),
            ImVec2(closeMax.x - EditorUiMetric::sceneTabCloseIconPaddingX, closeMax.y - EditorUiMetric::sceneTabCloseIconPaddingY),
            ImGui::GetColorU32(editorSceneTabCloseIconColor()));
    }
    if (closeHovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Close scene tab");
    } else if (hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Active scene: %s", title.c_str());
    }
    return clicked && closeHovered;
}

enum class DockTabCloseTarget {
    Scene,
    Hierarchy,
    RenderSettings,
    Inspector,
    MaterialEditor,
    Content,
    Timeline,
    Log,
};

struct DockTabIconSpec {
    const char* dockId;
    EditorGlyphIcon icon;
    DockTabCloseTarget closeTarget;
};

constexpr std::array<DockTabIconSpec, 8> kDockTabIconSpecs{{
    {"Scene", EditorGlyphIcon::Sky, DockTabCloseTarget::Scene},
    {"Hierarchy", EditorGlyphIcon::Hierarchy, DockTabCloseTarget::Hierarchy},
    {"Render Settings", EditorGlyphIcon::ViewSettings, DockTabCloseTarget::RenderSettings},
    {"Inspector", EditorGlyphIcon::Details, DockTabCloseTarget::Inspector},
    {"Material Editor", EditorGlyphIcon::Material, DockTabCloseTarget::MaterialEditor},
    {"Content", EditorGlyphIcon::Folder, DockTabCloseTarget::Content},
    {"Timeline", EditorGlyphIcon::Timeline, DockTabCloseTarget::Timeline},
    {"Log", EditorGlyphIcon::List, DockTabCloseTarget::Log},
}};

void closeDockTab(const DockTabIconSpec& spec, EditorPanelVisibility& visibility, EditorRequests& requests) {
    switch (spec.closeTarget) {
    case DockTabCloseTarget::Scene:
        requests.closeScene = true;
        break;
    case DockTabCloseTarget::Hierarchy:
        visibility.sceneHierarchy = false;
        break;
    case DockTabCloseTarget::RenderSettings:
        visibility.renderSettings = false;
        break;
    case DockTabCloseTarget::Inspector:
        visibility.inspector = false;
        break;
    case DockTabCloseTarget::MaterialEditor:
        visibility.materialEditor = false;
        break;
    case DockTabCloseTarget::Content:
        visibility.assetBrowser = false;
        break;
    case DockTabCloseTarget::Timeline:
        visibility.timeline = false;
        break;
    case DockTabCloseTarget::Log:
        visibility.log = false;
        break;
    }
}

void drawDockPanelChromeOverlay(const DockTabIconSpec& spec) {
    ImGuiWindow* window = ImGui::FindWindowByID(ImHashStr(spec.dockId));
    if (window == nullptr || !window->WasActive || window->Hidden) {
        return;
    }

    ImRect rect = window->OuterRectClipped;
    if (rect.GetWidth() <= 2.0f || rect.GetHeight() <= 2.0f) {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImU32 border = ImGui::GetColorU32(editorPanelBorderColor());
    drawList->AddRect(
        rect.Min,
        rect.Max,
        border,
        0.0f,
        0,
        EditorUiMetric::dockPanelBorderThickness);

    const bool focused = ImGui::GetCurrentContext() != nullptr && ImGui::GetCurrentContext()->NavWindow == window;
    const bool visibleDockTab = window->DockTabIsVisible;
    if (focused || visibleDockTab) {
        const float accentWidth = EditorUiMetric::dockPanelActiveAccentWidth;
        drawList->AddRectFilled(
            ImVec2(rect.Min.x, rect.Min.y),
            ImVec2(rect.Min.x + accentWidth, rect.Max.y),
            ImGui::GetColorU32(editorPanelActiveAccentColor()));
    }
}

void drawDockTabIconOverlay(const DockTabIconSpec& spec, EditorPanelVisibility& visibility, EditorRequests& requests) {
    ImGuiWindow* window = ImGui::FindWindowByID(ImHashStr(spec.dockId));
    if (window == nullptr || window->DockNode == nullptr || window->DockNode->TabBar == nullptr) {
        return;
    }

    ImGuiTabBar* tabBar = window->DockNode->TabBar;
    const int frameCount = ImGui::GetFrameCount();
    for (const ImGuiTabItem& tab : tabBar->Tabs) {
        if (tab.Window != window || tab.LastFrameVisible < frameCount - 1) {
            continue;
        }

        const bool centralSection = (tab.Flags & ImGuiTabItemFlags_SectionMask_) == 0;
        const float scrollOffset = centralSection ? tabBar->ScrollingAnim : 0.0f;
        const ImVec2 tabMin(tabBar->BarRect.Min.x + IM_TRUNC(tab.Offset - scrollOffset), tabBar->BarRect.Min.y);
        const ImVec2 tabMax(tabMin.x + tab.Width, tabBar->BarRect.Max.y);
        if (tabMax.x <= tabBar->BarRect.Min.x || tabMin.x >= tabBar->BarRect.Max.x) {
            return;
        }

        const float tabHeight = tabMax.y - tabMin.y;
        const float iconSize = std::min(EditorUiMetric::dockTabIconSize, std::max(8.0f, tabHeight - 7.0f));
        const float iconX = tabMin.x + EditorUiMetric::dockTabIconPaddingX;
        const float iconY = tabMin.y + (tabHeight - iconSize) * 0.5f;
        ImVec2 clipMin = tabMin;
        ImVec2 clipMax = tabMax;
        if (centralSection) {
            clipMin.x = std::max(clipMin.x, tabBar->ScrollingRectMinX);
            clipMax.x = std::min(clipMax.x, tabBar->ScrollingRectMaxX);
        }
        if (clipMax.x <= clipMin.x) {
            return;
        }

        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        const bool active = tab.ID == tabBar->SelectedTabId || tab.ID == tabBar->VisibleTabId;
        ImVec4 tint = editorIconTint(active);
        if (!active) {
            tint.w = 0.78f;
        }
        const float closeSize = std::min(12.0f, std::max(8.0f, tabHeight - 8.0f));
        const ImVec2 closeMin(tabMax.x - closeSize - 6.0f, tabMin.y + (tabHeight - closeSize) * 0.5f);
        const ImVec2 closeMax(closeMin.x + closeSize, closeMin.y + closeSize);
        const bool closeHovered = ImGui::IsMouseHoveringRect(closeMin, closeMax, true);
        drawList->PushClipRect(clipMin, clipMax, true);
        editorDrawTablerIconGlyph(
            drawList,
            spec.icon,
            ImVec2(iconX, iconY),
            ImVec2(iconX + iconSize, iconY + iconSize),
            ImGui::GetColorU32(tint));
        if (closeHovered) {
            drawList->AddRectFilled(closeMin, closeMax, ImGui::GetColorU32(editorSceneTabCloseHoverColor()), EditorUiMetric::compactButtonRounding);
        }
        ImVec4 closeTint = editorSceneTabCloseIconColor();
        if (!active && !closeHovered) {
            closeTint.w = 0.55f;
        }
        editorDrawTablerIconGlyph(
            drawList,
            EditorGlyphIcon::Exit,
            ImVec2(closeMin.x + 1.0f, closeMin.y + 1.0f),
            ImVec2(closeMax.x - 1.0f, closeMax.y - 1.0f),
            ImGui::GetColorU32(closeTint));
        drawList->PopClipRect();
        if (closeHovered) {
            ImGui::SetTooltip("Close %s", spec.dockId);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                closeDockTab(spec, visibility, requests);
            }
        }
        return;
    }
}

} // namespace

void EditorDockspace::begin(EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests) {
    drawMainMenu(state, visibility, requests);

    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, EditorUiMetric::dockTabRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, EditorUiMetric::dockTabBorderSize);
    ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextBorderSize, EditorUiMetric::dockTabBorderSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, EditorUiMetric::dockTabBorderSize);
    ImGui::PushStyleColor(ImGuiCol_Tab, editorTabColor(false));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, editorTabColor(false, true));
    ImGui::PushStyleColor(ImGuiCol_TabActive, editorTabColor(true));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocused, ImVec4(0.075f, 0.080f, 0.090f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TabUnfocusedActive, ImVec4(0.105f, 0.115f, 0.130f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_DockingPreview, ImVec4(0.18f, 0.36f, 0.62f, 0.52f));
    ImGui::PushStyleColor(ImGuiCol_Separator, editorPanelSplitterColor());
    ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, editorPanelSplitterHoveredColor());
    ImGui::PushStyleColor(ImGuiCol_SeparatorActive, editorPanelActiveAccentColor());
    ImGui::PushStyleColor(ImGuiCol_Border, editorPanelBorderColor());
    dockChromeStylePushed_ = true;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EditorDockspaceHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    if (layoutResetRequested_) {
        buildDefaultLayout();
        layoutResetRequested_ = false;
        requests.resetLayout = false;
    }
    drawHelpWindows();
}

void EditorDockspace::end(EditorPanelVisibility& visibility, EditorRequests& requests) {
    drawDockPanelChromeOverlays();
    drawDockTabIconOverlays(visibility, requests);
    ImGui::End();
    if (dockChromeStylePushed_) {
        ImGui::PopStyleColor(10);
        ImGui::PopStyleVar(4);
        dockChromeStylePushed_ = false;
    }
}

void EditorDockspace::drawDockTabIconOverlays(EditorPanelVisibility& visibility, EditorRequests& requests) {
    for (const DockTabIconSpec& spec : kDockTabIconSpecs) {
        drawDockTabIconOverlay(spec, visibility, requests);
    }
}

void EditorDockspace::drawDockPanelChromeOverlays() {
    for (const DockTabIconSpec& spec : kDockTabIconSpecs) {
        drawDockPanelChromeOverlay(spec);
    }
}

void EditorDockspace::requestResetLayout() {
    layoutResetRequested_ = true;
}

void EditorDockspace::setProfilePath(const std::filesystem::path& scenePath) {
    std::filesystem::path next = scenePath;
    if (next.empty()) {
        return;
    }
    next.replace_extension(".layout.ini");
    if (next == profilePath_) {
        return;
    }
    profilePath_ = std::move(next);
    loadLayout();
}

void EditorDockspace::saveLayout() const {
    if (!profilePath_.empty()) {
        const std::filesystem::path parent = profilePath_.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        ImGui::SaveIniSettingsToDisk(profilePath_.string().c_str());
    }
}

void EditorDockspace::setProfileFile(const std::filesystem::path& layoutPath) {
    if (layoutPath.empty() || layoutPath == profilePath_) {
        return;
    }
    profilePath_ = layoutPath;
    loadLayout();
}

void EditorDockspace::loadLayout() {
    if (!profilePath_.empty() && std::filesystem::exists(profilePath_)) {
        ImGui::LoadIniSettingsFromDisk(profilePath_.string().c_str());
        layoutResetRequested_ = false;
    }
}

void EditorDockspace::buildDefaultLayout() {
    ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    ImGuiID rightBottom = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, EditorUiMetric::dockRightPanelRatio, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, EditorUiMetric::dockBottomPanelRatio, &bottom, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, EditorUiMetric::dockRightInspectorRatio, &rightBottom, &right);

    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Hierarchy", right);
    ImGui::DockBuilderDockWindow("Render Settings", right);
    ImGui::DockBuilderDockWindow("Inspector", rightBottom);
    ImGui::DockBuilderDockWindow("Material Editor", rightBottom);
    ImGui::DockBuilderDockWindow("Content", bottom);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockspace::executeCommand(EditorCommandId id, EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests) {
    switch (id) {
    case EditorCommandId::ProjectManager:
        requests.showProjectManager = true;
        break;
    case EditorCommandId::CloseProject:
        requests.closeProject = true;
        break;
    case EditorCommandId::NewScene:
        requests.newScene = true;
        break;
    case EditorCommandId::OpenScene:
        visibility.assetBrowser = true;
        if (auto path = openSceneJsonFileDialog()) {
            requests.openScene = *path;
        }
        break;
    case EditorCommandId::SaveScene:
        visibility.assetBrowser = true;
        if (state.scenePath != nullptr && state.scenePath->has_value()) {
            requests.saveScene = **state.scenePath;
            setProfilePath(**state.scenePath);
            saveLayout();
        } else if (auto path = saveSceneJsonFileDialog()) {
            requests.saveSceneAs = *path;
            setProfilePath(*path);
            saveLayout();
        }
        break;
    case EditorCommandId::SaveSceneAs:
        visibility.assetBrowser = true;
        if (auto path = saveSceneJsonFileDialog()) {
            requests.saveSceneAs = *path;
            setProfilePath(*path);
            saveLayout();
        }
        break;
    case EditorCommandId::ImportAsset:
        visibility.assetBrowser = true;
        if (auto path = openGltfFileDialog()) {
            requests.importAsset = EditorImportAssetRequest{.sourcePath = *path};
        }
        break;
    case EditorCommandId::ImportAndPlace:
        visibility.assetBrowser = true;
        if (auto path = openGltfFileDialog()) {
            EditorImportAssetRequest request;
            request.sourcePath = *path;
            request.mode = "ImportAndPlace";
            requests.importAndPlace = std::move(request);
        }
        break;
    case EditorCommandId::ImportSceneAsNewScene:
        visibility.assetBrowser = true;
        if (auto path = openGltfFileDialog()) {
            requests.importSceneAsNewScene = *path;
        }
        break;
    case EditorCommandId::MergeScene:
        visibility.assetBrowser = true;
        if (auto path = openGltfFileDialog()) {
            requests.mergeScene = *path;
        }
        break;
    case EditorCommandId::ImportHdri:
        visibility.assetBrowser = true;
        if (auto path = openHdrFileDialog()) {
            requests.loadHdr = *path;
        }
        break;
    case EditorCommandId::Exit:
        requests.exit = true;
        break;
    case EditorCommandId::CreateEmptyEntity:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Empty, {}};
        break;
    case EditorCommandId::CreateCamera:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Camera, {}};
        break;
    case EditorCommandId::CreatePointLight:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::Light, {}};
        break;
    case EditorCommandId::CreateSpotLight:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::SpotLight, {}};
        break;
    case EditorCommandId::CreateAreaLight:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::AreaLight, {}};
        break;
    case EditorCommandId::CreatePrimarySun:
        requests.ensurePrimarySun = true;
        break;
    case EditorCommandId::CreateEnvironmentLight:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::EnvironmentLight, {}};
        break;
    case EditorCommandId::CreateSkyAtmosphere:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::SkyAtmosphere, {}};
        break;
    case EditorCommandId::CreateHeightFog:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::HeightFog, {}};
        break;
    case EditorCommandId::CreateVolumetricCloud:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::VolumetricCloud, {}};
        break;
    case EditorCommandId::CreatePostProcessVolume:
        requests.createEntity = EditorEntityCreateRequest{EditorEntityCreateKind::PostProcessVolume, {}};
        break;
    case EditorCommandId::ReloadShaders:
        requests.reloadShaders = true;
        requests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        break;
    case EditorCommandId::ShowControls:
        showControls_ = true;
        break;
    case EditorCommandId::ShowRendererInfo:
        showRendererInfo_ = true;
        break;
    case EditorCommandId::CommandPalette:
        requests.showCommandPalette = true;
        break;
    case EditorCommandId::ResetAccumulation:
        requests.resetAccumulation = AccumulationResetReason::Manual;
        break;
    case EditorCommandId::ToggleDenoiser:
        requests.toggleDenoiser = true;
        break;
    case EditorCommandId::CycleDebugView:
        requests.toggleDebugView = true;
        break;
    case EditorCommandId::CycleIntermediateView:
        requests.cycleIntermediateView = true;
        break;
    case EditorCommandId::RenderCurrentViewport:
        requests.renderCurrentViewport = true;
        break;
    case EditorCommandId::RenderImage:
        requests.renderImage = true;
        break;
    case EditorCommandId::RenderSequence:
        requests.renderSequence = true;
        break;
    case EditorCommandId::StopRender:
        requests.stopRender = true;
        break;
    case EditorCommandId::OpenOutputFolder:
        requests.openOutputFolder = true;
        break;
    case EditorCommandId::SetDebugBeauty:
    case EditorCommandId::SetDebugDirectLighting:
    case EditorCommandId::SetDebugIndirectLighting:
    case EditorCommandId::SetDebugNormals:
    case EditorCommandId::SetDebugDepth:
    case EditorCommandId::SetDebugMotionVectors:
    case EditorCommandId::SetDebugVariance:
    case EditorCommandId::SetDebugAlbedo: {
        RendererSettings settings = state.renderer.settings();
        switch (id) {
        case EditorCommandId::SetDebugBeauty: settings.debugView = RendererDebugView::Beauty; break;
        case EditorCommandId::SetDebugDirectLighting: settings.debugView = RendererDebugView::DirectLighting; break;
        case EditorCommandId::SetDebugIndirectLighting: settings.debugView = RendererDebugView::IndirectLighting; break;
        case EditorCommandId::SetDebugNormals: settings.debugView = RendererDebugView::Normals; break;
        case EditorCommandId::SetDebugDepth: settings.debugView = RendererDebugView::Depth; break;
        case EditorCommandId::SetDebugMotionVectors: settings.debugView = RendererDebugView::MotionVectors; break;
        case EditorCommandId::SetDebugVariance: settings.debugView = RendererDebugView::Variance; break;
        case EditorCommandId::SetDebugAlbedo: settings.debugView = RendererDebugView::Albedo; break;
        default: break;
        }
        requestSettings(requests, settings);
        break;
    }
    case EditorCommandId::SaveLayout:
        requests.saveLayout = true;
        saveLayout();
        break;
    case EditorCommandId::ResetLayout:
        requests.resetLayout = true;
        requestResetLayout();
        break;
    case EditorCommandId::Undo:
        requests.undo = true;
        break;
    case EditorCommandId::Redo:
        requests.redo = true;
        break;
    default:
        break;
    }
}

void EditorDockspace::drawMainMenu(EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    static std::array<char, 96> fileSearch{};
    static std::array<char, 96> createSearch{};
    static std::array<char, 96> engineSearch{};
    static std::array<char, 96> windowSearch{};
    static std::array<char, 96> renderSearch{};
    static std::array<char, 96> layoutSearch{};
    const EditorPreferences* prefs = state.editorPrefs;
    const auto applyDefaultEditorLayout = [&]() {
        visibility.viewport = true;
        visibility.sceneHierarchy = true;
        visibility.inspector = true;
        visibility.assetBrowser = true;
        visibility.renderWorldSettings = false;
        visibility.timeline = true;
        visibility.log = true;
        visibility.console = false;
        visibility.materialEditor = false;
        visibility.renderSettings = true;
        visibility.debugProfiler = false;
        visibility.sceneStats = false;
        visibility.gpuDiagnostics = false;
        requests.resetLayout = true;
        requestResetLayout();
    };
    const auto applyContentLayout = [&]() {
        visibility.viewport = true;
        visibility.sceneHierarchy = true;
        visibility.inspector = true;
        visibility.assetBrowser = true;
        visibility.timeline = false;
        visibility.log = true;
        visibility.console = false;
        visibility.materialEditor = true;
        visibility.renderSettings = false;
        visibility.debugProfiler = false;
        visibility.sceneStats = false;
        visibility.gpuDiagnostics = false;
    };
    const auto applyLightingLayout = [&]() {
        visibility.viewport = true;
        visibility.sceneHierarchy = true;
        visibility.inspector = true;
        visibility.assetBrowser = true;
        visibility.timeline = false;
        visibility.log = true;
        visibility.console = false;
        visibility.materialEditor = false;
        visibility.renderSettings = true;
        visibility.debugProfiler = false;
        visibility.sceneStats = true;
        visibility.gpuDiagnostics = false;
    };
    const auto applyTimelineLayout = [&]() {
        visibility.viewport = true;
        visibility.sceneHierarchy = true;
        visibility.inspector = true;
        visibility.assetBrowser = true;
        visibility.timeline = true;
        visibility.log = true;
        visibility.console = false;
        visibility.materialEditor = false;
        visibility.renderSettings = false;
        visibility.debugProfiler = false;
        visibility.sceneStats = false;
        visibility.gpuDiagnostics = false;
    };
    const auto applyDebugLayout = [&]() {
        visibility.viewport = true;
        visibility.sceneHierarchy = true;
        visibility.inspector = true;
        visibility.assetBrowser = true;
        visibility.timeline = true;
        visibility.log = true;
        visibility.console = true;
        visibility.materialEditor = false;
        visibility.renderSettings = true;
        visibility.debugProfiler = true;
        visibility.sceneStats = true;
        visibility.gpuDiagnostics = true;
    };

    if (ImGui::BeginMenu("File")) {
        drawMenuSearch("##FileMenuSearch", fileSearch);
        menuSection("OPEN");
        if (filteredCommandMenuItem(EditorCommandId::NewScene, prefs, fileSearch.data())) { executeCommand(EditorCommandId::NewScene, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::OpenScene, prefs, fileSearch.data())) { executeCommand(EditorCommandId::OpenScene, state, visibility, requests); }
        filteredMenuItem("Favorite Scenes", fileSearch.data(), nullptr, false, false, "Favorite scene storage is not implemented yet.", EditorGlyphIcon::SceneFile);
        if (filteredMenuItem("Open Asset...", fileSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Folder)) {
            visibility.assetBrowser = true;
        }
        menuSection("SAVE");
        if (filteredCommandMenuItem(EditorCommandId::SaveScene, prefs, fileSearch.data())) { executeCommand(EditorCommandId::SaveScene, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SaveSceneAs, prefs, fileSearch.data())) { executeCommand(EditorCommandId::SaveSceneAs, state, visibility, requests); }
        filteredMenuItem("Save All", fileSearch.data(), nullptr, false, false, "Multi-document save is not available in this build.", EditorGlyphIcon::Save);
        filteredMenuItem("Choose Files to Save...", fileSearch.data(), nullptr, false, false, "Selective save is not available in this build.", EditorGlyphIcon::Save);
        if (filteredMenuItem("Close Scene", fileSearch.data(), nullptr, false, state.scenePath != nullptr && state.scenePath->has_value(), "No saved scene is currently open.", EditorGlyphIcon::SceneFile)) {
            requests.closeScene = true;
        }
        menuSection("EDIT");
        if (menuFilterMatches(fileSearch.data(), "Undo")) {
            const std::string undoLabel = state.undoStack != nullptr && state.undoStack->canUndo()
                ? std::string("Undo ") + state.undoStack->undoLabel()
                : std::string("Undo");
            const std::string undoShortcut = editorCommandShortcutDisplay(EditorCommandId::Undo, prefs);
            const bool undoEnabled = state.undoStack != nullptr && state.undoStack->canUndo();
            const std::string undoMenuLabel = menuLabelWithGlyphPadding(undoLabel.c_str());
            if (ImGui::MenuItem(undoMenuLabel.c_str(), undoShortcut.empty() ? nullptr : undoShortcut.c_str(), false, undoEnabled)) {
                executeCommand(EditorCommandId::Undo, state, visibility, requests);
            }
            drawMenuItemGlyph(EditorGlyphIcon::Undo, undoEnabled);
        }
        if (menuFilterMatches(fileSearch.data(), "Redo")) {
            const std::string redoLabel = state.undoStack != nullptr && state.undoStack->canRedo()
                ? std::string("Redo ") + state.undoStack->redoLabel()
                : std::string("Redo");
            const std::string redoShortcut = editorCommandShortcutDisplay(EditorCommandId::Redo, prefs);
            const bool redoEnabled = state.undoStack != nullptr && state.undoStack->canRedo();
            const std::string redoMenuLabel = menuLabelWithGlyphPadding(redoLabel.c_str());
            if (ImGui::MenuItem(redoMenuLabel.c_str(), redoShortcut.empty() ? nullptr : redoShortcut.c_str(), false, redoEnabled)) {
                executeCommand(EditorCommandId::Redo, state, visibility, requests);
            }
            drawMenuItemGlyph(EditorGlyphIcon::Redo, redoEnabled);
        }
        menuSection("IMPORT / EXPORT");
        if (filteredCommandMenuItem(EditorCommandId::ImportAsset, prefs, fileSearch.data())) { executeCommand(EditorCommandId::ImportAsset, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ImportAndPlace, prefs, fileSearch.data())) { executeCommand(EditorCommandId::ImportAndPlace, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ImportSceneAsNewScene, prefs, fileSearch.data())) { executeCommand(EditorCommandId::ImportSceneAsNewScene, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::MergeScene, prefs, fileSearch.data())) { executeCommand(EditorCommandId::MergeScene, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ImportHdri, prefs, fileSearch.data())) { executeCommand(EditorCommandId::ImportHdri, state, visibility, requests); }
        filteredMenuItem("Import Texture", fileSearch.data(), nullptr, false, false, "Texture asset import is routed through the Content browser import pipeline.", EditorGlyphIcon::Texture);
        filteredMenuItem("Import IES Profile", fileSearch.data(), nullptr, false, false, "IES profile import storage is not implemented yet.", EditorGlyphIcon::IesProfile);
        filteredMenuItem("Export All...", fileSearch.data(), nullptr, false, false, "Scene export is not implemented yet.", EditorGlyphIcon::SceneFile);
        filteredMenuItem("Export Selected...", fileSearch.data(), nullptr, false, false, "Select an entity or asset after scene export support is implemented.", EditorGlyphIcon::Entity);
        menuSection("PROJECT");
        if (filteredCommandMenuItem(EditorCommandId::ProjectManager, prefs, fileSearch.data())) { executeCommand(EditorCommandId::ProjectManager, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CloseProject, prefs, fileSearch.data(), state.project != nullptr, "No project is currently open.")) { executeCommand(EditorCommandId::CloseProject, state, visibility, requests); }
        filteredMenuItem("Project Settings", fileSearch.data(), nullptr, false, false, "Project settings are edited from the Project Manager in this build.", EditorGlyphIcon::ProjectFile);
        filteredMenuItem("Zip Project", fileSearch.data(), nullptr, false, false, "Project packaging is not wired to the editor shell yet.", EditorGlyphIcon::ProjectFile);
        filteredMenuItem("Open Current Project Directory", fileSearch.data(), nullptr, false, false, "Project directory reveal is not wired to this menu yet.", EditorGlyphIcon::Folder);
        filteredMenuItem("Recent Projects", fileSearch.data(), nullptr, false, false, "Recent projects are shown in the Project Manager.", EditorGlyphIcon::ProjectFile);
        menuSection("APPLICATION");
        if (filteredCommandMenuItem(EditorCommandId::Exit, prefs, fileSearch.data())) { executeCommand(EditorCommandId::Exit, state, visibility, requests); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create")) {
        drawMenuSearch("##CreateMenuSearch", createSearch);
        menuSection("ENTITY");
        if (filteredCommandMenuItem(EditorCommandId::CreateEmptyEntity, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateEmptyEntity, state, visibility, requests); }
        if (filteredMenuItem("Folder / Group", createSearch.data(), nullptr, false, false, "Scene folder/group authoring is not available in this build.", EditorGlyphIcon::Group)) {}
        menuSection("3D OBJECT");
        if (filteredMenuItem("Cube", createSearch.data(), nullptr, false, false, "Primitive mesh creation is not available in this build.", EditorGlyphIcon::Model)) {}
        if (filteredMenuItem("Sphere", createSearch.data(), nullptr, false, false, "Primitive mesh creation is not available in this build.", EditorGlyphIcon::Model)) {}
        if (filteredMenuItem("Plane", createSearch.data(), nullptr, false, false, "Primitive mesh creation is not available in this build.", EditorGlyphIcon::Model)) {}
        if (filteredMenuItem("Cylinder", createSearch.data(), nullptr, false, false, "Primitive mesh creation is not available in this build.", EditorGlyphIcon::Model)) {}
        if (filteredMenuItem("Cone", createSearch.data(), nullptr, false, false, "Primitive mesh creation is not available in this build.", EditorGlyphIcon::Model)) {}
        if (filteredMenuItem("Mesh From Asset", createSearch.data(), nullptr, false, false, "Select a mesh or prefab in Content and place it from the asset actions.", EditorGlyphIcon::Model)) {}
        menuSection("LIGHT");
        if (filteredCommandMenuItem(EditorCommandId::CreatePrimarySun, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreatePrimarySun, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreatePointLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreatePointLight, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateSpotLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateSpotLight, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateAreaLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateAreaLight, state, visibility, requests); }
        if (filteredMenuItem("Disk Area Light", createSearch.data(), nullptr, false, false, "Disk area light shape is not available in this build.", EditorGlyphIcon::Light)) {}
        if (filteredMenuItem("Sphere Light", createSearch.data(), nullptr, false, false, "Sphere light shape is not available in this build.", EditorGlyphIcon::Light)) {}
        if (filteredMenuItem("Emissive Mesh Light", createSearch.data(), nullptr, false, false, "Emissive mesh light authoring is not available in this build.", EditorGlyphIcon::Light)) {}
        menuSection("CAMERA");
        if (filteredCommandMenuItem(EditorCommandId::CreateCamera, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateCamera, state, visibility, requests); }
        if (filteredMenuItem("Cine Camera", createSearch.data(), nullptr, false, false, "Cinematic camera actor storage is not available in this build.", EditorGlyphIcon::Camera)) {}
        menuSection("ENVIRONMENT");
        if (filteredCommandMenuItem(EditorCommandId::CreateEnvironmentLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateEnvironmentLight, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateSkyAtmosphere, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateSkyAtmosphere, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateHeightFog, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateHeightFog, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateVolumetricCloud, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateVolumetricCloud, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreatePostProcessVolume, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreatePostProcessVolume, state, visibility, requests); }
        menuSection("ASSET");
        if (filteredMenuItem("Material", createSearch.data(), nullptr, false, false, "Material asset creation is not available from this menu yet.", EditorGlyphIcon::Material)) {}
        if (filteredMenuItem("Material Instance", createSearch.data(), nullptr, false, false, "Material instance asset creation is not available from this menu yet.", EditorGlyphIcon::Material)) {}
        if (filteredMenuItem("Prefab From Selection", createSearch.data(), nullptr, false, false, "Prefab authoring is not available in this build.", EditorGlyphIcon::Group)) {}
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Engine")) {
        drawMenuSearch("##EngineMenuSearch", engineSearch);
        menuSection("SETTINGS");
        filteredMenuItem("Project Settings...", engineSearch.data(), nullptr, false, false, "Project settings are edited from the Project Manager in this build.", EditorGlyphIcon::ProjectFile);
        filteredMenuItem("Editor Preferences...", engineSearch.data(), nullptr, false, false, "Editor preferences are edited from the Project Manager preferences view.", EditorGlyphIcon::ConfigFile);
        filteredMenuItem("Engine Settings...", engineSearch.data(), nullptr, false, false, "Engine settings are not exposed as an editor panel yet.", EditorGlyphIcon::ConfigFile);
        menuSection("ASSET REGISTRY");
        filteredMenuItem("Rebuild Asset Registry", engineSearch.data(), nullptr, false, false, "Asset registry rebuild is not wired to the top menu yet.", EditorGlyphIcon::Refresh);
        filteredMenuItem("Validate Asset References", engineSearch.data(), nullptr, false, false, "Use the asset registry validator script from the command line for now.", EditorGlyphIcon::Command);
        menuSection("CACHE");
        filteredMenuItem("Clear Derived Data Cache...", engineSearch.data(), nullptr, false, false, "Derived data cache clearing is not wired to the editor shell yet.", EditorGlyphIcon::Trash);
        filteredMenuItem("Open Cache Directory", engineSearch.data(), nullptr, false, false, "Cache directory reveal is not wired to this menu yet.", EditorGlyphIcon::Folder);
        menuSection("VALIDATION");
        filteredMenuItem("Run Validation Suite", engineSearch.data(), nullptr, false, false, "Use the validation scripts from the command line; in-editor launch is pending.", EditorGlyphIcon::Command);
        filteredMenuItem("Run Current Scene Checks", engineSearch.data(), nullptr, false, false, "Current-scene validation is not wired to the editor shell yet.", EditorGlyphIcon::SceneFile);
        menuSection("DIAGNOSTICS");
        filteredMenuItem("Open Log Folder", engineSearch.data(), nullptr, false, false, "The editor log directory command is not wired yet.", EditorGlyphIcon::Folder);
        filteredMenuItem("Open Debug Package Folder", engineSearch.data(), nullptr, false, false, "Debug package folder reveal is available from generated debug-package notifications.", EditorGlyphIcon::Folder);
        filteredMenuItem("Copy System Info", engineSearch.data(), nullptr, false, false, "System info clipboard export is not wired to the editor shell yet.", EditorGlyphIcon::Command);
        if (filteredCommandMenuItem(EditorCommandId::CommandPalette, prefs, engineSearch.data())) { executeCommand(EditorCommandId::CommandPalette, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ShowControls, prefs, engineSearch.data())) { executeCommand(EditorCommandId::ShowControls, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ShowRendererInfo, prefs, engineSearch.data())) { executeCommand(EditorCommandId::ShowRendererInfo, state, visibility, requests); }
        menuSection("DEVELOPER");
        if (filteredCommandMenuItem(EditorCommandId::ReloadShaders, prefs, engineSearch.data())) { executeCommand(EditorCommandId::ReloadShaders, state, visibility, requests); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        drawMenuSearch("##WindowMenuSearch", windowSearch);
        menuSection("PANELS");
        filteredToggleMenuItem("Hierarchy", windowSearch.data(), &visibility.sceneHierarchy, EditorGlyphIcon::Group);
        filteredToggleMenuItem("Inspector", windowSearch.data(), &visibility.inspector, EditorGlyphIcon::Details);
        filteredToggleMenuItem("Content", windowSearch.data(), &visibility.assetBrowser, EditorGlyphIcon::Folder);
        filteredToggleMenuItem("Timeline", windowSearch.data(), &visibility.timeline, EditorGlyphIcon::TimelineKey);
        filteredToggleMenuItem("Log", windowSearch.data(), &visibility.log, EditorGlyphIcon::File);
        filteredToggleMenuItem("Render World Settings", windowSearch.data(), &visibility.renderSettings, EditorGlyphIcon::Render);
        filteredToggleMenuItem("Scene", windowSearch.data(), &visibility.viewport, EditorGlyphIcon::SceneFile);
        filteredToggleMenuItem("Material Editor", windowSearch.data(), &visibility.materialEditor, EditorGlyphIcon::Material);
        filteredToggleMenuItem("Console", windowSearch.data(), &visibility.console, EditorGlyphIcon::Command);
        menuSection("DEBUG / ADVANCED");
        filteredToggleMenuItem("Debug / Profiler", windowSearch.data(), &visibility.debugProfiler, EditorGlyphIcon::Stats);
        filteredToggleMenuItem("Scene Stats", windowSearch.data(), &visibility.sceneStats, EditorGlyphIcon::Stats);
        filteredToggleMenuItem("GPU Diagnostics", windowSearch.data(), &visibility.gpuDiagnostics, EditorGlyphIcon::Stats);
        filteredMenuItem("Floating Render Controls", windowSearch.data(), nullptr, false, false, "Floating render controls are not implemented yet; use the Render menu and viewport strip.", EditorGlyphIcon::Render);
        menuSection("LAYOUT");
        if (filteredCommandMenuItem(EditorCommandId::SaveLayout, prefs, windowSearch.data())) { executeCommand(EditorCommandId::SaveLayout, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ResetLayout, prefs, windowSearch.data())) { executeCommand(EditorCommandId::ResetLayout, state, visibility, requests); }
        filteredMenuItem("Load Layout...", windowSearch.data(), nullptr, false, false, "Named layout loading is not implemented yet.", EditorGlyphIcon::Layout);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        drawMenuSearch("##RenderMenuSearch", renderSearch);
        menuSection("OUTPUT");
        if (filteredCommandMenuItem(EditorCommandId::RenderCurrentViewport, prefs, renderSearch.data())) { executeCommand(EditorCommandId::RenderCurrentViewport, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::RenderImage, prefs, renderSearch.data())) { executeCommand(EditorCommandId::RenderImage, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::RenderSequence, prefs, renderSearch.data())) { executeCommand(EditorCommandId::RenderSequence, state, visibility, requests); }
        filteredMenuItem("Pause / Resume Render", renderSearch.data(), nullptr, false, false, "Pause/resume render jobs are not available in this build.", EditorGlyphIcon::Pause);
        if (filteredCommandMenuItem(EditorCommandId::StopRender, prefs, renderSearch.data())) { executeCommand(EditorCommandId::StopRender, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::OpenOutputFolder, prefs, renderSearch.data())) { executeCommand(EditorCommandId::OpenOutputFolder, state, visibility, requests); }
        filteredMenuItem("Screenshot", renderSearch.data(), nullptr, false, false, "Viewport screenshot capture is not wired to this menu yet.", EditorGlyphIcon::Camera);
        filteredMenuItem("High Resolution Render", renderSearch.data(), nullptr, false, false, "High-resolution tiled rendering is not available in this build.", EditorGlyphIcon::Render);
        menuSection("PREVIEW");
        if (filteredCommandMenuItem(EditorCommandId::ResetAccumulation, prefs, renderSearch.data())) { executeCommand(EditorCommandId::ResetAccumulation, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ToggleDenoiser, prefs, renderSearch.data())) { executeCommand(EditorCommandId::ToggleDenoiser, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CycleDebugView, prefs, renderSearch.data())) { executeCommand(EditorCommandId::CycleDebugView, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CycleIntermediateView, prefs, renderSearch.data())) { executeCommand(EditorCommandId::CycleIntermediateView, state, visibility, requests); }
        filteredToggleMenuItem("Render World Settings", renderSearch.data(), &visibility.renderSettings, EditorGlyphIcon::Render);
        filteredMenuItem("Quality Preset", renderSearch.data(), nullptr, false, false, "Use the Render World Settings panel for preset changes.", EditorGlyphIcon::Render);
        menuSection("DIAGNOSTICS");
        filteredMenuItem("Capture RenderDoc", renderSearch.data(), nullptr, false, false, "RenderDoc capture remains available through the existing runtime capture path; top-menu launch is pending.", EditorGlyphIcon::Render);
        filteredMenuItem("Export Debug Views", renderSearch.data(), nullptr, false, false, "Use headless/debug package export until in-editor export is wired.", EditorGlyphIcon::DrawDebug);
        filteredMenuItem("Export Debug Package", renderSearch.data(), nullptr, false, false, "Use the debug package command-line export until in-editor export is wired.", EditorGlyphIcon::Folder);
        filteredMenuItem("Dump RenderGraph", renderSearch.data(), nullptr, false, false, "RenderGraph dump is available through headless diagnostics for now.", EditorGlyphIcon::ConfigFile);
        filteredMenuItem("Profile Current Scene", renderSearch.data(), nullptr, false, false, "Profiling export is available through headless diagnostics for now.", EditorGlyphIcon::Stats);
        menuSection("DEBUG VIEWS");
        if (filteredCommandMenuItem(EditorCommandId::SetDebugBeauty, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugBeauty, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugDirectLighting, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugDirectLighting, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugIndirectLighting, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugIndirectLighting, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugNormals, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugNormals, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugDepth, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugDepth, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugMotionVectors, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugMotionVectors, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugVariance, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugVariance, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugAlbedo, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugAlbedo, state, visibility, requests); }
        filteredMenuItem("View Mode", renderSearch.data(), nullptr, false, false, "Viewport view-mode switching is exposed through Draw Debug for now.", EditorGlyphIcon::DrawDebug);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Layout")) {
        drawMenuSearch("##LayoutMenuSearch", layoutSearch);
        menuSection("WORKSPACE");
        if (filteredMenuItem("Default Editor", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Layout)) { applyDefaultEditorLayout(); }
        if (filteredMenuItem("Content Editing", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Folder)) { applyContentLayout(); }
        if (filteredMenuItem("Lighting", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Light)) { applyLightingLayout(); }
        if (filteredMenuItem("Animation / Timeline", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::TimelineKey)) { applyTimelineLayout(); }
        if (filteredMenuItem("Debug / Profiling", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Stats)) { applyDebugLayout(); }
        menuSection("LAYOUT FILES");
        if (filteredCommandMenuItem(EditorCommandId::SaveLayout, prefs, layoutSearch.data())) { executeCommand(EditorCommandId::SaveLayout, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ResetLayout, prefs, layoutSearch.data())) { executeCommand(EditorCommandId::ResetLayout, state, visibility, requests); }
        filteredMenuItem("Manage Layouts...", layoutSearch.data(), nullptr, false, false, "Named layout management is not implemented yet.", EditorGlyphIcon::Layout);
        menuSection("APPEARANCE");
        filteredMenuItem("UI Scale", layoutSearch.data(), nullptr, false, false, "UI scale is edited from Project Manager preferences.", EditorGlyphIcon::Layout);
        filteredMenuItem("Theme", layoutSearch.data(), nullptr, false, false, "Theme is edited from Project Manager preferences.", EditorGlyphIcon::Layout);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (drawSceneTabChrome(activeSceneTitle(state))) {
        requests.closeScene = true;
    }
    const float fps = state.cpuFrameMs > 0.0f ? 1000.0f / state.cpuFrameMs : 0.0f;
    const char* fmt = fps > 0.0f ? "fps: %.0f | Ms: %.0f" : "fps: -- | Ms: %.0f";
    const float rightWidth = 122.0f;
    const float availX = ImGui::GetContentRegionAvail().x;
    if (availX > rightWidth) {
        ImGui::SameLine(ImGui::GetCursorPosX() + availX - rightWidth);
    } else {
        ImGui::SameLine();
    }
    ImGui::TextDisabled(fmt, fps, state.cpuFrameMs);

    ImGui::EndMainMenuBar();
}

void EditorDockspace::drawHelpWindows() {
    if (showControls_) {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls", &showControls_)) {
            std::string currentCategory;
            bool categoryOpen = false;
            for (const KeyBinding& binding : allKeyBindings()) {
                if (binding.category != currentCategory) {
                    currentCategory = binding.category;
                    categoryOpen = ImGui::CollapsingHeader(currentCategory.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                }
                if (categoryOpen) {
                    ImGui::BulletText("%s: %s", binding.key.c_str(), binding.description.c_str());
                }
            }
        }
        ImGui::End();
    }
    if (showRendererInfo_) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 160.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Renderer Info", &showRendererInfo_)) {
            ImGui::TextUnformatted("Hardware RT path tracing, temporal denoising, debug views, glTF loading, HDR environments, and GPU profiling are owned by the existing renderer.");
            ImGui::TextUnformatted("The editor layer submits requests and displays renderer state without replacing the render pipeline.");
        }
        ImGui::End();
    }
}

} // namespace rtv
