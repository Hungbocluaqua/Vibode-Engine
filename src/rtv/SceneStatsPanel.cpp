#include "rtv/SceneStatsPanel.h"

#include "rtv/CameraBookmark.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/PathTracerRenderer.h"

#include <imgui.h>

#include <array>
#include <cstdio>

namespace rtv {

namespace {

void drawStatRow(const char* label, const char* value) {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, EditorUiMetric::propertyRowHeight);
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value);
}

template <typename T>
void drawStatRow(const char* label, T value) {
    const std::string text = std::to_string(value);
    drawStatRow(label, text.c_str());
}

} // namespace

void SceneStatsPanel::draw(const EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Scene Explorer")) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("SceneExplorerTabs")) {
        if (ImGui::BeginTabItem("Statistics")) {
            const MeshParamsUniform& meshParams = state.renderer.scene().meshParams();
            const RayTracingRendererStats rt = state.renderer.rayTracingStats();
            if (ImGui::BeginTable("SceneStatisticsTable", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
                ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                if (state.sceneDocument != nullptr) {
                    drawStatRow("Entities", state.sceneDocument->registry().liveCount());
                }
                drawStatRow("Primitives", meshParams.primitiveCount);
                drawStatRow("Instances", meshParams.instanceCount);
                drawStatRow("Triangles", meshParams.triangleCount);
                drawStatRow("Lights", meshParams.lightCount);
                drawStatRow("Materials", meshParams.materialCount);
                char textures[64]{};
                std::snprintf(
                    textures,
                    sizeof(textures),
                    "%u / %u",
                    state.renderer.scene().materialTextureCount(),
                    state.renderer.scene().materialTextureSlotCapacity());
                drawStatRow("Textures", static_cast<const char*>(textures));
                if (rt.active) {
                    char memory[64]{};
                    std::snprintf(memory, sizeof(memory), "%.2f MB", static_cast<double>(rt.accelerationStructureBytes) / (1024.0 * 1024.0));
                    drawStatRow("RT memory", static_cast<const char*>(memory));
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Bookmarks")) {
            static std::array<char, 96> name{};
            ImGui::SetNextItemWidth(-92.0f);
            ImGui::InputTextWithHint("##BookmarkName", "Bookmark name", name.data(), name.size());
            ImGui::SameLine();
            if (editorIconTextButton("SaveCameraBookmark", EditorGlyphIcon::Save, "Save")) {
                std::string label = name.data();
                if (label.empty()) {
                    label = "Camera " + std::to_string(state.cameraBookmarks != nullptr ? state.cameraBookmarks->count() + 1u : 1u);
                }
                requests.saveCameraBookmark = std::move(label);
                name.fill('\0');
            }

            ImGui::Separator();
            if (state.cameraBookmarks == nullptr || state.cameraBookmarks->bookmarks().empty()) {
                ImGui::TextDisabled("No camera bookmarks");
            } else {
                const auto& bookmarks = state.cameraBookmarks->bookmarks();
                for (size_t index = 0; index < bookmarks.size(); ++index) {
                    ImGui::PushID(static_cast<int>(index));
                    const CameraBookmark& bookmark = bookmarks[index];
                    if (ImGui::Selectable(bookmark.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            requests.loadCameraBookmarkIndex = index;
                        }
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("Double-click to load");
                    }
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - 56.0f);
                    if (editorIconButton("LoadBookmark", EditorGlyphIcon::Camera, false, ImVec2(24.0f, 22.0f))) {
                        requests.loadCameraBookmarkIndex = index;
                    }
                    ImGui::SameLine();
                    if (editorIconButton("DeleteBookmark", EditorGlyphIcon::Trash, false, ImVec2(24.0f, 22.0f))) {
                        requests.deleteCameraBookmarkIndex = index;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace rtv
