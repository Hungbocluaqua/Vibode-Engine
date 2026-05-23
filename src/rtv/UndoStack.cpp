#include "rtv/UndoStack.h"

#include <algorithm>
#include <utility>

namespace rtv {

CommandTransaction::CommandTransaction(UndoStack& stack, std::string label)
    : stack_(stack), label_(std::move(label)) {}

CommandTransaction::~CommandTransaction() {
    if (active_) {
        cancel();
    }
}

void CommandTransaction::append(std::unique_ptr<ICommand> cmd) {
    pending_.push_back(std::move(cmd));
}

void CommandTransaction::commit() {
    if (pending_.empty()) {
        active_ = false;
        return;
    }
    auto macro = std::make_unique<MacroCommand>(label_);
    for (auto& cmd : pending_) {
        macro->addCommand(std::move(cmd));
    }
    pending_.clear();
    stack_.pushCommand(std::move(macro));
    active_ = false;
}

void CommandTransaction::cancel() {
    pending_.clear();
    active_ = false;
}

void UndoStack::pushCommand(std::unique_ptr<ICommand> command) {
    if (!command) {
        return;
    }
    if (cursor_ < commands_.size()) {
        commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_), commands_.end());
    }
    commands_.push_back(std::move(command));
    cursor_ = commands_.size();
    if (commands_.size() > maxCommands_) {
        const size_t removeCount = commands_.size() - maxCommands_;
        commands_.erase(commands_.begin(), commands_.begin() + static_cast<std::ptrdiff_t>(removeCount));
        cursor_ = cursor_ > removeCount ? cursor_ - removeCount : 0;
    }
}

bool UndoStack::undo() {
    if (!canUndo()) {
        return false;
    }
    --cursor_;
    commands_[cursor_]->undo();
    return true;
}

bool UndoStack::redo() {
    if (!canRedo()) {
        return false;
    }
    commands_[cursor_]->redo();
    ++cursor_;
    return true;
}

void UndoStack::clear() {
    commands_.clear();
    cursor_ = 0;
}

const char* UndoStack::undoLabel() const {
    return canUndo() ? commands_[cursor_ - 1]->label().c_str() : "";
}

const char* UndoStack::redoLabel() const {
    return canRedo() ? commands_[cursor_]->label().c_str() : "";
}

} // namespace rtv
