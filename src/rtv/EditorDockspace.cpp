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

bool assetRegistryDirty(const EditorRuntimeState& state) {
    return state.assetRegistry != nullptr && state.assetRegistry->dirty();
}

std::string editorSaveStateLabel(const EditorRuntimeState& state) {
    const bool registryDirty = assetRegistryDirty(state);
    if (!state.sceneDirty && !state.projectSettingsDirty && !registryDirty) {
        return "Saved";
    }
    std::string label = "Dirty: ";
    bool any = false;
    const auto append = [&](const char* value) {
        if (any) {
            label += ", ";
        }
        label += value;
        any = true;
    };
    if (state.sceneDirty) {
        append("Level");
    }
    if (state.projectSettingsDirty) {
        append("Project");
    }
    if (registryDirty) {
        append("Registry");
    }
    return label;
}

void drawEditorSaveStateLabel(const EditorRuntimeState& state, const std::string& label) {
    const bool dirty = state.sceneDirty || state.projectSettingsDirty || assetRegistryDirty(state);
    const ImVec4 color = dirty ? ImVec4(0.95f, 0.70f, 0.25f, 1.0f) : ImVec4(0.55f, 0.75f, 0.58f, 1.0f);
    ImGui::TextColored(color, "%s", label.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "Level: %s\nProject Settings: %s\nAsset Registry: %s",
            state.sceneDirty ? "dirty" : "saved",
            state.projectSettingsDirty ? "dirty" : "saved",
            assetRegistryDirty(state) ? "dirty" : "saved");
    }
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
    case EditorCommandId::ProjectSettings:
        return EditorGlyphIcon::ProjectFile;
    case EditorCommandId::NewScene:
    case EditorCommandId::OpenScene:
    case EditorCommandId::SaveScene:
    case EditorCommandId::SaveSceneAs:
        return EditorGlyphIcon::SceneFile;
    case EditorCommandId::SaveMaterial:
        return EditorGlyphIcon::Material;
    case EditorCommandId::OpenProjectDirectory:
    case EditorCommandId::OpenLogFolder:
    case EditorCommandId::OpenAsset:
        return EditorGlyphIcon::Folder;
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
    case EditorCommandId::JobCenter:
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
    const float iconSize = editorIconSizeForRow(rowHeight);
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
    EditorGlyphIcon glyph = EditorGlyphIcon::File,
    const char* description = nullptr) {
    if (!menuFilterMatches(filter, label)) {
        return false;
    }
    const std::string decorated = menuLabelWithGlyphPadding(label);
    const bool clicked = ImGui::MenuItem(decorated.c_str(), shortcut, selected, enabled);
    drawMenuItemGlyph(glyph, enabled);
    const char* reason = !enabled && disabledReason == nullptr ? "Not available in current context" : disabledReason;
    menuItemTooltip(description != nullptr ? description : label, enabled ? nullptr : reason);
    return clicked;
}

bool filteredPlaceholderMenuItem(const char* label, const char* filter, EditorGlyphIcon glyph = EditorGlyphIcon::File) {
    const EditorCommandPlaceholder* placeholder = editorCommandPlaceholder(label);
    return filteredMenuItem(
        label,
        filter,
        nullptr,
        false,
        false,
        placeholder != nullptr ? placeholder->disabledReason.c_str() : "Not available in this build.",
        glyph,
        placeholder != nullptr ? placeholder->description.c_str() : label);
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
    Content,
    Timeline,
    Log,
};

struct DockTabIconSpec {
    const char* dockId;
    EditorGlyphIcon icon;
    DockTabCloseTarget closeTarget;
};

constexpr std::array<DockTabIconSpec, 7> kDockTabIconSpecs{{
    {"Scene", EditorGlyphIcon::Sky, DockTabCloseTarget::Scene},
    {"Hierarchy", EditorGlyphIcon::Hierarchy, DockTabCloseTarget::Hierarchy},
    {"Render Settings", EditorGlyphIcon::ViewSettings, DockTabCloseTarget::RenderSettings},
    {"Inspector", EditorGlyphIcon::Details, DockTabCloseTarget::Inspector},
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

    ImDrawList* drawList = window->DrawList;
    if (drawList == nullptr) {
        return;
    }
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

        ImDrawList* drawList = window->DrawList;
        if (drawList == nullptr) {
            return;
        }
        const bool active = tab.ID == tabBar->SelectedTabId || tab.ID == tabBar->VisibleTabId;
        ImVec4 tint = editorIconTint(active);
        if (!active) {
            tint.w = 0.78f;
        }
        const float closeSize = std::min(12.0f, std::max(8.0f, tabHeight - 8.0f));
        const ImVec2 closeMin(tabMax.x - closeSize - 6.0f, tabMin.y + (tabHeight - closeSize) * 0.5f);
        const ImVec2 closeMax(closeMin.x + closeSize, closeMin.y + closeSize);
        const ImGuiContext* context = ImGui::GetCurrentContext();
        const bool popupOpen = context != nullptr && context->OpenPopupStack.Size > 0;
        const bool closeHovered = !popupOpen && ImGui::IsMouseHoveringRect(closeMin, closeMax, true);
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
    if (requests.showControls) {
        showControls_ = true;
        requests.showControls = false;
    }
    if (requests.showRendererInfo) {
        showRendererInfo_ = true;
        requests.showRendererInfo = false;
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
    case EditorCommandId::ProjectSettings:
        requests.showProjectSettings = true;
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
    case EditorCommandId::SaveAll:
        requests.saveAll = true;
        saveLayout();
        break;
    case EditorCommandId::OpenProjectDirectory:
        requests.openProjectDirectory = true;
        break;
    case EditorCommandId::OpenLogFolder:
        requests.openLogFolder = true;
        break;
    case EditorCommandId::OpenAsset:
        visibility.assetBrowser = true;
        requests.openSelectedAsset = true;
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
    case EditorCommandId::JobCenter:
        visibility.jobCenter = true;
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
    case EditorCommandId::ToggleMovingDenoiser: {
        RendererSettings settings = state.renderer.settings();
        settings.denoiseWhileMoving = !settings.denoiseWhileMoving;
        requestSettings(requests, settings);
        break;
    }
    case EditorCommandId::ToggleSun:
        requests.togglePrimarySun = true;
        break;
    case EditorCommandId::ToggleEnvironment: {
        RendererSettings settings = state.renderer.settings();
        settings.environmentEnabled = !settings.environmentEnabled;
        requestSettings(requests, settings);
        break;
    }
    case EditorCommandId::ToggleDirectLighting: {
        RendererSettings settings = state.renderer.settings();
        settings.directLightingEnabled = !settings.directLightingEnabled;
        requestSettings(requests, settings);
        break;
    }
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
    case EditorCommandId::Screenshot:
        requests.renderCurrentViewport = true;
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
    case EditorCommandId::SetToneMapperLinear:
    case EditorCommandId::SetToneMapperReinhard:
    case EditorCommandId::SetToneMapperAces:
    case EditorCommandId::SetToneMapperPbrNeutral:
    case EditorCommandId::SetToneMapperAgx: {
        RendererSettings settings = state.renderer.settings();
        switch (id) {
        case EditorCommandId::SetToneMapperLinear: settings.toneMapper = ToneMapper::Linear; break;
        case EditorCommandId::SetToneMapperReinhard: settings.toneMapper = ToneMapper::Reinhard; break;
        case EditorCommandId::SetToneMapperAces: settings.toneMapper = ToneMapper::ACES; break;
        case EditorCommandId::SetToneMapperPbrNeutral: settings.toneMapper = ToneMapper::PBRNeutral; break;
        case EditorCommandId::SetToneMapperAgx: settings.toneMapper = ToneMapper::AgX; break;
        default: break;
        }
        requestSettings(requests, settings);
        break;
    }
    case EditorCommandId::ToggleAutoExposure: {
        RendererSettings settings = state.renderer.settings();
        settings.autoExposureEnabled = !settings.autoExposureEnabled;
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
    case EditorCommandId::ToggleFullscreen:
        requests.toggleFullscreen = true;
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
    const auto applyRuntimeViewerLayout = [&]() {
        visibility.viewport = true;
        visibility.sceneHierarchy = false;
        visibility.inspector = false;
        visibility.assetBrowser = false;
        visibility.timeline = false;
        visibility.log = false;
        visibility.console = false;
        visibility.materialEditor = false;
        visibility.renderSettings = true;
        visibility.debugProfiler = false;
        visibility.sceneStats = false;
        visibility.gpuDiagnostics = false;
        visibility.renderWorldSettings = false;
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
        filteredPlaceholderMenuItem("Favorite Scenes", fileSearch.data(), EditorGlyphIcon::SceneFile);
        if (filteredCommandMenuItem(EditorCommandId::OpenAsset, prefs, fileSearch.data())) { executeCommand(EditorCommandId::OpenAsset, state, visibility, requests); }
        menuSection("SAVE");
        if (filteredCommandMenuItem(EditorCommandId::SaveScene, prefs, fileSearch.data())) { executeCommand(EditorCommandId::SaveScene, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SaveSceneAs, prefs, fileSearch.data())) { executeCommand(EditorCommandId::SaveSceneAs, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SaveAll, prefs, fileSearch.data())) { executeCommand(EditorCommandId::SaveAll, state, visibility, requests); }
        filteredPlaceholderMenuItem("Choose Files to Save...", fileSearch.data(), EditorGlyphIcon::Save);
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
        filteredPlaceholderMenuItem("Import Texture", fileSearch.data(), EditorGlyphIcon::Texture);
        filteredPlaceholderMenuItem("Import IES Profile", fileSearch.data(), EditorGlyphIcon::IesProfile);
        filteredPlaceholderMenuItem("Export All...", fileSearch.data(), EditorGlyphIcon::SceneFile);
        filteredPlaceholderMenuItem("Export Selected...", fileSearch.data(), EditorGlyphIcon::Entity);
        menuSection("PROJECT");
        if (filteredCommandMenuItem(EditorCommandId::ProjectManager, prefs, fileSearch.data())) { executeCommand(EditorCommandId::ProjectManager, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CloseProject, prefs, fileSearch.data(), state.project != nullptr, "No project is currently open.")) { executeCommand(EditorCommandId::CloseProject, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ProjectSettings, prefs, fileSearch.data(), state.project != nullptr, "No project is currently open.")) { executeCommand(EditorCommandId::ProjectSettings, state, visibility, requests); }
        filteredPlaceholderMenuItem("Zip Project", fileSearch.data(), EditorGlyphIcon::ProjectFile);
        if (filteredCommandMenuItem(EditorCommandId::OpenProjectDirectory, prefs, fileSearch.data(), state.project != nullptr, "No project is currently open.")) { executeCommand(EditorCommandId::OpenProjectDirectory, state, visibility, requests); }
        filteredPlaceholderMenuItem("Recent Projects", fileSearch.data(), EditorGlyphIcon::ProjectFile);
        menuSection("APPLICATION");
        if (filteredCommandMenuItem(EditorCommandId::Exit, prefs, fileSearch.data())) { executeCommand(EditorCommandId::Exit, state, visibility, requests); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create")) {
        drawMenuSearch("##CreateMenuSearch", createSearch);
        menuSection("ENTITY");
        if (filteredCommandMenuItem(EditorCommandId::CreateEmptyEntity, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateEmptyEntity, state, visibility, requests); }
        if (filteredPlaceholderMenuItem("Folder / Group", createSearch.data(), EditorGlyphIcon::Group)) {}
        menuSection("3D OBJECT");
        if (filteredPlaceholderMenuItem("Cube", createSearch.data(), EditorGlyphIcon::Model)) {}
        if (filteredPlaceholderMenuItem("Sphere", createSearch.data(), EditorGlyphIcon::Model)) {}
        if (filteredPlaceholderMenuItem("Plane", createSearch.data(), EditorGlyphIcon::Model)) {}
        if (filteredPlaceholderMenuItem("Cylinder", createSearch.data(), EditorGlyphIcon::Model)) {}
        if (filteredPlaceholderMenuItem("Cone", createSearch.data(), EditorGlyphIcon::Model)) {}
        if (filteredPlaceholderMenuItem("Mesh From Asset", createSearch.data(), EditorGlyphIcon::Model)) {}
        menuSection("LIGHT");
        if (filteredCommandMenuItem(EditorCommandId::CreatePrimarySun, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreatePrimarySun, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreatePointLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreatePointLight, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateSpotLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateSpotLight, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateAreaLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateAreaLight, state, visibility, requests); }
        if (filteredPlaceholderMenuItem("Disk Area Light", createSearch.data(), EditorGlyphIcon::Light)) {}
        if (filteredPlaceholderMenuItem("Sphere Light", createSearch.data(), EditorGlyphIcon::Light)) {}
        if (filteredPlaceholderMenuItem("Emissive Mesh Light", createSearch.data(), EditorGlyphIcon::Light)) {}
        menuSection("CAMERA");
        if (filteredCommandMenuItem(EditorCommandId::CreateCamera, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateCamera, state, visibility, requests); }
        if (filteredPlaceholderMenuItem("Cine Camera", createSearch.data(), EditorGlyphIcon::Camera)) {}
        menuSection("ENVIRONMENT");
        if (filteredCommandMenuItem(EditorCommandId::CreateEnvironmentLight, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateEnvironmentLight, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateSkyAtmosphere, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateSkyAtmosphere, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateHeightFog, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateHeightFog, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreateVolumetricCloud, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreateVolumetricCloud, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CreatePostProcessVolume, prefs, createSearch.data())) { executeCommand(EditorCommandId::CreatePostProcessVolume, state, visibility, requests); }
        menuSection("ASSET");
        if (filteredPlaceholderMenuItem("Material", createSearch.data(), EditorGlyphIcon::Material)) {}
        if (filteredPlaceholderMenuItem("Material Instance", createSearch.data(), EditorGlyphIcon::Material)) {}
        if (filteredPlaceholderMenuItem("Prefab From Selection", createSearch.data(), EditorGlyphIcon::Group)) {}
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Engine")) {
        drawMenuSearch("##EngineMenuSearch", engineSearch);
        menuSection("SETTINGS");
        if (filteredCommandMenuItem(EditorCommandId::ProjectSettings, prefs, engineSearch.data(), state.project != nullptr, "No project is currently open.")) { executeCommand(EditorCommandId::ProjectSettings, state, visibility, requests); }
        filteredPlaceholderMenuItem("Editor Preferences...", engineSearch.data(), EditorGlyphIcon::ConfigFile);
        filteredPlaceholderMenuItem("Engine Settings...", engineSearch.data(), EditorGlyphIcon::ConfigFile);
        menuSection("ASSET REGISTRY");
        filteredPlaceholderMenuItem("Rebuild Asset Registry", engineSearch.data(), EditorGlyphIcon::Refresh);
        filteredPlaceholderMenuItem("Validate Asset References", engineSearch.data(), EditorGlyphIcon::Command);
        menuSection("CACHE");
        filteredPlaceholderMenuItem("Clear Derived Data Cache...", engineSearch.data(), EditorGlyphIcon::Trash);
        filteredPlaceholderMenuItem("Open Cache Directory", engineSearch.data(), EditorGlyphIcon::Folder);
        menuSection("VALIDATION");
        filteredPlaceholderMenuItem("Run Validation Suite", engineSearch.data(), EditorGlyphIcon::Command);
        filteredPlaceholderMenuItem("Run Current Scene Checks", engineSearch.data(), EditorGlyphIcon::SceneFile);
        menuSection("DIAGNOSTICS");
        if (filteredCommandMenuItem(EditorCommandId::OpenLogFolder, prefs, engineSearch.data())) { executeCommand(EditorCommandId::OpenLogFolder, state, visibility, requests); }
        filteredPlaceholderMenuItem("Open Debug Package Folder", engineSearch.data(), EditorGlyphIcon::Folder);
        filteredPlaceholderMenuItem("Copy System Info", engineSearch.data(), EditorGlyphIcon::Command);
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
        if (filteredCommandMenuItem(EditorCommandId::JobCenter, prefs, windowSearch.data())) { executeCommand(EditorCommandId::JobCenter, state, visibility, requests); }
        menuSection("DEBUG / ADVANCED");
        filteredToggleMenuItem("Debug / Profiler", windowSearch.data(), &visibility.debugProfiler, EditorGlyphIcon::Stats);
        filteredToggleMenuItem("Scene Stats", windowSearch.data(), &visibility.sceneStats, EditorGlyphIcon::Stats);
        filteredToggleMenuItem("GPU Diagnostics", windowSearch.data(), &visibility.gpuDiagnostics, EditorGlyphIcon::Stats);
        filteredPlaceholderMenuItem("Floating Render Controls", windowSearch.data(), EditorGlyphIcon::Render);
        menuSection("LAYOUT");
        if (filteredCommandMenuItem(EditorCommandId::SaveLayout, prefs, windowSearch.data())) { executeCommand(EditorCommandId::SaveLayout, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ResetLayout, prefs, windowSearch.data())) { executeCommand(EditorCommandId::ResetLayout, state, visibility, requests); }
        filteredPlaceholderMenuItem("Load Layout...", windowSearch.data(), EditorGlyphIcon::Layout);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        drawMenuSearch("##RenderMenuSearch", renderSearch);
        menuSection("OUTPUT");
        if (filteredCommandMenuItem(EditorCommandId::RenderCurrentViewport, prefs, renderSearch.data())) { executeCommand(EditorCommandId::RenderCurrentViewport, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::RenderImage, prefs, renderSearch.data())) { executeCommand(EditorCommandId::RenderImage, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::RenderSequence, prefs, renderSearch.data())) { executeCommand(EditorCommandId::RenderSequence, state, visibility, requests); }
        filteredPlaceholderMenuItem("Pause / Resume Render", renderSearch.data(), EditorGlyphIcon::Pause);
        if (filteredCommandMenuItem(EditorCommandId::StopRender, prefs, renderSearch.data())) { executeCommand(EditorCommandId::StopRender, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::OpenOutputFolder, prefs, renderSearch.data())) { executeCommand(EditorCommandId::OpenOutputFolder, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::Screenshot, prefs, renderSearch.data())) { executeCommand(EditorCommandId::Screenshot, state, visibility, requests); }
        filteredPlaceholderMenuItem("High Resolution Render", renderSearch.data(), EditorGlyphIcon::Render);
        menuSection("PREVIEW");
        if (filteredCommandMenuItem(EditorCommandId::ResetAccumulation, prefs, renderSearch.data())) { executeCommand(EditorCommandId::ResetAccumulation, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ToggleDenoiser, prefs, renderSearch.data())) { executeCommand(EditorCommandId::ToggleDenoiser, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CycleDebugView, prefs, renderSearch.data())) { executeCommand(EditorCommandId::CycleDebugView, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::CycleIntermediateView, prefs, renderSearch.data())) { executeCommand(EditorCommandId::CycleIntermediateView, state, visibility, requests); }
        filteredToggleMenuItem("Render World Settings", renderSearch.data(), &visibility.renderSettings, EditorGlyphIcon::Render);
        filteredPlaceholderMenuItem("Quality Preset", renderSearch.data(), EditorGlyphIcon::Render);
        menuSection("DIAGNOSTICS");
        filteredPlaceholderMenuItem("Capture RenderDoc", renderSearch.data(), EditorGlyphIcon::Render);
        filteredPlaceholderMenuItem("Export Debug Views", renderSearch.data(), EditorGlyphIcon::DrawDebug);
        filteredPlaceholderMenuItem("Export Debug Package", renderSearch.data(), EditorGlyphIcon::Folder);
        filteredPlaceholderMenuItem("Dump RenderGraph", renderSearch.data(), EditorGlyphIcon::ConfigFile);
        filteredPlaceholderMenuItem("Profile Current Scene", renderSearch.data(), EditorGlyphIcon::Stats);
        menuSection("DEBUG VIEWS");
        if (filteredCommandMenuItem(EditorCommandId::SetDebugBeauty, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugBeauty, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugDirectLighting, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugDirectLighting, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugIndirectLighting, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugIndirectLighting, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugNormals, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugNormals, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugDepth, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugDepth, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugMotionVectors, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugMotionVectors, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugVariance, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugVariance, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::SetDebugAlbedo, prefs, renderSearch.data())) { executeCommand(EditorCommandId::SetDebugAlbedo, state, visibility, requests); }
        filteredPlaceholderMenuItem("View Mode", renderSearch.data(), EditorGlyphIcon::DrawDebug);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Layout")) {
        drawMenuSearch("##LayoutMenuSearch", layoutSearch);
        menuSection("WORKSPACE");
        if (filteredMenuItem("Default Editor", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Layout)) { applyDefaultEditorLayout(); }
        if (filteredMenuItem("Content Editing", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Folder)) { applyContentLayout(); }
        if (filteredMenuItem("Lighting", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Light)) { applyLightingLayout(); }
        if (filteredMenuItem("Runtime Viewer", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::ViewSettings)) { applyRuntimeViewerLayout(); }
        if (filteredMenuItem("Animation / Timeline", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::TimelineKey)) { applyTimelineLayout(); }
        if (filteredMenuItem("Debug / Profiling", layoutSearch.data(), nullptr, false, true, nullptr, EditorGlyphIcon::Stats)) { applyDebugLayout(); }
        menuSection("LAYOUT FILES");
        if (filteredCommandMenuItem(EditorCommandId::SaveLayout, prefs, layoutSearch.data())) { executeCommand(EditorCommandId::SaveLayout, state, visibility, requests); }
        if (filteredCommandMenuItem(EditorCommandId::ResetLayout, prefs, layoutSearch.data())) { executeCommand(EditorCommandId::ResetLayout, state, visibility, requests); }
        filteredPlaceholderMenuItem("Manage Layouts...", layoutSearch.data(), EditorGlyphIcon::Layout);
        menuSection("APPEARANCE");
        filteredPlaceholderMenuItem("UI Scale", layoutSearch.data(), EditorGlyphIcon::Layout);
        filteredPlaceholderMenuItem("Theme", layoutSearch.data(), EditorGlyphIcon::Layout);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (drawSceneTabChrome(activeSceneTitle(state))) {
        requests.closeScene = true;
    }
    const std::string saveState = editorSaveStateLabel(state);
    const float saveStateWidth = ImGui::CalcTextSize(saveState.c_str()).x;
    const float fps = state.cpuFrameMs > 0.0f ? 1000.0f / state.cpuFrameMs : 0.0f;
    const char* fmt = fps > 0.0f ? "fps: %.0f | Ms: %.0f" : "fps: -- | Ms: %.0f";
    const float rightWidth = 122.0f + saveStateWidth + ImGui::GetStyle().ItemSpacing.x;
    const float availX = ImGui::GetContentRegionAvail().x;
    if (availX > rightWidth) {
        ImGui::SameLine(ImGui::GetCursorPosX() + availX - rightWidth);
    } else {
        ImGui::SameLine();
    }
    drawEditorSaveStateLabel(state, saveState);
    ImGui::SameLine();
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
