#include "commands/command.h"

namespace loki::commands {

MacroCommand::MacroCommand(std::string text, std::vector<std::unique_ptr<Command>> children)
    : m_text(std::move(text)), m_children(std::move(children)) {}

void MacroCommand::redo(model::Project& project) {
    std::size_t done = 0;
    try {
        for (; done < m_children.size(); ++done) {
            m_children[done]->redo(project);
        }
    } catch (...) {
        while (done > 0) {
            m_children[--done]->undo(project);
        }
        throw;
    }
}

void MacroCommand::undo(model::Project& project) {
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        (*it)->undo(project);
    }
}

} // namespace loki::commands
