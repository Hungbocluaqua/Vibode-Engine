#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

enum class EditorCommandId : uint32_t {
    ProjectManager,
    CloseProject,
    NewScene,
    OpenScene,
    SaveScene,
    SaveSceneAs,
    ImportAsset,
    ImportAndPlace,
    ImportSceneAsNewScene,
    MergeScene,
    ImportHdri,
    Exit,
    CreateEmptyEntity,
    CreateCamera,
    CreatePointLight,
    CreateSpotLight,
    CreateAreaLight,
    CreatePrimarySun,
    CreateEnvironmentLight,
    CreateSkyAtmosphere,
    CreateHeightFog,
    CreateVolumetricCloud,
    CreatePostProcessVolume,
    ReloadShaders,
    ShowControls,
    ShowRendererInfo,
    ResetAccumulation,
    ToggleDenoiser,
    ToggleMovingDenoiser,
    ToggleSun,
    ToggleEnvironment,
    ToggleDirectLighting,
    CycleDebugView,
    CycleIntermediateView,
    SetDebugBeauty,
    SetDebugDirectLighting,
    SetDebugIndirectLighting,
    SetDebugNormals,
    SetDebugDepth,
    SetDebugMotionVectors,
    SetDebugVariance,
    SetDebugAlbedo,
    SetToneMapperLinear,
    SetToneMapperReinhard,
    SetToneMapperAces,
    SetToneMapperPbrNeutral,
    SetToneMapperAgx,
    ToggleAutoExposure,
    SaveLayout,
    ResetLayout,
    Undo,
    Redo,
    ToggleFullscreen,
    ViewportSelect,
    ViewportMove,
    ViewportRotate,
    ViewportScale,
    ViewportToggleLocal,
    ViewportToggleSnap,
    ViewportFrameSelected,
    ViewportToggleGrid,
    ViewportToggleAxes,
};

enum class EditorCommandContext : uint32_t {
    Global,
    SceneEditing,
    Viewport,
    Modal,
    TextInput,
};

struct EditorKeybinding {
    int glfwKey = -1;
    int imguiKey = -1;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    EditorCommandContext context = EditorCommandContext::Global;
    std::string display;
};

struct EditorCommand {
    EditorCommandId id = EditorCommandId::NewScene;
    std::string name;
    std::string category;
    std::string description;
    EditorKeybinding defaultKeybinding{};
};

class CommandRegistry {
public:
    void registerCommand(EditorCommand command);
    [[nodiscard]] const EditorCommand* find(EditorCommandId id) const;
    [[nodiscard]] const std::vector<EditorCommand>& commands() const { return commands_; }
    [[nodiscard]] std::vector<std::string> detectConflicts() const;

private:
    std::vector<EditorCommand> commands_;
};

[[nodiscard]] const CommandRegistry& defaultEditorCommandRegistry();
[[nodiscard]] const EditorCommand* editorCommand(EditorCommandId id);
[[nodiscard]] const char* editorCommandShortcut(EditorCommandId id);
[[nodiscard]] const char* editorCommandName(EditorCommandId id);
[[nodiscard]] uint32_t editorCommandContextPrecedence(EditorCommandContext context);

} // namespace rtv
