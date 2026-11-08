#include "commands/project_commands.h"

#include <algorithm>

namespace loki::commands {

using namespace loki::model;

namespace {

TrackRef requireTrack(TrackRef ref) {
    if (ref.track == nullptr) {
        throw CommandError("Track does not exist");
    }
    return ref;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Media

AddMediaCommand::AddMediaCommand(MediaItem item) : m_item(std::move(item)) {
    if (m_item.id.isNull()) {
        m_item.id = MediaId::generate();
    }
}

void AddMediaCommand::redo(Project& project) {
    m_index = project.media().size();
    mutate(project).insertMedia(m_index, m_item);
}

void AddMediaCommand::undo(Project& project) {
    mutate(project).removeMedia(m_item.id);
}

void RemoveMediaCommand::redo(Project& project) {
    if (project.findMedia(m_id) == nullptr) {
        throw CommandError("Media does not exist");
    }
    auto mutator = mutate(project);
    m_clips.clear();
    for (const auto clipId : project.clipsUsingMedia(m_id)) {
        const auto trackId = project.findClip(clipId).track->id;
        m_clips.emplace_back(trackId, mutator.removeClip(clipId));
    }
    m_item = mutator.removeMedia(m_id, &m_index);
}

void RemoveMediaCommand::undo(Project& project) {
    auto mutator = mutate(project);
    mutator.insertMedia(m_index, *m_item);
    for (auto it = m_clips.rbegin(); it != m_clips.rend(); ++it) {
        mutator.insertClip(it->first, it->second);
    }
}

EditMediaCommand::EditMediaCommand(MediaId id, std::string text, Edit edit)
    : m_id(id), m_text(std::move(text)), m_edit(std::move(edit)) {}

void EditMediaCommand::redo(Project& project) {
    if (!m_after) {
        const auto* current = project.findMedia(m_id);
        if (current == nullptr) {
            throw CommandError("Media does not exist");
        }
        MediaItem after = *current;
        m_edit(after);
        after.id = m_id; // identity is not editable
        m_before = *current;
        mutate(project).replaceMedia(after);
        m_after = std::move(after);
        return;
    }
    mutate(project).replaceMedia(*m_after);
}

void EditMediaCommand::undo(Project& project) {
    mutate(project).replaceMedia(*m_before);
}

std::unique_ptr<Command> renameMedia(MediaId id, std::string name) {
    return std::make_unique<EditMediaCommand>(id, "Rename", [name = std::move(name)](MediaItem& m) { m.name = name; });
}

std::unique_ptr<Command> moveMediaToBin(MediaId id, BinId bin) {
    return std::make_unique<EditMediaCommand>(id, "Move to Bin", [bin](MediaItem& m) { m.bin = bin; });
}

std::unique_ptr<Command> setMediaMarkIn(MediaId id, std::optional<Rational> in) {
    return std::make_unique<EditMediaCommand>(id, in ? "Mark In" : "Clear In", [in](MediaItem& m) {
        m.markIn = in;
        if (in && m.markOut && !(*in < *m.markOut)) {
            m.markOut.reset(); // like Premiere: a new In past the Out clears the Out
        }
    });
}

std::unique_ptr<Command> setMediaMarkOut(MediaId id, std::optional<Rational> out) {
    return std::make_unique<EditMediaCommand>(id, out ? "Mark Out" : "Clear Out", [out](MediaItem& m) {
        m.markOut = out;
        if (out && m.markIn && !(*m.markIn < *out)) {
            m.markIn.reset();
        }
    });
}

std::unique_ptr<Command> setMediaInfo(MediaId id, MediaInfo info) {
    return std::make_unique<EditMediaCommand>(id, "Update Media Info",
                                              [info = std::move(info)](MediaItem& m) { m.info = info; });
}

// ---------------------------------------------------------------------------------------------
// Bins

AddBinCommand::AddBinCommand(Bin bin) : m_bin(std::move(bin)) {
    if (m_bin.id.isNull()) {
        m_bin.id = BinId::generate();
    }
}

void AddBinCommand::redo(Project& project) {
    m_index = project.bins().size();
    mutate(project).insertBin(m_index, m_bin);
}

void AddBinCommand::undo(Project& project) {
    mutate(project).removeBin(m_bin.id);
}

void RemoveBinCommand::redo(Project& project) {
    m_bin = mutate(project).removeBin(m_id, &m_index);
}

void RemoveBinCommand::undo(Project& project) {
    mutate(project).insertBin(m_index, *m_bin);
}

EditBinCommand::EditBinCommand(BinId id, std::string text, Edit edit)
    : m_id(id), m_text(std::move(text)), m_edit(std::move(edit)) {}

void EditBinCommand::redo(Project& project) {
    if (!m_after) {
        const auto* current = project.findBin(m_id);
        if (current == nullptr) {
            throw CommandError("Bin does not exist");
        }
        Bin after = *current;
        m_edit(after);
        after.id = m_id;
        m_before = *current;
        mutate(project).replaceBin(after);
        m_after = std::move(after);
        return;
    }
    mutate(project).replaceBin(*m_after);
}

void EditBinCommand::undo(Project& project) {
    mutate(project).replaceBin(*m_before);
}

std::unique_ptr<Command> renameBin(BinId id, std::string name) {
    return std::make_unique<EditBinCommand>(id, "Rename Bin", [name = std::move(name)](Bin& b) { b.name = name; });
}

std::unique_ptr<Command> moveBin(BinId id, BinId newParent) {
    return std::make_unique<EditBinCommand>(id, "Move Bin", [newParent](Bin& b) { b.parent = newParent; });
}

std::unique_ptr<Command> removeBinRecursive(const Project& project, BinId id) {
    if (project.findBin(id) == nullptr) {
        throw CommandError("Bin does not exist");
    }
    std::vector<std::unique_ptr<Command>> children;
    for (const auto& media : project.media()) {
        if (!media.bin.isNull() && project.binIsWithin(media.bin, id)) {
            children.push_back(std::make_unique<RemoveMediaCommand>(media.id));
        }
    }
    // Deepest bins first so each one is empty when removed.
    std::vector<std::pair<std::size_t, BinId>> bins;
    for (const auto& bin : project.bins()) {
        if (project.binIsWithin(bin.id, id)) {
            std::size_t depth = 0;
            for (auto parent = bin.parent; !parent.isNull() && depth <= project.bins().size(); ++depth) {
                parent = project.findBin(parent)->parent;
            }
            bins.emplace_back(depth, bin.id);
        }
    }
    std::stable_sort(bins.begin(), bins.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [depth, binId] : bins) {
        (void)depth;
        children.push_back(std::make_unique<RemoveBinCommand>(binId));
    }
    return std::make_unique<MacroCommand>("Remove Bin", std::move(children));
}

// ---------------------------------------------------------------------------------------------
// Sequences

AddSequenceCommand::AddSequenceCommand(Sequence sequence) : m_sequence(std::move(sequence)) {
    if (m_sequence.id.isNull()) {
        m_sequence.id = SequenceId::generate();
    }
}

void AddSequenceCommand::redo(Project& project) {
    m_index = project.sequences().size();
    mutate(project).insertSequence(m_index, m_sequence);
}

void AddSequenceCommand::undo(Project& project) {
    mutate(project).removeSequence(m_sequence.id);
}

void RemoveSequenceCommand::redo(Project& project) {
    m_sequence = mutate(project).removeSequence(m_id, &m_index);
}

void RemoveSequenceCommand::undo(Project& project) {
    mutate(project).insertSequence(m_index, *m_sequence);
}

void RenameSequenceCommand::redo(Project& project) {
    const auto* seq = project.findSequence(m_id);
    if (seq == nullptr) {
        throw CommandError("Sequence does not exist");
    }
    m_oldName = seq->name;
    mutate(project).setSequenceName(m_id, m_name);
}

void RenameSequenceCommand::undo(Project& project) {
    mutate(project).setSequenceName(m_id, m_oldName);
}

void SetSequenceSettingsCommand::redo(Project& project) {
    const auto* seq = project.findSequence(m_id);
    if (seq == nullptr) {
        throw CommandError("Sequence does not exist");
    }
    m_oldSettings = seq->settings;
    mutate(project).setSequenceSettings(m_id, m_settings);
}

void SetSequenceSettingsCommand::undo(Project& project) {
    mutate(project).setSequenceSettings(m_id, m_oldSettings);
}

// ---------------------------------------------------------------------------------------------
// Tracks

AddTrackCommand::AddTrackCommand(SequenceId sequence, TrackKind kind, std::optional<std::size_t> index,
                                 std::string name)
    : m_sequence(sequence), m_requestedIndex(index) {
    m_track.id = TrackId::generate(); // stable across undo/redo
    m_track.kind = kind;
    m_track.state.name = std::move(name);
}

void AddTrackCommand::redo(Project& project) {
    const auto* seq = project.findSequence(m_sequence);
    if (seq == nullptr) {
        throw CommandError("Sequence does not exist");
    }
    const auto count = seq->tracks(m_track.kind).size();
    const auto index = std::min(m_requestedIndex.value_or(count), count);
    Track track = m_track;
    if (track.state.name.empty()) {
        track.state.name = (m_track.kind == TrackKind::Video ? "V" : "A") + std::to_string(count + 1);
    }
    mutate(project).insertTrack(m_sequence, index, std::move(track));
}

void AddTrackCommand::undo(Project& project) {
    mutate(project).removeTrack(m_track.id);
}

void RemoveTrackCommand::redo(Project& project) {
    const auto ref = requireTrack(project.findTrack(m_id));
    m_sequence = ref.sequence->id;
    m_track = mutate(project).removeTrack(m_id, &m_index);
}

void RemoveTrackCommand::undo(Project& project) {
    mutate(project).insertTrack(m_sequence, m_index, *m_track);
}

void SetTrackStateCommand::redo(Project& project) {
    const auto ref = requireTrack(project.findTrack(m_id));
    m_oldState = ref.track->state;
    mutate(project).setTrackState(m_id, m_state);
}

void SetTrackStateCommand::undo(Project& project) {
    mutate(project).setTrackState(m_id, m_oldState);
}

namespace {

template <typename Fn>
std::unique_ptr<Command> editTrackState(const Project& project, TrackId id, std::string text, Fn fn) {
    const auto ref = project.findTrack(id);
    if (ref.track == nullptr) {
        throw CommandError("Track does not exist");
    }
    TrackState state = ref.track->state;
    fn(state);
    return std::make_unique<SetTrackStateCommand>(id, std::move(state), std::move(text));
}

} // namespace

std::unique_ptr<Command> setTrackLocked(const Project& project, TrackId id, bool locked) {
    return editTrackState(project, id, locked ? "Lock Track" : "Unlock Track",
                          [locked](TrackState& s) { s.locked = locked; });
}

std::unique_ptr<Command> setTrackHidden(const Project& project, TrackId id, bool hidden) {
    return editTrackState(project, id, hidden ? "Hide Track" : "Show Track",
                          [hidden](TrackState& s) { s.hidden = hidden; });
}

std::unique_ptr<Command> setTrackMuted(const Project& project, TrackId id, bool muted) {
    return editTrackState(project, id, muted ? "Mute Track" : "Unmute Track",
                          [muted](TrackState& s) { s.muted = muted; });
}

std::unique_ptr<Command> renameTrack(const Project& project, TrackId id, std::string name) {
    return editTrackState(project, id, "Rename Track", [&name](TrackState& s) { s.name = std::move(name); });
}

} // namespace loki::commands
