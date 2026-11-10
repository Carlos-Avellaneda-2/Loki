#pragma once

#include "commands/clip_commands.h"
#include "commands/command.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace loki::commands {

struct PlacedClip {
    model::TrackId track;
    model::Clip clip;

    friend bool operator==(const PlacedClip&, const PlacedClip&) = default;
};

// Atomically replaces one set of clips with another (any tracks of one sequence): every clip in
// `before` is removed, then every clip in `after` is inserted. Undo does the reverse. If anything
// fails the partial work is rolled back (strong guarantee). This single primitive implements
// moves, ripples, trims, splits, insert and overwrite edits.
class ReplaceClipsCommand final : public Command {
public:
    ReplaceClipsCommand(std::string text, std::vector<PlacedClip> before, std::vector<PlacedClip> after,
                        GestureId gesture = 0);
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;
    [[nodiscard]] int mergeId() const override { return m_gesture == 0 ? -1 : kMergeReplaceClips; }
    bool mergeWith(const Command& next) override;

    [[nodiscard]] const std::vector<PlacedClip>& before() const noexcept { return m_before; }
    [[nodiscard]] const std::vector<PlacedClip>& after() const noexcept { return m_after; }

    static constexpr int kMergeReplaceClips = 3;

private:
    static void apply(model::Project& project, const std::vector<PlacedClip>& remove,
                      const std::vector<PlacedClip>& insert);

    std::string m_text;
    std::vector<PlacedClip> m_before;
    std::vector<PlacedClip> m_after;
    GestureId m_gesture;
    std::vector<std::pair<model::TrackId, model::Transition>> m_dropped; // removed by the last redo
};

// Working copy of some tracks of a sequence. Edit operations change the copy; build() diffs it
// against the project and returns one ReplaceClipsCommand (or nullptr when nothing changed).
// Every touched track must be unlocked (CommandError otherwise).
class EditPlan {
public:
    EditPlan(const model::Project& project, model::SequenceId sequence);

    [[nodiscard]] const model::Sequence& sequence() const noexcept { return *m_sequence; }
    [[nodiscard]] const model::Project& project() const noexcept { return m_project; }

    // Clips of a track in the working copy (sorted by start once normalized).
    std::vector<model::Clip>& clips(model::TrackId track);
    [[nodiscard]] bool isLocked(model::TrackId track) const;

    // Makes [start, end) empty on `track`: removes clips inside it, trims the ones crossing an edge
    // and splits a clip spanning the whole range. Clips whose id is in `keep` are left alone.
    void clearRange(model::TrackId track, const Rational& start, const Rational& end,
                    const std::set<model::ClipId>& keep = {});
    // Splits the clip crossing `at` (if any). Returns the id of the right part, or null.
    model::ClipId splitAt(model::TrackId track, const Rational& at);
    // Moves every clip starting at or after `from` by `delta`.
    void shiftFrom(model::TrackId track, const Rational& from, const Rational& delta);
    void add(model::TrackId track, model::Clip clip);
    // Removes a clip from wherever it is in the working copy; returns it with its track.
    std::optional<PlacedClip> take(model::ClipId id);
    // Finds a clip in the working copy (loads its track).
    [[nodiscard]] std::optional<PlacedClip> find(model::ClipId id);

    // Clip ids linked to `id` (including itself), from the project.
    [[nodiscard]] std::vector<model::ClipId> withLinked(model::ClipId id) const;

    // Validates (sorted, no overlap) and returns the command; nullptr if nothing changed.
    [[nodiscard]] std::unique_ptr<Command> build(std::string text, GestureId gesture = 0);

private:
    void load(model::TrackId track);
    model::LinkId splitLinkFor(model::LinkId original);

    const model::Project& m_project;
    const model::Sequence* m_sequence;
    std::map<model::TrackId, std::vector<model::Clip>> m_tracks;
    std::unordered_map<model::LinkId, model::LinkId> m_splitLinks; // keeps split pieces linked
};

// --- High-level edits (they validate and throw CommandError for invalid requests) -------------

// What to place from the Source Monitor: a media item and a source range.
struct SourceRange {
    model::MediaId media;
    Rational sourceIn;
    Rational duration; // > 0
};

// Default source range of a media item: its In/Out marks, else the whole item (stills: 5 s).
[[nodiscard]] SourceRange sourceRangeOf(const model::MediaItem& item);
// Same, but a nested sequence covers its current duration (the item's info may be stale).
[[nodiscard]] SourceRange sourceRangeOf(const model::Project& project, const model::MediaItem& item);

// Destination tracks; null = do not place that part (e.g. video only).
struct EditTargets {
    model::TrackId video;
    model::TrackId audio;
};

// Overwrite edit: places the range at `at`, replacing whatever was there on the target tracks.
// Video and audio parts are linked.
[[nodiscard]] std::unique_ptr<Command> makeOverwriteEdit(const model::Project& project, model::SequenceId sequence,
                                                         const SourceRange& range, const Rational& at,
                                                         const EditTargets& targets);

// Insert edit: splits at `at` and pushes everything after it on every unlocked track to the right
// by the range duration, then places the range on the target tracks.
[[nodiscard]] std::unique_ptr<Command> makeInsertEdit(const model::Project& project, model::SequenceId sequence,
                                                      const SourceRange& range, const Rational& at,
                                                      const EditTargets& targets);

// Razor: splits the given clips (and their linked partners) at `at`. Clips not crossing `at` are
// ignored. Returns nullptr if nothing would be split.
[[nodiscard]] std::unique_ptr<Command> makeRazorEdit(const model::Project& project, model::SequenceId sequence,
                                                     const std::vector<model::ClipId>& clips, const Rational& at);

// Razor across every unlocked track at `at` (Shift+click with the Razor tool).
[[nodiscard]] std::unique_ptr<Command> makeRazorAllTracks(const model::Project& project, model::SequenceId sequence,
                                                          const Rational& at);

// Moves clips (plus linked partners) by `timeDelta`; clips of `dragKind` also move `trackDelta`
// tracks (positive = up). Destination ranges are overwritten like Premiere's default drag.
[[nodiscard]] std::unique_ptr<Command> makeMoveClips(const model::Project& project, model::SequenceId sequence,
                                                     const std::vector<model::ClipId>& clips,
                                                     const Rational& timeDelta, int trackDelta,
                                                     model::TrackKind dragKind, GestureId gesture = 0);

enum class ClipEdge { In, Out };

// Trims one edge of a clip (and its linked partners) by `delta` (positive = later). The result is
// clamped to the media's available source range, a 1-frame minimum duration and the neighbouring
// clips (a trim never overwrites).
[[nodiscard]] std::unique_ptr<Command> makeTrimEdge(const model::Project& project, model::SequenceId sequence,
                                                    model::ClipId clip, ClipEdge edge, const Rational& delta,
                                                    bool includeLinked = true, GestureId gesture = 0);

// Lift: deletes clips (with linked partners when includeLinked) leaving gaps.
[[nodiscard]] std::unique_ptr<Command> makeDeleteClips(const model::Project& project, model::SequenceId sequence,
                                                       const std::vector<model::ClipId>& clips,
                                                       bool includeLinked = true);

// --- Phase 7: advanced edits -----------------------------------------------------------------

// Ripple trim (B): trims an edge like makeTrimEdge but the clips after the edit move to keep
// everything butted (every unlocked track shifts). The trimmed clip keeps its start.
[[nodiscard]] std::unique_ptr<Command> makeRippleTrim(const model::Project& project, model::SequenceId sequence,
                                                      model::ClipId clip, ClipEdge edge, const Rational& delta,
                                                      bool includeLinked = true);

// Rolling edit (N): moves the edit point between `clip` and its adjacent neighbour on that edge
// (out of the left clip and in of the right one change together; total duration unchanged).
[[nodiscard]] std::unique_ptr<Command> makeRollEdit(const model::Project& project, model::SequenceId sequence,
                                                    model::ClipId clip, ClipEdge edge, const Rational& delta,
                                                    bool includeLinked = true);

// Slip (Y): changes which part of the source the clip shows; position and duration stay.
// Positive `delta` shows later source content.
[[nodiscard]] std::unique_ptr<Command> makeSlipEdit(const model::Project& project, model::SequenceId sequence,
                                                    model::ClipId clip, const Rational& delta,
                                                    bool includeLinked = true);

// Slide (U): moves the clip in time keeping its content; the adjacent neighbours absorb the move
// (left one extends/shortens its out, right one its in). Without a neighbour the gap absorbs it.
[[nodiscard]] std::unique_ptr<Command> makeSlideEdit(const model::Project& project, model::SequenceId sequence,
                                                     model::ClipId clip, const Rational& delta,
                                                     bool includeLinked = true);

// Ripple delete: deletes the clips (with linked partners) and closes the gap on every unlocked
// track whose range is free after the deletion (D14).
[[nodiscard]] std::unique_ptr<Command> makeRippleDelete(const model::Project& project, model::SequenceId sequence,
                                                        const std::vector<model::ClipId>& clips);

// Lift removes [in, out) on every unlocked track leaving a gap; Extract also closes it.
[[nodiscard]] std::unique_ptr<Command> makeLiftRange(const model::Project& project, model::SequenceId sequence,
                                                     const Rational& in, const Rational& out);
[[nodiscard]] std::unique_ptr<Command> makeExtractRange(const model::Project& project, model::SequenceId sequence,
                                                        const Rational& in, const Rational& out);

// Link clips into one A/V group (they move, trim and cut together) / remove their links.
[[nodiscard]] std::unique_ptr<Command> makeLinkClips(const model::Project& project, model::SequenceId sequence,
                                                     const std::vector<model::ClipId>& clips);
[[nodiscard]] std::unique_ptr<Command> makeUnlinkClips(const model::Project& project, model::SequenceId sequence,
                                                       const std::vector<model::ClipId>& clips);

// --- Titles (Phase 9) --------------------------------------------------------------------------

// Creates a title (generated media, D20) in the project and places `duration` of it at `at` on
// `track` (overwrite), as one undo step. Returns the command; `titleMedia` receives the new id.
[[nodiscard]] std::unique_ptr<Command> makeNewTitle(const model::Project& project, model::SequenceId sequence,
                                                    model::TrackId track, const Rational& at, const Rational& duration,
                                                    std::string name, model::MediaId* titleMedia = nullptr);

// Places `duration` of a new generated media item (`generator`, e.g. kAdjustmentGenerator) at `at`
// on a video `track` (overwrite), creating the item in the project, as one undo step.
[[nodiscard]] std::unique_ptr<Command> makeNewGeneratedClip(const model::Project& project, model::SequenceId sequence,
                                                            model::TrackId track, const Rational& at,
                                                            const Rational& duration, std::string name,
                                                            std::string_view generator,
                                                            model::MediaId* newMedia = nullptr);

// --- Speed (D28) -------------------------------------------------------------------------------

// Speed/Duration: sets `speed` (> 0, 1 = 100 %) and `reverse` on the clips (and linked partners).
// The source content stays, so the duration becomes span / speed (whole frames, limited by the
// media). Without `ripple` a longer clip stops at the next clip; with it the following clips of
// that track move.
[[nodiscard]] std::unique_ptr<Command> makeSetClipSpeed(const model::Project& project, model::SequenceId sequence,
                                                        const std::vector<model::ClipId>& clips, const Rational& speed,
                                                        bool reverse, bool ripple);

// Rate Stretch tool (R): drags an edge changing the duration but not the content, so the speed
// follows (span / duration). Never overwrites neighbours.
[[nodiscard]] std::unique_ptr<Command> makeRateStretch(const model::Project& project, model::SequenceId sequence,
                                                       model::ClipId clip, ClipEdge edge, const Rational& delta,
                                                       bool includeLinked = true, GestureId gesture = 0);

// Add Frame Hold: from `at` to the end the video clip shows the frame at `at` (speed 0). `at` at
// the clip start holds the whole clip. Linked audio is unchanged.
[[nodiscard]] std::unique_ptr<Command> makeAddFrameHold(const model::Project& project, model::SequenceId sequence,
                                                        model::ClipId clip, const Rational& at);

// --- Nest (D30) --------------------------------------------------------------------------------

// Moves the clips (and linked partners) into a new sequence (same settings, same track indices,
// times relative to the first clip) and puts one nested clip per kind in their place on the
// lowest track used (video and audio parts linked). Transitions between nested clips move too.
[[nodiscard]] std::unique_ptr<Command> makeNest(const model::Project& project, model::SequenceId sequence,
                                                const std::vector<model::ClipId>& clips, std::string name,
                                                model::MediaId* nestMedia = nullptr,
                                                model::SequenceId* nestedSequence = nullptr);

// A nested-sequence media item for `sequence` (D30), to place it from the Project panel.
[[nodiscard]] model::MediaItem makeNestMedia(const model::Sequence& sequence, std::string name);

// --- Copy / paste (D18) ------------------------------------------------------------------------

// Clips copied from a sequence. Starts are relative to the earliest copied clip (0); trackIndex
// is relative to the lowest copied track of the same kind (0).
struct ClipboardItem {
    model::TrackKind kind = model::TrackKind::Video;
    std::size_t trackIndex = 0;
    model::Clip clip;

    friend bool operator==(const ClipboardItem&, const ClipboardItem&) = default;
};
struct Clipboard {
    std::vector<ClipboardItem> items;
    Rational duration; // end of the last copied clip

    [[nodiscard]] bool empty() const noexcept { return items.empty(); }
};

// Copies the clips (and their linked partners) of a sequence.
[[nodiscard]] Clipboard copyClips(const model::Project& project, model::SequenceId sequence,
                                  const std::vector<model::ClipId>& clips);

// Pastes at `at`. Copied clips go to the target tracks (the lowest copied video track to
// `targets.video`, audio likewise; a null target keeps the copied track numbers). New ids; linked
// groups stay linked to each other. Overwrite (Ctrl+V) or insert (Ctrl+Shift+V: every unlocked
// track makes room first).
[[nodiscard]] std::unique_ptr<Command> makePaste(const model::Project& project, model::SequenceId sequence,
                                                 const Clipboard& clipboard, const Rational& at,
                                                 const EditTargets& targets, bool insert);

} // namespace loki::commands
