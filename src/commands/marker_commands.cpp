#include "commands/marker_commands.h"

#include "commands/project_commands.h"

#include <algorithm>

namespace loki::commands {

using namespace model;

std::vector<Marker> withMarker(std::vector<Marker> markers, Marker marker) {
    if (marker.id.isNull()) {
        marker.id = MarkerId::generate();
    }
    std::erase_if(markers, [&](const Marker& m) { return m.id == marker.id; });
    // After markers at the same time, so the newest is last.
    const auto pos = std::upper_bound(markers.begin(), markers.end(), marker.time,
                                      [](const Rational& t, const Marker& m) { return t < m.time; });
    markers.insert(pos, std::move(marker));
    return markers;
}

std::vector<Marker> withoutMarker(std::vector<Marker> markers, MarkerId id) {
    std::erase_if(markers, [&](const Marker& m) { return m.id == id; });
    return markers;
}

const Marker* findMarker(const std::vector<Marker>& markers, MarkerId id) noexcept {
    const auto it = std::find_if(markers.begin(), markers.end(), [&](const Marker& m) { return m.id == id; });
    return it == markers.end() ? nullptr : &*it;
}

std::optional<Rational> nextMarkerTime(const std::vector<Marker>& markers, const Rational& t) {
    for (const auto& m : markers) {
        if (t < m.time) {
            return m.time;
        }
    }
    return std::nullopt;
}

std::optional<Rational> previousMarkerTime(const std::vector<Marker>& markers, const Rational& t) {
    std::optional<Rational> result;
    for (const auto& m : markers) {
        if (m.time < t) {
            result = m.time;
        }
    }
    return result;
}

const char* markerColorName(MarkerColor color) noexcept {
    switch (color) {
    case MarkerColor::Green: return "Green";
    case MarkerColor::Red: return "Red";
    case MarkerColor::Purple: return "Purple";
    case MarkerColor::Orange: return "Orange";
    case MarkerColor::Yellow: return "Yellow";
    case MarkerColor::White: return "White";
    case MarkerColor::Blue: return "Blue";
    case MarkerColor::Cyan: return "Cyan";
    }
    return "Green";
}

void SetSequenceMarkersCommand::redo(Project& project) {
    const auto* seq = project.findSequence(m_id);
    if (seq == nullptr) {
        throw CommandError("Sequence does not exist");
    }
    m_old = seq->markers;
    mutate(project).setSequenceMarkers(m_id, m_markers);
}

void SetSequenceMarkersCommand::undo(Project& project) {
    mutate(project).setSequenceMarkers(m_id, m_old);
}

namespace {

const Sequence& requireSequence(const Project& project, SequenceId id) {
    const auto* seq = project.findSequence(id);
    if (seq == nullptr) {
        throw CommandError("Sequence does not exist");
    }
    return *seq;
}

const MediaItem& requireMedia(const Project& project, MediaId id) {
    const auto* media = project.findMedia(id);
    if (media == nullptr) {
        throw CommandError("Media does not exist");
    }
    return *media;
}

void checkMarker(const Marker& marker) {
    if (marker.time.isNegative() || marker.duration.isNegative()) {
        throw CommandError("Marker time and duration must not be negative");
    }
}

std::unique_ptr<Command> setMediaMarkers(MediaId id, std::vector<Marker> markers, std::string text) {
    return std::make_unique<EditMediaCommand>(id, std::move(text),
                                              [markers = std::move(markers)](MediaItem& m) { m.markers = markers; });
}

} // namespace

std::unique_ptr<Command> makeAddSequenceMarker(const Project& project, SequenceId id, Marker marker) {
    checkMarker(marker);
    const auto& seq = requireSequence(project, id);
    return std::make_unique<SetSequenceMarkersCommand>(id, withMarker(seq.markers, std::move(marker)), "Add Marker");
}

std::unique_ptr<Command> makeEditSequenceMarker(const Project& project, SequenceId id, Marker marker) {
    checkMarker(marker);
    const auto& seq = requireSequence(project, id);
    const auto* old = findMarker(seq.markers, marker.id);
    if (old == nullptr) {
        throw CommandError("Marker does not exist");
    }
    if (*old == marker) {
        return nullptr;
    }
    return std::make_unique<SetSequenceMarkersCommand>(id, withMarker(seq.markers, std::move(marker)), "Edit Marker");
}

std::unique_ptr<Command> makeRemoveSequenceMarker(const Project& project, SequenceId id, MarkerId marker) {
    const auto& seq = requireSequence(project, id);
    if (findMarker(seq.markers, marker) == nullptr) {
        return nullptr;
    }
    return std::make_unique<SetSequenceMarkersCommand>(id, withoutMarker(seq.markers, marker), "Clear Marker");
}

std::unique_ptr<Command> makeClearSequenceMarkers(const Project& project, SequenceId id) {
    const auto& seq = requireSequence(project, id);
    if (seq.markers.empty()) {
        return nullptr;
    }
    return std::make_unique<SetSequenceMarkersCommand>(id, std::vector<Marker>{}, "Clear All Markers");
}

std::unique_ptr<Command> makeAddMediaMarker(const Project& project, MediaId id, Marker marker) {
    checkMarker(marker);
    const auto& media = requireMedia(project, id);
    return setMediaMarkers(id, withMarker(media.markers, std::move(marker)), "Add Clip Marker");
}

std::unique_ptr<Command> makeEditMediaMarker(const Project& project, MediaId id, Marker marker) {
    checkMarker(marker);
    const auto& media = requireMedia(project, id);
    const auto* old = findMarker(media.markers, marker.id);
    if (old == nullptr) {
        throw CommandError("Marker does not exist");
    }
    if (*old == marker) {
        return nullptr;
    }
    return setMediaMarkers(id, withMarker(media.markers, std::move(marker)), "Edit Clip Marker");
}

std::unique_ptr<Command> makeRemoveMediaMarker(const Project& project, MediaId id, MarkerId marker) {
    const auto& media = requireMedia(project, id);
    if (findMarker(media.markers, marker) == nullptr) {
        return nullptr;
    }
    return setMediaMarkers(id, withoutMarker(media.markers, marker), "Clear Clip Marker");
}

} // namespace loki::commands
