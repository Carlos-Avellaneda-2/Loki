#include "commands/clip_commands.h"

#include <algorithm>

namespace loki::commands {

using namespace loki::model;

namespace {

void requireUnlocked(const Track& track) {
    if (track.state.locked) {
        throw CommandError("Track " + track.state.name + " is locked");
    }
}

ClipRef requireClip(const Project& project, ClipId id) {
    const auto ref = project.findClip(id);
    if (ref.clip == nullptr) {
        throw CommandError("Clip does not exist");
    }
    return ref;
}

Effect& requireEffect(Clip& clip, EffectId id) {
    const auto it = std::find_if(clip.effects.begin(), clip.effects.end(), [&](const Effect& e) { return e.id == id; });
    if (it == clip.effects.end()) {
        throw CommandError("Effect does not exist");
    }
    return *it;
}

Parameter& requireParameter(Effect& effect, const std::string& key) {
    const auto it = std::find_if(effect.parameters.begin(), effect.parameters.end(),
                                 [&](const Parameter& p) { return p.key == key; });
    if (it == effect.parameters.end()) {
        throw CommandError("Parameter '" + key + "' does not exist");
    }
    return *it;
}

} // namespace

// ---------------------------------------------------------------------------------------------

AddClipCommand::AddClipCommand(TrackId track, Clip clip) : m_track(track), m_clip(std::move(clip)) {
    if (m_clip.id.isNull()) {
        m_clip.id = ClipId::generate();
    }
}

void AddClipCommand::redo(Project& project) {
    if (!m_checked) {
        const auto ref = project.findTrack(m_track);
        if (ref.track == nullptr) {
            throw CommandError("Track does not exist");
        }
        requireUnlocked(*ref.track);
        m_checked = true;
    }
    mutate(project).insertClip(m_track, m_clip);
}

void AddClipCommand::undo(Project& project) {
    mutate(project).removeClip(m_clip.id);
}

void RemoveClipCommand::redo(Project& project) {
    if (!m_clip) {
        const auto ref = requireClip(project, m_id);
        requireUnlocked(*ref.track);
        m_track = ref.track->id;
    }
    m_clip = mutate(project).removeClip(m_id);
}

void RemoveClipCommand::undo(Project& project) {
    mutate(project).insertClip(m_track, *m_clip);
}

// ---------------------------------------------------------------------------------------------

MoveClipCommand::MoveClipCommand(ClipId id, TrackId targetTrack, Rational newStart, GestureId gesture)
    : m_id(id), m_targetTrack(targetTrack), m_newStart(newStart), m_gesture(gesture) {}

void MoveClipCommand::moveTo(Project& project, ClipId id, TrackId track, const Rational& start) {
    auto mutator = mutate(project);
    const auto ref = requireClip(project, id);
    if (ref.track->id == track) {
        Clip moved = *ref.clip;
        moved.start = start;
        mutator.replaceClip(std::move(moved));
        return;
    }
    const TrackId sourceTrack = ref.track->id;
    Clip clip = mutator.removeClip(id);
    Clip moved = clip;
    moved.start = start;
    try {
        mutator.insertClip(track, std::move(moved));
    } catch (...) {
        mutator.insertClip(sourceTrack, std::move(clip)); // restore (strong guarantee)
        throw;
    }
}

void MoveClipCommand::redo(Project& project) {
    if (!m_checked) {
        const auto ref = requireClip(project, m_id);
        const auto target = project.findTrack(m_targetTrack);
        if (target.track == nullptr) {
            throw CommandError("Target track does not exist");
        }
        if (target.track->kind != ref.track->kind) {
            throw CommandError("Clips cannot move between video and audio tracks");
        }
        if (target.sequence != ref.sequence) {
            throw CommandError("Clips cannot move between sequences");
        }
        requireUnlocked(*ref.track);
        requireUnlocked(*target.track);
        m_oldTrack = ref.track->id;
        m_oldStart = ref.clip->start;
        moveTo(project, m_id, m_targetTrack, m_newStart);
        m_checked = true;
        return;
    }
    moveTo(project, m_id, m_targetTrack, m_newStart);
}

void MoveClipCommand::undo(Project& project) {
    moveTo(project, m_id, m_oldTrack, m_oldStart);
}

bool MoveClipCommand::mergeWith(const Command& next) {
    const auto* other = dynamic_cast<const MoveClipCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || m_gesture == 0 || other->m_gesture != m_gesture) {
        return false;
    }
    m_targetTrack = other->m_targetTrack;
    m_newStart = other->m_newStart;
    return true;
}

// ---------------------------------------------------------------------------------------------

SplitClipCommand::SplitClipCommand(ClipId id, Rational at, ClipId rightId)
    : m_id(id), m_at(at), m_rightId(rightId.isNull() ? ClipId::generate() : rightId) {}

void SplitClipCommand::redo(Project& project) {
    const auto ref = requireClip(project, m_id);
    if (!m_original) {
        requireUnlocked(*ref.track);
        if (!(ref.clip->start < m_at && m_at < ref.clip->end())) {
            throw CommandError("Split point must be inside the clip");
        }
    }
    const Clip original = *ref.clip;
    const TrackId trackId = ref.track->id;

    Clip left = original;
    left.duration = m_at - original.start;
    Clip right = original;
    right.id = m_rightId;
    right.start = m_at;
    right.sourceIn = original.sourceIn + left.duration;
    right.duration = original.end() - m_at;
    right.link = LinkId(); // relinking split A/V pairs is handled by Phase 7 link-aware edits

    auto mutator = mutate(project);
    mutator.replaceClip(left);
    try {
        mutator.insertClip(trackId, std::move(right));
    } catch (...) {
        mutator.replaceClip(original);
        throw;
    }
    m_original = original;
}

void SplitClipCommand::undo(Project& project) {
    auto mutator = mutate(project);
    mutator.removeClip(m_rightId);
    mutator.replaceClip(*m_original);
}

// ---------------------------------------------------------------------------------------------

EditClipCommand::EditClipCommand(ClipId id, std::string text, Edit edit, std::string mergeKey)
    : m_id(id), m_text(std::move(text)), m_edit(std::move(edit)), m_mergeKey(std::move(mergeKey)) {}

void EditClipCommand::redo(Project& project) {
    if (!m_after) {
        const auto ref = requireClip(project, m_id);
        requireUnlocked(*ref.track);
        Clip after = *ref.clip;
        m_edit(after);
        after.id = m_id;
        const Clip before = *ref.clip;
        mutate(project).replaceClip(after);
        m_before = before;
        m_after = std::move(after);
        return;
    }
    mutate(project).replaceClip(*m_after);
}

void EditClipCommand::undo(Project& project) {
    mutate(project).replaceClip(*m_before);
}

bool EditClipCommand::mergeWith(const Command& next) {
    const auto* other = dynamic_cast<const EditClipCommand*>(&next);
    if (other == nullptr || other->m_id != m_id || other->m_mergeKey != m_mergeKey || m_mergeKey.empty()) {
        return false;
    }
    m_after = other->m_after;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Factories

namespace {

std::string gestureKey(GestureId gesture, const std::string& property) {
    return gesture == 0 ? std::string() : std::to_string(gesture) + ":" + property;
}

} // namespace

std::unique_ptr<Command> trimClip(ClipId id, Rational start, Rational sourceIn, Rational duration, GestureId gesture) {
    return std::make_unique<EditClipCommand>(
        id, "Trim Clip",
        [=](Clip& c) {
            c.start = start;
            c.sourceIn = sourceIn;
            c.duration = duration;
        },
        gestureKey(gesture, "trim"));
}

std::unique_ptr<Command> setClipEnabled(ClipId id, bool enabled) {
    return std::make_unique<EditClipCommand>(id, enabled ? "Enable Clip" : "Disable Clip",
                                             [enabled](Clip& c) { c.enabled = enabled; });
}

std::unique_ptr<Command> renameClip(ClipId id, std::string name) {
    return std::make_unique<EditClipCommand>(id, "Rename Clip", [name = std::move(name)](Clip& c) { c.name = name; });
}

std::unique_ptr<Command> addEffect(ClipId id, Effect effect) {
    if (effect.id.isNull()) {
        effect.id = EffectId::generate();
    }
    return std::make_unique<EditClipCommand>(id, "Add Effect",
                                             [effect = std::move(effect)](Clip& c) { c.effects.push_back(effect); });
}

std::unique_ptr<Command> removeEffect(ClipId id, EffectId effectId) {
    return std::make_unique<EditClipCommand>(id, "Remove Effect", [effectId](Clip& c) {
        requireEffect(c, effectId);
        std::erase_if(c.effects, [&](const Effect& e) { return e.id == effectId; });
    });
}

std::unique_ptr<Command> setEffectEnabled(ClipId id, EffectId effectId, bool enabled) {
    return std::make_unique<EditClipCommand>(id, enabled ? "Enable Effect" : "Disable Effect",
                                             [=](Clip& c) { requireEffect(c, effectId).enabled = enabled; });
}

std::unique_ptr<Command> setClipFades(ClipId id, Rational fadeIn, Rational fadeOut, GestureId gesture) {
    if (fadeIn.isNegative() || fadeOut.isNegative()) {
        throw CommandError("Fades cannot be negative");
    }
    return std::make_unique<EditClipCommand>(
        id, "Audio Fade",
        [=](Clip& c) {
            c.fadeIn = min(fadeIn, c.duration);
            c.fadeOut = min(fadeOut, c.duration);
        },
        gestureKey(gesture, "fades"));
}

std::unique_ptr<Command> setParameterValue(ClipId id, EffectId effectId, std::string key, double value,
                                           GestureId gesture) {
    std::string mergeKey = gestureKey(gesture, "param:" + effectId.toString() + ":" + key);
    return std::make_unique<EditClipCommand>(
        id, "Change " + key,
        [effectId, key = std::move(key), value](Clip& c) {
            requireParameter(requireEffect(c, effectId), key).value = value;
        },
        std::move(mergeKey));
}

std::unique_ptr<Command> setKeyframe(ClipId id, EffectId effectId, std::string key, Keyframe keyframe) {
    return std::make_unique<EditClipCommand>(
        id, "Set Keyframe", [effectId, key = std::move(key), keyframe](Clip& c) {
            auto& keys = requireParameter(requireEffect(c, effectId), key).keyframes;
            const auto it = std::lower_bound(keys.begin(), keys.end(), keyframe.time,
                                             [](const Keyframe& k, const Rational& t) { return k.time < t; });
            if (it != keys.end() && it->time == keyframe.time) {
                *it = keyframe;
            } else {
                keys.insert(it, keyframe);
            }
        });
}

std::unique_ptr<Command> removeKeyframe(ClipId id, EffectId effectId, std::string key, Rational time) {
    return std::make_unique<EditClipCommand>(id, "Remove Keyframe", [effectId, key = std::move(key), time](Clip& c) {
        auto& keys = requireParameter(requireEffect(c, effectId), key).keyframes;
        const auto before = keys.size();
        std::erase_if(keys, [&](const Keyframe& k) { return k.time == time; });
        if (keys.size() == before) {
            throw CommandError("No keyframe at that time");
        }
    });
}

} // namespace loki::commands
