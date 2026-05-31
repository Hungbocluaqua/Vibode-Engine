#include "rtv/EditorDockspace.h"

#include "rtv/EditorCommands.h"
#include "rtv/FileDialog.h"
#include "rtv/KeyBindings.h"
#include "rtv/UndoStack.h"

#include <imgui.h>
#include <imgui_internal.h>

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

bool commandMenuItem(EditorCommandId id, bool enabled = true) {
    return ImGui::MenuItem(editorCommandName(id), editorCommandShortcut(id), false, enabled);
}

} // namespace

void EditorDockspace::begin(EditorRuntimeState& state, EditorPanelVisibility& visibility, EditorRequests& requests) {
    drawMainMenu(state, visibility, requests);

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
        ImGui::SaveIniSettingsToDisk(profilePath_.string().c_str());
    }
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
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.285f, &right, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.292f, &bottom, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.470f, &rightBottom, &right);

    ImGui::DockBuilderDockWindow("Scene", center);
    ImGui::DockBuilderDockWindow("Hierarchy", right);
    ImGui::DockBuilderDockWindow("Render World Settings", right);
    ImGui::DockBuilderDockWindow("Inspector", rightBottom);
    ImGui::DockBuilderDockWindow("Material Editor", rightBottom);
    ImGui::DockBuilderDockWindow("Content", bottom);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderDockWindow("Log", bottom);
    ImGui::DockBuilderDockWindow("Debug / Profiler", bottom);
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
            requests.importAndPlace = *path;
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

    if (ImGui::BeginMenu("File")) {
        if (commandMenuItem(EditorCommandId::ProjectManager)) { executeCommand(EditorCommandId::ProjectManager, state, visibility, requests); }
        if (state.project != nullptr) {
            if (commandMenuItem(EditorCommandId::CloseProject)) { executeCommand(EditorCommandId::CloseProject, state, visibility, requests); }
        }
        ImGui::Separator();
        if (commandMenuItem(EditorCommandId::NewScene)) { executeCommand(EditorCommandId::NewScene, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::OpenScene)) { executeCommand(EditorCommandId::OpenScene, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SaveScene)) { executeCommand(EditorCommandId::SaveScene, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SaveSceneAs)) { executeCommand(EditorCommandId::SaveSceneAs, state, visibility, requests); }
        ImGui::Separator();
        const std::string undoLabel = state.undoStack != nullptr && state.undoStack->canUndo()
            ? std::string("Undo ") + state.undoStack->undoLabel()
            : std::string("Undo");
        if (ImGui::MenuItem(undoLabel.c_str(), editorCommandShortcut(EditorCommandId::Undo), false, state.undoStack != nullptr && state.undoStack->canUndo())) {
            executeCommand(EditorCommandId::Undo, state, visibility, requests);
        }
        const std::string redoLabel = state.undoStack != nullptr && state.undoStack->canRedo()
            ? std::string("Redo ") + state.undoStack->redoLabel()
            : std::string("Redo");
        if (ImGui::MenuItem(redoLabel.c_str(), editorCommandShortcut(EditorCommandId::Redo), false, state.undoStack != nullptr && state.undoStack->canRedo())) {
            executeCommand(EditorCommandId::Redo, state, visibility, requests);
        }
        ImGui::Separator();
        if (commandMenuItem(EditorCommandId::ImportAsset)) { executeCommand(EditorCommandId::ImportAsset, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::ImportAndPlace)) { executeCommand(EditorCommandId::ImportAndPlace, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::ImportSceneAsNewScene)) { executeCommand(EditorCommandId::ImportSceneAsNewScene, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::MergeScene)) { executeCommand(EditorCommandId::MergeScene, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::ImportHdri)) { executeCommand(EditorCommandId::ImportHdri, state, visibility, requests); }
        ImGui::Separator();
        if (commandMenuItem(EditorCommandId::Exit)) { executeCommand(EditorCommandId::Exit, state, visibility, requests); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create")) {
        if (commandMenuItem(EditorCommandId::CreateEmptyEntity)) { executeCommand(EditorCommandId::CreateEmptyEntity, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreateCamera)) { executeCommand(EditorCommandId::CreateCamera, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreatePointLight)) { executeCommand(EditorCommandId::CreatePointLight, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreateSpotLight)) { executeCommand(EditorCommandId::CreateSpotLight, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreateAreaLight)) { executeCommand(EditorCommandId::CreateAreaLight, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreatePrimarySun)) { executeCommand(EditorCommandId::CreatePrimarySun, state, visibility, requests); }
        ImGui::Separator();
        if (commandMenuItem(EditorCommandId::CreateEnvironmentLight)) { executeCommand(EditorCommandId::CreateEnvironmentLight, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreateSkyAtmosphere)) { executeCommand(EditorCommandId::CreateSkyAtmosphere, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreateHeightFog)) { executeCommand(EditorCommandId::CreateHeightFog, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreateVolumetricCloud)) { executeCommand(EditorCommandId::CreateVolumetricCloud, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CreatePostProcessVolume)) { executeCommand(EditorCommandId::CreatePostProcessVolume, state, visibility, requests); }
        ImGui::Separator();
        ImGui::MenuItem("Mesh From Asset", nullptr, false, false);
        ImGui::MenuItem("Prefab", nullptr, false, false);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Engine")) {
        ImGui::MenuItem("Play", nullptr, false, false);
        ImGui::MenuItem("Simulate", nullptr, false, false);
        ImGui::MenuItem("Pause", nullptr, false, false);
        ImGui::MenuItem("Stop", nullptr, false, false);
        ImGui::Separator();
        if (commandMenuItem(EditorCommandId::ReloadShaders)) { executeCommand(EditorCommandId::ReloadShaders, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::ShowControls)) { executeCommand(EditorCommandId::ShowControls, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::ShowRendererInfo)) { executeCommand(EditorCommandId::ShowRendererInfo, state, visibility, requests); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        ImGui::MenuItem("Scene", nullptr, &visibility.viewport);
        ImGui::MenuItem("Hierarchy", nullptr, &visibility.sceneHierarchy);
        ImGui::MenuItem("Render World Settings", nullptr, &visibility.renderWorldSettings);
        ImGui::MenuItem("Inspector", nullptr, &visibility.inspector);
        ImGui::MenuItem("Content", nullptr, &visibility.assetBrowser);
        ImGui::MenuItem("Timeline", nullptr, &visibility.timeline);
        ImGui::MenuItem("Log", nullptr, &visibility.log);
        ImGui::MenuItem("Console", nullptr, &visibility.console);
        ImGui::Separator();
        ImGui::MenuItem("Material Editor", nullptr, &visibility.materialEditor);
        ImGui::MenuItem("Technical Render Settings", nullptr, &visibility.renderSettings);
        ImGui::MenuItem("Debug / Profiler", nullptr, &visibility.debugProfiler);
        ImGui::MenuItem("Scene Stats", nullptr, &visibility.sceneStats);
        ImGui::MenuItem("GPU Diagnostics", nullptr, &visibility.gpuDiagnostics);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        if (commandMenuItem(EditorCommandId::ResetAccumulation)) { executeCommand(EditorCommandId::ResetAccumulation, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::ToggleDenoiser)) { executeCommand(EditorCommandId::ToggleDenoiser, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CycleDebugView)) { executeCommand(EditorCommandId::CycleDebugView, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::CycleIntermediateView)) { executeCommand(EditorCommandId::CycleIntermediateView, state, visibility, requests); }
        ImGui::Separator();
        if (commandMenuItem(EditorCommandId::SetDebugBeauty)) { executeCommand(EditorCommandId::SetDebugBeauty, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SetDebugDirectLighting)) { executeCommand(EditorCommandId::SetDebugDirectLighting, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SetDebugIndirectLighting)) { executeCommand(EditorCommandId::SetDebugIndirectLighting, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SetDebugNormals)) { executeCommand(EditorCommandId::SetDebugNormals, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SetDebugDepth)) { executeCommand(EditorCommandId::SetDebugDepth, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SetDebugMotionVectors)) { executeCommand(EditorCommandId::SetDebugMotionVectors, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SetDebugVariance)) { executeCommand(EditorCommandId::SetDebugVariance, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::SetDebugAlbedo)) { executeCommand(EditorCommandId::SetDebugAlbedo, state, visibility, requests); }
        ImGui::Separator();
        ImGui::MenuItem("View Mode", nullptr, false, false);
        ImGui::MenuItem("Quality Preset", nullptr, false, false);
        ImGui::MenuItem("Technical Render Settings", nullptr, &visibility.renderSettings);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Layout")) {
        if (commandMenuItem(EditorCommandId::SaveLayout)) { executeCommand(EditorCommandId::SaveLayout, state, visibility, requests); }
        if (commandMenuItem(EditorCommandId::ResetLayout)) { executeCommand(EditorCommandId::ResetLayout, state, visibility, requests); }
        ImGui::Separator();
        ImGui::MenuItem("Workspace: Level Editing", nullptr, true, false);
        ImGui::MenuItem("UI Scale", nullptr, false, false);
        ImGui::MenuItem("Theme", nullptr, false, false);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::Text("| %s |", activeSceneTitle(state).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x##CloseSceneTab")) {
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
