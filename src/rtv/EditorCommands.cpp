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

std::vector<std::string> CommandRegistry::detectConflicts() const {
    std::vector<std::string> conflicts;
    std::unordered_map<std::string, std::string> firstCommand;
    for (const EditorCommand& command : commands_) {
        const std::string key = conflictKey(command.defaultKeybinding);
        if (key.empty()) {
            continue;
        }
        const auto [it, inserted] = firstCommand.emplace(key, command.name);
        if (!inserted) {
            conflicts.push_back(it->second + " conflicts with " + command.name + " on " + command.defaultKeybinding.display);
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
        add(EditorCommandId::CloseProject, "Close Project", "Project", "Close the current project");
        add(EditorCommandId::NewScene, "New Scene", "Scene", "Create a new scene");
        add(EditorCommandId::OpenScene, "Open Scene", "Scene", "Open an rtlevel scene", ctrlKey(GLFW_KEY_O, ImGuiKey_O, "Ctrl+O"));
        add(EditorCommandId::SaveScene, "Save Scene", "Scene", "Save the current scene", ctrlKey(GLFW_KEY_S, ImGuiKey_S, "Ctrl+S"));
        add(EditorCommandId::SaveSceneAs, "Save Scene As", "Scene", "Save the current scene to a new path");
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

const EditorCommand* editorCommand(EditorCommandId id) {
    return defaultEditorCommandRegistry().find(id);
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
    if (preferences != nullptr) {
        const auto it = preferences->commandShortcutOverrides.find(editorCommandPreferenceKey(*command));
        if (it != preferences->commandShortcutOverrides.end()) {
            if (std::optional<EditorKeybinding> parsed = parseShortcutDisplay(it->second, command->defaultKeybinding.context)) {
                return *parsed;
            }
        }
    }
    return command->defaultKeybinding;
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
