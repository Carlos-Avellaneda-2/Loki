#pragma once

#include "model/ids.h"
#include "util/rational.h"

#include <optional>

namespace loki::ui {

class ProjectController;

// Sequence-level edits shared by the Timeline and the Program Monitor (same keys in both).
// Failures are reported through ProjectController::commandFailed.
namespace sequence_edits {

// In at the playhead frame; Out is exclusive: the end of the playhead frame.
void markIn(ProjectController& controller, model::SequenceId sequence, const Rational& playhead);
void markOut(ProjectController& controller, model::SequenceId sequence, const Rational& playhead);
void clearMarks(ProjectController& controller, model::SequenceId sequence);
[[nodiscard]] std::optional<Rational> markInOf(const ProjectController& controller, model::SequenceId sequence);
// Last frame inside the Out mark (where "Go to Out" puts the playhead).
[[nodiscard]] std::optional<Rational> markOutFrameOf(const ProjectController& controller, model::SequenceId sequence);
// Lift / Extract between the sequence marks.
void lift(ProjectController& controller, model::SequenceId sequence);
void extract(ProjectController& controller, model::SequenceId sequence);

} // namespace sequence_edits
} // namespace loki::ui
