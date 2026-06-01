#pragma once

#include "rtv/SceneComponents.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace rtv {

namespace EditorUiMetric {
inline constexpr float panelPaddingX = 5.0f;
inline constexpr float panelPaddingY = 4.0f;
inline constexpr float rowPaddingX = 5.0f;
inline constexpr float rowPaddingY = 2.0f;
inline constexpr float compactButtonRounding = 1.0f;
inline constexpr float sidebarWidth = 204.0f;
inline constexpr float detailsWidth = 284.0f;
inline constexpr float cardRounding = 4.0f;
inline constexpr float cardPadding = 8.0f;
inline constexpr float projectCardWidth = 214.0f;
inline constexpr float projectCardHeight = 128.0f;
inline constexpr float projectTemplateCardWidth = 205.0f;
inline constexpr float projectCardPreviewHeight = 58.0f;
inline constexpr float contentGridCellWidth = 98.0f;
inline constexpr float contentGridThumbWidth = 78.0f;
inline constexpr float contentGridThumbHeight = 46.0f;
inline constexpr float assetPreviewMaxWidth = 244.0f;
inline constexpr float assetPreviewHeight = 104.0f;
inline constexpr float contentTreePanelRatio = 0.32f;
inline constexpr float contentDetailsPanelRatio = 0.33f;
inline constexpr float contentTreeMinWidth = 220.0f;
inline constexpr float contentTreeMaxWidth = 420.0f;
inline constexpr float contentDetailsMinWidth = 260.0f;
inline constexpr float contentDetailsMaxWidth = 420.0f;
inline constexpr float contentListMinWidth = 220.0f;
inline constexpr float progressColumnWidth = 128.0f;
inline constexpr float hierarchyRowHeight = 22.0f;
inline constexpr float hierarchyIndentSpacing = 18.0f;
inline constexpr float hierarchyIconSize = 16.0f;
inline constexpr float hierarchyRowRightFadeWidth = 58.0f;
inline constexpr float contentRowHeight = 18.0f;
inline constexpr float timelineTrackRowHeight = 24.0f;
inline constexpr float timelineFrameWidth = 96.0f;
inline constexpr float timelineRangeFrameWidth = 82.0f;
inline constexpr float timelineFrameRateWidth = 64.0f;
inline constexpr float timelineRulerHeight = 44.0f;
inline constexpr float timelineRulerTickCount = 10.0f;
inline constexpr float timelineRulerTickTop = 18.0f;
inline constexpr float timelineRulerLabelOffsetX = 3.0f;
inline constexpr float timelineRulerLabelOffsetY = 2.0f;
inline constexpr float timelineRulerRangeTextOffsetX = 8.0f;
inline constexpr float timelineRulerRangeTextOffsetY = 24.0f;
inline constexpr float timelineRulerKeyTop = 24.0f;
inline constexpr float timelineRulerKeyBottom = 34.0f;
inline constexpr float timelineKeyMarkerRadius = 5.0f;
inline constexpr float timelineKeySelectedRadius = 6.5f;
inline constexpr float timelineKeyHitSize = 14.0f;
inline constexpr float timelineKeyDragThreshold = 2.0f;
inline constexpr float timelineTrackColumnWidth = 190.0f;
inline constexpr float timelineTrackRowVisualHeight = 28.0f;
inline constexpr float timelineTrackLaneHeight = 22.0f;
inline constexpr float timelineKeyEditorTrackWidth = 170.0f;
inline constexpr float timelineKeyEditorFrameWidth = 74.0f;
inline constexpr float timelineKeyEditorActionWidth = 78.0f;
inline constexpr float timelineKeyDeleteButtonWidth = 22.0f;
inline constexpr float timelineKeyDeleteButtonHeight = 20.0f;
inline constexpr float inspectorRowHeight = 24.0f;
inline constexpr float inspectorLabelWidth = 132.0f;
inline constexpr float inspectorComponentHeaderHeight = 30.0f;
inline constexpr float inspectorComponentHeaderIconSize = 16.0f;
inline constexpr float inspectorComponentActionSize = 20.0f;
inline constexpr float viewportOverlayPaddingX = 5.0f;
inline constexpr float viewportOverlayPaddingY = 2.0f;
inline constexpr float viewportOverlayRounding = 2.0f;
inline constexpr float dockRightPanelRatio = 0.300f;
inline constexpr float dockBottomPanelRatio = 0.405f;
inline constexpr float dockRightInspectorRatio = 0.485f;
inline constexpr float dockSplitterThickness = 5.0f;
inline constexpr float dockTabRounding = 2.0f;
inline constexpr float dockTabBorderSize = 1.0f;
inline constexpr float dockTabIconSize = 14.0f;
inline constexpr float dockTabIconPaddingX = 6.0f;
inline constexpr int glyphLabelPrefixSpaces = 15;
inline constexpr float iconTextButtonTextOffsetX = 36.0f;
inline constexpr float iconTextReadoutTextGap = 14.0f;
inline constexpr float dockPanelBorderThickness = 1.0f;
inline constexpr float dockPanelActiveAccentWidth = 1.0f;
inline constexpr float sceneTabMinWidth = 92.0f;
inline constexpr float sceneTabMaxWidth = 300.0f;
inline constexpr float sceneTabExtraWidth = 30.0f;
inline constexpr float sceneTabIconMinX = 8.0f;
inline constexpr float sceneTabIconMaxX = 22.0f;
inline constexpr float sceneTabIconPaddingY = 5.0f;
inline constexpr float sceneTabTextX = 12.0f;
inline constexpr float sceneTabCloseMinOffset = 20.0f;
inline constexpr float sceneTabCloseMaxOffset = 6.0f;
inline constexpr float sceneTabClosePaddingY = 4.0f;
inline constexpr float sceneTabCloseIconPaddingX = 3.0f;
inline constexpr float sceneTabCloseIconPaddingY = 2.0f;
} // namespace EditorUiMetric

namespace EditorDockWindowTitle {
inline constexpr const char* Scene = "        Scene        ###Scene";
inline constexpr const char* Hierarchy = "        Hierarchy        ###Hierarchy";
inline constexpr const char* RenderSettings = "        Render World Settings        ###Render Settings";
inline constexpr const char* Inspector = "        Inspector        ###Inspector";
inline constexpr const char* MaterialEditor = "        Material Editor        ###Material Editor";
inline constexpr const char* Content = "        Content        ###Content";
inline constexpr const char* Timeline = "        Timeline        ###Timeline";
inline constexpr const char* Log = "        Log        ###Log";
} // namespace EditorDockWindowTitle

inline std::string editorGlyphLabelPrefix() {
    return std::string(EditorUiMetric::glyphLabelPrefixSpaces, ' ');
}

inline std::string editorGlyphLabel(const char* label) {
    return editorGlyphLabelPrefix() + (label != nullptr ? label : "");
}

inline std::string editorGlyphLabel(const std::string& label) {
    return editorGlyphLabel(label.c_str());
}

enum class EditorGlyphIcon {
    Select,
    Move,
    Rotate,
    Scale,
    LocalSpace,
    WorldSpace,
    Snap,
    Grid,
    Axes,
    Frame,
    ViewSettings,
    Stats,
    DrawDebug,
    Camera,
    EyeVisible,
    EyeHidden,
    Lock,
    Unlock,
    Folder,
    File,
    Texture,
    Environment,
    Model,
    Material,
    SceneFile,
    ProjectFile,
    IesProfile,
    VolumeFile,
    ShaderFile,
    ConfigFile,
    Light,
    Sun,
    Sky,
    Fog,
    Cloud,
    PostProcess,
    Group,
    Entity,
    Add,
    Back,
    Forward,
    Up,
    Refresh,
    Hierarchy,
    List,
    Details,
    Reset,
    More,
    Play,
    Pause,
    Stop,
    Timeline,
    TimelineKey,
    Trash,
    Save,
    Import,
    Render,
    Command,
    Layout,
    Window,
    Undo,
    Redo,
    Exit,
};

inline ImFont*& editorTablerIconFontStorage() {
    static ImFont* font = nullptr;
    return font;
}

inline void editorSetTablerIconFont(ImFont* font) {
    editorTablerIconFontStorage() = font;
}

inline ImFont* editorTablerIconFont() {
    return editorTablerIconFontStorage();
}

inline ImVec4 editorIconTint(bool active) {
    return active ? ImVec4(0.42f, 0.62f, 0.86f, 1.0f) : ImVec4(0.70f, 0.72f, 0.74f, 1.0f);
}

inline ImVec4 editorSelectedRowColor() {
    return ImVec4(0.145f, 0.285f, 0.475f, 0.94f);
}

inline ImVec4 editorHoveredRowColor() {
    return ImVec4(0.165f, 0.190f, 0.225f, 0.94f);
}

inline ImVec4 editorActiveRowColor() {
    return ImVec4(0.165f, 0.340f, 0.560f, 0.96f);
}

inline ImVec4 editorRowBandColor(bool alternate = false) {
    return alternate ? ImVec4(0.128f, 0.134f, 0.145f, 0.54f) : ImVec4(0.145f, 0.151f, 0.162f, 0.48f);
}

inline ImVec4 editorWindowBgColor() {
    return ImVec4(0.105f, 0.110f, 0.118f, 1.0f);
}

inline ImVec4 editorChildBgColor() {
    return ImVec4(0.128f, 0.132f, 0.140f, 1.0f);
}

inline ImVec4 editorPopupBgColor() {
    return ImVec4(0.105f, 0.110f, 0.118f, 1.0f);
}

inline ImVec4 editorBorderColor() {
    return ImVec4(0.215f, 0.225f, 0.245f, 0.92f);
}

inline ImVec4 editorFrameBgColor() {
    return ImVec4(0.150f, 0.155f, 0.165f, 1.0f);
}

inline ImVec4 editorFrameBgHoveredColor() {
    return ImVec4(0.185f, 0.195f, 0.215f, 1.0f);
}

inline ImVec4 editorFrameBgActiveColor() {
    return ImVec4(0.205f, 0.225f, 0.260f, 1.0f);
}

inline ImVec4 editorTitleBgColor(bool active) {
    return active ? ImVec4(0.118f, 0.125f, 0.137f, 1.0f) : ImVec4(0.085f, 0.090f, 0.098f, 1.0f);
}

inline ImVec4 editorMenuBarBgColor() {
    return ImVec4(0.070f, 0.074f, 0.082f, 1.0f);
}

inline ImVec4 editorTabColor(bool active, bool hovered = false) {
    if (hovered) {
        return ImVec4(0.155f, 0.170f, 0.195f, 1.0f);
    }
    return active ? ImVec4(0.135f, 0.145f, 0.160f, 1.0f) : ImVec4(0.088f, 0.094f, 0.104f, 1.0f);
}

inline ImVec4 editorHeaderColor(bool active, bool hovered = false) {
    if (active) {
        return ImVec4(0.155f, 0.315f, 0.520f, 0.95f);
    }
    return hovered ? ImVec4(0.170f, 0.185f, 0.210f, 1.0f) : ImVec4(0.145f, 0.152f, 0.165f, 1.0f);
}

inline ImVec4 editorButtonColor(bool active, bool hovered = false) {
    if (active) {
        return ImVec4(0.160f, 0.260f, 0.390f, 1.0f);
    }
    return hovered ? ImVec4(0.170f, 0.185f, 0.210f, 1.0f) : ImVec4(0.120f, 0.128f, 0.140f, 1.0f);
}

inline ImVec4 editorCheckMarkColor() {
    return ImVec4(0.300f, 0.560f, 0.980f, 1.0f);
}

inline ImVec4 editorSliderGrabColor() {
    return ImVec4(0.260f, 0.500f, 0.900f, 1.0f);
}

inline ImVec4 editorSeparatorColor() {
    return ImVec4(0.205f, 0.215f, 0.235f, 1.0f);
}

inline ImVec4 editorResizeGripColor() {
    return ImVec4(0.230f, 0.280f, 0.360f, 0.45f);
}

inline ImVec4 editorDisabledTextColor() {
    return ImVec4(0.40f, 0.45f, 0.52f, 0.86f);
}

inline ImVec4 editorDisabledIconTint() {
    return ImVec4(0.34f, 0.39f, 0.46f, 0.82f);
}

inline ImVec4 editorDisabledRowAccentColor() {
    return ImVec4(0.25f, 0.30f, 0.38f, 0.55f);
}

inline ImVec4 editorSceneTabBgColor(bool hovered) {
    return hovered ? ImVec4(0.128f, 0.140f, 0.160f, 0.96f) : ImVec4(0.090f, 0.100f, 0.116f, 0.96f);
}

inline ImVec4 editorSceneTabBorderColor() {
    return ImVec4(0.235f, 0.255f, 0.295f, 0.82f);
}

inline ImVec4 editorSceneTabIconColor() {
    return ImVec4(0.620f, 0.720f, 0.880f, 1.0f);
}

inline ImVec4 editorSceneTabTextColor() {
    return ImVec4(0.878f, 0.894f, 0.918f, 1.0f);
}

inline ImVec4 editorSceneTabCloseHoverColor() {
    return ImVec4(0.185f, 0.205f, 0.240f, 0.90f);
}

inline ImVec4 editorSceneTabCloseIconColor() {
    return ImVec4(0.690f, 0.722f, 0.769f, 1.0f);
}

inline ImVec4 editorPanelBorderColor() {
    return ImVec4(0.125f, 0.138f, 0.160f, 0.92f);
}

inline ImVec4 editorPanelSplitterColor() {
    return ImVec4(0.120f, 0.135f, 0.160f, 1.0f);
}

inline ImVec4 editorPanelSplitterHoveredColor() {
    return ImVec4(0.195f, 0.305f, 0.465f, 1.0f);
}

inline ImVec4 editorPanelActiveAccentColor() {
    return ImVec4(0.120f, 0.245f, 0.400f, 0.62f);
}

inline ImVec4 editorViewportOverlayBgColor() {
    return ImVec4(0.018f, 0.020f, 0.024f, 0.68f);
}

inline ImVec4 editorViewportOverlayBorderColor() {
    return ImVec4(0.115f, 0.130f, 0.155f, 0.58f);
}

inline ImVec4 editorTimelineRulerBgColor() {
    return ImVec4(0.094f, 0.106f, 0.122f, 1.0f);
}

inline ImVec4 editorTimelineRulerBorderColor() {
    return ImVec4(0.196f, 0.227f, 0.275f, 0.86f);
}

inline ImVec4 editorTimelineRulerTickColor() {
    return ImVec4(0.337f, 0.357f, 0.392f, 0.86f);
}

inline ImVec4 editorTimelineRulerLabelColor() {
    return ImVec4(0.588f, 0.604f, 0.635f, 0.90f);
}

inline ImVec4 editorTimelineRangeTextColor() {
    return ImVec4(0.690f, 0.722f, 0.776f, 0.92f);
}

inline ImVec4 editorTimelineDurationTextColor() {
    return ImVec4(0.667f, 0.706f, 0.761f, 1.0f);
}

inline ImVec4 editorTimelineLaneBgColor() {
    return ImVec4(0.071f, 0.078f, 0.094f, 1.0f);
}

inline ImVec4 editorTimelineLaneBorderColor() {
    return ImVec4(0.149f, 0.173f, 0.212f, 0.82f);
}

inline ImVec4 editorTimelineKeyColor(bool selected) {
    return selected ? ImVec4(0.294f, 0.569f, 1.0f, 1.0f) : ImVec4(0.961f, 0.698f, 0.275f, 0.94f);
}

inline ImVec4 editorTimelineKeyOutlineColor() {
    return ImVec4(0.745f, 0.863f, 1.0f, 0.96f);
}

inline ImVec4 editorTimelinePlayheadColor() {
    return ImVec4(0.294f, 0.569f, 1.0f, 1.0f);
}

inline void editorPushRowSelectionStyle() {
    ImGui::PushStyleColor(ImGuiCol_Header, editorSelectedRowColor());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, editorHoveredRowColor());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, editorActiveRowColor());
}

inline void editorPopRowSelectionStyle() {
    ImGui::PopStyleColor(3);
}

inline void editorDrawPreRowBand(float rowHeight) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const float left = windowPos.x + ImGui::GetWindowContentRegionMin().x;
    const float right = windowPos.x + ImGui::GetWindowContentRegionMax().x;
    if (right <= left || rowHeight <= 0.0f) {
        return;
    }
    const int rowIndex = static_cast<int>(std::floor((cursor.y - windowPos.y) / std::max(1.0f, rowHeight)));
    const bool alternate = (rowIndex & 1) != 0;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(left, cursor.y),
        ImVec2(right, cursor.y + rowHeight),
        ImGui::GetColorU32(editorRowBandColor(alternate)));
}

inline void editorPushDisabledTextStyle() {
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, editorDisabledTextColor());
}

inline void editorPopDisabledTextStyle() {
    ImGui::PopStyleColor(1);
}

inline void editorDrawDisabledRowChrome(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent = ImGui::GetColorU32(editorDisabledRowAccentColor());
    dl->AddLine(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(min.x + 2.0f, max.y - 3.0f), accent, 1.0f);
    dl->AddRect(min, max, accent, 1.0f);
}

inline ImVec2 editorRowFramePadding(float targetHeight) {
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImVec2(style.FramePadding.x, std::max(style.FramePadding.y, (targetHeight - ImGui::GetTextLineHeight()) * 0.5f));
}

inline uint32_t editorTablerIconCodepoint(EditorGlyphIcon icon) {
    switch (icon) {
    case EditorGlyphIcon::Select: return 0xF265;       // pointer
    case EditorGlyphIcon::Move: return 0xF22F;         // arrows-move
    case EditorGlyphIcon::Rotate: return 0xEB16;       // rotate
    case EditorGlyphIcon::Scale: return 0xEA28;        // arrows-maximize
    case EditorGlyphIcon::LocalSpace: return 0xEF45;   // axis-x
    case EditorGlyphIcon::WorldSpace: return 0xEB54;   // world
    case EditorGlyphIcon::Snap: return 0xEAE3;         // magnet
    case EditorGlyphIcon::Grid: return 0xEABA;         // grid-dots
    case EditorGlyphIcon::Axes: return 0xEF45;         // axis-x
    case EditorGlyphIcon::Frame: return 0xEB8D;        // focus
    case EditorGlyphIcon::ViewSettings: return 0xEB20; // settings
    case EditorGlyphIcon::Stats: return 0xEA59;        // chart-bar
    case EditorGlyphIcon::DrawDebug: return 0xEA48;    // bug
    case EditorGlyphIcon::Camera: return 0xEA54;       // camera
    case EditorGlyphIcon::EyeVisible: return 0xEA9A;   // eye
    case EditorGlyphIcon::EyeHidden: return 0xECF0;    // eye-off
    case EditorGlyphIcon::Lock: return 0xEAE2;         // lock
    case EditorGlyphIcon::Unlock: return 0xEAE1;       // lock-open
    case EditorGlyphIcon::Folder: return 0xEAAD;       // folder
    case EditorGlyphIcon::File: return 0xEAA4;         // file
    case EditorGlyphIcon::Texture: return 0xEB0A;      // photo
    case EditorGlyphIcon::Environment: return 0xEB54;  // world
    case EditorGlyphIcon::Model: return 0xEA45;        // box
    case EditorGlyphIcon::Material: return 0xEB01;     // palette
    case EditorGlyphIcon::SceneFile: return 0xF53D;    // file-delta
    case EditorGlyphIcon::ProjectFile: return 0xF02C;  // layout-dashboard
    case EditorGlyphIcon::IesProfile: return 0xEDEC;   // file-info
    case EditorGlyphIcon::VolumeFile: return 0xFAB8;   // sphere
    case EditorGlyphIcon::ShaderFile: return 0xEBD0;   // file-code
    case EditorGlyphIcon::ConfigFile: return 0xF029;   // file-settings
    case EditorGlyphIcon::Light: return 0xEA51;        // bulb
    case EditorGlyphIcon::Sun: return 0xEB30;          // sun
    case EditorGlyphIcon::Sky: return 0xEF97;          // mountain
    case EditorGlyphIcon::Fog: return 0xEC30;          // mist
    case EditorGlyphIcon::Cloud: return 0xEA76;        // cloud
    case EditorGlyphIcon::PostProcess: return 0xFEA9;  // layers-selected
    case EditorGlyphIcon::Group: return 0xEE17;        // box-multiple
    case EditorGlyphIcon::Entity: return 0xFA97;       // cube
    case EditorGlyphIcon::Add: return 0xEB0B;          // plus
    case EditorGlyphIcon::Back: return 0xEA19;         // arrow-left
    case EditorGlyphIcon::Forward: return 0xEA1F;      // arrow-right
    case EditorGlyphIcon::Up: return 0xEA25;           // arrow-up
    case EditorGlyphIcon::Refresh: return 0xEB13;      // refresh
    case EditorGlyphIcon::Hierarchy: return 0xFAFA;    // list-tree
    case EditorGlyphIcon::List: return 0xEB6B;         // list
    case EditorGlyphIcon::Details: return 0xEF40;      // list-details
    case EditorGlyphIcon::Reset: return 0xFAFD;        // restore
    case EditorGlyphIcon::More: return 0xEA95;         // dots
    case EditorGlyphIcon::Play: return 0xED46;         // player-play
    case EditorGlyphIcon::Pause: return 0xED45;        // player-pause
    case EditorGlyphIcon::Stop: return 0xED4A;         // player-stop
    case EditorGlyphIcon::Timeline: return 0xF031;     // timeline
    case EditorGlyphIcon::TimelineKey: return 0xEB65;  // diamond
    case EditorGlyphIcon::Trash: return 0xEB41;        // trash
    case EditorGlyphIcon::Save: return 0xEB62;         // device-floppy
    case EditorGlyphIcon::Import: return 0xEB47;       // upload
    case EditorGlyphIcon::Render: return 0xEB58;       // aperture
    case EditorGlyphIcon::Command: return 0xEA78;      // command
    case EditorGlyphIcon::Layout: return 0xEADB;       // layout
    case EditorGlyphIcon::Window: return 0xEF06;       // window
    case EditorGlyphIcon::Undo: return 0xEB77;         // arrow-back-up
    case EditorGlyphIcon::Redo: return 0xEB78;         // arrow-forward-up
    case EditorGlyphIcon::Exit: return 0xEB55;         // x
    }
    return 0xEAA4;
}

inline int editorEncodeUtf8(uint32_t codepoint, char (&out)[5]) {
    if (codepoint <= 0x7Fu) {
        out[0] = static_cast<char>(codepoint);
        out[1] = '\0';
        return 1;
    }
    if (codepoint <= 0x7FFu) {
        out[0] = static_cast<char>(0xC0u | (codepoint >> 6u));
        out[1] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        out[2] = '\0';
        return 2;
    }
    if (codepoint <= 0xFFFFu) {
        out[0] = static_cast<char>(0xE0u | (codepoint >> 12u));
        out[1] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        out[3] = '\0';
        return 3;
    }
    out[0] = static_cast<char>(0xF0u | (codepoint >> 18u));
    out[1] = static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
    out[3] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
    out[4] = '\0';
    return 4;
}

inline bool editorDrawTablerIconGlyph(ImDrawList* drawList, EditorGlyphIcon icon, ImVec2 min, ImVec2 max, ImU32 color) {
    if (drawList == nullptr) {
        return false;
    }
    ImFont* font = editorTablerIconFont();
    if (font == nullptr) {
        return false;
    }

    char glyph[5]{};
    const int glyphLength = editorEncodeUtf8(editorTablerIconCodepoint(icon), glyph);
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    if (w <= 0.0f || h <= 0.0f || glyphLength <= 0) {
        return true;
    }

    const float iconSize = std::min(w, h);
    const float fontSize = iconSize * 1.16f;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), 0.0f, glyph, glyph + glyphLength);
    const ImVec2 pos(
        min.x + (w - textSize.x) * 0.5f,
        min.y + (h - textSize.y) * 0.5f - iconSize * 0.02f);
    drawList->AddText(font, fontSize, pos, color, glyph, glyph + glyphLength);
    return true;
}

inline bool editorDrawTablerIconGlyph(EditorGlyphIcon icon, ImVec2 min, ImVec2 max, ImU32 color) {
    return editorDrawTablerIconGlyph(ImGui::GetWindowDrawList(), icon, min, max, color);
}

inline void editorDrawIconGlyph(EditorGlyphIcon icon, ImVec2 min, ImVec2 max, ImU32 color) {
    if (editorDrawTablerIconGlyph(icon, min, max, color)) {
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    const ImVec2 c(min.x + w * 0.5f, min.y + h * 0.5f);
    const float s = std::min(w, h);
    const float r = s * 0.36f;
    const float th = std::max(1.25f, s * 0.095f);
    constexpr float pi = 3.14159265358979323846f;

    switch (icon) {
    case EditorGlyphIcon::Select:
        dl->AddTriangleFilled(ImVec2(min.x + w * 0.24f, min.y + h * 0.18f), ImVec2(min.x + w * 0.25f, min.y + h * 0.80f), ImVec2(min.x + w * 0.68f, min.y + h * 0.58f), color);
        dl->AddLine(ImVec2(min.x + w * 0.48f, min.y + h * 0.56f), ImVec2(min.x + w * 0.70f, min.y + h * 0.82f), color, th);
        break;
    case EditorGlyphIcon::Move:
        dl->AddLine(ImVec2(c.x, min.y + h * 0.17f), ImVec2(c.x, max.y - h * 0.17f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.17f, c.y), ImVec2(max.x - w * 0.17f, c.y), color, th);
        dl->AddTriangleFilled(ImVec2(c.x, min.y + h * 0.08f), ImVec2(c.x - w * 0.09f, min.y + h * 0.24f), ImVec2(c.x + w * 0.09f, min.y + h * 0.24f), color);
        dl->AddTriangleFilled(ImVec2(c.x, max.y - h * 0.08f), ImVec2(c.x - w * 0.09f, max.y - h * 0.24f), ImVec2(c.x + w * 0.09f, max.y - h * 0.24f), color);
        dl->AddTriangleFilled(ImVec2(min.x + w * 0.08f, c.y), ImVec2(min.x + w * 0.24f, c.y - h * 0.09f), ImVec2(min.x + w * 0.24f, c.y + h * 0.09f), color);
        dl->AddTriangleFilled(ImVec2(max.x - w * 0.08f, c.y), ImVec2(max.x - w * 0.24f, c.y - h * 0.09f), ImVec2(max.x - w * 0.24f, c.y + h * 0.09f), color);
        break;
    case EditorGlyphIcon::Rotate:
        dl->PathArcTo(c, r, -2.55f, 1.05f, 24);
        dl->PathStroke(color, 0, th);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.58f, c.y + r * 0.88f), ImVec2(c.x + r * 0.95f, c.y + r * 0.42f), ImVec2(c.x + r * 0.28f, c.y + r * 0.34f), color);
        break;
    case EditorGlyphIcon::Scale:
        dl->AddRect(ImVec2(min.x + w * 0.20f, min.y + h * 0.36f), ImVec2(min.x + w * 0.54f, min.y + h * 0.70f), color, 0.0f, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.43f, min.y + h * 0.57f), ImVec2(max.x - w * 0.18f, min.y + h * 0.22f), color, th);
        dl->AddTriangleFilled(ImVec2(max.x - w * 0.12f, min.y + h * 0.15f), ImVec2(max.x - w * 0.35f, min.y + h * 0.20f), ImVec2(max.x - w * 0.18f, min.y + h * 0.36f), color);
        break;
    case EditorGlyphIcon::LocalSpace:
        dl->AddLine(ImVec2(min.x + w * 0.28f, min.y + h * 0.20f), ImVec2(min.x + w * 0.28f, max.y - h * 0.18f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.28f, max.y - h * 0.18f), ImVec2(max.x - w * 0.22f, max.y - h * 0.18f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.52f, min.y + h * 0.34f), ImVec2(max.x - w * 0.18f, min.y + h * 0.34f), color, th);
        break;
    case EditorGlyphIcon::WorldSpace:
        dl->AddCircle(c, r, color, 28, th);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), color, th);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), color, th);
        break;
    case EditorGlyphIcon::Snap:
        dl->PathArcTo(ImVec2(c.x - r * 0.40f, c.y), r * 0.52f, pi, 0.0f, 12);
        dl->PathStroke(color, 0, th);
        dl->AddLine(ImVec2(c.x - r * 0.92f, c.y), ImVec2(c.x - r * 0.92f, max.y - h * 0.22f), color, th);
        dl->AddLine(ImVec2(c.x + r * 0.12f, c.y), ImVec2(c.x + r * 0.12f, max.y - h * 0.22f), color, th);
        dl->AddLine(ImVec2(c.x - r * 0.92f, max.y - h * 0.22f), ImVec2(c.x - r * 0.50f, max.y - h * 0.22f), color, th);
        dl->AddLine(ImVec2(c.x + r * 0.12f, max.y - h * 0.22f), ImVec2(c.x + r * 0.54f, max.y - h * 0.22f), color, th);
        break;
    case EditorGlyphIcon::Grid:
        for (int i = 1; i <= 3; ++i) {
            const float t = static_cast<float>(i) / 4.0f;
            dl->AddLine(ImVec2(min.x + w * t, min.y + h * 0.18f), ImVec2(min.x + w * t, max.y - h * 0.18f), color, th * 0.75f);
            dl->AddLine(ImVec2(min.x + w * 0.18f, min.y + h * t), ImVec2(max.x - w * 0.18f, min.y + h * t), color, th * 0.75f);
        }
        break;
    case EditorGlyphIcon::Axes:
        dl->AddLine(c, ImVec2(max.x - w * 0.16f, c.y), IM_COL32(255, 90, 90, 255), th);
        dl->AddLine(c, ImVec2(c.x, min.y + h * 0.16f), IM_COL32(96, 220, 120, 255), th);
        dl->AddLine(c, ImVec2(min.x + w * 0.22f, max.y - h * 0.18f), IM_COL32(105, 150, 255, 255), th);
        break;
    case EditorGlyphIcon::Frame:
        dl->AddLine(ImVec2(min.x + w * 0.22f, min.y + h * 0.22f), ImVec2(min.x + w * 0.22f, min.y + h * 0.44f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.22f, min.y + h * 0.22f), ImVec2(min.x + w * 0.44f, min.y + h * 0.22f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.22f, min.y + h * 0.22f), ImVec2(max.x - w * 0.44f, min.y + h * 0.22f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.22f, min.y + h * 0.22f), ImVec2(max.x - w * 0.22f, min.y + h * 0.44f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.22f, max.y - h * 0.22f), ImVec2(min.x + w * 0.22f, max.y - h * 0.44f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.22f, max.y - h * 0.22f), ImVec2(min.x + w * 0.44f, max.y - h * 0.22f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.22f, max.y - h * 0.22f), ImVec2(max.x - w * 0.44f, max.y - h * 0.22f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.22f, max.y - h * 0.22f), ImVec2(max.x - w * 0.22f, max.y - h * 0.44f), color, th);
        break;
    case EditorGlyphIcon::ViewSettings:
        dl->AddRect(ImVec2(min.x + w * 0.18f, min.y + h * 0.24f), ImVec2(max.x - w * 0.18f, max.y - h * 0.30f), color, 1.0f, 0, th);
        dl->AddLine(ImVec2(c.x, max.y - h * 0.30f), ImVec2(c.x, max.y - h * 0.16f), color, th);
        dl->AddLine(ImVec2(c.x - w * 0.18f, max.y - h * 0.16f), ImVec2(c.x + w * 0.18f, max.y - h * 0.16f), color, th);
        break;
    case EditorGlyphIcon::Stats:
        dl->AddRectFilled(ImVec2(min.x + w * 0.22f, max.y - h * 0.30f), ImVec2(min.x + w * 0.34f, max.y - h * 0.18f), color);
        dl->AddRectFilled(ImVec2(min.x + w * 0.44f, max.y - h * 0.54f), ImVec2(min.x + w * 0.56f, max.y - h * 0.18f), color);
        dl->AddRectFilled(ImVec2(min.x + w * 0.66f, max.y - h * 0.76f), ImVec2(min.x + w * 0.78f, max.y - h * 0.18f), color);
        break;
    case EditorGlyphIcon::DrawDebug:
        dl->AddCircle(c, r * 0.58f, color, 18, th);
        dl->AddLine(ImVec2(c.x - r * 0.42f, c.y - r * 0.56f), ImVec2(c.x - r * 0.68f, c.y - r * 0.86f), color, th * 0.75f);
        dl->AddLine(ImVec2(c.x + r * 0.42f, c.y - r * 0.56f), ImVec2(c.x + r * 0.68f, c.y - r * 0.86f), color, th * 0.75f);
        dl->AddLine(ImVec2(c.x - r * 0.60f, c.y), ImVec2(c.x - r * 0.95f, c.y), color, th * 0.75f);
        dl->AddLine(ImVec2(c.x + r * 0.60f, c.y), ImVec2(c.x + r * 0.95f, c.y), color, th * 0.75f);
        dl->AddLine(ImVec2(c.x - r * 0.32f, c.y + r * 0.58f), ImVec2(c.x - r * 0.58f, c.y + r * 0.90f), color, th * 0.75f);
        dl->AddLine(ImVec2(c.x + r * 0.32f, c.y + r * 0.58f), ImVec2(c.x + r * 0.58f, c.y + r * 0.90f), color, th * 0.75f);
        break;
    case EditorGlyphIcon::Camera:
        dl->AddRect(ImVec2(min.x + w * 0.18f, min.y + h * 0.34f), ImVec2(max.x - w * 0.24f, max.y - h * 0.24f), color, 1.0f, 0, th);
        dl->AddTriangleFilled(ImVec2(max.x - w * 0.24f, c.y - h * 0.14f), ImVec2(max.x - w * 0.08f, c.y - h * 0.24f), ImVec2(max.x - w * 0.08f, c.y + h * 0.24f), color);
        dl->AddCircle(ImVec2(min.x + w * 0.42f, c.y), r * 0.22f, color, 12, th * 0.75f);
        break;
    case EditorGlyphIcon::EyeVisible:
        dl->AddBezierCubic(ImVec2(min.x + w * 0.12f, c.y), ImVec2(min.x + w * 0.34f, min.y + h * 0.24f), ImVec2(max.x - w * 0.34f, min.y + h * 0.24f), ImVec2(max.x - w * 0.12f, c.y), color, th, 12);
        dl->AddBezierCubic(ImVec2(min.x + w * 0.12f, c.y), ImVec2(min.x + w * 0.34f, max.y - h * 0.24f), ImVec2(max.x - w * 0.34f, max.y - h * 0.24f), ImVec2(max.x - w * 0.12f, c.y), color, th, 12);
        dl->AddCircle(c, r * 0.32f, color, 14, th);
        break;
    case EditorGlyphIcon::EyeHidden:
        dl->AddBezierCubic(ImVec2(min.x + w * 0.12f, c.y), ImVec2(min.x + w * 0.34f, min.y + h * 0.24f), ImVec2(max.x - w * 0.34f, min.y + h * 0.24f), ImVec2(max.x - w * 0.12f, c.y), color, th, 12);
        dl->AddBezierCubic(ImVec2(min.x + w * 0.12f, c.y), ImVec2(min.x + w * 0.34f, max.y - h * 0.24f), ImVec2(max.x - w * 0.34f, max.y - h * 0.24f), ImVec2(max.x - w * 0.12f, c.y), color, th, 12);
        dl->AddLine(ImVec2(min.x + w * 0.20f, max.y - h * 0.18f), ImVec2(max.x - w * 0.18f, min.y + h * 0.18f), color, th);
        break;
    case EditorGlyphIcon::Lock:
        dl->AddRect(ImVec2(min.x + w * 0.24f, c.y - h * 0.02f), ImVec2(max.x - w * 0.24f, max.y - h * 0.20f), color, 1.0f, 0, th);
        dl->PathArcTo(ImVec2(c.x, c.y - h * 0.03f), r * 0.55f, pi, 0.0f, 12);
        dl->PathStroke(color, 0, th);
        break;
    case EditorGlyphIcon::Unlock:
        dl->AddRect(ImVec2(min.x + w * 0.24f, c.y - h * 0.02f), ImVec2(max.x - w * 0.24f, max.y - h * 0.20f), color, 1.0f, 0, th);
        dl->PathArcTo(ImVec2(c.x - w * 0.08f, c.y - h * 0.03f), r * 0.55f, pi, -0.20f, 12);
        dl->PathStroke(color, 0, th);
        dl->AddLine(ImVec2(c.x + r * 0.48f, c.y - h * 0.03f), ImVec2(max.x - w * 0.16f, c.y - h * 0.16f), color, th);
        break;
    case EditorGlyphIcon::Folder:
        dl->AddRectFilled(ImVec2(min.x + w * 0.14f, min.y + h * 0.35f), ImVec2(max.x - w * 0.12f, max.y - h * 0.18f), color, 2.0f);
        dl->AddRectFilled(ImVec2(min.x + w * 0.18f, min.y + h * 0.22f), ImVec2(min.x + w * 0.52f, min.y + h * 0.40f), color, 2.0f);
        break;
    case EditorGlyphIcon::File:
        dl->AddRect(ImVec2(min.x + w * 0.24f, min.y + h * 0.14f), ImVec2(max.x - w * 0.20f, max.y - h * 0.12f), color, 1.0f, 0, th);
        dl->AddLine(ImVec2(max.x - w * 0.36f, min.y + h * 0.14f), ImVec2(max.x - w * 0.20f, min.y + h * 0.30f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.36f, min.y + h * 0.14f), ImVec2(max.x - w * 0.36f, min.y + h * 0.30f), color, th);
        break;
    case EditorGlyphIcon::Texture:
        dl->AddRect(ImVec2(min.x + w * 0.16f, min.y + h * 0.20f), ImVec2(max.x - w * 0.16f, max.y - h * 0.20f), color, 1.0f, 0, th);
        dl->AddCircleFilled(ImVec2(min.x + w * 0.36f, min.y + h * 0.38f), s * 0.055f, color);
        dl->AddLine(ImVec2(min.x + w * 0.22f, max.y - h * 0.24f), ImVec2(min.x + w * 0.44f, min.y + h * 0.56f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.44f, min.y + h * 0.56f), ImVec2(min.x + w * 0.58f, max.y - h * 0.34f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.58f, max.y - h * 0.34f), ImVec2(max.x - w * 0.22f, max.y - h * 0.24f), color, th);
        break;
    case EditorGlyphIcon::Environment:
        dl->AddCircle(c, r, color, 24, th);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), color, th);
        dl->PathArcTo(c, r * 0.62f, -pi * 0.5f, pi * 0.5f, 12);
        dl->PathStroke(color, 0, th);
        dl->PathArcTo(c, r * 0.62f, pi * 0.5f, pi * 1.5f, 12);
        dl->PathStroke(color, 0, th);
        break;
    case EditorGlyphIcon::Model:
        dl->AddLine(ImVec2(c.x, min.y + h * 0.16f), ImVec2(max.x - w * 0.18f, min.y + h * 0.34f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.18f, min.y + h * 0.34f), ImVec2(max.x - w * 0.18f, max.y - h * 0.30f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.18f, max.y - h * 0.30f), ImVec2(c.x, max.y - h * 0.12f), color, th);
        dl->AddLine(ImVec2(c.x, max.y - h * 0.12f), ImVec2(min.x + w * 0.18f, max.y - h * 0.30f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.18f, max.y - h * 0.30f), ImVec2(min.x + w * 0.18f, min.y + h * 0.34f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.18f, min.y + h * 0.34f), ImVec2(c.x, min.y + h * 0.16f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.18f, min.y + h * 0.34f), ImVec2(c.x, min.y + h * 0.52f), color, th * 0.75f);
        dl->AddLine(ImVec2(max.x - w * 0.18f, min.y + h * 0.34f), ImVec2(c.x, min.y + h * 0.52f), color, th * 0.75f);
        dl->AddLine(ImVec2(c.x, min.y + h * 0.52f), ImVec2(c.x, max.y - h * 0.12f), color, th * 0.75f);
        break;
    case EditorGlyphIcon::Material:
        dl->AddCircleFilled(c, r * 0.82f, color);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.25f, c.y - r * 0.18f), r * 0.16f, IM_COL32(20, 23, 27, 255));
        dl->AddCircleFilled(ImVec2(c.x + r * 0.24f, c.y + r * 0.08f), r * 0.13f, IM_COL32(20, 23, 27, 255));
        break;
    case EditorGlyphIcon::SceneFile:
        dl->AddRect(ImVec2(min.x + w * 0.18f, min.y + h * 0.18f), ImVec2(max.x - w * 0.18f, max.y - h * 0.18f), color, 1.0f, 0, th);
        dl->AddCircle(ImVec2(c.x, c.y + r * 0.08f), r * 0.34f, color, 16, th);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.42f), ImVec2(c.x, c.y + r * 0.42f), color, th * 0.8f);
        dl->AddLine(ImVec2(c.x - r * 0.42f, c.y), ImVec2(c.x + r * 0.42f, c.y), color, th * 0.8f);
        break;
    case EditorGlyphIcon::ProjectFile:
        dl->AddRect(ImVec2(min.x + w * 0.16f, min.y + h * 0.24f), ImVec2(max.x - w * 0.16f, max.y - h * 0.20f), color, 2.0f, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.28f, min.y + h * 0.46f), ImVec2(max.x - w * 0.28f, min.y + h * 0.46f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.28f, min.y + h * 0.62f), ImVec2(max.x - w * 0.40f, min.y + h * 0.62f), color, th);
        break;
    case EditorGlyphIcon::IesProfile:
        dl->AddLine(ImVec2(c.x, min.y + h * 0.18f), ImVec2(c.x, max.y - h * 0.18f), color, th);
        dl->PathArcTo(c, r * 0.85f, 0.18f, pi - 0.18f, 18);
        dl->PathStroke(color, 0, th);
        dl->AddLine(c, ImVec2(min.x + w * 0.24f, max.y - h * 0.22f), color, th * 0.75f);
        dl->AddLine(c, ImVec2(max.x - w * 0.24f, max.y - h * 0.22f), color, th * 0.75f);
        break;
    case EditorGlyphIcon::VolumeFile:
        dl->AddCircle(ImVec2(c.x - r * 0.26f, c.y), r * 0.42f, color, 16, th);
        dl->AddCircle(ImVec2(c.x + r * 0.26f, c.y), r * 0.42f, color, 16, th);
        dl->AddCircle(ImVec2(c.x, c.y - r * 0.30f), r * 0.42f, color, 16, th);
        break;
    case EditorGlyphIcon::ShaderFile:
        dl->AddLine(ImVec2(min.x + w * 0.36f, min.y + h * 0.24f), ImVec2(min.x + w * 0.18f, c.y), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.18f, c.y), ImVec2(min.x + w * 0.36f, max.y - h * 0.24f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.36f, min.y + h * 0.24f), ImVec2(max.x - w * 0.18f, c.y), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.18f, c.y), ImVec2(max.x - w * 0.36f, max.y - h * 0.24f), color, th);
        dl->AddLine(ImVec2(c.x + r * 0.20f, min.y + h * 0.18f), ImVec2(c.x - r * 0.20f, max.y - h * 0.18f), color, th);
        break;
    case EditorGlyphIcon::ConfigFile:
        dl->AddCircle(c, r * 0.26f, color, 14, th);
        for (int i = 0; i < 8; ++i) {
            const float a = (pi * 2.0f * static_cast<float>(i)) / 8.0f;
            const ImVec2 p0(c.x + std::cos(a) * r * 0.45f, c.y + std::sin(a) * r * 0.45f);
            const ImVec2 p1(c.x + std::cos(a) * r * 0.74f, c.y + std::sin(a) * r * 0.74f);
            dl->AddLine(p0, p1, color, th * 0.75f);
        }
        break;
    case EditorGlyphIcon::Light:
        dl->AddCircle(c, r * 0.42f, color, 18, th);
        for (int i = 0; i < 8; ++i) {
            const float a = (pi * 2.0f * static_cast<float>(i)) / 8.0f;
            const ImVec2 p0(c.x + std::cos(a) * r * 0.60f, c.y + std::sin(a) * r * 0.60f);
            const ImVec2 p1(c.x + std::cos(a) * r * 0.86f, c.y + std::sin(a) * r * 0.86f);
            dl->AddLine(p0, p1, color, th * 0.75f);
        }
        break;
    case EditorGlyphIcon::Sun:
        dl->AddCircleFilled(c, r * 0.42f, color);
        for (int i = 0; i < 8; ++i) {
            const float a = (pi * 2.0f * static_cast<float>(i)) / 8.0f;
            const ImVec2 p0(c.x + std::cos(a) * r * 0.58f, c.y + std::sin(a) * r * 0.58f);
            const ImVec2 p1(c.x + std::cos(a) * r * 0.88f, c.y + std::sin(a) * r * 0.88f);
            dl->AddLine(p0, p1, color, th * 0.75f);
        }
        break;
    case EditorGlyphIcon::Sky:
        dl->PathArcTo(ImVec2(c.x, c.y + r * 0.36f), r * 0.84f, pi, 0.0f, 18);
        dl->PathStroke(color, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.16f, c.y + r * 0.36f), ImVec2(max.x - w * 0.16f, c.y + r * 0.36f), color, th);
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.20f), r * 0.16f, color);
        break;
    case EditorGlyphIcon::Fog:
        for (int i = 0; i < 3; ++i) {
            const float y = min.y + h * (0.34f + static_cast<float>(i) * 0.18f);
            dl->AddLine(ImVec2(min.x + w * 0.18f, y), ImVec2(max.x - w * (i == 1 ? 0.28f : 0.18f), y), color, th);
        }
        break;
    case EditorGlyphIcon::Cloud:
        dl->PathArcTo(ImVec2(c.x - r * 0.34f, c.y + r * 0.10f), r * 0.36f, pi, pi * 2.0f, 10);
        dl->PathArcTo(ImVec2(c.x, c.y - r * 0.08f), r * 0.48f, pi, pi * 2.0f, 12);
        dl->PathArcTo(ImVec2(c.x + r * 0.42f, c.y + r * 0.12f), r * 0.34f, pi, pi * 2.0f, 10);
        dl->PathStroke(color, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.22f, c.y + r * 0.12f), ImVec2(max.x - w * 0.18f, c.y + r * 0.12f), color, th);
        break;
    case EditorGlyphIcon::PostProcess:
        dl->AddRect(ImVec2(min.x + w * 0.18f, min.y + h * 0.24f), ImVec2(max.x - w * 0.18f, max.y - h * 0.24f), color, 1.0f, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.26f, c.y), ImVec2(max.x - w * 0.26f, c.y), color, th);
        dl->AddCircleFilled(ImVec2(min.x + w * 0.38f, c.y), r * 0.13f, color);
        dl->AddCircleFilled(ImVec2(max.x - w * 0.34f, c.y), r * 0.13f, color);
        break;
    case EditorGlyphIcon::Group:
        dl->AddRect(ImVec2(min.x + w * 0.16f, min.y + h * 0.24f), ImVec2(max.x - w * 0.28f, max.y - h * 0.18f), color, 1.0f, 0, th);
        dl->AddRect(ImVec2(min.x + w * 0.28f, min.y + h * 0.14f), ImVec2(max.x - w * 0.16f, max.y - h * 0.30f), color, 1.0f, 0, th);
        break;
    case EditorGlyphIcon::Entity:
        dl->AddCircle(c, r * 0.68f, color, 18, th);
        dl->AddCircleFilled(c, r * 0.14f, color);
        break;
    case EditorGlyphIcon::Add:
        dl->AddLine(ImVec2(c.x, min.y + h * 0.22f), ImVec2(c.x, max.y - h * 0.22f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.22f, c.y), ImVec2(max.x - w * 0.22f, c.y), color, th);
        break;
    case EditorGlyphIcon::Back:
        dl->AddTriangleFilled(ImVec2(min.x + w * 0.20f, c.y), ImVec2(min.x + w * 0.56f, min.y + h * 0.24f), ImVec2(min.x + w * 0.56f, max.y - h * 0.24f), color);
        dl->AddLine(ImVec2(min.x + w * 0.52f, c.y), ImVec2(max.x - w * 0.20f, c.y), color, th);
        break;
    case EditorGlyphIcon::Forward:
        dl->AddTriangleFilled(ImVec2(max.x - w * 0.20f, c.y), ImVec2(max.x - w * 0.56f, min.y + h * 0.24f), ImVec2(max.x - w * 0.56f, max.y - h * 0.24f), color);
        dl->AddLine(ImVec2(min.x + w * 0.20f, c.y), ImVec2(max.x - w * 0.52f, c.y), color, th);
        break;
    case EditorGlyphIcon::Up:
        dl->AddTriangleFilled(ImVec2(c.x, min.y + h * 0.18f), ImVec2(min.x + w * 0.24f, c.y + h * 0.08f), ImVec2(max.x - w * 0.24f, c.y + h * 0.08f), color);
        dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, max.y - h * 0.18f), color, th);
        break;
    case EditorGlyphIcon::Refresh:
        dl->PathArcTo(c, r * 0.78f, -2.55f, 0.55f, 20);
        dl->PathStroke(color, 0, th);
        dl->PathArcTo(c, r * 0.78f, 0.58f, 3.70f, 20);
        dl->PathStroke(color, 0, th);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.76f, c.y + r * 0.28f), ImVec2(c.x + r * 0.34f, c.y + r * 0.22f), ImVec2(c.x + r * 0.58f, c.y + r * 0.62f), color);
        break;
    case EditorGlyphIcon::Hierarchy:
    case EditorGlyphIcon::List:
        for (int i = 0; i < 3; ++i) {
            const float y = min.y + h * (0.28f + static_cast<float>(i) * 0.22f);
            dl->AddCircleFilled(ImVec2(min.x + w * 0.24f, y), th, color);
            dl->AddLine(ImVec2(min.x + w * 0.38f, y), ImVec2(max.x - w * 0.18f, y), color, th);
        }
        break;
    case EditorGlyphIcon::Details:
        dl->AddRect(ImVec2(min.x + w * 0.18f, min.y + h * 0.18f), ImVec2(max.x - w * 0.18f, max.y - h * 0.18f), color, 1.0f, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.34f, min.y + h * 0.38f), ImVec2(max.x - w * 0.30f, min.y + h * 0.38f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.34f, min.y + h * 0.55f), ImVec2(max.x - w * 0.30f, min.y + h * 0.55f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.34f, min.y + h * 0.72f), ImVec2(max.x - w * 0.44f, min.y + h * 0.72f), color, th);
        break;
    case EditorGlyphIcon::Reset:
        dl->PathArcTo(c, r * 0.78f, -2.75f, 1.50f, 24);
        dl->PathStroke(color, 0, th);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.82f, c.y - r * 0.18f), ImVec2(c.x - r * 0.30f, c.y - r * 0.36f), ImVec2(c.x - r * 0.56f, c.y + r * 0.12f), color);
        break;
    case EditorGlyphIcon::More:
        dl->AddCircleFilled(ImVec2(c.x - r * 0.52f, c.y), th * 1.25f, color);
        dl->AddCircleFilled(c, th * 1.25f, color);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.52f, c.y), th * 1.25f, color);
        break;
    case EditorGlyphIcon::Play:
        dl->AddTriangleFilled(ImVec2(min.x + w * 0.32f, min.y + h * 0.22f), ImVec2(min.x + w * 0.32f, max.y - h * 0.22f), ImVec2(max.x - w * 0.24f, c.y), color);
        break;
    case EditorGlyphIcon::Pause:
        dl->AddRectFilled(ImVec2(min.x + w * 0.30f, min.y + h * 0.22f), ImVec2(min.x + w * 0.42f, max.y - h * 0.22f), color);
        dl->AddRectFilled(ImVec2(max.x - w * 0.42f, min.y + h * 0.22f), ImVec2(max.x - w * 0.30f, max.y - h * 0.22f), color);
        break;
    case EditorGlyphIcon::Stop:
        dl->AddRectFilled(ImVec2(min.x + w * 0.28f, min.y + h * 0.28f), ImVec2(max.x - w * 0.28f, max.y - h * 0.28f), color);
        break;
    case EditorGlyphIcon::Timeline:
        dl->AddLine(ImVec2(min.x + w * 0.18f, c.y), ImVec2(max.x - w * 0.18f, c.y), color, th);
        dl->AddCircleFilled(ImVec2(min.x + w * 0.30f, c.y), r * 0.17f, color);
        dl->AddCircleFilled(ImVec2(c.x, c.y), r * 0.17f, color);
        dl->AddCircleFilled(ImVec2(max.x - w * 0.30f, c.y), r * 0.17f, color);
        break;
    case EditorGlyphIcon::TimelineKey:
        dl->AddQuadFilled(ImVec2(c.x, min.y + h * 0.18f), ImVec2(max.x - w * 0.18f, c.y), ImVec2(c.x, max.y - h * 0.18f), ImVec2(min.x + w * 0.18f, c.y), color);
        dl->AddCircleFilled(c, r * 0.12f, IM_COL32(20, 23, 27, 255));
        break;
    case EditorGlyphIcon::Trash:
        dl->AddLine(ImVec2(min.x + w * 0.26f, min.y + h * 0.30f), ImVec2(max.x - w * 0.26f, min.y + h * 0.30f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.40f, min.y + h * 0.20f), ImVec2(max.x - w * 0.40f, min.y + h * 0.20f), color, th);
        dl->AddRect(ImVec2(min.x + w * 0.30f, min.y + h * 0.34f), ImVec2(max.x - w * 0.30f, max.y - h * 0.18f), color, 1.0f, 0, th);
        break;
    case EditorGlyphIcon::Save:
        dl->AddRect(ImVec2(min.x + w * 0.20f, min.y + h * 0.18f), ImVec2(max.x - w * 0.20f, max.y - h * 0.18f), color, 1.0f, 0, th);
        dl->AddRectFilled(ImVec2(min.x + w * 0.32f, min.y + h * 0.22f), ImVec2(max.x - w * 0.34f, min.y + h * 0.42f), color);
        dl->AddLine(ImVec2(min.x + w * 0.34f, max.y - h * 0.30f), ImVec2(max.x - w * 0.34f, max.y - h * 0.30f), color, th);
        break;
    case EditorGlyphIcon::Import:
        dl->AddRect(ImVec2(min.x + w * 0.22f, min.y + h * 0.18f), ImVec2(max.x - w * 0.18f, max.y - h * 0.18f), color, 1.0f, 0, th);
        dl->AddTriangleFilled(ImVec2(c.x, min.y + h * 0.30f), ImVec2(c.x - w * 0.16f, min.y + h * 0.48f), ImVec2(c.x + w * 0.16f, min.y + h * 0.48f), color);
        dl->AddLine(ImVec2(c.x, min.y + h * 0.32f), ImVec2(c.x, max.y - h * 0.34f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.34f, max.y - h * 0.32f), ImVec2(max.x - w * 0.30f, max.y - h * 0.32f), color, th);
        break;
    case EditorGlyphIcon::Render:
        dl->AddCircle(c, r * 0.76f, color, 22, th);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.20f, c.y - r * 0.42f), ImVec2(c.x - r * 0.20f, c.y + r * 0.42f), ImVec2(c.x + r * 0.52f, c.y), color);
        break;
    case EditorGlyphIcon::Command:
        dl->AddRect(ImVec2(min.x + w * 0.18f, min.y + h * 0.24f), ImVec2(max.x - w * 0.18f, max.y - h * 0.24f), color, 1.0f, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.28f, c.y), ImVec2(min.x + w * 0.42f, c.y + r * 0.20f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.28f, c.y), ImVec2(min.x + w * 0.42f, c.y - r * 0.20f), color, th);
        dl->AddLine(ImVec2(min.x + w * 0.52f, c.y), ImVec2(max.x - w * 0.26f, c.y), color, th);
        break;
    case EditorGlyphIcon::Layout:
        dl->AddRect(ImVec2(min.x + w * 0.16f, min.y + h * 0.18f), ImVec2(max.x - w * 0.16f, max.y - h * 0.18f), color, 1.0f, 0, th);
        dl->AddLine(ImVec2(c.x, min.y + h * 0.18f), ImVec2(c.x, max.y - h * 0.18f), color, th * 0.75f);
        dl->AddLine(ImVec2(min.x + w * 0.16f, c.y), ImVec2(max.x - w * 0.16f, c.y), color, th * 0.75f);
        break;
    case EditorGlyphIcon::Window:
        dl->AddRect(ImVec2(min.x + w * 0.16f, min.y + h * 0.20f), ImVec2(max.x - w * 0.16f, max.y - h * 0.18f), color, 1.0f, 0, th);
        dl->AddLine(ImVec2(min.x + w * 0.16f, min.y + h * 0.34f), ImVec2(max.x - w * 0.16f, min.y + h * 0.34f), color, th * 0.75f);
        dl->AddCircleFilled(ImVec2(min.x + w * 0.28f, min.y + h * 0.27f), th * 0.70f, color);
        dl->AddCircleFilled(ImVec2(min.x + w * 0.40f, min.y + h * 0.27f), th * 0.70f, color);
        break;
    case EditorGlyphIcon::Undo:
        dl->PathArcTo(c, r * 0.76f, -2.80f, 0.65f, 24);
        dl->PathStroke(color, 0, th);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.88f, c.y - r * 0.10f), ImVec2(c.x - r * 0.40f, c.y - r * 0.34f), ImVec2(c.x - r * 0.52f, c.y + r * 0.20f), color);
        break;
    case EditorGlyphIcon::Redo:
        dl->PathArcTo(c, r * 0.76f, pi - 0.65f, pi + 2.80f, 24);
        dl->PathStroke(color, 0, th);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.88f, c.y - r * 0.10f), ImVec2(c.x + r * 0.40f, c.y - r * 0.34f), ImVec2(c.x + r * 0.52f, c.y + r * 0.20f), color);
        break;
    case EditorGlyphIcon::Exit:
        dl->AddLine(ImVec2(min.x + w * 0.26f, min.y + h * 0.26f), ImVec2(max.x - w * 0.26f, max.y - h * 0.26f), color, th);
        dl->AddLine(ImVec2(max.x - w * 0.26f, min.y + h * 0.26f), ImVec2(min.x + w * 0.26f, max.y - h * 0.26f), color, th);
        break;
    }
}

inline ImVec2 editorIconButtonSize() {
    return ImVec2(22.0f, 20.0f);
}

inline float editorIconTextButtonWidth(const char* label) {
    return EditorUiMetric::iconTextButtonTextOffsetX + ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x;
}

inline bool editorIconButton(const char* id, EditorGlyphIcon icon, bool active, ImVec2 size = editorIconButtonSize()) {
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered || ImGui::IsItemActive()) {
        const ImU32 bg = active ? IM_COL32(30, 62, 104, 205) : IM_COL32(29, 34, 42, 175);
        dl->AddRectFilled(min, max, bg, EditorUiMetric::compactButtonRounding);
        dl->AddRect(min, max, active ? IM_COL32(62, 112, 178, 220) : IM_COL32(58, 66, 78, 150), EditorUiMetric::compactButtonRounding);
    }
    editorDrawIconGlyph(icon, ImVec2(min.x + 4.0f, min.y + 3.0f), ImVec2(max.x - 4.0f, max.y - 3.0f), ImGui::GetColorU32(editorIconTint(active)));
    return pressed;
}

inline bool editorIconTextButton(const char* id, EditorGlyphIcon icon, const char* label, bool active = false) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 size(EditorUiMetric::iconTextButtonTextOffsetX + textSize.x + style.FramePadding.x, editorIconButtonSize().y);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered || ImGui::IsItemActive()) {
        const ImU32 bg = active ? IM_COL32(30, 62, 104, 205) : IM_COL32(29, 34, 42, 175);
        dl->AddRectFilled(min, max, bg, EditorUiMetric::compactButtonRounding);
        dl->AddRect(min, max, active ? IM_COL32(62, 112, 178, 220) : IM_COL32(58, 66, 78, 150), EditorUiMetric::compactButtonRounding);
    }
    const ImU32 color = ImGui::GetColorU32(editorIconTint(active));
    editorDrawIconGlyph(icon, ImVec2(min.x + 6.0f, min.y + 3.0f), ImVec2(min.x + 22.0f, max.y - 3.0f), color);
    dl->AddText(ImVec2(min.x + EditorUiMetric::iconTextButtonTextOffsetX, min.y + (size.y - textSize.y) * 0.5f), color, label);
    return pressed;
}

inline void editorIconTextReadout(EditorGlyphIcon icon, const char* label, ImU32 color) {
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 size(18.0f + EditorUiMetric::iconTextReadoutTextGap + textSize.x, editorIconButtonSize().y);
    ImGui::InvisibleButton("##iconTextReadout", size);
    const ImVec2 min = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    editorDrawIconGlyph(icon, ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(min.x + 18.0f, min.y + size.y - 3.0f), color);
    dl->AddText(ImVec2(min.x + 18.0f + EditorUiMetric::iconTextReadoutTextGap, min.y + (size.y - textSize.y) * 0.5f), color, label);
}

inline bool editorGlyphMenuItem(EditorGlyphIcon icon, const char* label, bool enabled = true, const char* shortcut = nullptr, bool selected = false) {
    const std::string padded = editorGlyphLabel(label);
    const bool clicked = ImGui::MenuItem(padded.c_str(), shortcut, selected, enabled);
    if (ImGui::IsItemVisible()) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float rowHeight = max.y - min.y;
        const float iconSize = rowHeight > 0.0f ? std::min(16.0f, rowHeight - 4.0f) : 14.0f;
        const float y = min.y + (rowHeight - iconSize) * 0.5f;
        if (!enabled) {
            editorDrawDisabledRowChrome(min, max);
        }
        const ImVec4 tint = enabled ? editorIconTint(selected) : editorDisabledIconTint();
        editorDrawIconGlyph(
            icon,
            ImVec2(min.x + 8.0f, y),
            ImVec2(min.x + 8.0f + iconSize, y + iconSize),
            ImGui::GetColorU32(tint));
    }
    return clicked;
}

inline bool editorGlyphBeginMenu(EditorGlyphIcon icon, const char* label, bool enabled = true) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const std::string padded = editorGlyphLabel(label);
    const bool open = ImGui::BeginMenu(padded.c_str(), enabled);
    if (ImGui::IsItemVisible()) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float rowHeight = max.y - min.y;
        const float iconSize = rowHeight > 0.0f ? std::min(16.0f, rowHeight - 4.0f) : 14.0f;
        const float y = min.y + (rowHeight - iconSize) * 0.5f;
        if (!enabled && drawList != nullptr) {
            const ImU32 accent = ImGui::GetColorU32(editorDisabledRowAccentColor());
            drawList->AddLine(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(min.x + 2.0f, max.y - 3.0f), accent, 1.0f);
            drawList->AddRect(min, max, accent, 1.0f);
        }
        const ImVec4 tint = enabled ? editorIconTint(open) : editorDisabledIconTint();
        editorDrawIconGlyph(
            icon,
            ImVec2(min.x + 8.0f, y),
            ImVec2(min.x + 8.0f + iconSize, y + iconSize),
            ImGui::GetColorU32(tint));
    }
    return open;
}

inline std::string editorLowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline bool editorIsTexturePath(const std::filesystem::path& path) {
    const std::string ext = editorLowercase(path.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".dds" || ext == ".ktx2";
}

inline EditorGlyphIcon editorGlyphForPath(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) return EditorGlyphIcon::Folder;
    const std::string ext = editorLowercase(path.extension().string());
    if (ext == ".rtlevel" || ext == ".mscene") return EditorGlyphIcon::SceneFile;
    if (ext == ".vproject") return EditorGlyphIcon::ProjectFile;
    if (ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".fbx") return EditorGlyphIcon::Model;
    if (ext == ".mtl") return EditorGlyphIcon::Material;
    if (editorIsTexturePath(path)) return EditorGlyphIcon::Texture;
    if (ext == ".hdr" || ext == ".exr") return EditorGlyphIcon::Environment;
    if (ext == ".ies") return EditorGlyphIcon::IesProfile;
    if (ext == ".vdb") return EditorGlyphIcon::VolumeFile;
    if (ext == ".glsl" || ext == ".hlsl" || ext == ".spv") return EditorGlyphIcon::ShaderFile;
    if (ext == ".json" || ext == ".ini" || ext == ".toml" || ext == ".yaml" || ext == ".yml") return EditorGlyphIcon::ConfigFile;
    return EditorGlyphIcon::File;
}

inline EditorGlyphIcon editorGlyphForEntity(const Entity& entity) {
    if (entity.camera.has_value()) return EditorGlyphIcon::Camera;
    if (entity.sun.has_value()) return EditorGlyphIcon::Sun;
    if (entity.light.has_value()) return EditorGlyphIcon::Light;
    if (entity.environmentLight.has_value()) return EditorGlyphIcon::Environment;
    if (entity.skyAtmosphere.has_value()) return EditorGlyphIcon::Sky;
    if (entity.heightFog.has_value()) return EditorGlyphIcon::Fog;
    if (entity.volumetricCloud.has_value()) return EditorGlyphIcon::Cloud;
    if (entity.postProcessVolume.has_value()) return EditorGlyphIcon::PostProcess;
    if (entity.meshRenderer.has_value()) return EditorGlyphIcon::Model;
    if (!entity.children.empty()) return EditorGlyphIcon::Group;
    return EditorGlyphIcon::Entity;
}

} // namespace rtv
