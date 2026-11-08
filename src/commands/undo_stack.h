#pragma once

#include "commands/command.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace loki::commands {

// Unlimited undo/redo history bound to one Project. Every model mutation goes through push().
class UndoStack {
public:
    explicit UndoStack(model::Project& project) noexcept : m_project(project) {}
    UndoStack(const UndoStack&) = delete;
    UndoStack& operator=(const UndoStack&) = delete;
    UndoStack(UndoStack&&) = delete;
    UndoStack& operator=(UndoStack&&) = delete;
    ~UndoStack() = default;

    // Executes `command` and records it. On failure the exception propagates and neither the
    // project nor the history changes. Discards the redo history.
    void push(std::unique_ptr<Command> command);

    // Groups the following pushes into one undo step. Pushed commands execute immediately.
    // Macros nest; only the outermost one becomes a history entry.
    void beginMacro(std::string text);
    void endMacro();
    // Undoes everything pushed since the matching beginMacro() and discards it.
    void abortMacro();
    [[nodiscard]] bool inMacro() const noexcept { return !m_openMacros.empty(); }

    [[nodiscard]] bool canUndo() const noexcept { return !inMacro() && m_index > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return !inMacro() && m_index < m_commands.size(); }
    void undo();
    void redo();
    [[nodiscard]] std::string undoText() const;
    [[nodiscard]] std::string redoText() const;

    [[nodiscard]] std::size_t count() const noexcept { return m_commands.size(); }
    [[nodiscard]] std::size_t index() const noexcept { return m_index; }

    // "Clean" = matches the last saved state.
    [[nodiscard]] bool isClean() const noexcept { return m_cleanIndex && *m_cleanIndex == m_index; }
    void setClean();

    // Identifies the current history state, for asynchronous saves: capture it when the save
    // starts and pass it to markCleanAt() when the save succeeds. If the history changed so that
    // state is no longer reachable, markCleanAt() returns false and nothing changes.
    struct Point {
        std::size_t index = 0;
        std::uint64_t serial = 0; // serial of the entry at index-1 (or the epoch when index == 0)
    };
    [[nodiscard]] Point currentPoint() const noexcept;
    bool markCleanAt(const Point& point);
    // Drops all history (the project is left as is) and marks the current state clean.
    void clear();

    // Called after any change of index, count, clean state or texts.
    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

private:
    struct OpenMacro {
        std::string text;
        std::vector<std::unique_ptr<Command>> children;
    };

    void record(std::unique_ptr<Command> executed);
    void changed() const;

    [[nodiscard]] std::uint64_t serialAt(std::size_t index) const noexcept;

    model::Project& m_project;
    std::vector<std::unique_ptr<Command>> m_commands;
    std::vector<std::uint64_t> m_serials; // parallel to m_commands
    std::uint64_t m_nextSerial = 1;
    std::uint64_t m_epoch = 0; // serial of the empty history; renewed by clear()
    std::size_t m_index = 0;
    std::optional<std::size_t> m_cleanIndex = 0;
    std::vector<OpenMacro> m_openMacros;
    std::function<void()> m_changed;
};

// RAII macro: aborts (undoes) the macro unless commit() is called, e.g. when an exception
// escapes halfway through a compound edit.
class MacroGuard {
public:
    MacroGuard(UndoStack& stack, std::string text) : m_stack(stack) { m_stack.beginMacro(std::move(text)); }
    ~MacroGuard() {
        if (!m_done) {
            m_stack.abortMacro();
        }
    }
    MacroGuard(const MacroGuard&) = delete;
    MacroGuard& operator=(const MacroGuard&) = delete;
    MacroGuard(MacroGuard&&) = delete;
    MacroGuard& operator=(MacroGuard&&) = delete;

    void commit() {
        m_done = true;
        m_stack.endMacro();
    }

private:
    UndoStack& m_stack;
    bool m_done = false;
};

} // namespace loki::commands
