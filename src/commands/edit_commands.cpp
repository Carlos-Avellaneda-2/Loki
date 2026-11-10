#include "commands/edit_commands.h"

#include "commands/project_commands.h"
#include "model/transitions.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_set>

namespace loki::commands {

using namespace loki::model;

namespace {

bool overlaps(const Clip& a, const Clip& b) {
    return a.start < b.end() && b.start < a.end();
}

const Track* findTrackIn(const Sequence& seq, TrackId id) {
    for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
        for (const auto& t : *list) {
            if (t.id == id) {
                return &t;
            }
        }
    }
    return nullptr;
}

std::unordered_set<ClipId> idsOf(const std::vector<PlacedClip>& clips) {
    std::unordered_set<ClipId> ids;
    for (const auto& c : clips) {
        ids.insert(c.clip.id);
    }
    return ids;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// ReplaceClipsCommand

ReplaceClipsCommand::ReplaceClipsCommand(std::string text, std::vector<PlacedClip> before,
                                         std::vector<PlacedClip> after, GestureId gesture)
    : m_text(std::move(text)), m_before(std::move(before)), m_after(std::move(after)), m_gesture(gesture) {}

void ReplaceClipsCommand::apply(Project& project, const std::vector<PlacedClip>& remove,
                                const std::vector<PlacedClip>& insert) {
    auto mutator = mutate(project);
    std::size_t removed = 0;
    std::size_t inserted = 0;
    try {
        for (; removed < remove.size(); ++removed) {
            mutator.removeClip(remove[removed].clip.id);
        }
        for (; inserted < insert.size(); ++inserted) {
            mutator.insertClip(insert[inserted].track, insert[inserted].clip);
        }
    } catch (...) {
        while (inserted > 0) {
            --inserted;
            mutator.removeClip(insert[inserted].clip.id);
        }
        while (removed > 0) {
            --removed;
            mutator.insertClip(remove[removed].track, remove[removed].clip);
        }
        throw;
    }
}

void ReplaceClipsCommand::redo(Project& project) {
    apply(project, m_before, m_after);
    // Transitions whose clips were removed or pulled apart are deleted with the edit (D19).
    m_dropped.clear();
    std::set<TrackId> tracks;
    for (const auto* list : {&m_before, &m_after}) {
        for (const auto& c : *list) {
            tracks.insert(c.track);
        }
    }
    auto mutator = mutate(project);
    for (const auto trackId : tracks) {
        const auto ref = project.findTrack(trackId);
        if (ref.track == nullptr) {
            continue;
        }
        std::vector<TransitionId> dormant;
        for (const auto& t : ref.track->transitions) {
            if (!isTransitionLive(*ref.track, t)) {
                dormant.push_back(t.id);
            }
        }
        for (const auto id : dormant) {
            m_dropped.push_back({trackId, mutator.removeTransition(id)});
        }
    }
}

void ReplaceClipsCommand::undo(Project& project) {
    apply(project, m_after, m_before);
    auto mutator = mutate(project);
    for (auto it = m_dropped.rbegin(); it != m_dropped.rend(); ++it) {
        mutator.addTransition(it->first, it->second);
    }
    m_dropped.clear();
}

bool ReplaceClipsCommand::mergeWith(const Command& next) {
    const auto* other = dynamic_cast<const ReplaceClipsCommand*>(&next);
    if (other == nullptr || m_gesture == 0 || other->m_gesture != m_gesture) {
        return false;
    }
    // Net effect of (this, then other): see the class comment of the merge in the docs.
    const auto myAfterIds = idsOf(m_after);
    const auto otherBeforeIds = idsOf(other->m_before);
    for (const auto& c : other->m_before) {
        if (!myAfterIds.contains(c.clip.id)) {
            m_before.push_back(c);
        }
    }
    std::vector<PlacedClip> after;
    for (const auto& c : m_after) {
        if (!otherBeforeIds.contains(c.clip.id)) {
            after.push_back(c);
        }
    }
    after.insert(after.end(), other->m_after.begin(), other->m_after.end());
    m_after = std::move(after);
    m_dropped.insert(m_dropped.end(), other->m_dropped.begin(), other->m_dropped.end());
    return true;
}

// ---------------------------------------------------------------------------------------------
// EditPlan

EditPlan::EditPlan(const Project& project, SequenceId sequence)
    : m_project(project), m_sequence(project.findSequence(sequence)) {
    if (m_sequence == nullptr) {
        throw CommandError("Sequence does not exist");
    }
}

void EditPlan::load(TrackId track) {
    if (m_tracks.contains(track)) {
        return;
    }
    const auto* t = findTrackIn(*m_sequence, track);
    if (t == nullptr) {
        throw CommandError("Track is not part of this sequence");
    }
    m_tracks.emplace(track, t->clips);
}

std::vector<Clip>& EditPlan::clips(TrackId track) {
    load(track);
    return m_tracks.at(track);
}

bool EditPlan::isLocked(TrackId track) const {
    const auto* t = findTrackIn(*m_sequence, track);
    return t != nullptr && t->state.locked;
}

LinkId EditPlan::splitLinkFor(LinkId original) {
    if (original.isNull()) {
        return {};
    }
    auto [it, inserted] = m_splitLinks.try_emplace(original, LinkId());
    if (inserted) {
        it->second = LinkId::generate();
    }
    return it->second;
}

void EditPlan::clearRange(TrackId track, const Rational& start, const Rational& end, const std::set<ClipId>& keep) {
    if (!(start < end)) {
        return;
    }
    auto& list = clips(track);
    std::vector<Clip> result;
    result.reserve(list.size() + 1);
    for (auto& c : list) {
        if (keep.contains(c.id) || c.end() <= start || c.start >= end) {
            result.push_back(std::move(c));
            continue;
        }
        const bool coversStart = c.start < start;
        const bool coversEnd = c.end() > end;
        if (coversStart && coversEnd) { // spans the range: keep both sides
            Clip right = c;
            right.id = ClipId::generate();
            right.moveIn(end - c.start);
            right.link = splitLinkFor(c.link);
            c.moveOut(start - c.end());
            result.push_back(std::move(c));
            result.push_back(std::move(right));
        } else if (coversStart) {
            c.moveOut(start - c.end());
            result.push_back(std::move(c));
        } else if (coversEnd) {
            c.moveIn(end - c.start);
            result.push_back(std::move(c));
        }
        // fully inside: dropped
    }
    list = std::move(result);
}

ClipId EditPlan::splitAt(TrackId track, const Rational& at) {
    auto& list = clips(track);
    for (auto& c : list) {
        if (c.start < at && at < c.end()) {
            Clip right = c;
            right.id = ClipId::generate();
            right.moveIn(at - c.start);
            right.link = splitLinkFor(c.link);
            c.moveOut(at - c.end());
            const auto id = right.id;
            list.push_back(std::move(right));
            return id;
        }
    }
    return {};
}

void EditPlan::shiftFrom(TrackId track, const Rational& from, const Rational& delta) {
    for (auto& c : clips(track)) {
        if (c.start >= from) {
            c.start += delta;
            if (c.start.isNegative()) {
                throw CommandError("Clips cannot move before the start of the sequence");
            }
        }
    }
}

void EditPlan::add(TrackId track, Clip clip) {
    clips(track).push_back(std::move(clip));
}

std::optional<PlacedClip> EditPlan::find(ClipId id) {
    const auto ref = m_project.findClip(id);
    if (ref.clip != nullptr && ref.sequence == m_sequence) {
        load(ref.track->id);
    }
    for (auto& [track, list] : m_tracks) {
        for (const auto& c : list) {
            if (c.id == id) {
                return PlacedClip{track, c};
            }
        }
    }
    return std::nullopt;
}

std::optional<PlacedClip> EditPlan::take(ClipId id) {
    auto found = find(id);
    if (found) {
        std::erase_if(m_tracks.at(found->track), [&](const Clip& c) { return c.id == id; });
    }
    return found;
}

std::vector<ClipId> EditPlan::withLinked(ClipId id) const {
    const auto ref = m_project.findClip(id);
    if (ref.clip == nullptr || ref.sequence != m_sequence) {
        throw CommandError("Clip is not part of this sequence");
    }
    if (ref.clip->link.isNull()) {
        return {id};
    }
    std::vector<ClipId> ids;
    for (const auto* list : {&m_sequence->videoTracks, &m_sequence->audioTracks}) {
        for (const auto& t : *list) {
            for (const auto& c : t.clips) {
                if (c.link == ref.clip->link) {
                    ids.push_back(c.id);
                }
            }
        }
    }
    return ids;
}

std::unique_ptr<Command> EditPlan::build(std::string text, GestureId gesture) {
    std::unordered_map<ClipId, PlacedClip> original;
    std::unordered_map<ClipId, PlacedClip> working;
    for (auto& [track, list] : m_tracks) {
        std::sort(list.begin(), list.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        for (std::size_t i = 1; i < list.size(); ++i) {
            if (overlaps(list[i - 1], list[i])) {
                throw CommandError("Clips would overlap");
            }
        }
        for (const auto& c : list) {
            working.emplace(c.id, PlacedClip{track, c});
        }
        for (const auto& c : findTrackIn(*m_sequence, track)->clips) {
            original.emplace(c.id, PlacedClip{track, c});
        }
    }
    std::vector<PlacedClip> before;
    std::vector<PlacedClip> after;
    std::set<TrackId> touched;
    for (const auto& [id, o] : original) {
        const auto it = working.find(id);
        if (it == working.end() || !(it->second == o)) {
            before.push_back(o);
            touched.insert(o.track);
        }
    }
    for (const auto& [id, w] : working) {
        const auto it = original.find(id);
        if (it == original.end() || !(it->second == w)) {
            after.push_back(w);
            touched.insert(w.track);
        }
    }
    if (before.empty() && after.empty()) {
        return nullptr;
    }
    for (const auto track : touched) {
        if (isLocked(track)) {
            throw CommandError("Track " + findTrackIn(*m_sequence, track)->state.name + " is locked");
        }
    }
    // Deterministic order (helps tests and debugging).
    const auto byStart = [](const PlacedClip& a, const PlacedClip& b) { return a.clip.start < b.clip.start; };
    std::sort(before.begin(), before.end(), byStart);
    std::sort(after.begin(), after.end(), byStart);
    return std::make_unique<ReplaceClipsCommand>(std::move(text), std::move(before), std::move(after), gesture);
}

// ---------------------------------------------------------------------------------------------
// High-level edits

namespace {

const Track& requireTrack(const Sequence& seq, TrackId id, TrackKind kind) {
    const auto* t = findTrackIn(seq, id);
    if (t == nullptr) {
        throw CommandError("Target track is not part of this sequence");
    }
    if (t->kind != kind) {
        throw CommandError(kind == TrackKind::Video ? "Video must go on a video track" : "Audio must go on an audio track");
    }
    if (t->state.locked) {
        throw CommandError("Track " + t->state.name + " is locked");
    }
    return *t;
}

// Clips to place for a source range, linked when both video and audio are placed.
std::vector<PlacedClip> clipsFor(const Project& project, const Sequence& seq, const SourceRange& range,
                                 const Rational& at, const EditTargets& targets) {
    const auto* media = project.findMedia(range.media);
    if (media == nullptr) {
        throw CommandError("Media does not exist");
    }
    if (!range.duration.isPositive()) {
        throw CommandError("Nothing to edit: the source range is empty");
    }
    if (at.isNegative()) {
        throw CommandError("Edit point is before the start of the sequence");
    }
    std::vector<PlacedClip> placed;
    const bool video = media->info.hasVideo && !targets.video.isNull();
    const bool audio = media->info.hasAudio && !targets.audio.isNull();
    if (!video && !audio) {
        throw CommandError("No target track for this media");
    }
    const LinkId link = video && audio ? LinkId::generate() : LinkId();
    const auto make = [&](TrackId track) {
        Clip c;
        c.id = ClipId::generate();
        c.media = media->id;
        c.name = media->name;
        c.start = at;
        c.sourceIn = range.sourceIn;
        c.duration = range.duration;
        c.link = link;
        return PlacedClip{track, std::move(c)};
    };
    if (video) {
        requireTrack(seq, targets.video, TrackKind::Video);
        placed.push_back(make(targets.video));
    }
    if (audio) {
        requireTrack(seq, targets.audio, TrackKind::Audio);
        placed.push_back(make(targets.audio));
    }
    return placed;
}

Rational frameDuration(const Sequence& seq) {
    return seq.settings.frameRate.reciprocal();
}

// Whole sequence frames in `t` (source room of retimed clips is not frame aligned).
Rational floorToFrame(const Rational& t, const Sequence& seq) {
    return quantizeToFrame(max(Rational(), t), seq.settings.frameRate, Rounding::Floor);
}

// Media duration limit for a clip (nullopt = unlimited: stills, generated media such as nested
// sequences, or unknown duration).
std::optional<Rational> mediaLimit(const Project& project, const Clip& clip) {
    const auto* media = project.findMedia(clip.media);
    if (media == nullptr || media->info.isStill || media->isGenerated() || !media->info.duration.isPositive()) {
        return std::nullopt;
    }
    return media->info.duration;
}

} // namespace

SourceRange sourceRangeOf(const MediaItem& item) {
    SourceRange r;
    r.media = item.id;
    r.sourceIn = item.markIn.value_or(Rational());
    const Rational full = item.info.isStill ? Rational(5) : item.info.duration;
    const Rational out = item.markOut.value_or(full);
    r.duration = max(Rational(), out - r.sourceIn);
    return r;
}

SourceRange sourceRangeOf(const Project& project, const MediaItem& item) {
    if (!item.isNest()) {
        return sourceRangeOf(item);
    }
    MediaItem live = item;
    const auto* seq = project.findSequence(item.sequence);
    live.info.duration = seq != nullptr && seq->duration().isPositive() ? seq->duration() : Rational(5);
    return sourceRangeOf(live);
}

std::unique_ptr<Command> makeOverwriteEdit(const Project& project, SequenceId sequence, const SourceRange& range,
                                           const Rational& at, const EditTargets& targets) {
    EditPlan plan(project, sequence);
    for (auto& placed : clipsFor(project, plan.sequence(), range, at, targets)) {
        plan.clearRange(placed.track, at, at + range.duration);
        plan.add(placed.track, std::move(placed.clip));
    }
    return plan.build("Overwrite");
}

std::unique_ptr<Command> makeInsertEdit(const Project& project, SequenceId sequence, const SourceRange& range,
                                        const Rational& at, const EditTargets& targets) {
    EditPlan plan(project, sequence);
    auto placed = clipsFor(project, plan.sequence(), range, at, targets);
    for (const auto* list : {&plan.sequence().videoTracks, &plan.sequence().audioTracks}) {
        for (const auto& t : *list) {
            if (!t.state.locked) {
                plan.splitAt(t.id, at);
                plan.shiftFrom(t.id, at, range.duration);
            }
        }
    }
    for (auto& p : placed) {
        plan.add(p.track, std::move(p.clip));
    }
    return plan.build("Insert");
}

std::unique_ptr<Command> makeRazorEdit(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips,
                                       const Rational& at) {
    EditPlan plan(project, sequence);
    std::set<ClipId> ids;
    for (const auto id : clips) {
        for (const auto linked : plan.withLinked(id)) {
            ids.insert(linked);
        }
    }
    for (const auto id : ids) {
        const auto placed = plan.find(id);
        if (placed && placed->clip.start < at && at < placed->clip.end()) {
            if (plan.isLocked(placed->track)) {
                throw CommandError("Track is locked");
            }
            plan.splitAt(placed->track, at);
        }
    }
    return plan.build("Razor");
}

std::unique_ptr<Command> makeRazorAllTracks(const Project& project, SequenceId sequence, const Rational& at) {
    EditPlan plan(project, sequence);
    for (const auto* list : {&plan.sequence().videoTracks, &plan.sequence().audioTracks}) {
        for (const auto& t : *list) {
            if (!t.state.locked) {
                plan.splitAt(t.id, at);
            }
        }
    }
    return plan.build("Razor");
}

std::unique_ptr<Command> makeMoveClips(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips,
                                       const Rational& timeDelta, int trackDelta, TrackKind dragKind,
                                       GestureId gesture) {
    EditPlan plan(project, sequence);
    std::set<ClipId> ids;
    for (const auto id : clips) {
        for (const auto linked : plan.withLinked(id)) {
            ids.insert(linked);
        }
    }
    const auto& seq = plan.sequence();
    std::vector<PlacedClip> moved;
    for (const auto id : ids) {
        auto placed = plan.take(id);
        if (!placed) {
            continue;
        }
        const auto* track = findTrackIn(seq, placed->track);
        TrackId destination = placed->track;
        if (track->kind == dragKind && trackDelta != 0) {
            const auto& list = seq.tracks(dragKind);
            const auto index = static_cast<std::ptrdiff_t>(std::distance(
                list.begin(), std::find_if(list.begin(), list.end(), [&](const Track& t) { return t.id == track->id; })));
            const auto target = index + trackDelta;
            if (target < 0 || target >= static_cast<std::ptrdiff_t>(list.size())) {
                throw CommandError("No track there");
            }
            destination = list[static_cast<std::size_t>(target)].id;
        }
        placed->track = destination;
        placed->clip.start += timeDelta;
        if (placed->clip.start.isNegative()) {
            throw CommandError("Clips cannot move before the start of the sequence");
        }
        moved.push_back(std::move(*placed));
    }
    for (auto& p : moved) {
        plan.clearRange(p.track, p.clip.start, p.clip.end());
    }
    for (auto& p : moved) {
        plan.add(p.track, std::move(p.clip));
    }
    return plan.build("Move", gesture);
}

std::unique_ptr<Command> makeTrimEdge(const Project& project, SequenceId sequence, ClipId clip, ClipEdge edge,
                                      const Rational& delta, bool includeLinked, GestureId gesture) {
    EditPlan plan(project, sequence);
    const auto ids = includeLinked ? plan.withLinked(clip) : std::vector<ClipId>{clip};
    const Rational minDuration = frameDuration(plan.sequence());

    // Common allowed range [lo, hi] for every clip of the group.
    std::optional<Rational> lo;
    std::optional<Rational> hi;
    const auto lower = [&](const Rational& v) { lo = lo ? max(*lo, v) : v; };
    const auto upper = [&](const Rational& v) { hi = hi ? min(*hi, v) : v; };
    for (const auto id : ids) {
        const auto placed = plan.find(id);
        if (!placed) {
            continue;
        }
        const auto& c = placed->clip;
        const auto limit = mediaLimit(project, c);
        const auto& list = plan.clips(placed->track);
        Rational prevEnd;
        std::optional<Rational> nextStart;
        for (const auto& other : list) {
            if (other.id == c.id) {
                continue;
            }
            if (other.end() <= c.start) {
                prevEnd = max(prevEnd, other.end());
            } else if (other.start >= c.end()) {
                nextStart = nextStart ? min(*nextStart, other.start) : other.start;
            }
        }
        if (edge == ClipEdge::In) {
            if (const auto room = c.roomBefore(limit)) {
                lower(-floorToFrame(*room, plan.sequence())); // source starts at 0
            }
            lower(prevEnd - c.start); // no overwrite of the previous clip
            upper(c.duration - minDuration);
        } else {
            if (const auto room = c.roomAfter(limit)) {
                upper(floorToFrame(*room, plan.sequence()));
            }
            if (nextStart) {
                upper(*nextStart - c.end());
            }
            lower(minDuration - c.duration);
        }
    }
    // A missing bound is unlimited (stills, generated media, no neighbour).
    if (lo && hi && *hi < *lo) {
        return nullptr;
    }
    Rational d = delta;
    if (lo) {
        d = max(d, *lo);
    }
    if (hi) {
        d = min(d, *hi);
    }
    if (d.isZero()) {
        return nullptr;
    }
    for (const auto id : ids) {
        auto placed = plan.take(id);
        if (!placed) {
            continue;
        }
        auto& c = placed->clip;
        if (edge == ClipEdge::In) {
            c.moveIn(d);
        } else {
            c.moveOut(d);
        }
        plan.add(placed->track, std::move(c));
    }
    return plan.build("Trim", gesture);
}

std::unique_ptr<Command> makeDeleteClips(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips,
                                         bool includeLinked) {
    EditPlan plan(project, sequence);
    std::set<ClipId> ids;
    for (const auto id : clips) {
        if (includeLinked) {
            for (const auto linked : plan.withLinked(id)) {
                ids.insert(linked);
            }
        } else {
            ids.insert(id);
        }
    }
    for (const auto id : ids) {
        plan.take(id);
    }
    return plan.build("Delete");
}

// ---------------------------------------------------------------------------------------------
// Phase 7

namespace {

struct Range {
    std::optional<Rational> lo;
    std::optional<Rational> hi;
    void lower(const Rational& v) { lo = lo ? max(*lo, v) : v; }
    void upper(const Rational& v) { hi = hi ? min(*hi, v) : v; }
    [[nodiscard]] std::optional<Rational> clamp(const Rational& d) const {
        if (lo && hi && *hi < *lo) {
            return std::nullopt;
        }
        Rational r = d;
        if (lo) {
            r = max(r, *lo);
        }
        if (hi) {
            r = min(r, *hi);
        }
        return r;
    }
};

std::vector<ClipId> groupOf(const EditPlan& plan, ClipId clip, bool includeLinked) {
    return includeLinked ? plan.withLinked(clip) : std::vector<ClipId>{clip};
}

const Clip* neighbourAt(const std::vector<Clip>& clips, const Rational& time, bool endingThere, ClipId except) {
    for (const auto& c : clips) {
        if (c.id != except && (endingThere ? c.end() == time : c.start == time)) {
            return &c;
        }
    }
    return nullptr;
}

void forUnlockedTracks(const Sequence& seq, const std::function<void(const Track&)>& fn) {
    for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
        for (const auto& t : *list) {
            if (!t.state.locked) {
                fn(t);
            }
        }
    }
}

// Gap limits around clip `c` on its track: end of the previous clip and start of the next one.
std::pair<Rational, std::optional<Rational>> gapAround(const std::vector<Clip>& list, const Clip& c) {
    Rational gapStart;
    std::optional<Rational> gapEnd;
    for (const auto& other : list) {
        if (other.id == c.id) {
            continue;
        }
        if (other.end() <= c.start) {
            gapStart = max(gapStart, other.end());
        } else if (other.start >= c.end()) {
            gapEnd = gapEnd ? min(*gapEnd, other.start) : other.start;
        }
    }
    return {gapStart, gapEnd};
}

} // namespace

std::unique_ptr<Command> makeRippleTrim(const Project& project, SequenceId sequence, ClipId clip, ClipEdge edge,
                                        const Rational& delta, bool includeLinked) {
    EditPlan plan(project, sequence);
    const auto ids = groupOf(plan, clip, includeLinked);
    const Rational minDuration = frameDuration(plan.sequence());
    Range range;
    Rational oldEnd;
    for (const auto id : ids) {
        const auto placed = plan.find(id);
        if (!placed) {
            continue;
        }
        const auto& c = placed->clip;
        oldEnd = c.end();
        const auto limit = mediaLimit(project, c);
        if (edge == ClipEdge::In) {
            if (const auto room = c.roomBefore(limit)) {
                range.lower(-floorToFrame(*room, plan.sequence()));
            }
            range.upper(c.duration - minDuration);
        } else {
            if (const auto room = c.roomAfter(limit)) {
                range.upper(floorToFrame(*room, plan.sequence()));
            }
            range.lower(minDuration - c.duration);
        }
    }
    const auto d = range.clamp(delta);
    if (!d || d->isZero()) {
        return nullptr;
    }
    // Everything after the edit point (on every unlocked track) follows the change in duration.
    const Rational shift = edge == ClipEdge::In ? -*d : *d;
    std::vector<PlacedClip> trimmed;
    for (const auto id : ids) {
        if (auto placed = plan.take(id)) {
            const Rational start = placed->clip.start;
            if (edge == ClipEdge::In) {
                placed->clip.moveIn(*d);
                placed->clip.start = start; // ripple: the clip keeps its start
            } else {
                placed->clip.moveOut(*d);
            }
            trimmed.push_back(std::move(*placed));
        }
    }
    forUnlockedTracks(plan.sequence(), [&](const Track& t) { plan.shiftFrom(t.id, oldEnd, shift); });
    for (auto& p : trimmed) {
        plan.add(p.track, std::move(p.clip));
    }
    return plan.build("Ripple Trim");
}

std::unique_ptr<Command> makeRollEdit(const Project& project, SequenceId sequence, ClipId clip, ClipEdge edge,
                                      const Rational& delta, bool includeLinked) {
    EditPlan plan(project, sequence);
    const Rational minDuration = frameDuration(plan.sequence());
    struct Pair {
        ClipId left;
        ClipId right;
    };
    std::vector<Pair> pairs;
    Range range;
    for (const auto id : groupOf(plan, clip, includeLinked)) {
        const auto placed = plan.find(id);
        if (!placed) {
            continue;
        }
        const auto c = placed->clip;
        const auto& list = plan.clips(placed->track);
        const Clip* left = edge == ClipEdge::Out ? &c : neighbourAt(list, c.start, true, c.id);
        const Clip* right = edge == ClipEdge::In ? &c : neighbourAt(list, c.end(), false, c.id);
        Pair pair;
        if (left != nullptr) {
            pair.left = left->id;
            if (const auto room = left->roomAfter(mediaLimit(project, *left))) {
                range.upper(floorToFrame(*room, plan.sequence()));
            }
            range.lower(minDuration - left->duration);
        }
        if (right != nullptr) {
            pair.right = right->id;
            if (const auto room = right->roomBefore(mediaLimit(project, *right))) {
                range.lower(-floorToFrame(*room, plan.sequence()));
            }
            range.upper(right->duration - minDuration);
        }
        if (left == nullptr || right == nullptr) {
            // No neighbour on that side: the gap absorbs the roll like a normal trim.
            const auto [gapStart, gapEnd] = gapAround(list, c);
            if (edge == ClipEdge::Out && gapEnd) {
                range.upper(*gapEnd - c.end());
            }
            if (edge == ClipEdge::In) {
                range.lower(gapStart - c.start);
            }
        }
        pairs.push_back(pair);
    }
    const auto d = range.clamp(delta);
    if (!d || d->isZero()) {
        return nullptr;
    }
    for (const auto& pair : pairs) {
        if (pair.left) {
            auto l = plan.take(pair.left);
            l->clip.moveOut(*d);
            plan.add(l->track, std::move(l->clip));
        }
        if (pair.right) {
            auto r = plan.take(pair.right);
            r->clip.moveIn(*d);
            plan.add(r->track, std::move(r->clip));
        }
    }
    return plan.build("Rolling Edit");
}

std::unique_ptr<Command> makeSlipEdit(const Project& project, SequenceId sequence, ClipId clip, const Rational& delta,
                                      bool includeLinked) {
    EditPlan plan(project, sequence);
    const auto ids = groupOf(plan, clip, includeLinked);
    Range range;
    for (const auto id : ids) {
        if (const auto placed = plan.find(id)) {
            const auto& c = placed->clip;
            range.lower(-floorToFrame(c.sourceIn / c.slipFactor(), plan.sequence()));
            if (const auto limit = mediaLimit(project, c)) {
                range.upper(floorToFrame(max(Rational(), *limit - c.sourceOut()) / c.slipFactor(), plan.sequence()));
            }
        }
    }
    const auto d = range.clamp(delta);
    if (!d || d->isZero()) {
        return nullptr;
    }
    for (const auto id : ids) {
        if (auto placed = plan.take(id)) {
            placed->clip.sourceIn += *d * placed->clip.slipFactor();
            plan.add(placed->track, std::move(placed->clip));
        }
    }
    return plan.build("Slip");
}

std::unique_ptr<Command> makeSlideEdit(const Project& project, SequenceId sequence, ClipId clip, const Rational& delta,
                                       bool includeLinked) {
    EditPlan plan(project, sequence);
    const auto ids = groupOf(plan, clip, includeLinked);
    const Rational minDuration = frameDuration(plan.sequence());
    struct Neighbours {
        ClipId self;
        ClipId left;
        ClipId right;
    };
    std::vector<Neighbours> all;
    Range range;
    for (const auto id : ids) {
        const auto placed = plan.find(id);
        if (!placed) {
            continue;
        }
        const auto c = placed->clip;
        const auto& list = plan.clips(placed->track);
        Neighbours n{c.id, {}, {}};
        const auto [gapStart, gapEnd] = gapAround(list, c);
        if (const auto* left = neighbourAt(list, c.start, true, c.id)) {
            n.left = left->id;
            range.lower(minDuration - left->duration);
            if (const auto room = left->roomAfter(mediaLimit(project, *left))) {
                range.upper(floorToFrame(*room, plan.sequence()));
            }
        } else {
            range.lower(gapStart - c.start);
        }
        if (const auto* right = neighbourAt(list, c.end(), false, c.id)) {
            n.right = right->id;
            if (const auto room = right->roomBefore(mediaLimit(project, *right))) {
                range.lower(-floorToFrame(*room, plan.sequence()));
            }
            range.upper(right->duration - minDuration);
        } else if (gapEnd) {
            range.upper(*gapEnd - c.end());
        }
        all.push_back(n);
    }
    const auto d = range.clamp(delta);
    if (!d || d->isZero()) {
        return nullptr;
    }
    for (const auto& n : all) {
        if (n.left) {
            auto l = plan.take(n.left);
            l->clip.moveOut(*d);
            plan.add(l->track, std::move(l->clip));
        }
        if (n.right) {
            auto r = plan.take(n.right);
            r->clip.moveIn(*d);
            plan.add(r->track, std::move(r->clip));
        }
        auto self = plan.take(n.self);
        self->clip.start += *d;
        plan.add(self->track, std::move(self->clip));
    }
    return plan.build("Slide");
}

std::unique_ptr<Command> makeRippleDelete(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips) {
    EditPlan plan(project, sequence);
    std::set<ClipId> ids;
    for (const auto id : clips) {
        for (const auto linked : plan.withLinked(id)) {
            ids.insert(linked);
        }
    }
    std::optional<Rational> start;
    std::optional<Rational> end;
    for (const auto id : ids) {
        if (auto placed = plan.take(id)) {
            start = start ? min(*start, placed->clip.start) : placed->clip.start;
            end = end ? max(*end, placed->clip.end()) : placed->clip.end();
        }
    }
    if (!start) {
        return nullptr;
    }
    const Rational gap = *end - *start;
    forUnlockedTracks(plan.sequence(), [&](const Track& t) {
        const auto& list = plan.clips(t.id);
        const bool free = std::none_of(list.begin(), list.end(),
                                       [&](const Clip& c) { return c.start < *end && *start < c.end(); });
        if (free) {
            plan.shiftFrom(t.id, *end, -gap);
        }
    });
    return plan.build("Ripple Delete");
}

std::unique_ptr<Command> makeLiftRange(const Project& project, SequenceId sequence, const Rational& in,
                                       const Rational& out) {
    if (!(in < out)) {
        throw CommandError("Set In and Out points first");
    }
    EditPlan plan(project, sequence);
    forUnlockedTracks(plan.sequence(), [&](const Track& t) { plan.clearRange(t.id, in, out); });
    return plan.build("Lift");
}

std::unique_ptr<Command> makeExtractRange(const Project& project, SequenceId sequence, const Rational& in,
                                          const Rational& out) {
    if (!(in < out)) {
        throw CommandError("Set In and Out points first");
    }
    EditPlan plan(project, sequence);
    forUnlockedTracks(plan.sequence(), [&](const Track& t) {
        plan.clearRange(t.id, in, out);
        plan.shiftFrom(t.id, out, in - out);
    });
    return plan.build("Extract");
}

std::unique_ptr<Command> makeLinkClips(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips) {
    if (clips.size() < 2) {
        throw CommandError("Select at least two clips to link");
    }
    EditPlan plan(project, sequence);
    const LinkId link = LinkId::generate();
    for (const auto id : clips) {
        if (auto placed = plan.take(id)) {
            placed->clip.link = link;
            plan.add(placed->track, std::move(placed->clip));
        }
    }
    return plan.build("Link");
}

std::unique_ptr<Command> makeUnlinkClips(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips) {
    EditPlan plan(project, sequence);
    std::set<ClipId> ids;
    for (const auto id : clips) {
        for (const auto linked : plan.withLinked(id)) {
            ids.insert(linked);
        }
    }
    for (const auto id : ids) {
        if (auto placed = plan.take(id)) {
            placed->clip.link = LinkId();
            plan.add(placed->track, std::move(placed->clip));
        }
    }
    return plan.build("Unlink");
}

// --- Titles -------------------------------------------------------------------------------------

std::unique_ptr<Command> makeNewTitle(const Project& project, SequenceId sequence, TrackId track, const Rational& at,
                                      const Rational& duration, std::string name, MediaId* titleMedia) {
    return makeNewGeneratedClip(project, sequence, track, at, duration, std::move(name), kTitleGenerator, titleMedia);
}

std::unique_ptr<Command> makeNewGeneratedClip(const Project& project, SequenceId sequence, TrackId track,
                                              const Rational& at, const Rational& duration, std::string name,
                                              std::string_view generator, MediaId* newMedia) {
    EditPlan plan(project, sequence);
    const auto& seq = plan.sequence();
    const auto ref = project.findTrack(track);
    if (ref.track == nullptr || ref.sequence != &seq || ref.track->kind != TrackKind::Video) {
        throw CommandError("This item goes on a video track of the sequence");
    }
    if (plan.isLocked(track)) {
        throw CommandError("Track is locked");
    }
    if (at.isNegative() || !duration.isPositive()) {
        throw CommandError("Invalid placement");
    }
    MediaItem media;
    media.id = MediaId::generate();
    media.name = std::move(name);
    media.generator = std::string(generator);
    media.info.hasVideo = true;
    media.info.isStill = true;
    media.info.width = seq.settings.width;
    media.info.height = seq.settings.height;
    media.info.frameRate = seq.settings.frameRate;
    Clip clip;
    clip.id = ClipId::generate();
    clip.media = media.id;
    clip.name = media.name;
    clip.start = at;
    clip.duration = duration;
    plan.clearRange(track, at, at + duration);
    plan.add(track, std::move(clip));
    if (newMedia != nullptr) {
        *newMedia = media.id;
    }
    const std::string text = "New " + media.name;
    std::vector<std::unique_ptr<Command>> steps;
    steps.push_back(std::make_unique<AddMediaCommand>(std::move(media)));
    steps.push_back(plan.build("Place " + text.substr(4)));
    return std::make_unique<MacroCommand>(text, std::move(steps));
}

// --- Speed ----------------------------------------------------------------------------------------

std::unique_ptr<Command> makeSetClipSpeed(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips,
                                          const Rational& speed, bool reverse, bool ripple) {
    if (!speed.isPositive() || speed > Rational(kMaxClipSpeed)) {
        throw CommandError("Speed must be between 0 and 10000 %");
    }
    EditPlan plan(project, sequence);
    const auto& seq = plan.sequence();
    const Rational frame = frameDuration(seq);
    std::set<ClipId> ids;
    for (const auto id : clips) {
        for (const auto linked : plan.withLinked(id)) {
            ids.insert(linked);
        }
    }
    struct Change {
        TrackId track;
        Clip clip;
        Rational oldEnd;
    };
    std::vector<Change> changes;
    for (const auto id : ids) {
        auto placed = plan.find(id);
        if (!placed) {
            continue;
        }
        if (plan.isLocked(placed->track)) {
            throw CommandError("Track is locked");
        }
        Clip c = placed->clip;
        const Rational oldEnd = c.end();
        Rational duration = c.speed.isZero()
                                ? c.duration
                                : max(frame, quantizeToFrame(c.sourceSpan() / speed, seq.settings.frameRate, Rounding::Nearest));
        if (const auto limit = mediaLimit(project, c)) {
            duration = min(duration, max(frame, floorToFrame((*limit - c.sourceIn) / speed, seq)));
        }
        if (!ripple) {
            const auto [gapStart, gapEnd] = gapAround(plan.clips(placed->track), c);
            (void)gapStart;
            if (gapEnd) {
                duration = min(duration, *gapEnd - c.start);
            }
        }
        c.speed = speed;
        c.reverse = reverse;
        c.duration = duration;
        changes.push_back({placed->track, std::move(c), oldEnd});
    }
    if (changes.empty()) {
        return nullptr;
    }
    for (const auto& change : changes) {
        plan.take(change.clip.id);
    }
    // Latest first, so each shift sees the original positions after its edit point.
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return b.oldEnd < a.oldEnd; });
    if (ripple) {
        // On each track everything after a changed clip follows its new end (accumulated): the
        // other clips through shiftFrom, the changed ones by the deltas of those before them.
        std::vector<Rational> starts;
        for (const auto& change : changes) {
            starts.push_back(change.clip.start);
        }
        for (const auto& change : changes) {
            const Rational delta = change.clip.end() - change.oldEnd;
            plan.shiftFrom(change.track, change.oldEnd, delta);
            for (std::size_t i = 0; i < changes.size(); ++i) {
                if (changes[i].track == change.track && change.oldEnd <= starts[i]) {
                    changes[i].clip.start += delta;
                }
            }
        }
    }
    for (auto& change : changes) {
        plan.add(change.track, std::move(change.clip));
    }
    return plan.build("Speed/Duration");
}

std::unique_ptr<Command> makeRateStretch(const Project& project, SequenceId sequence, ClipId clip, ClipEdge edge,
                                         const Rational& delta, bool includeLinked, GestureId gesture) {
    EditPlan plan(project, sequence);
    const auto& seq = plan.sequence();
    const Rational frame = frameDuration(seq);
    const auto ids = groupOf(plan, clip, includeLinked);
    Range range;
    for (const auto id : ids) {
        const auto placed = plan.find(id);
        if (!placed) {
            continue;
        }
        const auto& c = placed->clip;
        // The shortest duration keeps the speed at or below the maximum.
        const Rational shortest = max(frame, quantizeToFrame(c.sourceSpan() / Rational(kMaxClipSpeed),
                                                             seq.settings.frameRate, Rounding::Ceil));
        const auto [gapStart, gapEnd] = gapAround(plan.clips(placed->track), c);
        if (edge == ClipEdge::In) {
            range.lower(gapStart - c.start);
            range.upper(c.duration - shortest);
        } else {
            if (gapEnd) {
                range.upper(*gapEnd - c.end());
            }
            range.lower(shortest - c.duration);
        }
    }
    const auto d = range.clamp(delta);
    if (!d || d->isZero()) {
        return nullptr;
    }
    for (const auto id : ids) {
        auto placed = plan.take(id);
        if (!placed) {
            continue;
        }
        auto& c = placed->clip;
        const Rational span = c.sourceSpan();
        if (edge == ClipEdge::In) {
            c.start += *d;
            c.duration -= *d;
        } else {
            c.duration += *d;
        }
        if (!c.speed.isZero()) {
            c.speed = span / c.duration;
        }
        plan.add(placed->track, std::move(c));
    }
    return plan.build("Rate Stretch", gesture);
}

std::unique_ptr<Command> makeAddFrameHold(const Project& project, SequenceId sequence, ClipId clip, const Rational& at) {
    EditPlan plan(project, sequence);
    auto placed = plan.find(clip);
    if (!placed) {
        throw CommandError("Clip is not part of this sequence");
    }
    const auto ref = project.findTrack(placed->track);
    if (ref.track == nullptr || ref.track->kind != TrackKind::Video) {
        throw CommandError("Frame Hold needs a video clip");
    }
    if (plan.isLocked(placed->track)) {
        throw CommandError("Track is locked");
    }
    const Clip c = placed->clip;
    if (at < c.start || !(at < c.end())) {
        throw CommandError("The playhead must be over the clip");
    }
    plan.take(clip);
    Clip hold = c;
    if (c.start < at) {
        Clip left = c;
        left.moveOut(at - c.end());
        plan.add(placed->track, std::move(left));
        hold.id = ClipId::generate();
        hold.link = LinkId();
        hold.moveIn(at - c.start);
    }
    hold.sourceIn = c.toSourceTime(at);
    hold.speed = Rational();
    hold.reverse = false;
    hold.fadeIn = {};
    plan.add(placed->track, std::move(hold));
    return plan.build("Add Frame Hold");
}

// --- Nest -----------------------------------------------------------------------------------------

MediaItem makeNestMedia(const Sequence& sequence, std::string name) {
    MediaItem media;
    media.id = MediaId::generate();
    media.name = std::move(name);
    media.generator = std::string(kNestGenerator);
    media.sequence = sequence.id;
    media.info.hasVideo = true;
    media.info.hasAudio = true;
    media.info.width = sequence.settings.width;
    media.info.height = sequence.settings.height;
    media.info.frameRate = sequence.settings.frameRate;
    media.info.duration = sequence.duration();
    media.info.sampleRate = sequence.settings.sampleRate;
    media.info.audioChannels = 2;
    return media;
}

std::unique_ptr<Command> makeNest(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips,
                                  std::string name, MediaId* nestMedia, SequenceId* nestedSequence) {
    EditPlan plan(project, sequence);
    const auto& seq = plan.sequence();
    std::set<ClipId> ids;
    for (const auto id : clips) {
        for (const auto linked : plan.withLinked(id)) {
            ids.insert(linked);
        }
    }
    if (ids.empty()) {
        throw CommandError("Select the clips to nest");
    }
    // Range and lowest track used per kind.
    std::optional<Rational> first;
    Rational last;
    std::optional<std::size_t> lowest[2];
    std::size_t highest[2] = {0, 0};
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto k = static_cast<std::size_t>(kind);
        const auto& tracks = seq.tracks(kind);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            for (const auto& c : tracks[i].clips) {
                if (!ids.contains(c.id)) {
                    continue;
                }
                if (tracks[i].state.locked) {
                    throw CommandError("Track is locked");
                }
                first = first ? min(*first, c.start) : c.start;
                last = max(last, c.end());
                lowest[k] = lowest[k] ? std::min(*lowest[k], i) : i;
                highest[k] = std::max(highest[k], i);
            }
        }
    }
    if (!first) {
        return nullptr;
    }
    // The nested sequence: same settings, tracks up to the highest used, clips shifted to 0.
    const auto videoCount = static_cast<int>(lowest[0] ? highest[0] + 1 : 1);
    const auto audioCount = static_cast<int>(lowest[1] ? highest[1] + 1 : 1);
    Sequence nested = makeDefaultSequence(name, seq.settings, std::max(videoCount, 1), std::max(audioCount, 1));
    std::unordered_map<ClipId, ClipId> newIds;
    std::unordered_map<LinkId, LinkId> newLinks;
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = seq.tracks(kind);
        auto& target = kind == TrackKind::Video ? nested.videoTracks : nested.audioTracks;
        for (std::size_t i = 0; i < tracks.size() && i < target.size(); ++i) {
            for (const auto& c : tracks[i].clips) {
                if (!ids.contains(c.id)) {
                    continue;
                }
                Clip copy = c;
                copy.id = ClipId::generate();
                copy.start = c.start - *first;
                if (!c.link.isNull()) {
                    auto [it, inserted] = newLinks.try_emplace(c.link, LinkId());
                    if (inserted) {
                        it->second = LinkId::generate();
                    }
                    copy.link = it->second;
                }
                newIds.emplace(c.id, copy.id);
                target[i].clips.push_back(std::move(copy));
            }
            // Transitions whose clips all move along.
            for (const auto& t : tracks[i].transitions) {
                const bool fromIn = t.from.isNull() || newIds.contains(t.from);
                const bool toIn = t.to.isNull() || newIds.contains(t.to);
                if (fromIn && toIn && (!t.from.isNull() || !t.to.isNull())) {
                    Transition copy = t;
                    copy.id = TransitionId::generate();
                    copy.from = t.from.isNull() ? ClipId() : newIds.at(t.from);
                    copy.to = t.to.isNull() ? ClipId() : newIds.at(t.to);
                    target[i].transitions.push_back(std::move(copy));
                }
            }
        }
    }
    MediaItem media = makeNestMedia(nested, name);
    media.info.hasVideo = lowest[0].has_value();
    media.info.hasAudio = lowest[1].has_value();
    // The parent: remove the clips, place the nested clip(s).
    for (const auto id : ids) {
        plan.take(id);
    }
    const LinkId link = lowest[0] && lowest[1] ? LinkId::generate() : LinkId();
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto k = static_cast<std::size_t>(kind);
        if (!lowest[k]) {
            continue;
        }
        const auto track = seq.tracks(kind)[*lowest[k]].id;
        plan.clearRange(track, *first, last);
        Clip clip;
        clip.id = ClipId::generate();
        clip.media = media.id;
        clip.name = media.name;
        clip.start = *first;
        clip.duration = last - *first;
        clip.link = link;
        plan.add(track, std::move(clip));
    }
    if (nestMedia != nullptr) {
        *nestMedia = media.id;
    }
    if (nestedSequence != nullptr) {
        *nestedSequence = nested.id;
    }
    std::vector<std::unique_ptr<Command>> steps;
    steps.push_back(std::make_unique<AddSequenceCommand>(std::move(nested)));
    steps.push_back(std::make_unique<AddMediaCommand>(std::move(media)));
    steps.push_back(plan.build("Replace with Nested Sequence"));
    return std::make_unique<MacroCommand>("Nest", std::move(steps));
}

// --- Copy / paste --------------------------------------------------------------------------------

Clipboard copyClips(const Project& project, SequenceId sequence, const std::vector<ClipId>& clips) {
    EditPlan plan(project, sequence);
    std::set<ClipId> ids;
    for (const auto id : clips) {
        for (const auto linked : plan.withLinked(id)) {
            ids.insert(linked);
        }
    }
    Clipboard board;
    const auto& seq = plan.sequence();
    std::optional<Rational> first;
    std::size_t lowest[2] = {SIZE_MAX, SIZE_MAX};
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = seq.tracks(kind);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            for (const auto& c : tracks[i].clips) {
                if (ids.contains(c.id)) {
                    board.items.push_back({kind, i, c});
                    first = first ? min(*first, c.start) : c.start;
                    auto& low = lowest[kind == TrackKind::Video ? 0 : 1];
                    low = std::min(low, i);
                }
            }
        }
    }
    for (auto& item : board.items) {
        item.trackIndex -= lowest[item.kind == TrackKind::Video ? 0 : 1];
        item.clip.start -= *first;
        board.duration = max(board.duration, item.clip.end());
    }
    return board;
}

std::unique_ptr<Command> makePaste(const Project& project, SequenceId sequence, const Clipboard& clipboard,
                                   const Rational& at, const EditTargets& targets, bool insert) {
    if (clipboard.empty()) {
        throw CommandError("Nothing to paste");
    }
    if (at.isNegative()) {
        throw CommandError("Cannot paste before the sequence start");
    }
    EditPlan plan(project, sequence);
    const auto& seq = plan.sequence();
    const auto baseIndex = [&](TrackKind kind) -> std::size_t {
        const auto target = kind == TrackKind::Video ? targets.video : targets.audio;
        const auto& tracks = seq.tracks(kind);
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].id == target) {
                return i;
            }
        }
        return 0; // no target: paste from V1 / A1 up
    };
    const std::size_t base[2] = {baseIndex(TrackKind::Video), baseIndex(TrackKind::Audio)};
    if (insert) {
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
            for (const auto& t : *list) {
                if (!t.state.locked) {
                    plan.splitAt(t.id, at);
                    plan.shiftFrom(t.id, at, clipboard.duration);
                }
            }
        }
    }
    std::unordered_map<LinkId, LinkId> links;
    for (const auto& item : clipboard.items) {
        const auto& tracks = seq.tracks(item.kind);
        const auto index = base[item.kind == TrackKind::Video ? 0 : 1] + item.trackIndex;
        if (index >= tracks.size()) {
            throw CommandError(item.kind == TrackKind::Video ? "Not enough video tracks to paste"
                                                             : "Not enough audio tracks to paste");
        }
        const auto track = tracks[index].id;
        if (plan.isLocked(track)) {
            throw CommandError("Track is locked");
        }
        Clip clip = item.clip;
        clip.id = ClipId::generate();
        clip.start = at + item.clip.start;
        if (!clip.link.isNull()) {
            auto [it, inserted] = links.try_emplace(clip.link, LinkId());
            if (inserted) {
                it->second = LinkId::generate();
            }
            clip.link = it->second;
        }
        for (auto& effect : clip.effects) {
            effect.id = EffectId::generate();
        }
        if (!insert) {
            plan.clearRange(track, clip.start, clip.end());
        }
        plan.add(track, std::move(clip));
    }
    return plan.build(insert ? "Paste Insert" : "Paste");
}

} // namespace loki::commands
