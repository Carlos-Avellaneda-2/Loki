#include "ui/sequence_edits.h"

#include "commands/edit_commands.h"
#include "commands/project_commands.h"
#include "ui/project_controller.h"

namespace loki::ui::sequence_edits {

namespace {

template <typename Make>
void run(ProjectController& controller, Make make) {
    try {
        if (auto command = make()) {
            controller.execute(std::move(command));
        }
    } catch (const std::exception& e) {
        emit controller.commandFailed(QString::fromUtf8(e.what()));
    }
}

Rational frameRateOf(const ProjectController& controller, model::SequenceId sequence) {
    const auto* seq = controller.project().findSequence(sequence);
    return seq != nullptr ? seq->settings.frameRate : Rational(30);
}

} // namespace

void markIn(ProjectController& controller, model::SequenceId sequence, const Rational& playhead) {
    const Rational at = quantizeToFrame(playhead, frameRateOf(controller, sequence), Rounding::Floor);
    run(controller, [&] { return commands::setSequenceMarkIn(controller.project(), sequence, at); });
}

void markOut(ProjectController& controller, model::SequenceId sequence, const Rational& playhead) {
    const Rational rate = frameRateOf(controller, sequence);
    const Rational at = quantizeToFrame(playhead, rate, Rounding::Floor) + rate.reciprocal();
    run(controller, [&] { return commands::setSequenceMarkOut(controller.project(), sequence, at); });
}

void clearMarks(ProjectController& controller, model::SequenceId sequence) {
    run(controller, [&] {
        return std::make_unique<commands::SetSequenceMarksCommand>(sequence, std::nullopt, std::nullopt,
                                                                   "Clear In and Out");
    });
}

std::optional<Rational> markInOf(const ProjectController& controller, model::SequenceId sequence) {
    const auto* seq = controller.project().findSequence(sequence);
    return seq != nullptr ? seq->markIn : std::nullopt;
}

std::optional<Rational> markOutFrameOf(const ProjectController& controller, model::SequenceId sequence) {
    const auto* seq = controller.project().findSequence(sequence);
    if (seq == nullptr || !seq->markOut) {
        return std::nullopt;
    }
    return max(Rational(), *seq->markOut - seq->settings.frameRate.reciprocal());
}

void lift(ProjectController& controller, model::SequenceId sequence) {
    const auto* seq = controller.project().findSequence(sequence);
    if (seq == nullptr) {
        return;
    }
    if (!seq->markIn || !seq->markOut) {
        emit controller.commandFailed(QObject::tr("Set sequence In and Out points first (I / O)"));
        return;
    }
    const auto in = *seq->markIn;
    const auto out = *seq->markOut;
    run(controller, [&] { return commands::makeLiftRange(controller.project(), sequence, in, out); });
}

void extract(ProjectController& controller, model::SequenceId sequence) {
    const auto* seq = controller.project().findSequence(sequence);
    if (seq == nullptr) {
        return;
    }
    if (!seq->markIn || !seq->markOut) {
        emit controller.commandFailed(QObject::tr("Set sequence In and Out points first (I / O)"));
        return;
    }
    const auto in = *seq->markIn;
    const auto out = *seq->markOut;
    run(controller, [&] { return commands::makeExtractRange(controller.project(), sequence, in, out); });
}

} // namespace loki::ui::sequence_edits
