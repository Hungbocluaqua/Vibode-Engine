#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace rtv {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    [[nodiscard]] virtual const std::string& label() const = 0;
};

class UndoStack {
public:
    void pushCommand(std::unique_ptr<ICommand> command);
    bool undo();
    bool redo();
    void clear();

    [[nodiscard]] bool canUndo() const { return cursor_ > 0; }
    [[nodiscard]] bool canRedo() const { return cursor_ < commands_.size(); }
    [[nodiscard]] const char* undoLabel() const;
    [[nodiscard]] const char* redoLabel() const;

private:
    std::vector<std::unique_ptr<ICommand>> commands_;
    size_t cursor_ = 0;
    static constexpr size_t maxCommands_ = 256;
};

} // namespace rtv
