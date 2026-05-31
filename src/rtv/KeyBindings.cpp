#include "rtv/KeyBindings.h"

#include "rtv/EditorCommands.h"

namespace rtv {

const std::vector<KeyBinding>& allKeyBindings() {
    static const std::vector<KeyBinding> bindings = [] {
        std::vector<KeyBinding> result = {
            {"Viewport.Navigate", "Right Mouse", "Navigation", "Look and move the viewport camera"},
            {"Viewport.MoveForward", "W", "Navigation", "Move forward while navigating"},
            {"Viewport.MoveBack", "S", "Navigation", "Move backward while navigating"},
            {"Viewport.MoveLeft", "A", "Navigation", "Move left while navigating"},
            {"Viewport.MoveRight", "D", "Navigation", "Move right while navigating"},
            {"Viewport.MoveDown", "Q / Ctrl", "Navigation", "Move down while navigating"},
            {"Viewport.MoveUp", "E / Space", "Navigation", "Move up while navigating"},
            {"Viewport.FastMove", "Shift", "Navigation", "Use fast camera movement"},
            {"Viewport.RotateSun", "Hold Ctrl+L + Drag", "Navigation", "Rotate the Primary Sun"},
            {"Viewport.TranslateLegacy", "T", "Editing", "Use move gizmo compatibility shortcut"},
            {"Viewport.ScaleLegacy", "S", "Editing", "Use scale gizmo compatibility shortcut"},
        };
        for (const EditorCommand& command : defaultEditorCommandRegistry().commands()) {
            if (command.defaultKeybinding.display.empty()) {
                continue;
            }
            result.push_back(KeyBinding{
                command.name,
                command.defaultKeybinding.display,
                command.category,
                command.description,
            });
        }
        return result;
    }();
    return bindings;
}

} // namespace rtv
