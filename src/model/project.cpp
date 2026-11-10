#include "model/project.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_set>

namespace loki::model {

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw ModelError(message);
}

template <typename Container, typename IdT>
auto findById(Container& items, IdT id) noexcept {
    return std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.id == id; });
}

bool overlaps(const Clip& a, const Clip& b) {
    return a.start < b.end() && b.start < a.end();
}

// Shape checks that do not depend on the rest of the project.
std::string clipShapeProblem(const Clip& clip) {
    if (clip.id.isNull()) {
        return "clip has a null id";
    }
    if (clip.media.isNull()) {
        return "clip " + clip.id.toString() + " has no media";
    }
    if (!clip.duration.isPositive()) {
        return "clip " + clip.id.toString() + " has non-positive duration";
    }
    if (clip.sourceIn.isNegative()) {
        return "clip " + clip.id.toString() + " has negative source in";
    }
    if (clip.fadeIn.isNegative() || clip.fadeOut.isNegative()) {
        return "clip " + clip.id.toString() + " has a negative fade";
    }
    if (clip.start.isNegative()) {
        return "clip " + clip.id.toString() + " starts before zero";
    }
    if (clip.speed.isNegative() || clip.speed > Rational(kMaxClipSpeed)) {
        return "clip " + clip.id.toString() + " has an invalid speed";
    }
    if (clip.angle < 0 || clip.angle > 98) {
        return "clip " + clip.id.toString() + " has an invalid multicam angle";
    }
    std::unordered_set<EffectId> effectIds;
    for (const auto& effect : clip.effects) {
        if (effect.id.isNull() || !effectIds.insert(effect.id).second) {
            return "clip " + clip.id.toString() + " has a null or duplicate effect id";
        }
        std::unordered_set<std::string> keys;
        for (const auto& p : effect.parameters) {
            if (!keys.insert(p.key).second) {
                return "effect " + effect.id.toString() + " has duplicate parameter '" + p.key + "'";
            }
            if (!std::isfinite(p.value) || !keyframesValid(p.keyframes)) {
                return "parameter '" + p.key + "' has invalid value or keyframes";
            }
        }
        std::unordered_set<MaskId> maskIds;
        for (const auto& mask : effect.masks) {
            if (auto problem = maskProblem(mask); !problem.empty()) {
                return "effect " + effect.id.toString() + ": " + problem;
            }
            if (!maskIds.insert(mask.id).second) {
                return "effect " + effect.id.toString() + " has a duplicate mask id";
            }
        }
        for (const auto& [name, points] : effect.curves) {
            if (auto problem = curveProblem(points); !problem.empty()) {
                return "effect " + effect.id.toString() + " curve '" + name + "': " + problem;
            }
        }
        if (!std::all_of(effect.samples.begin(), effect.samples.end(), [](double v) { return std::isfinite(v); })) {
            return "effect " + effect.id.toString() + " has non-finite analysis data";
        }
    }
    return {};
}

std::string trackProblem(const Track& track) {
    if (track.id.isNull()) {
        return "track has a null id";
    }
    for (const auto& t : track.transitions) {
        if (t.id.isNull() || t.type.empty() || !t.duration.isPositive()) {
            return "track " + track.id.toString() + " has an invalid transition";
        }
    }
    if (!std::isfinite(track.state.volumeDb) || !(track.state.pan >= -100.0 && track.state.pan <= 100.0)) {
        return "track " + track.id.toString() + " has an invalid volume or pan";
    }
    if (!keyframesValid(track.state.volumeAutomation) || !keyframesValid(track.state.panAutomation)) {
        return "track " + track.id.toString() + " has invalid automation";
    }
    for (std::size_t i = 0; i < track.clips.size(); ++i) {
        if (auto problem = clipShapeProblem(track.clips[i]); !problem.empty()) {
            return problem;
        }
        if (i > 0) {
            const auto& prev = track.clips[i - 1];
            if (overlaps(prev, track.clips[i])) {
                return "track " + track.id.toString() + " has overlapping clips";
            }
            if (!(prev.start < track.clips[i].start)) {
                return "track " + track.id.toString() + " clips are not sorted";
            }
        }
    }
    return {};
}

std::string markersProblem(const std::vector<Marker>& markers) {
    std::unordered_set<MarkerId> ids;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const auto& m = markers[i];
        if (m.id.isNull() || !ids.insert(m.id).second) {
            return "null or duplicate marker id";
        }
        if (m.time.isNegative() || m.duration.isNegative()) {
            return "marker " + m.id.toString() + " has a negative time or duration";
        }
        if (static_cast<int>(m.color) < 0 || static_cast<int>(m.color) >= kMarkerColorCount) {
            return "marker " + m.id.toString() + " has an invalid color";
        }
        if (i > 0 && m.time < markers[i - 1].time) {
            return "markers are not sorted by time";
        }
    }
    return {};
}

std::string captionsProblem(const std::vector<Caption>& captions, const CaptionStyle& style) {
    std::unordered_set<CaptionId> ids;
    for (std::size_t i = 0; i < captions.size(); ++i) {
        const auto& c = captions[i];
        if (c.id.isNull() || !ids.insert(c.id).second) {
            return "null or duplicate caption id";
        }
        if (c.start.isNegative() || !c.duration.isPositive()) {
            return "caption " + c.id.toString() + " has an invalid time";
        }
        if (i > 0 && captions[i - 1].end() > c.start) {
            return "captions overlap or are not sorted";
        }
    }
    if (!(style.size > 0.0) || !std::isfinite(style.positionY) || !std::isfinite(style.backgroundOpacity)) {
        return "invalid caption style";
    }
    return {};
}

std::string settingsProblem(const SequenceSettings& s) {
    if (s.width <= 0 || s.height <= 0) {
        return "sequence size must be positive";
    }
    if (!s.frameRate.isPositive()) {
        return "sequence frame rate must be positive";
    }
    if (s.sampleRate <= 0) {
        return "sequence sample rate must be positive";
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Project

Project::Project(const Project& other)
    : m_media(other.m_media), m_bins(other.m_bins), m_sequences(other.m_sequences) {}

Project& Project::operator=(const Project& other) {
    if (this != &other) {
        m_media = other.m_media;
        m_bins = other.m_bins;
        m_sequences = other.m_sequences;
    }
    return *this;
}

Project::Project(Project&& other) noexcept
    : m_media(std::move(other.m_media)), m_bins(std::move(other.m_bins)),
      m_sequences(std::move(other.m_sequences)) {}

Project& Project::operator=(Project&& other) noexcept {
    if (this != &other) {
        m_media = std::move(other.m_media);
        m_bins = std::move(other.m_bins);
        m_sequences = std::move(other.m_sequences);
    }
    return *this;
}

const MediaItem* Project::findMedia(MediaId id) const noexcept {
    const auto it = findById(m_media, id);
    return it == m_media.end() ? nullptr : &*it;
}

const Bin* Project::findBin(BinId id) const noexcept {
    const auto it = findById(m_bins, id);
    return it == m_bins.end() ? nullptr : &*it;
}

const Sequence* Project::findSequence(SequenceId id) const noexcept {
    const auto it = findById(m_sequences, id);
    return it == m_sequences.end() ? nullptr : &*it;
}

TrackRef Project::findTrack(TrackId id) const noexcept {
    for (const auto& seq : m_sequences) {
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
            for (std::size_t i = 0; i < list->size(); ++i) {
                if ((*list)[i].id == id) {
                    return {&seq, &(*list)[i], i};
                }
            }
        }
    }
    return {};
}

ClipRef Project::findClip(ClipId id) const noexcept {
    for (const auto& seq : m_sequences) {
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
            for (const auto& track : *list) {
                for (std::size_t i = 0; i < track.clips.size(); ++i) {
                    if (track.clips[i].id == id) {
                        return {&seq, &track, &track.clips[i], i};
                    }
                }
            }
        }
    }
    return {};
}

TransitionRef Project::findTransition(TransitionId id) const noexcept {
    for (const auto& seq : m_sequences) {
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
            for (const auto& track : *list) {
                if (const auto* t = track.findTransition(id)) {
                    return {&seq, &track, t};
                }
            }
        }
    }
    return {};
}

std::vector<ClipId> Project::clipsUsingMedia(MediaId id) const {
    std::vector<ClipId> result;
    for (const auto& seq : m_sequences) {
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
            for (const auto& track : *list) {
                for (const auto& clip : track.clips) {
                    if (clip.media == id) {
                        result.push_back(clip.id);
                    }
                }
            }
        }
    }
    return result;
}

std::vector<SequenceId> Project::nestedSequencesOf(const Sequence& sequence) const {
    std::vector<SequenceId> result;
    for (const auto* list : {&sequence.videoTracks, &sequence.audioTracks}) {
        for (const auto& track : *list) {
            for (const auto& clip : track.clips) {
                const auto* media = findMedia(clip.media);
                if (media != nullptr && media->isNest() &&
                    std::find(result.begin(), result.end(), media->sequence) == result.end()) {
                    result.push_back(media->sequence);
                }
            }
        }
    }
    return result;
}

bool Project::sequenceContains(SequenceId outer, SequenceId target) const {
    std::vector<SequenceId> stack{outer};
    std::unordered_set<SequenceId> seen;
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        if (id == target) {
            return true;
        }
        if (!seen.insert(id).second) {
            continue;
        }
        if (const auto* seq = findSequence(id)) {
            for (const auto nested : nestedSequencesOf(*seq)) {
                stack.push_back(nested);
            }
        }
    }
    return false;
}

bool Project::binIsWithin(BinId bin, BinId ancestor) const noexcept {
    // Bounded walk: a corrupt (cyclic) tree cannot loop forever.
    for (std::size_t guard = 0; !bin.isNull() && guard <= m_bins.size(); ++guard) {
        if (bin == ancestor) {
            return true;
        }
        const auto* b = findBin(bin);
        if (b == nullptr) {
            return false;
        }
        bin = b->parent;
    }
    return false;
}

std::vector<std::string> Project::validate() const {
    std::vector<std::string> problems;
    std::unordered_set<Uuid> ids;
    const auto unique = [&](const Uuid& id, const char* what) {
        if (id.isNull() || !ids.insert(id).second) {
            problems.push_back(std::string("null or duplicate ") + what + " id " + id.toString());
        }
    };

    for (const auto& bin : m_bins) {
        unique(bin.id.uuid(), "bin");
        if (!bin.parent.isNull() && findBin(bin.parent) == nullptr) {
            problems.push_back("bin " + bin.id.toString() + " has unknown parent");
        }
        if (!bin.parent.isNull() && binIsWithin(bin.parent, bin.id)) {
            problems.push_back("bin " + bin.id.toString() + " is part of a cycle");
        }
    }
    for (const auto& media : m_media) {
        unique(media.id.uuid(), "media");
        if (media.markIn && media.markOut && !(*media.markIn < *media.markOut)) {
            problems.push_back("media " + media.id.toString() + " has In not before Out");
        }
        if (!media.bin.isNull() && findBin(media.bin) == nullptr) {
            problems.push_back("media " + media.id.toString() + " is in unknown bin");
        }
        if (media.isNest() && findSequence(media.sequence) == nullptr) {
            problems.push_back("nested media " + media.id.toString() + " references unknown sequence");
        }
        if (auto p = markersProblem(media.markers); !p.empty()) {
            problems.push_back(p);
        }
    }
    for (const auto& seq : m_sequences) {
        unique(seq.id.uuid(), "sequence");
        if (auto p = settingsProblem(seq.settings); !p.empty()) {
            problems.push_back(p);
        }
        if (auto p = markersProblem(seq.markers); !p.empty()) {
            problems.push_back(p);
        }
        if (auto p = captionsProblem(seq.captions, seq.captionStyle); !p.empty()) {
            problems.push_back(p);
        }
        for (const auto nested : nestedSequencesOf(seq)) {
            if (sequenceContains(nested, seq.id)) {
                problems.push_back("sequence " + seq.id.toString() + " contains itself (nesting cycle)");
                break;
            }
        }
        for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
            for (const auto& track : seq.tracks(kind)) {
                unique(track.id.uuid(), "track");
                if (track.kind != kind) {
                    problems.push_back("track " + track.id.toString() + " is in the wrong list");
                }
                if (auto p = trackProblem(track); !p.empty()) {
                    problems.push_back(p);
                }
                for (const auto& clip : track.clips) {
                    unique(clip.id.uuid(), "clip");
                    if (findMedia(clip.media) == nullptr) {
                        problems.push_back("clip " + clip.id.toString() + " references unknown media");
                    }
                }
                for (const auto& transition : track.transitions) {
                    unique(transition.id.uuid(), "transition");
                }
            }
        }
    }
    return problems;
}

void Project::addObserver(ProjectObserver* observer) {
    if (observer != nullptr && std::find(m_observers.begin(), m_observers.end(), observer) == m_observers.end()) {
        m_observers.push_back(observer);
    }
}

void Project::removeObserver(ProjectObserver* observer) noexcept {
    m_observers.erase(std::remove(m_observers.begin(), m_observers.end(), observer), m_observers.end());
}

void Project::notify(const ChangeEvent& event) const {
    // Copy: an observer may unregister itself while being notified.
    const auto observers = m_observers;
    for (auto* observer : observers) {
        observer->onProjectChanged(event);
    }
}

// ---------------------------------------------------------------------------------------------
// ProjectMutator

namespace {

void checkMarks(const MediaItem& item) {
    if (auto p = markersProblem(item.markers); !p.empty()) {
        fail(p);
    }
    if (item.isNest() && item.sequence.isNull()) {
        fail("nested media needs a sequence");
    }
    if (item.markIn && item.markOut && !(*item.markIn < *item.markOut)) {
        fail("media In mark must be before Out mark");
    }
    if ((item.markIn && item.markIn->isNegative()) || (item.markOut && item.markOut->isNegative())) {
        fail("media marks must not be negative");
    }
}

} // namespace

void ProjectMutator::insertMedia(std::size_t index, MediaItem item) {
    checkMarks(item);
    auto& media = m_project.m_media;
    if (item.id.isNull() || m_project.findMedia(item.id) != nullptr) {
        fail("media id is null or already used");
    }
    if (!item.bin.isNull() && m_project.findBin(item.bin) == nullptr) {
        fail("media bin does not exist");
    }
    if (index > media.size()) {
        fail("media index out of range");
    }
    const ChangeEvent event{ChangeKind::MediaAdded, item.id.uuid(), item.bin.uuid()};
    media.insert(media.begin() + static_cast<std::ptrdiff_t>(index), std::move(item));
    m_project.notify(event);
}

MediaItem ProjectMutator::removeMedia(MediaId id, std::size_t* indexOut) {
    auto& media = m_project.m_media;
    const auto it = findById(media, id);
    if (it == media.end()) {
        fail("media does not exist");
    }
    if (!m_project.clipsUsingMedia(id).empty()) {
        fail("media is still used by clips");
    }
    if (indexOut != nullptr) {
        *indexOut = static_cast<std::size_t>(std::distance(media.begin(), it));
    }
    MediaItem removed = std::move(*it);
    media.erase(it);
    m_project.notify({ChangeKind::MediaRemoved, removed.id.uuid(), removed.bin.uuid()});
    return removed;
}

void ProjectMutator::replaceMedia(MediaItem item) {
    checkMarks(item);
    auto& media = m_project.m_media;
    const auto it = findById(media, item.id);
    if (it == media.end()) {
        fail("media does not exist");
    }
    if (!item.bin.isNull() && m_project.findBin(item.bin) == nullptr) {
        fail("media bin does not exist");
    }
    *it = std::move(item);
    m_project.notify({ChangeKind::MediaChanged, it->id.uuid(), it->bin.uuid()});
}

void ProjectMutator::insertBin(std::size_t index, Bin bin) {
    auto& bins = m_project.m_bins;
    if (bin.id.isNull() || m_project.findBin(bin.id) != nullptr) {
        fail("bin id is null or already used");
    }
    if (!bin.parent.isNull() && m_project.findBin(bin.parent) == nullptr) {
        fail("parent bin does not exist");
    }
    if (index > bins.size()) {
        fail("bin index out of range");
    }
    const ChangeEvent event{ChangeKind::BinAdded, bin.id.uuid(), bin.parent.uuid()};
    bins.insert(bins.begin() + static_cast<std::ptrdiff_t>(index), std::move(bin));
    m_project.notify(event);
}

Bin ProjectMutator::removeBin(BinId id, std::size_t* indexOut) {
    auto& bins = m_project.m_bins;
    const auto it = findById(bins, id);
    if (it == bins.end()) {
        fail("bin does not exist");
    }
    const bool hasChildren =
        std::any_of(bins.begin(), bins.end(), [&](const Bin& b) { return b.parent == id; }) ||
        std::any_of(m_project.m_media.begin(), m_project.m_media.end(), [&](const MediaItem& m) { return m.bin == id; });
    if (hasChildren) {
        fail("bin is not empty");
    }
    if (indexOut != nullptr) {
        *indexOut = static_cast<std::size_t>(std::distance(bins.begin(), it));
    }
    Bin removed = std::move(*it);
    bins.erase(it);
    m_project.notify({ChangeKind::BinRemoved, removed.id.uuid(), removed.parent.uuid()});
    return removed;
}

void ProjectMutator::replaceBin(Bin bin) {
    auto& bins = m_project.m_bins;
    const auto it = findById(bins, bin.id);
    if (it == bins.end()) {
        fail("bin does not exist");
    }
    if (!bin.parent.isNull()) {
        if (m_project.findBin(bin.parent) == nullptr) {
            fail("parent bin does not exist");
        }
        if (m_project.binIsWithin(bin.parent, bin.id)) {
            fail("a bin cannot be moved into itself or its descendants");
        }
    }
    *it = std::move(bin);
    m_project.notify({ChangeKind::BinChanged, it->id.uuid(), it->parent.uuid()});
}

void ProjectMutator::insertSequence(std::size_t index, Sequence sequence) {
    auto& sequences = m_project.m_sequences;
    if (sequence.id.isNull() || m_project.findSequence(sequence.id) != nullptr) {
        fail("sequence id is null or already used");
    }
    if (auto p = settingsProblem(sequence.settings); !p.empty()) {
        fail(p);
    }
    if (index > sequences.size()) {
        fail("sequence index out of range");
    }
    std::unordered_set<Uuid> ids;
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        for (const auto& track : sequence.tracks(kind)) {
            if (track.kind != kind) {
                fail("track kind does not match its list");
            }
            if (auto p = trackProblem(track); !p.empty()) {
                fail(p);
            }
            if (!ids.insert(track.id.uuid()).second || m_project.findTrack(track.id).track != nullptr) {
                fail("duplicate track id");
            }
            for (const auto& clip : track.clips) {
                if (!ids.insert(clip.id.uuid()).second || m_project.findClip(clip.id).clip != nullptr) {
                    fail("duplicate clip id");
                }
                if (m_project.findMedia(clip.media) == nullptr) {
                    fail("clip references unknown media");
                }
            }
        }
    }
    const ChangeEvent event{ChangeKind::SequenceAdded, sequence.id.uuid(), {}};
    sequences.insert(sequences.begin() + static_cast<std::ptrdiff_t>(index), std::move(sequence));
    m_project.notify(event);
}

Sequence ProjectMutator::removeSequence(SequenceId id, std::size_t* indexOut) {
    auto& sequences = m_project.m_sequences;
    const auto it = findById(sequences, id);
    if (it == sequences.end()) {
        fail("sequence does not exist");
    }
    for (const auto& media : m_project.m_media) {
        if (media.isNest() && media.sequence == id) {
            fail("sequence is used by nested media " + media.name);
        }
    }
    if (indexOut != nullptr) {
        *indexOut = static_cast<std::size_t>(std::distance(sequences.begin(), it));
    }
    Sequence removed = std::move(*it);
    sequences.erase(it);
    m_project.notify({ChangeKind::SequenceRemoved, removed.id.uuid(), {}});
    return removed;
}

Sequence& ProjectMutator::sequence(SequenceId id) {
    const auto it = findById(m_project.m_sequences, id);
    if (it == m_project.m_sequences.end()) {
        fail("sequence does not exist");
    }
    return *it;
}

void ProjectMutator::setSequenceName(SequenceId id, std::string name) {
    sequence(id).name = std::move(name);
    m_project.notify({ChangeKind::SequenceChanged, id.uuid(), {}});
}

void ProjectMutator::setSequenceMarks(SequenceId id, std::optional<Rational> in, std::optional<Rational> out) {
    auto& seq = sequence(id);
    if ((in && in->isNegative()) || (out && out->isNegative()) || (in && out && !(*in < *out))) {
        fail("sequence In must be before Out and not negative");
    }
    seq.markIn = in;
    seq.markOut = out;
    m_project.notify({ChangeKind::SequenceChanged, id.uuid(), {}});
}

void ProjectMutator::setSequenceCaptions(SequenceId id, std::vector<Caption> captions, CaptionStyle style) {
    if (auto p = captionsProblem(captions, style); !p.empty()) {
        fail(p);
    }
    auto& seq = sequence(id);
    seq.captions = std::move(captions);
    seq.captionStyle = std::move(style);
    m_project.notify({ChangeKind::SequenceChanged, id.uuid(), {}});
}

void ProjectMutator::setSequenceMarkers(SequenceId id, std::vector<Marker> markers) {
    if (auto p = markersProblem(markers); !p.empty()) {
        fail(p);
    }
    sequence(id).markers = std::move(markers);
    m_project.notify({ChangeKind::SequenceChanged, id.uuid(), {}});
}

void ProjectMutator::setSequenceMasterVolume(SequenceId id, double db) {
    if (!std::isfinite(db)) {
        fail("master volume must be finite");
    }
    sequence(id).masterVolumeDb = db;
    m_project.notify({ChangeKind::SequenceChanged, id.uuid(), {}});
}

void ProjectMutator::setSequenceSettings(SequenceId id, SequenceSettings settings) {
    auto& seq = sequence(id);
    if (auto p = settingsProblem(settings); !p.empty()) {
        fail(p);
    }
    seq.settings = std::move(settings);
    m_project.notify({ChangeKind::SequenceChanged, id.uuid(), {}});
}

std::pair<Sequence*, std::vector<Track>*> ProjectMutator::trackList(TrackId id, std::size_t& index) {
    for (auto& seq : m_project.m_sequences) {
        for (auto* list : {&seq.videoTracks, &seq.audioTracks}) {
            for (std::size_t i = 0; i < list->size(); ++i) {
                if ((*list)[i].id == id) {
                    index = i;
                    return {&seq, list};
                }
            }
        }
    }
    fail("track does not exist");
}

Track& ProjectMutator::track(TrackId id) {
    std::size_t index = 0;
    auto [seq, list] = trackList(id, index);
    (void)seq;
    return (*list)[index];
}

void ProjectMutator::insertTrack(SequenceId sequenceId, std::size_t index, Track newTrack) {
    auto& seq = sequence(sequenceId);
    if (newTrack.id.isNull() || m_project.findTrack(newTrack.id).track != nullptr) {
        fail("track id is null or already used");
    }
    if (auto p = trackProblem(newTrack); !p.empty()) {
        fail(p);
    }
    std::unordered_set<ClipId> clipIds;
    for (const auto& clip : newTrack.clips) {
        if (!clipIds.insert(clip.id).second || m_project.findClip(clip.id).clip != nullptr) {
            fail("duplicate clip id");
        }
        if (m_project.findMedia(clip.media) == nullptr) {
            fail("clip references unknown media");
        }
    }
    auto& list = newTrack.kind == TrackKind::Video ? seq.videoTracks : seq.audioTracks;
    if (index > list.size()) {
        fail("track index out of range");
    }
    const ChangeEvent event{ChangeKind::TrackAdded, newTrack.id.uuid(), sequenceId.uuid()};
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), std::move(newTrack));
    m_project.notify(event);
}

Track ProjectMutator::removeTrack(TrackId id, std::size_t* indexOut) {
    std::size_t index = 0;
    auto [seq, list] = trackList(id, index);
    if (indexOut != nullptr) {
        *indexOut = index;
    }
    Track removed = std::move((*list)[index]);
    list->erase(list->begin() + static_cast<std::ptrdiff_t>(index));
    m_project.notify({ChangeKind::TrackRemoved, id.uuid(), seq->id.uuid()});
    return removed;
}

void ProjectMutator::addTransition(TrackId track, Transition transition) {
    if (transition.id.isNull() || transition.type.empty() || !transition.duration.isPositive()) {
        fail("transition needs an id, a type and a positive duration");
    }
    if (m_project.findTransition(transition.id).transition != nullptr) {
        fail("transition id already exists");
    }
    std::size_t index = 0;
    auto [seq, list] = trackList(track, index);
    (*list)[index].transitions.push_back(std::move(transition));
    m_project.notify({ChangeKind::TrackChanged, track.uuid(), seq->id.uuid()});
}

Transition ProjectMutator::removeTransition(TransitionId id) {
    const auto ref = m_project.findTransition(id);
    if (ref.transition == nullptr) {
        fail("transition does not exist");
    }
    std::size_t index = 0;
    auto [seq, list] = trackList(ref.track->id, index);
    auto& transitions = (*list)[index].transitions;
    const auto it = std::find_if(transitions.begin(), transitions.end(), [&](const Transition& t) { return t.id == id; });
    Transition removed = *it;
    transitions.erase(it);
    m_project.notify({ChangeKind::TrackChanged, (*list)[index].id.uuid(), seq->id.uuid()});
    return removed;
}

void ProjectMutator::replaceTransition(Transition transition) {
    const auto ref = m_project.findTransition(transition.id);
    if (ref.transition == nullptr) {
        fail("transition does not exist");
    }
    if (transition.type.empty() || !transition.duration.isPositive()) {
        fail("transition needs a type and a positive duration");
    }
    std::size_t index = 0;
    auto [seq, list] = trackList(ref.track->id, index);
    for (auto& t : (*list)[index].transitions) {
        if (t.id == transition.id) {
            t = std::move(transition);
            break;
        }
    }
    m_project.notify({ChangeKind::TrackChanged, (*list)[index].id.uuid(), seq->id.uuid()});
}

void ProjectMutator::setTrackState(TrackId id, TrackState state) {
    if (!std::isfinite(state.volumeDb) || !std::isfinite(state.pan) || state.pan < -100.0 || state.pan > 100.0) {
        fail("track volume must be finite and pan within [-100, 100]");
    }
    if (!keyframesValid(state.volumeAutomation) || !keyframesValid(state.panAutomation)) {
        fail("track automation keyframes are invalid");
    }
    std::size_t index = 0;
    auto [seq, list] = trackList(id, index);
    (*list)[index].state = std::move(state);
    m_project.notify({ChangeKind::TrackChanged, id.uuid(), seq->id.uuid()});
}

void ProjectMutator::checkClip(const Track& target, const Clip& clip, ClipId ignore) const {
    if (auto p = clipShapeProblem(clip); !p.empty()) {
        fail(p);
    }
    const auto* media = m_project.findMedia(clip.media);
    if (media == nullptr) {
        fail("clip references unknown media");
    }
    if (media->isNest()) {
        const auto* owner = m_project.findTrack(target.id).sequence;
        if (owner != nullptr && m_project.sequenceContains(media->sequence, owner->id)) {
            fail("a sequence cannot contain itself");
        }
    }
    for (const auto& other : target.clips) {
        if (other.id != ignore && overlaps(other, clip)) {
            fail("clip would overlap clip " + other.id.toString());
        }
    }
}

void ProjectMutator::insertClip(TrackId trackId, Clip clip) {
    auto& target = track(trackId);
    if (m_project.findClip(clip.id).clip != nullptr) {
        fail("clip id already used");
    }
    checkClip(target, clip, ClipId());
    const auto pos = std::upper_bound(target.clips.begin(), target.clips.end(), clip.start,
                                      [](const Rational& t, const Clip& c) { return t < c.start; });
    const ChangeEvent event{ChangeKind::ClipAdded, clip.id.uuid(), trackId.uuid()};
    target.clips.insert(pos, std::move(clip));
    m_project.notify(event);
}

Clip ProjectMutator::removeClip(ClipId id) {
    const auto ref = m_project.findClip(id);
    if (ref.clip == nullptr) {
        fail("clip does not exist");
    }
    auto& target = track(ref.track->id);
    Clip removed = std::move(target.clips[ref.index]);
    target.clips.erase(target.clips.begin() + static_cast<std::ptrdiff_t>(ref.index));
    m_project.notify({ChangeKind::ClipRemoved, id.uuid(), target.id.uuid()});
    return removed;
}

void ProjectMutator::replaceClip(Clip clip) {
    const auto ref = m_project.findClip(clip.id);
    if (ref.clip == nullptr) {
        fail("clip does not exist");
    }
    auto& target = track(ref.track->id);
    checkClip(target, clip, clip.id);
    const ClipId id = clip.id;
    target.clips[ref.index] = std::move(clip);
    // The start may have changed: restore ordering (no overlaps, so starts are unique).
    std::sort(target.clips.begin(), target.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    m_project.notify({ChangeKind::ClipChanged, id.uuid(), target.id.uuid()});
}

// ---------------------------------------------------------------------------------------------
// ProjectBuilder

ProjectBuilder& ProjectBuilder::addMedia(MediaItem item) {
    m_mutator.insertMedia(m_project.media().size(), std::move(item));
    return *this;
}

ProjectBuilder& ProjectBuilder::addBin(Bin bin) {
    m_mutator.insertBin(m_project.bins().size(), std::move(bin));
    return *this;
}

ProjectBuilder& ProjectBuilder::addSequence(Sequence sequence) {
    m_mutator.insertSequence(m_project.sequences().size(), std::move(sequence));
    return *this;
}

Project ProjectBuilder::build() {
    if (const auto problems = m_project.validate(); !problems.empty()) {
        fail("invalid project: " + problems.front());
    }
    return std::move(m_project);
}

// ---------------------------------------------------------------------------------------------
// Defaults

Sequence makeDefaultSequence(std::string name, SequenceSettings settings, int videoTracks, int audioTracks) {
    Sequence seq;
    seq.id = SequenceId::generate();
    seq.name = std::move(name);
    seq.settings = std::move(settings);
    for (int i = 0; i < videoTracks; ++i) {
        Track t;
        t.id = TrackId::generate();
        t.kind = TrackKind::Video;
        t.state.name = "V" + std::to_string(i + 1);
        seq.videoTracks.push_back(std::move(t));
    }
    for (int i = 0; i < audioTracks; ++i) {
        Track t;
        t.id = TrackId::generate();
        t.kind = TrackKind::Audio;
        t.state.name = "A" + std::to_string(i + 1);
        seq.audioTracks.push_back(std::move(t));
    }
    return seq;
}

Project makeDefaultProject() {
    ProjectBuilder builder;
    builder.addSequence(makeDefaultSequence("Sequence 01"));
    return builder.build();
}

} // namespace loki::model
