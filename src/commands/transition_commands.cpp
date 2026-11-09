#include "commands/transition_commands.h"

#include "model/transitions.h"

namespace loki::commands {

using namespace loki::model;

void AddTransitionCommand::redo(Project& project) {
    mutate(project).addTransition(m_track, m_transition);
}

void AddTransitionCommand::undo(Project& project) {
    (void)mutate(project).removeTransition(m_transition.id);
}

void RemoveTransitionCommand::redo(Project& project) {
    const auto ref = project.findTransition(m_id);
    if (ref.transition == nullptr) {
        throw CommandError("Transition does not exist");
    }
    m_track = ref.track->id;
    m_removed = mutate(project).removeTransition(m_id);
}

void RemoveTransitionCommand::undo(Project& project) {
    mutate(project).addTransition(m_track, *m_removed);
}

void EditTransitionCommand::redo(Project& project) {
    const auto ref = project.findTransition(m_after.id);
    if (ref.transition == nullptr) {
        throw CommandError("Transition does not exist");
    }
    if (!m_before) {
        m_before = *ref.transition;
    }
    mutate(project).replaceTransition(m_after);
}

void EditTransitionCommand::undo(Project& project) {
    mutate(project).replaceTransition(*m_before);
}

bool EditTransitionCommand::mergeWith(const Command& next) {
    const auto* other = dynamic_cast<const EditTransitionCommand*>(&next);
    if (other == nullptr || m_gesture == 0 || other->m_gesture != m_gesture || other->m_after.id != m_after.id) {
        return false;
    }
    m_after = other->m_after;
    return true;
}

std::unique_ptr<Command> makeApplyTransition(const Project& project, ClipId clipId, ClipEdge edge,
                                             std::string_view type, Rational duration) {
    const auto ref = project.findClip(clipId);
    if (ref.clip == nullptr) {
        throw CommandError("Clip does not exist");
    }
    const auto* def = findTransitionDef(type);
    if (def == nullptr) {
        throw CommandError("Unknown transition " + std::string(type));
    }
    if (def->kind != ref.track->kind) {
        throw CommandError(std::string(def->label) +
                           (def->kind == TrackKind::Video ? " goes on video clips" : " goes on audio clips"));
    }
    if (ref.track->state.locked) {
        throw CommandError("Track is locked");
    }
    if (!duration.isPositive()) {
        throw CommandError("Transition duration must be positive");
    }
    const auto& clip = *ref.clip;
    Transition t;
    t.id = TransitionId::generate();
    t.type = std::string(type);
    const Clip* neighbour = nullptr;
    for (const auto& other : ref.track->clips) {
        if (edge == ClipEdge::Out ? other.start == clip.end() : other.end() == clip.start) {
            neighbour = &other;
        }
    }
    if (edge == ClipEdge::Out) {
        t.from = clip.id;
        t.to = neighbour != nullptr ? neighbour->id : ClipId();
    } else {
        t.from = neighbour != nullptr ? neighbour->id : ClipId();
        t.to = clip.id;
    }
    Rational limit = clip.duration;
    if (neighbour != nullptr) {
        limit += neighbour->duration;
    }
    t.duration = min(duration, limit);
    // Replace whatever already sits at this cut / edge.
    std::vector<std::unique_ptr<Command>> steps;
    for (const auto& existing : ref.track->transitions) {
        if (existing.from == t.from && existing.to == t.to) {
            steps.push_back(std::make_unique<RemoveTransitionCommand>(existing.id));
        }
    }
    steps.push_back(std::make_unique<AddTransitionCommand>(ref.track->id, std::move(t)));
    if (steps.size() == 1) {
        return std::move(steps.front());
    }
    return std::make_unique<MacroCommand>("Apply Transition", std::move(steps));
}

namespace {

Transition requireTransition(const Project& project, TransitionId id) {
    const auto ref = project.findTransition(id);
    if (ref.transition == nullptr) {
        throw CommandError("Transition does not exist");
    }
    return *ref.transition;
}

} // namespace

std::unique_ptr<Command> setTransitionDuration(const Project& project, TransitionId id, Rational duration,
                                               GestureId gesture) {
    if (!duration.isPositive()) {
        throw CommandError("Transition duration must be positive");
    }
    auto t = requireTransition(project, id);
    t.duration = duration;
    return std::make_unique<EditTransitionCommand>(std::move(t), gesture);
}

std::unique_ptr<Command> setTransitionAlignment(const Project& project, TransitionId id,
                                                TransitionAlignment alignment) {
    auto t = requireTransition(project, id);
    t.alignment = alignment;
    return std::make_unique<EditTransitionCommand>(std::move(t));
}

} // namespace loki::commands
