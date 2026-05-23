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

class MacroCommand final : public ICommand {
public:
    explicit MacroCommand(std::string label) : label_(std::move(label)) {}

    void addCommand(std::unique_ptr<ICommand> command) {
        commands_.push_back(std::move(command));
    }

    void undo() override {
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
            (*it)->undo();
        }
    }

    void redo() override {
        for (auto& cmd : commands_) {
            cmd->redo();
        }
    }

    [[nodiscard]] const std::string& label() const override { return label_; }
    [[nodiscard]] size_t commandCount() const { return commands_.size(); }

private:
    std::string label_;
    std::vector<std::unique_ptr<ICommand>> commands_;
};

class UndoStack;

class CommandTransaction {
public:
    explicit CommandTransaction(UndoStack& stack, std::string label);
    ~CommandTransaction();

    CommandTransaction(const CommandTransaction&) = delete;
    CommandTransaction& operator=(const CommandTransaction&) = delete;

    void append(std::unique_ptr<ICommand> cmd);
    void commit();
    void cancel();

private:
    UndoStack& stack_;
    std::string label_;
    std::vector<std::unique_ptr<ICommand>> pending_;
    bool active_ = true;
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
