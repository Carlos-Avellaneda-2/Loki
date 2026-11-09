#pragma once

#include "commands/clip_commands.h"
#include "commands/command.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace loki::commands {

// ---------------------------------------------------------------------------------------------
// Media

class AddMediaCommand final : public Command {
public:
    explicit AddMediaCommand(model::MediaItem item);
    [[nodiscard]] std::string text() const override { return "Import " + m_item.name; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] model::MediaId mediaId() const noexcept { return m_item.id; }

private:
    model::MediaItem m_item;
    std::size_t m_index = 0;
};

// Removes a media item and every clip that uses it.
class RemoveMediaCommand final : public Command {
public:
    explicit RemoveMediaCommand(model::MediaId id) : m_id(id) {}
    [[nodiscard]] std::string text() const override { return "Remove Media"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::MediaId m_id;
    std::optional<model::MediaItem> m_item;
    std::size_t m_index = 0;
    std::vector<std::pair<model::TrackId, model::Clip>> m_clips;
};

// Generic before/after edit of a media item (rename, move to bin, metadata, proxy).
class EditMediaCommand final : public Command {
public:
    using Edit = std::function<void(model::MediaItem&)>;
    EditMediaCommand(model::MediaId id, std::string text, Edit edit);
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::MediaId m_id;
    std::string m_text;
    Edit m_edit;
    std::optional<model::MediaItem> m_before;
    std::optional<model::MediaItem> m_after;
};

[[nodiscard]] std::unique_ptr<Command> renameMedia(model::MediaId id, std::string name);
[[nodiscard]] std::unique_ptr<Command> moveMediaToBin(model::MediaId id, model::BinId bin);
// Source Monitor marks; nullopt clears a mark.
[[nodiscard]] std::unique_ptr<Command> setMediaMarkIn(model::MediaId id, std::optional<Rational> in);
[[nodiscard]] std::unique_ptr<Command> setMediaMarkOut(model::MediaId id, std::optional<Rational> out);
// Fills in probed metadata (and proxy path) without touching user edits.
[[nodiscard]] std::unique_ptr<Command> setMediaInfo(model::MediaId id, model::MediaInfo info);

// ---------------------------------------------------------------------------------------------
// Bins

class AddBinCommand final : public Command {
public:
    explicit AddBinCommand(model::Bin bin);
    [[nodiscard]] std::string text() const override { return "New Bin"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] model::BinId binId() const noexcept { return m_bin.id; }

private:
    model::Bin m_bin;
    std::size_t m_index = 0;
};

// Removes an empty bin. Use removeBinRecursive() for bins with content.
class RemoveBinCommand final : public Command {
public:
    explicit RemoveBinCommand(model::BinId id) : m_id(id) {}
    [[nodiscard]] std::string text() const override { return "Remove Bin"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::BinId m_id;
    std::optional<model::Bin> m_bin;
    std::size_t m_index = 0;
};

class EditBinCommand final : public Command {
public:
    using Edit = std::function<void(model::Bin&)>;
    EditBinCommand(model::BinId id, std::string text, Edit edit);
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::BinId m_id;
    std::string m_text;
    Edit m_edit;
    std::optional<model::Bin> m_before;
    std::optional<model::Bin> m_after;
};

[[nodiscard]] std::unique_ptr<Command> renameBin(model::BinId id, std::string name);
[[nodiscard]] std::unique_ptr<Command> moveBin(model::BinId id, model::BinId newParent);
// Removes a bin with all nested bins and media (and the clips using that media).
[[nodiscard]] std::unique_ptr<Command> removeBinRecursive(const model::Project& project, model::BinId id);

// ---------------------------------------------------------------------------------------------
// Sequences

class AddSequenceCommand final : public Command {
public:
    explicit AddSequenceCommand(model::Sequence sequence);
    [[nodiscard]] std::string text() const override { return "New Sequence"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] model::SequenceId sequenceId() const noexcept { return m_sequence.id; }

private:
    model::Sequence m_sequence;
    std::size_t m_index = 0;
};

class RemoveSequenceCommand final : public Command {
public:
    explicit RemoveSequenceCommand(model::SequenceId id) : m_id(id) {}
    [[nodiscard]] std::string text() const override { return "Remove Sequence"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::SequenceId m_id;
    std::optional<model::Sequence> m_sequence;
    std::size_t m_index = 0;
};

class RenameSequenceCommand final : public Command {
public:
    RenameSequenceCommand(model::SequenceId id, std::string name) : m_id(id), m_name(std::move(name)) {}
    [[nodiscard]] std::string text() const override { return "Rename Sequence"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::SequenceId m_id;
    std::string m_name;
    std::string m_oldName;
};

class SetSequenceSettingsCommand final : public Command {
public:
    SetSequenceSettingsCommand(model::SequenceId id, model::SequenceSettings settings)
        : m_id(id), m_settings(std::move(settings)) {}
    [[nodiscard]] std::string text() const override { return "Sequence Settings"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::SequenceId m_id;
    model::SequenceSettings m_settings;
    model::SequenceSettings m_oldSettings;
};

// Sets the sequence In/Out marks (nullopt clears one). A new In past the Out clears the Out and
// vice versa, like Premiere.
class SetSequenceMarksCommand final : public Command {
public:
    SetSequenceMarksCommand(model::SequenceId id, std::optional<Rational> in, std::optional<Rational> out,
                            std::string text)
        : m_id(id), m_in(in), m_out(out), m_text(std::move(text)) {}
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::SequenceId m_id;
    std::optional<Rational> m_in;
    std::optional<Rational> m_out;
    std::optional<Rational> m_oldIn;
    std::optional<Rational> m_oldOut;
    std::string m_text;
};

[[nodiscard]] std::unique_ptr<Command> setSequenceMarkIn(const model::Project& project, model::SequenceId id,
                                                         std::optional<Rational> in);
[[nodiscard]] std::unique_ptr<Command> setSequenceMarkOut(const model::Project& project, model::SequenceId id,
                                                          std::optional<Rational> out);

// ---------------------------------------------------------------------------------------------
// Tracks

class AddTrackCommand final : public Command {
public:
    // index: position in the V or A list (nullopt = on top / at the end).
    AddTrackCommand(model::SequenceId sequence, model::TrackKind kind, std::optional<std::size_t> index = std::nullopt,
                    std::string name = {});
    [[nodiscard]] std::string text() const override { return "Add Track"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] model::TrackId trackId() const noexcept { return m_track.id; }

private:
    model::SequenceId m_sequence;
    std::optional<std::size_t> m_requestedIndex;
    model::Track m_track;
};

// Removes a track together with its clips.
class RemoveTrackCommand final : public Command {
public:
    explicit RemoveTrackCommand(model::TrackId id) : m_id(id) {}
    [[nodiscard]] std::string text() const override { return "Delete Track"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::TrackId m_id;
    model::SequenceId m_sequence;
    std::optional<model::Track> m_track;
    std::size_t m_index = 0;
};

class SetTrackStateCommand final : public Command {
public:
    // Commands with the same non-zero gesture on the same track merge (fader / pan drags).
    SetTrackStateCommand(model::TrackId id, model::TrackState state, std::string text = "Track Settings",
                         GestureId gesture = 0)
        : m_id(id), m_state(std::move(state)), m_text(std::move(text)), m_gesture(gesture) {}
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] int mergeId() const override { return m_gesture == 0 ? -1 : kMergeTrackState; }
    bool mergeWith(const Command& next) override;

private:
    model::TrackId m_id;
    model::TrackState m_state;
    model::TrackState m_oldState;
    std::string m_text;
    GestureId m_gesture;
};

// Master fader of a sequence (D16). Merges within a gesture.
class SetMasterVolumeCommand final : public Command {
public:
    SetMasterVolumeCommand(model::SequenceId id, double db, GestureId gesture = 0)
        : m_id(id), m_db(db), m_gesture(gesture) {}
    [[nodiscard]] std::string text() const override { return "Master Volume"; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] int mergeId() const override { return m_gesture == 0 ? -1 : kMergeMasterVolume; }
    bool mergeWith(const Command& next) override;

private:
    model::SequenceId m_id;
    double m_db;
    double m_oldDb = 0.0;
    GestureId m_gesture;
};

[[nodiscard]] std::unique_ptr<Command> setTrackLocked(const model::Project& project, model::TrackId id, bool locked);
[[nodiscard]] std::unique_ptr<Command> setTrackHidden(const model::Project& project, model::TrackId id, bool hidden);
[[nodiscard]] std::unique_ptr<Command> setTrackMuted(const model::Project& project, model::TrackId id, bool muted);
// Audio mixer (D16). Volume in dB, pan in [-100, 100].
[[nodiscard]] std::unique_ptr<Command> setTrackVolume(const model::Project& project, model::TrackId id, double db,
                                                      GestureId gesture = 0);
[[nodiscard]] std::unique_ptr<Command> setTrackPan(const model::Project& project, model::TrackId id, double pan,
                                                   GestureId gesture = 0);
[[nodiscard]] std::unique_ptr<Command> setTrackSolo(const model::Project& project, model::TrackId id, bool solo);
[[nodiscard]] std::unique_ptr<Command> renameTrack(const model::Project& project, model::TrackId id, std::string name);

} // namespace loki::commands
