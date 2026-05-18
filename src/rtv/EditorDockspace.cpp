#include "rtv/EditorDockspace.h"

#include "rtv/FileDialog.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace rtv {

void EditorDockspace::begin(EditorPanelVisibility& visibility, EditorRequests& requests) {
    drawMainMenu(visibility, requests);

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

void EditorDockspace::end() {
    ImGui::End();
}

void EditorDockspace::requestResetLayout() {
    layoutResetRequested_ = true;
}

void EditorDockspace::buildDefaultLayout() {
    ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    ImGuiID rightBottom = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, &bottom, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, &rightBottom, &right);

    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector / Properties", right);
    ImGui::DockBuilderDockWindow("Material Editor", rightBottom);
    ImGui::DockBuilderDockWindow("Asset Browser", bottom);
    ImGui::DockBuilderDockWindow("Render Settings", bottom);
    ImGui::DockBuilderDockWindow("Debug / Profiler", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorDockspace::drawMainMenu(EditorPanelVisibility& visibility, EditorRequests& requests) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open glTF")) {
            visibility.assetBrowser = true;
            if (auto path = openGltfFileDialog()) {
                requests.loadGltf = *path;
            }
        }
        if (ImGui::MenuItem("Open HDR")) {
            visibility.assetBrowser = true;
            if (auto path = openHdrFileDialog()) {
                requests.loadHdr = *path;
            }
        }
        if (ImGui::MenuItem("Open Scene JSON")) {
            visibility.assetBrowser = true;
            if (auto path = openSceneJsonFileDialog()) {
                requests.loadSceneJson = *path;
            }
        }
        if (ImGui::MenuItem("Save Scene JSON")) {
            visibility.assetBrowser = true;
            if (auto path = saveSceneJsonFileDialog()) {
                requests.saveSceneJson = *path;
            }
        }
        if (ImGui::MenuItem("Reload Shaders")) {
            requests.reloadShaders = true;
            requests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (ImGui::MenuItem("Reset Layout")) {
            requests.resetLayout = true;
            requestResetLayout();
        }
        if (ImGui::MenuItem("Exit")) {
            requests.exit = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport", nullptr, &visibility.viewport);
        ImGui::MenuItem("Scene Hierarchy", nullptr, &visibility.sceneHierarchy);
        ImGui::MenuItem("Inspector / Properties", nullptr, &visibility.inspector);
        ImGui::MenuItem("Asset Browser", nullptr, &visibility.assetBrowser);
        ImGui::MenuItem("Material Editor", nullptr, &visibility.materialEditor);
        ImGui::MenuItem("Render Settings", nullptr, &visibility.renderSettings);
        ImGui::MenuItem("Debug / Profiler", nullptr, &visibility.debugProfiler);
        if (ImGui::MenuItem("Reset Dock Layout")) {
            requests.resetLayout = true;
            requestResetLayout();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        if (ImGui::MenuItem("Reset Accumulation")) {
            requests.resetAccumulation = AccumulationResetReason::Manual;
        }
        if (ImGui::MenuItem("Toggle Denoiser")) {
            requests.toggleDenoiser = true;
        }
        if (ImGui::MenuItem("Toggle Debug View")) {
            requests.toggleDebugView = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Show controls")) {
            showControls_ = true;
        }
        if (ImGui::MenuItem("Show renderer info")) {
            showRendererInfo_ = true;
            visibility.debugProfiler = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void EditorDockspace::drawHelpWindows() {
    if (showControls_) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 220.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls", &showControls_)) {
            ImGui::TextUnformatted("Hold right mouse in viewport: look and move camera");
            ImGui::TextUnformatted("Release right mouse or press Esc: stop camera navigation");
            ImGui::TextUnformatted("WASD: move while holding right mouse");
            ImGui::TextUnformatted("Q/E or Ctrl/Space: vertical movement");
            ImGui::TextUnformatted("Shift: fast movement");
            ImGui::TextUnformatted("F1-F6, R, +/- and bracket keys remain available when editor widgets are not typing.");
        }
        ImGui::End();
    }
    if (showRendererInfo_) {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 160.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Renderer Info", &showRendererInfo_)) {
            ImGui::TextUnformatted("Compute path tracing, temporal denoising, debug views, glTF loading, HDR environments, and GPU profiling are owned by the existing renderer.");
            ImGui::TextUnformatted("The editor layer submits requests and displays renderer state without replacing the render pipeline.");
        }
        ImGui::End();
    }
}

} // namespace rtv
