#pragma once

#include "model/project.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace loki::commands {

// A command was rejected (invalid target, locked track, ...). The project is unchanged.
class CommandError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reversible edit. Contract:
//  - redo() applies the edit; the first call may validate and throw (CommandError/ModelError),
//    in which case the project must be left exactly as it was (strong guarantee).
//  - undo() restores the exact state seen before the matching redo().
//  - redo()/undo() alternate, starting with redo().
class Command {
public:
    virtual ~Command() = default;
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;

    [[nodiscard]] virtual std::string text() const = 0;
    virtual void redo(model::Project& project) = 0;
    virtual void undo(model::Project& project) = 0;

    // Consecutive commands with the same non-negative id may merge (e.g. one undo step for a
    // whole drag). mergeWith() is called on the older command after `next` has been executed.
    [[nodiscard]] virtual int mergeId() const { return -1; }
    virtual bool mergeWith(const Command& next) {
        (void)next;
        return false;
    }

protected:
    Command() = default;

    // The single gateway to model mutation (D2).
    [[nodiscard]] static model::ProjectMutator mutate(model::Project& project) noexcept {
        return model::ProjectMutator(project);
    }
};

// Executes children in order; undoes them in reverse. If a child fails during redo, the
// children already applied are undone before the error propagates.
class MacroCommand final : public Command {
public:
    MacroCommand(std::string text, std::vector<std::unique_ptr<Command>> children);

    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

    [[nodiscard]] std::size_t size() const noexcept { return m_children.size(); }

private:
    std::string m_text;
    std::vector<std::unique_ptr<Command>> m_children;
};

} // namespace loki::commands
