#include "commands/undo_stack.h"

#include <stdexcept>

namespace loki::commands {

namespace {

// A macro whose children have already been executed: the first redo() is a no-op.
class ExecutedMacro final : public Command {
public:
    ExecutedMacro(std::string text, std::vector<std::unique_ptr<Command>> children)
        : m_macro(std::move(text), std::move(children)) {}

    [[nodiscard]] std::string text() const override { return m_macro.text(); }
    void redo(model::Project& project) override {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        m_macro.redo(project);
    }
    void undo(model::Project& project) override { m_macro.undo(project); }

private:
    MacroCommand m_macro;
    bool m_firstRedo = true;
};

} // namespace

void UndoStack::push(std::unique_ptr<Command> command) {
    if (!command) {
        throw std::invalid_argument("UndoStack::push: null command");
    }
    command->redo(m_project); // throws => nothing recorded, project unchanged

    if (inMacro()) {
        m_openMacros.back().children.push_back(std::move(command));
        return;
    }
    // Merge into the previous step unless that would hide the saved (clean) state.
    if (m_index > 0 && m_index == m_commands.size() && !(m_cleanIndex && *m_cleanIndex == m_index)) {
        auto& top = m_commands[m_index - 1];
        if (command->mergeId() >= 0 && top->mergeId() == command->mergeId() && top->mergeWith(*command)) {
            m_serials[m_index - 1] = m_nextSerial++; // the entry now means a different state
            changed();
            return;
        }
    }
    record(std::move(command));
}

void UndoStack::record(std::unique_ptr<Command> executed) {
    if (m_index < m_commands.size()) {
        m_commands.erase(m_commands.begin() + static_cast<std::ptrdiff_t>(m_index), m_commands.end());
        m_serials.resize(m_index);
        if (m_cleanIndex && *m_cleanIndex > m_index) {
            m_cleanIndex.reset(); // the saved state is no longer reachable
        }
    }
    m_commands.push_back(std::move(executed));
    m_serials.push_back(m_nextSerial++);
    m_index = m_commands.size();
    changed();
}

std::uint64_t UndoStack::serialAt(std::size_t index) const noexcept {
    return index == 0 ? m_epoch : m_serials[index - 1];
}

UndoStack::Point UndoStack::currentPoint() const noexcept {
    return {m_index, serialAt(m_index)};
}

bool UndoStack::markCleanAt(const Point& point) {
    if (point.index > m_commands.size() || serialAt(point.index) != point.serial) {
        return false;
    }
    m_cleanIndex = point.index;
    changed();
    return true;
}

void UndoStack::beginMacro(std::string text) {
    m_openMacros.push_back({std::move(text), {}});
}

void UndoStack::endMacro() {
    if (!inMacro()) {
        throw std::logic_error("UndoStack::endMacro without beginMacro");
    }
    OpenMacro macro = std::move(m_openMacros.back());
    m_openMacros.pop_back();
    if (macro.children.empty()) {
        return;
    }
    auto executed = std::make_unique<ExecutedMacro>(std::move(macro.text), std::move(macro.children));
    executed->redo(m_project); // consumes the "already executed" first redo
    if (inMacro()) {
        m_openMacros.back().children.push_back(std::move(executed));
    } else {
        record(std::move(executed));
    }
}

void UndoStack::abortMacro() {
    if (!inMacro()) {
        throw std::logic_error("UndoStack::abortMacro without beginMacro");
    }
    OpenMacro macro = std::move(m_openMacros.back());
    m_openMacros.pop_back();
    for (auto it = macro.children.rbegin(); it != macro.children.rend(); ++it) {
        (*it)->undo(m_project);
    }
}

void UndoStack::undo() {
    if (!canUndo()) {
        return;
    }
    m_commands[m_index - 1]->undo(m_project);
    --m_index;
    changed();
}

void UndoStack::redo() {
    if (!canRedo()) {
        return;
    }
    m_commands[m_index]->redo(m_project);
    ++m_index;
    changed();
}

std::string UndoStack::undoText() const {
    return canUndo() ? m_commands[m_index - 1]->text() : std::string();
}

std::string UndoStack::redoText() const {
    return canRedo() ? m_commands[m_index]->text() : std::string();
}

void UndoStack::setClean() {
    m_cleanIndex = m_index;
    changed();
}

void UndoStack::clear() {
    m_openMacros.clear();
    m_commands.clear();
    m_serials.clear();
    m_epoch = m_nextSerial++;
    m_index = 0;
    m_cleanIndex = 0;
    changed();
}

void UndoStack::changed() const {
    if (m_changed) {
        m_changed();
    }
}

} // namespace loki::commands
