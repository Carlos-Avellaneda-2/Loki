#pragma once

#include "commands/command.h"

#include <optional>
#include <string>
#include <vector>

namespace loki::commands {

// Markers (D29). Sequence markers live on the sequence (sequence time); clip markers on the media
// item (source time), so every clip of that media shows them.

// Pure helpers on a sorted marker list.
[[nodiscard]] std::vector<model::Marker> withMarker(std::vector<model::Marker> markers, model::Marker marker);
[[nodiscard]] std::vector<model::Marker> withoutMarker(std::vector<model::Marker> markers, model::MarkerId id);
[[nodiscard]] const model::Marker* findMarker(const std::vector<model::Marker>& markers, model::MarkerId id) noexcept;
// First marker strictly after / before `t` (navigation, Shift+M / Ctrl+Shift+M).
[[nodiscard]] std::optional<Rational> nextMarkerTime(const std::vector<model::Marker>& markers, const Rational& t);
[[nodiscard]] std::optional<Rational> previousMarkerTime(const std::vector<model::Marker>& markers, const Rational& t);
// Display name of a marker color ("Green", ...).
[[nodiscard]] const char* markerColorName(model::MarkerColor color) noexcept;

class SetSequenceMarkersCommand final : public Command {
public:
    SetSequenceMarkersCommand(model::SequenceId id, std::vector<model::Marker> markers, std::string text)
        : m_id(id), m_markers(std::move(markers)), m_text(std::move(text)) {}
    [[nodiscard]] std::string text() const override { return m_text; }
    void redo(model::Project& project) override;
    void undo(model::Project& project) override;

private:
    model::SequenceId m_id;
    std::vector<model::Marker> m_markers;
    std::vector<model::Marker> m_old;
    std::string m_text;
};

// Sequence markers. `marker.id` may be null (a new id is generated).
[[nodiscard]] std::unique_ptr<Command> makeAddSequenceMarker(const model::Project& project, model::SequenceId id,
                                                             model::Marker marker);
// Replaces the marker with the same id (name, comment, color, time, duration).
[[nodiscard]] std::unique_ptr<Command> makeEditSequenceMarker(const model::Project& project, model::SequenceId id,
                                                              model::Marker marker);
[[nodiscard]] std::unique_ptr<Command> makeRemoveSequenceMarker(const model::Project& project, model::SequenceId id,
                                                                model::MarkerId marker);
[[nodiscard]] std::unique_ptr<Command> makeClearSequenceMarkers(const model::Project& project, model::SequenceId id);

// Clip markers (on the media item, source time).
[[nodiscard]] std::unique_ptr<Command> makeAddMediaMarker(const model::Project& project, model::MediaId id,
                                                          model::Marker marker);
[[nodiscard]] std::unique_ptr<Command> makeEditMediaMarker(const model::Project& project, model::MediaId id,
                                                           model::Marker marker);
[[nodiscard]] std::unique_ptr<Command> makeRemoveMediaMarker(const model::Project& project, model::MediaId id,
                                                             model::MarkerId marker);

} // namespace loki::commands
