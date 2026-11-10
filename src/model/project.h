#pragma once

#include "model/ids.h"
#include "model/types.h"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace loki::commands {
class Command;
} // namespace loki::commands

namespace loki::model {

// Thrown when a mutation would break a model invariant (unknown id, overlapping clips, ...).
class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ChangeKind {
    MediaAdded,
    MediaRemoved,
    MediaChanged,
    BinAdded,
    BinRemoved,
    BinChanged,
    SequenceAdded,
    SequenceRemoved,
    SequenceChanged,
    TrackAdded,
    TrackRemoved,
    TrackChanged,
    ClipAdded,
    ClipRemoved,
    ClipChanged,
};

// Granular change notification. `id` is the changed object; `parent` its container
// (sequence for tracks, track for clips, bin for media/bins), null when not applicable.
struct ChangeEvent {
    ChangeKind kind;
    Uuid id;
    Uuid parent;
};

class ProjectObserver {
public:
    virtual ~ProjectObserver() = default;
    virtual void onProjectChanged(const ChangeEvent& event) = 0;

protected:
    ProjectObserver() = default;
    ProjectObserver(const ProjectObserver&) = default;
    ProjectObserver& operator=(const ProjectObserver&) = default;
    ProjectObserver(ProjectObserver&&) = default;
    ProjectObserver& operator=(ProjectObserver&&) = default;
};

// Location of a track inside the project (valid until the next mutation).
struct TrackRef {
    const Sequence* sequence = nullptr;
    const Track* track = nullptr;
    std::size_t index = 0; // index within sequence->tracks(track->kind)
};

// Location of a clip inside the project (valid until the next mutation).
struct ClipRef {
    const Sequence* sequence = nullptr;
    const Track* track = nullptr;
    const Clip* clip = nullptr;
    std::size_t index = 0; // index within track->clips
};

// Location of a transition inside the project (valid until the next mutation).
struct TransitionRef {
    const Sequence* sequence = nullptr;
    const Track* track = nullptr;
    const Transition* transition = nullptr;
};

class ProjectMutator;
class ProjectBuilder;

// Root of the document. Public API is read-only (D2): mutations go through ProjectMutator,
// which only Commands (and ProjectBuilder for brand-new projects) can obtain.
// Copying a Project copies the data but never the observers (cheap snapshots for other threads).
class Project {
public:
    Project() = default;
    Project(const Project& other);
    Project& operator=(const Project& other);
    Project(Project&& other) noexcept;
    Project& operator=(Project&& other) noexcept;
    ~Project() = default;

    [[nodiscard]] const std::vector<MediaItem>& media() const noexcept { return m_media; }
    [[nodiscard]] const std::vector<Bin>& bins() const noexcept { return m_bins; }
    [[nodiscard]] const std::vector<Sequence>& sequences() const noexcept { return m_sequences; }

    [[nodiscard]] const MediaItem* findMedia(MediaId id) const noexcept;
    [[nodiscard]] const Bin* findBin(BinId id) const noexcept;
    [[nodiscard]] const Sequence* findSequence(SequenceId id) const noexcept;
    [[nodiscard]] TrackRef findTrack(TrackId id) const noexcept;
    [[nodiscard]] ClipRef findClip(ClipId id) const noexcept;
    [[nodiscard]] TransitionRef findTransition(TransitionId id) const noexcept;

    // Clips (in every sequence) whose media is `id`.
    [[nodiscard]] std::vector<ClipId> clipsUsingMedia(MediaId id) const;
    // Sequences nested (through kNestGenerator media) directly in `sequence`.
    [[nodiscard]] std::vector<SequenceId> nestedSequencesOf(const Sequence& sequence) const;
    // True if `outer` is `target` or nests it at any depth (D30: cycles are forbidden).
    [[nodiscard]] bool sequenceContains(SequenceId outer, SequenceId target) const;
    // True if `ancestor` is `bin` or one of its parents.
    [[nodiscard]] bool binIsWithin(BinId bin, BinId ancestor) const noexcept;

    // Checks every invariant; returns human-readable problems (empty = valid).
    [[nodiscard]] std::vector<std::string> validate() const;

    void addObserver(ProjectObserver* observer);
    void removeObserver(ProjectObserver* observer) noexcept;

    // Data equality (observers ignored).
    friend bool operator==(const Project& a, const Project& b) {
        return a.m_media == b.m_media && a.m_bins == b.m_bins && a.m_sequences == b.m_sequences;
    }

private:
    friend class ProjectMutator;

    void notify(const ChangeEvent& event) const;

    std::vector<MediaItem> m_media;
    std::vector<Bin> m_bins;
    std::vector<Sequence> m_sequences;
    std::vector<ProjectObserver*> m_observers; // not owned, not copied
};

// The only write access to a Project. Every operation validates invariants first and either
// succeeds completely or throws ModelError leaving the project unchanged (strong guarantee),
// then notifies observers. Operations are low-level and reversible so Commands can undo them.
class ProjectMutator {
public:
    // Media
    void insertMedia(std::size_t index, MediaItem item);
    MediaItem removeMedia(MediaId id, std::size_t* indexOut = nullptr);
    void replaceMedia(MediaItem item);

    // Bins
    void insertBin(std::size_t index, Bin bin);
    Bin removeBin(BinId id, std::size_t* indexOut = nullptr); // bin must be empty
    void replaceBin(Bin bin);

    // Sequences
    void insertSequence(std::size_t index, Sequence sequence);
    Sequence removeSequence(SequenceId id, std::size_t* indexOut = nullptr);
    void setSequenceName(SequenceId id, std::string name);
    void setSequenceMasterVolume(SequenceId id, double db);
    void setSequenceMarkers(SequenceId id, std::vector<Marker> markers); // sorted by time
    void setSequenceSettings(SequenceId id, SequenceSettings settings);
    void setSequenceMarks(SequenceId id, std::optional<Rational> in, std::optional<Rational> out);

    // Tracks
    void insertTrack(SequenceId sequence, std::size_t index, Track track);
    Track removeTrack(TrackId id, std::size_t* indexOut = nullptr);
    void setTrackState(TrackId id, TrackState state);
    void addTransition(TrackId track, Transition transition);
    Transition removeTransition(TransitionId id);
    void replaceTransition(Transition transition); // same id; any field but the track

    // Clips (kept sorted by start; overlaps on the same track are rejected)
    void insertClip(TrackId track, Clip clip);
    Clip removeClip(ClipId id);
    void replaceClip(Clip clip); // same id, same track; may change any field

private:
    friend class commands::Command;
    friend class ProjectBuilder;

    explicit ProjectMutator(Project& project) noexcept : m_project(project) {}

    Sequence& sequence(SequenceId id);
    std::pair<Sequence*, std::vector<Track>*> trackList(TrackId id, std::size_t& index);
    Track& track(TrackId id);
    void checkClip(const Track& track, const Clip& clip, ClipId ignore) const;
    void checkUniqueClipIds(const Track& track) const;

    Project& m_project;
};

// Builds a brand-new Project (deserialization, templates, tests). It cannot touch an existing
// Project, so the "mutate only through Commands" rule still holds for live documents.
class ProjectBuilder {
public:
    ProjectBuilder() : m_mutator(m_project) {}
    ProjectBuilder(const ProjectBuilder&) = delete;
    ProjectBuilder& operator=(const ProjectBuilder&) = delete;
    ProjectBuilder(ProjectBuilder&&) = delete;
    ProjectBuilder& operator=(ProjectBuilder&&) = delete;
    ~ProjectBuilder() = default;

    ProjectBuilder& addMedia(MediaItem item);
    ProjectBuilder& addBin(Bin bin);
    ProjectBuilder& addSequence(Sequence sequence); // tracks and clips included, validated
    [[nodiscard]] const Project& peek() const noexcept { return m_project; }

    // Validates the whole project and hands it over. Throws ModelError if invalid.
    [[nodiscard]] Project build();

private:
    Project m_project;
    ProjectMutator m_mutator;
};

// Default project: one 1080p30 sequence with V1-V3 and A1-A3.
[[nodiscard]] Project makeDefaultProject();
[[nodiscard]] Sequence makeDefaultSequence(std::string name, SequenceSettings settings = {}, int videoTracks = 3,
                                           int audioTracks = 3);

} // namespace loki::model
