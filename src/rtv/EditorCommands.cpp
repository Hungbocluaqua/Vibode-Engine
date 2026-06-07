#include "rtv/EditorCommands.h"

#include "rtv/EditorPreferences.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace rtv {

namespace {

EditorKeybinding key(int glfwKey, int imguiKey, std::string display, EditorCommandContext context = EditorCommandContext::Global) {
    return EditorKeybinding{glfwKey, imguiKey, false, false, false, context, std::move(display)};
}

EditorKeybinding ctrlKey(int glfwKey, int imguiKey, std::string display, EditorCommandContext context = EditorCommandContext::Global) {
    return EditorKeybinding{glfwKey, imguiKey, true, false, false, context, std::move(display)};
}

EditorKeybinding ctrlShiftKey(int glfwKey, int imguiKey, std::string display, EditorCommandContext context = EditorCommandContext::Global) {
    return EditorKeybinding{glfwKey, imguiKey, true, true, false, context, std::move(display)};
}

std::string conflictKey(const EditorKeybinding& binding) {
    if (binding.display.empty()) {
        return {};
    }
    std::ostringstream out;
    out << static_cast<uint32_t>(binding.context) << ':';
    if (binding.ctrl) out << "Ctrl+";
    if (binding.shift) out << "Shift+";
    if (binding.alt) out << "Alt+";
    out << binding.display;
    return out.str();
}

std::string upperString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::optional<std::pair<int, int>> keyCodesForToken(const std::string& token) {
    if (token.size() == 1) {
        const char ch = token[0];
        if (ch >= 'A' && ch <= 'Z') {
            const int offset = ch - 'A';
            return std::make_pair(GLFW_KEY_A + offset, ImGuiKey_A + offset);
        }
        if (ch >= '0' && ch <= '9') {
            const int offset = ch - '0';
            return std::make_pair(GLFW_KEY_0 + offset, ImGuiKey_0 + offset);
        }
    }
    if (token.size() >= 2 && token[0] == 'F') {
        const int number = std::atoi(token.c_str() + 1);
        if (number >= 1 && number <= 12) {
            return std::make_pair(GLFW_KEY_F1 + (number - 1), ImGuiKey_F1 + (number - 1));
        }
    }
    if (token == "SPACE") return std::make_pair(GLFW_KEY_SPACE, ImGuiKey_Space);
    if (token == "TAB") return std::make_pair(GLFW_KEY_TAB, ImGuiKey_Tab);
    if (token == "ENTER" || token == "RETURN") return std::make_pair(GLFW_KEY_ENTER, ImGuiKey_Enter);
    if (token == "ESC" || token == "ESCAPE") return std::make_pair(GLFW_KEY_ESCAPE, ImGuiKey_Escape);
    if (token == "DELETE" || token == "DEL") return std::make_pair(GLFW_KEY_DELETE, ImGuiKey_Delete);
    if (token == "BACKSPACE") return std::make_pair(GLFW_KEY_BACKSPACE, ImGuiKey_Backspace);
    if (token == "LEFT") return std::make_pair(GLFW_KEY_LEFT, ImGuiKey_LeftArrow);
    if (token == "RIGHT") return std::make_pair(GLFW_KEY_RIGHT, ImGuiKey_RightArrow);
    if (token == "UP") return std::make_pair(GLFW_KEY_UP, ImGuiKey_UpArrow);
    if (token == "DOWN") return std::make_pair(GLFW_KEY_DOWN, ImGuiKey_DownArrow);
    return std::nullopt;
}

std::optional<EditorKeybinding> parseShortcutDisplay(std::string display, EditorCommandContext context) {
    display.erase(std::remove_if(display.begin(), display.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), display.end());
    if (display.empty()) {
        return std::nullopt;
    }
    EditorKeybinding binding;
    binding.context = context;
    binding.display = display;
    std::stringstream stream(display);
    std::string token;
    while (std::getline(stream, token, '+')) {
        token = upperString(token);
        if (token == "CTRL" || token == "CONTROL" || token == "CMD") {
            binding.ctrl = true;
            continue;
        }
        if (token == "SHIFT") {
            binding.shift = true;
            continue;
        }
        if (token == "ALT" || token == "OPTION") {
            binding.alt = true;
            continue;
        }
        const std::optional<std::pair<int, int>> codes = keyCodesForToken(token);
        if (!codes.has_value()) {
            return std::nullopt;
        }
        binding.glfwKey = codes->first;
        binding.imguiKey = codes->second;
    }
    return binding.glfwKey >= 0 || binding.imguiKey >= 0 ? std::optional<EditorKeybinding>{binding} : std::nullopt;
}

EditorKeybinding effectiveCommandKeybinding(const EditorCommand& command, const EditorPreferences* preferences) {
    if (preferences != nullptr) {
        const auto it = preferences->commandShortcutOverrides.find(editorCommandPreferenceKey(command));
        if (it != preferences->commandShortcutOverrides.end()) {
            if (std::optional<EditorKeybinding> parsed = parseShortcutDisplay(it->second, command.defaultKeybinding.context)) {
                return *parsed;
            }
        }
    }
    return command.defaultKeybinding;
}

} // namespace

void CommandRegistry::registerCommand(EditorCommand command) {
    commands_.push_back(std::move(command));
}

const EditorCommand* CommandRegistry::find(EditorCommandId id) const {
    for (const EditorCommand& command : commands_) {
        if (command.id == id) {
            return &command;
        }
    }
    return nullptr;
}

std::vector<std::string> CommandRegistry::detectConflicts(const EditorPreferences* preferences) const {
    std::vector<std::string> conflicts;
    std::unordered_map<std::string, std::string> firstCommand;
    for (const EditorCommand& command : commands_) {
        const EditorKeybinding binding = effectiveCommandKeybinding(command, preferences);
        const std::string key = conflictKey(binding);
        if (key.empty()) {
            continue;
        }
        const auto [it, inserted] = firstCommand.emplace(key, command.name);
        if (!inserted) {
            conflicts.push_back(it->second + " conflicts with " + command.name + " on " + binding.display);
        }
    }
    return conflicts;
}

const CommandRegistry& defaultEditorCommandRegistry() {
    static const CommandRegistry registry = [] {
        CommandRegistry r;
        auto add = [&](EditorCommandId id, const char* name, const char* category, const char* description, EditorKeybinding binding = {}) {
            r.registerCommand(EditorCommand{id, name, category, description, std::move(binding)});
        };

        add(EditorCommandId::ProjectManager, "Project Manager", "Project", "Open the Project Manager window");
        add(EditorCommandId::ProjectSettings, "Project Settings", "Project", "Open project settings in the Project Manager");
        add(EditorCommandId::CloseProject, "Close Project", "Project", "Close the current project");
        add(EditorCommandId::NewScene, "New Scene", "Scene", "Create a new scene");
        add(EditorCommandId::OpenScene, "Open Scene", "Scene", "Open an rtlevel scene", ctrlKey(GLFW_KEY_O, ImGuiKey_O, "Ctrl+O"));
        add(EditorCommandId::SaveScene, "Save Scene", "Scene", "Save the current scene", ctrlKey(GLFW_KEY_S, ImGuiKey_S, "Ctrl+S"));
        add(EditorCommandId::SaveSceneAs, "Save Scene As", "Scene", "Save the current scene to a new path");
        add(EditorCommandId::SaveAll, "Save All", "Project", "Save the current level, project metadata, asset registry, and editor preferences", ctrlShiftKey(GLFW_KEY_S, ImGuiKey_S, "Ctrl+Shift+S"));
        add(EditorCommandId::SaveMaterial, "Save Material", "Asset", "Save the selected dirty linked material asset metadata");
        add(EditorCommandId::OpenProjectDirectory, "Open Current Project Directory", "Project", "Reveal the current project root folder");
        add(EditorCommandId::OpenLogFolder, "Open Log Folder", "Engine", "Reveal the editor log output folder");
        add(EditorCommandId::OpenAsset, "Open Asset", "Asset", "Open or reveal the selected asset in the Content Browser");
        add(EditorCommandId::ImportAsset, "Import Asset", "Import", "Import reusable assets without mutating the scene");
        add(EditorCommandId::ImportAndPlace, "Import and Place", "Import", "Import assets and place the generated prefab");
        add(EditorCommandId::ImportSceneAsNewScene, "Import Scene as New Scene", "Import", "Replace the current scene with an imported model hierarchy");
        add(EditorCommandId::MergeScene, "Merge Scene into Current", "Import", "Append an external scene or model hierarchy");
        add(EditorCommandId::ImportHdri, "Import HDRI", "Import", "Load an HDR environment");
        add(EditorCommandId::Exit, "Exit", "Application", "Exit the editor");

        add(EditorCommandId::CreateEmptyEntity, "Empty Entity", "Create", "Create an empty scene entity");
        add(EditorCommandId::CreateCamera, "Camera", "Create", "Create a camera entity");
        add(EditorCommandId::CreatePointLight, "Point Light", "Create", "Create a point light entity");
        add(EditorCommandId::CreateSpotLight, "Spot Light", "Create", "Create a spot light entity");
        add(EditorCommandId::CreateAreaLight, "Area Light", "Create", "Create an area light entity");
        add(EditorCommandId::CreatePrimarySun, "Primary Sun", "Create", "Ensure a primary sun entity");
        add(EditorCommandId::CreateEnvironmentLight, "Environment Light", "Create", "Create an environment light entity");
        add(EditorCommandId::CreateSkyAtmosphere, "Sky Atmosphere", "Create", "Create a sky atmosphere entity");
        add(EditorCommandId::CreateHeightFog, "Height Fog", "Create", "Create a height fog entity");
        add(EditorCommandId::CreateVolumetricCloud, "Volumetric Cloud", "Create", "Create a volumetric cloud shell entity");
        add(EditorCommandId::CreatePostProcessVolume, "Post Process Volume", "Create", "Create a post process volume entity");

        add(EditorCommandId::ReloadShaders, "Reload Shaders", "Engine", "Reload renderer shaders", ctrlKey(GLFW_KEY_R, ImGuiKey_R, "Ctrl+R"));
        add(EditorCommandId::ShowControls, "Controls", "Engine", "Show controls reference");
        add(EditorCommandId::ShowRendererInfo, "Renderer Info", "Engine", "Show renderer information");
        add(EditorCommandId::JobCenter, "Job Center", "Window", "Show running editor jobs and recent job state");
        add(EditorCommandId::CommandPalette, "Command Palette", "Window", "Search and execute editor commands", ctrlShiftKey(GLFW_KEY_P, ImGuiKey_P, "Ctrl+Shift+P"));

        add(EditorCommandId::ResetAccumulation, "Reset Accumulation", "Render", "Reset path tracing accumulation", key(GLFW_KEY_R, ImGuiKey_R, "R"));
        add(EditorCommandId::ToggleDenoiser, "Toggle Denoiser", "Render", "Toggle the denoiser");
        add(EditorCommandId::ToggleMovingDenoiser, "Toggle Moving Denoiser", "Render", "Toggle denoise while moving");
        add(EditorCommandId::ToggleSun, "Toggle Primary Sun", "Render", "Toggle the primary sun");
        add(EditorCommandId::ToggleEnvironment, "Toggle Environment", "Render", "Toggle environment lighting");
        add(EditorCommandId::ToggleDirectLighting, "Toggle Direct Lighting", "Render", "Toggle direct lighting");
        add(EditorCommandId::CycleDebugView, "Cycle Debug View", "Render", "Cycle renderer debug view");
        add(EditorCommandId::CycleIntermediateView, "Cycle Intermediate Views", "Render", "Cycle intermediate debug views");
        add(EditorCommandId::SetDebugBeauty, "Beauty View", "Render", "Switch debug view to beauty", key(GLFW_KEY_F1, ImGuiKey_F1, "F1"));
        add(EditorCommandId::SetDebugDirectLighting, "Direct Lighting View", "Render", "Switch debug view to direct lighting", key(GLFW_KEY_F2, ImGuiKey_F2, "F2"));
        add(EditorCommandId::SetDebugIndirectLighting, "Indirect Lighting View", "Render", "Switch debug view to indirect lighting", key(GLFW_KEY_F3, ImGuiKey_F3, "F3"));
        add(EditorCommandId::SetDebugNormals, "Normals View", "Render", "Switch debug view to normals", key(GLFW_KEY_F4, ImGuiKey_F4, "F4"));
        add(EditorCommandId::SetDebugDepth, "Depth View", "Render", "Switch debug view to depth", key(GLFW_KEY_F5, ImGuiKey_F5, "F5"));
        add(EditorCommandId::SetDebugMotionVectors, "Motion Vectors View", "Render", "Switch debug view to motion vectors", key(GLFW_KEY_F6, ImGuiKey_F6, "F6"));
        add(EditorCommandId::SetDebugVariance, "Variance View", "Render", "Switch debug view to variance", key(GLFW_KEY_F7, ImGuiKey_F7, "F7"));
        add(EditorCommandId::SetDebugAlbedo, "Albedo View", "Render", "Switch debug view to albedo", key(GLFW_KEY_F8, ImGuiKey_F8, "F8"));
        add(EditorCommandId::SetToneMapperLinear, "Linear Tonemapper", "Render", "Switch tonemapper to Linear", key(GLFW_KEY_1, ImGuiKey_1, "1"));
        add(EditorCommandId::SetToneMapperReinhard, "Reinhard Tonemapper", "Render", "Switch tonemapper to Reinhard", key(GLFW_KEY_2, ImGuiKey_2, "2"));
        add(EditorCommandId::SetToneMapperAces, "ACES Tonemapper", "Render", "Switch tonemapper to ACES", key(GLFW_KEY_3, ImGuiKey_3, "3"));
        add(EditorCommandId::SetToneMapperPbrNeutral, "PBR Neutral Tonemapper", "Render", "Switch tonemapper to PBR Neutral", key(GLFW_KEY_4, ImGuiKey_4, "4"));
        add(EditorCommandId::SetToneMapperAgx, "AgX Tonemapper", "Render", "Switch tonemapper to AgX", key(GLFW_KEY_5, ImGuiKey_5, "5"));
        add(EditorCommandId::ToggleAutoExposure, "Toggle Auto Exposure", "Render", "Toggle auto exposure", key(GLFW_KEY_6, ImGuiKey_6, "6"));
        add(EditorCommandId::RenderCurrentViewport, "Render current viewport", "Render", "Render the current viewport to the render output folder");
        add(EditorCommandId::RenderImage, "Render image", "Render", "Open the still-image render workflow");
        add(EditorCommandId::RenderSequence, "Render sequence", "Render", "Render the current timeline range as an image sequence");
        add(EditorCommandId::Screenshot, "Screenshot", "Render", "Capture the current viewport to a PNG in the render output folder");
        add(EditorCommandId::StopRender, "Stop render", "Render", "Cancel the active editor render job");
        add(EditorCommandId::OpenOutputFolder, "Open Output Folder", "Render", "Open the editor render output folder");

        add(EditorCommandId::SaveLayout, "Save Layout", "Layout", "Save the current layout");
        add(EditorCommandId::ResetLayout, "Reset Layout", "Layout", "Reset the editor layout");
        add(EditorCommandId::Undo, "Undo", "Edit", "Undo the previous scene operation", ctrlKey(GLFW_KEY_Z, ImGuiKey_Z, "Ctrl+Z"));
        add(EditorCommandId::Redo, "Redo", "Edit", "Redo the next scene operation", ctrlKey(GLFW_KEY_Y, ImGuiKey_Y, "Ctrl+Y"));
        add(EditorCommandId::ToggleFullscreen, "Toggle Fullscreen", "Window", "Toggle borderless fullscreen", key(GLFW_KEY_F11, ImGuiKey_F11, "F11"));

        add(EditorCommandId::ViewportSelect, "Select Tool", "Viewport", "Use select tool", key(-1, ImGuiKey_Q, "Q", EditorCommandContext::Viewport));
        add(EditorCommandId::ViewportMove, "Move Tool", "Viewport", "Use move gizmo", key(-1, ImGuiKey_W, "W", EditorCommandContext::Viewport));
        add(EditorCommandId::ViewportRotate, "Rotate Tool", "Viewport", "Use rotate gizmo", key(-1, ImGuiKey_E, "E", EditorCommandContext::Viewport));
        add(EditorCommandId::ViewportScale, "Scale Tool", "Viewport", "Use scale gizmo", key(-1, ImGuiKey_R, "R", EditorCommandContext::Viewport));
        add(EditorCommandId::ViewportToggleLocal, "Toggle Local Space", "Viewport", "Toggle local/world gizmo space", key(-1, ImGuiKey_L, "L", EditorCommandContext::Viewport));
        add(EditorCommandId::ViewportToggleSnap, "Toggle Snap", "Viewport", "Toggle transform snapping");
        add(EditorCommandId::ViewportFrameSelected, "Frame Selected", "Viewport", "Frame the selected entity", key(-1, ImGuiKey_F, "F", EditorCommandContext::Viewport));
        add(EditorCommandId::ViewportToggleGrid, "Toggle Grid", "Viewport", "Toggle grid overlay", key(-1, ImGuiKey_G, "G", EditorCommandContext::Viewport));
        add(EditorCommandId::ViewportToggleAxes, "Toggle Axes", "Viewport", "Toggle axes overlay");
        return r;
    }();
    return registry;
}

const std::vector<EditorCommandPlaceholder>& defaultEditorCommandPlaceholders() {
    static const std::vector<EditorCommandPlaceholder> placeholders = {
        {"Favorite Scenes", "File", "Open or manage saved favorite scenes.", "Favorite scene storage is not implemented yet."},
        {"Choose Files to Save...", "File", "Choose specific dirty files to save.", "Selective save is not available in this build."},
        {"Import Texture", "File", "Import a standalone texture asset.", "Texture asset import is routed through the Content browser import pipeline."},
        {"Import IES Profile", "File", "Import an IES light profile asset.", "IES profile import storage is not implemented yet."},
        {"Export All...", "File", "Export the current scene.", "Scene export is not implemented yet."},
        {"Export Selected...", "File", "Export the selected entity or asset.", "Select an entity or asset after scene export support is implemented."},
        {"Zip Project", "File", "Create a distributable project archive.", "Project packaging is not wired to the editor shell yet."},
        {"Recent Projects", "File", "Open a recently used project.", "Recent projects are shown in the Project Manager."},

        {"Folder / Group", "Create", "Create a scene organization folder or group.", "Scene folder/group authoring is not available in this build."},
        {"Cube", "Create", "Create a cube primitive mesh actor.", "Primitive mesh creation is not available in this build."},
        {"Sphere", "Create", "Create a sphere primitive mesh actor.", "Primitive mesh creation is not available in this build."},
        {"Plane", "Create", "Create a plane primitive mesh actor.", "Primitive mesh creation is not available in this build."},
        {"Cylinder", "Create", "Create a cylinder primitive mesh actor.", "Primitive mesh creation is not available in this build."},
        {"Cone", "Create", "Create a cone primitive mesh actor.", "Primitive mesh creation is not available in this build."},
        {"Mesh From Asset", "Create", "Place a mesh or prefab asset into the current scene.", "Select a mesh or prefab in Content and place it from the asset actions."},
        {"Disk Area Light", "Create", "Create a disk-shaped area light.", "Disk area light shape is not available in this build."},
        {"Sphere Light", "Create", "Create a spherical light source.", "Sphere light shape is not available in this build."},
        {"Emissive Mesh Light", "Create", "Create a mesh-backed emissive light.", "Emissive mesh light authoring is not available in this build."},
        {"Cine Camera", "Create", "Create a cinematic camera actor.", "Cinematic camera actor storage is not available in this build."},
        {"Material", "Create", "Create a standalone material asset.", "Material asset creation is not available from this menu yet."},
        {"Material Instance", "Create", "Create a material instance asset.", "Material instance asset creation is not available from this menu yet."},
        {"Prefab From Selection", "Create", "Create a prefab from the current selection.", "Prefab authoring is not available in this build."},

        {"Editor Preferences...", "Engine", "Open editor preference settings.", "Editor preferences are edited from the Project Manager preferences view."},
        {"Engine Settings...", "Engine", "Open engine settings.", "Engine settings are not exposed as an editor panel yet."},
        {"Rebuild Asset Registry", "Engine", "Rebuild the loaded project asset registry.", "Asset registry rebuild is not wired to the top menu yet."},
        {"Validate Asset References", "Engine", "Validate asset reference metadata.", "Use the asset registry validator script from the command line for now."},
        {"Clear Derived Data Cache...", "Engine", "Clear generated derived-data cache files.", "Derived data cache clearing is not wired to the editor shell yet."},
        {"Open Cache Directory", "Engine", "Reveal the current project cache directory.", "Cache directory reveal is not wired to this menu yet."},
        {"Run Validation Suite", "Engine", "Run the renderer validation suite from the editor.", "Use the validation scripts from the command line; in-editor launch is pending."},
        {"Run Current Scene Checks", "Engine", "Run validation checks for the current scene.", "Current-scene validation is not wired to the editor shell yet."},
        {"Open Debug Package Folder", "Engine", "Reveal generated debug package output.", "Debug package folder reveal is available from generated debug-package notifications."},
        {"Copy System Info", "Engine", "Copy renderer and platform information to the clipboard.", "System info clipboard export is not wired to the editor shell yet."},

        {"Floating Render Controls", "Window", "Show floating render controls.", "Floating render controls are not implemented yet; use the Render menu and viewport strip."},
        {"Load Layout...", "Window", "Load a saved editor layout.", "Named layout loading is not implemented yet."},

        {"Pause / Resume Render", "Render", "Pause or resume the active editor render job.", "Pause/resume render jobs are not available in this build."},
        {"High Resolution Render", "Render", "Render a high-resolution tiled output.", "High-resolution tiled rendering is not available in this build."},
        {"Quality Preset", "Render", "Change the active render quality preset.", "Use the Render World Settings panel for preset changes."},
        {"Capture RenderDoc", "Render", "Capture the current frame with RenderDoc.", "RenderDoc capture remains available through the existing runtime capture path; top-menu launch is pending."},
        {"Export Debug Views", "Render", "Export renderer debug view images.", "Use headless/debug package export until in-editor export is wired."},
        {"Export Debug Package", "Render", "Export a self-contained renderer debug package.", "Use the debug package command-line export until in-editor export is wired."},
        {"Dump RenderGraph", "Render", "Write the current RenderGraph structure to disk.", "RenderGraph dump is available through headless diagnostics for now."},
        {"Profile Current Scene", "Render", "Profile the current scene and export timing data.", "Profiling export is available through headless diagnostics for now."},
        {"View Mode", "Render", "Switch the viewport view mode.", "Viewport view-mode switching is exposed through Draw Debug for now."},

        {"Manage Layouts...", "Layout", "Manage named editor layouts.", "Named layout management is not implemented yet."},
        {"UI Scale", "Layout", "Change editor UI scale.", "UI scale is edited from Project Manager preferences."},
        {"Theme", "Layout", "Change the editor theme.", "Theme is edited from Project Manager preferences."},
    };
    return placeholders;
}

const EditorCommand* editorCommand(EditorCommandId id) {
    return defaultEditorCommandRegistry().find(id);
}

const EditorCommandPlaceholder* editorCommandPlaceholder(const std::string& name) {
    const std::vector<EditorCommandPlaceholder>& placeholders = defaultEditorCommandPlaceholders();
    const auto it = std::find_if(placeholders.begin(), placeholders.end(), [&](const EditorCommandPlaceholder& placeholder) {
        return placeholder.name == name;
    });
    return it != placeholders.end() ? &*it : nullptr;
}

const char* editorCommandShortcut(EditorCommandId id) {
    const EditorCommand* command = editorCommand(id);
    return command != nullptr && !command->defaultKeybinding.display.empty()
        ? command->defaultKeybinding.display.c_str()
        : nullptr;
}

std::string editorCommandPreferenceKey(const EditorCommand& command) {
    return command.category + "." + command.name;
}

std::string editorCommandPreferenceKey(EditorCommandId id) {
    const EditorCommand* command = editorCommand(id);
    return command != nullptr ? editorCommandPreferenceKey(*command) : std::string{};
}

EditorKeybinding editorCommandKeybinding(EditorCommandId id, const EditorPreferences* preferences) {
    const EditorCommand* command = editorCommand(id);
    if (command == nullptr) {
        return {};
    }
    return effectiveCommandKeybinding(*command, preferences);
}

std::string editorCommandShortcutDisplay(EditorCommandId id, const EditorPreferences* preferences) {
    const EditorCommand* command = editorCommand(id);
    if (command == nullptr) {
        return {};
    }
    if (preferences != nullptr) {
        const auto it = preferences->commandShortcutOverrides.find(editorCommandPreferenceKey(*command));
        if (it != preferences->commandShortcutOverrides.end()) {
            return it->second;
        }
    }
    return command->defaultKeybinding.display;
}

const char* editorCommandName(EditorCommandId id) {
    const EditorCommand* command = editorCommand(id);
    return command != nullptr ? command->name.c_str() : "Unknown Command";
}

uint32_t editorCommandContextPrecedence(EditorCommandContext context) {
    switch (context) {
    case EditorCommandContext::TextInput:
        return 100u;
    case EditorCommandContext::Modal:
        return 90u;
    case EditorCommandContext::Viewport:
        return 70u;
    case EditorCommandContext::SceneEditing:
        return 50u;
    case EditorCommandContext::Global:
        return 10u;
    }
    return 0u;
}

} // namespace rtv
