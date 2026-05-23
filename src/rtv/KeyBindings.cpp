#include "rtv/KeyBindings.h"

namespace rtv {

const std::vector<KeyBinding>& allKeyBindings() {
    static const std::vector<KeyBinding> bindings = {
        {"Viewport.Navigate", "Right Mouse", "Navigation", "Look and move the viewport camera"},
        {"Viewport.MoveForward", "W", "Navigation", "Move forward while navigating"},
        {"Viewport.MoveBack", "S", "Navigation", "Move backward while navigating"},
        {"Viewport.MoveLeft", "A", "Navigation", "Move left while navigating"},
        {"Viewport.MoveRight", "D", "Navigation", "Move right while navigating"},
        {"Viewport.MoveDown", "Q / Ctrl", "Navigation", "Move down while navigating"},
        {"Viewport.MoveUp", "E / Space", "Navigation", "Move up while navigating"},
        {"Viewport.FastMove", "Shift", "Navigation", "Use fast camera movement"},
        {"Viewport.Translate", "T", "Editing", "Use translate gizmo"},
        {"Viewport.Rotate", "R", "Editing", "Use rotate gizmo or reset accumulation outside text fields"},
        {"Viewport.Scale", "S", "Editing", "Use scale gizmo"},
        {"Viewport.LocalSpace", "L", "Editing", "Toggle local/world gizmo space"},
        {"Render.CycleDebug", "F1", "Render", "Cycle renderer debug view"},
        {"Render.ToggleDenoiser", "F2", "Render", "Toggle denoiser"},
        {"Render.ToggleMovingDenoiser", "F3", "Render", "Toggle denoise while moving"},
        {"Render.ToggleSun", "F4", "Render", "Toggle sunlight"},
        {"Render.ToggleEnvironment", "F5", "Render", "Toggle environment"},
        {"Render.ToggleDirect", "F6", "Render", "Toggle direct lighting"},
        {"Render.CycleIntermediate", "F7", "Render", "Cycle intermediate buffer view (Beauty, Direct, Indirect, Variance, Normals, Depth, Velocity)"},
        {"Render.ResetAccumulation", "R", "Render", "Reset accumulation outside text fields"},
        {"Window.Fullscreen", "F11", "Window", "Toggle borderless fullscreen"},
    };
    return bindings;
}

} // namespace rtv
