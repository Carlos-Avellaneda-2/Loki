// Property test: random sequences of edits must always keep the project valid, rejected edits
// must change nothing, and undoing everything must return exactly to every earlier state.
#include "commands/clip_commands.h"
#include "commands/project_commands.h"
#include "commands/undo_stack.h"

#include "model_fixtures.h"

#include <gtest/gtest.h>

#include <random>

namespace {

using namespace loki;
using namespace loki::model;
using namespace loki::commands;

class RandomEditor {
public:
    RandomEditor(const Project& project, std::uint32_t seed) : m_project(project), m_rng(seed) {}

    std::unique_ptr<Command> next() {
        const auto& seq = m_project.sequences().front();
        switch (pick(12)) {
        case 0:
        case 1:
        case 2: { // add clip (may overlap and be rejected)
            const auto& track = randomTrack(seq);
            return std::make_unique<AddClipCommand>(
                track.id, test::makeClip(randomMedia(), randomTime(), Rational(1 + pick(8), 1 + pick(3))));
        }
        case 3:
            if (auto* c = randomClip(seq)) {
                return std::make_unique<RemoveClipCommand>(c->id);
            }
            break;
        case 4:
            if (auto* c = randomClip(seq)) {
                return std::make_unique<MoveClipCommand>(c->id, randomTrack(seq).id, randomTime());
            }
            break;
        case 5:
            if (auto* c = randomClip(seq)) {
                return std::make_unique<SplitClipCommand>(c->id, c->start + Rational(pick(10), 2));
            }
            break;
        case 6:
            if (auto* c = randomClip(seq)) {
                return trimClip(c->id, c->start, c->sourceIn + Rational(pick(3)), Rational(1 + pick(5), 2));
            }
            break;
        case 7: {
            const auto& track = randomTrack(seq);
            return setTrackLocked(m_project, track.id, pick(4) == 0);
        }
        case 8:
            return std::make_unique<AddTrackCommand>(seq.id, pick(2) == 0 ? TrackKind::Video : TrackKind::Audio);
        case 9:
            if (seq.videoTracks.size() + seq.audioTracks.size() > 2) {
                return std::make_unique<RemoveTrackCommand>(randomTrack(seq).id);
            }
            break;
        case 10:
            if (auto* c = randomClip(seq)) {
                return addEffect(c->id, test::makeOpacityEffect());
            }
            break;
        case 11:
            if (auto* c = randomClip(seq); c != nullptr && !c->effects.empty()) {
                return setKeyframe(c->id, c->effects.front().id, "opacity",
                                   Keyframe{Rational(pick(20), 2), static_cast<double>(pick(100))});
            }
            break;
        default: break;
        }
        return std::make_unique<AddMediaCommand>(test::makeMedia("m" + std::to_string(pick(1000))));
    }

private:
    std::uint32_t pick(std::uint32_t n) { return std::uniform_int_distribution<std::uint32_t>(0, n - 1)(m_rng); }

    Rational randomTime() { return Rational(pick(120), 1 + pick(4)); }

    MediaId randomMedia() {
        const auto& media = m_project.media();
        return media[pick(static_cast<std::uint32_t>(media.size()))].id;
    }

    const Track& randomTrack(const Sequence& seq) {
        const auto total = static_cast<std::uint32_t>(seq.videoTracks.size() + seq.audioTracks.size());
        const auto i = pick(total);
        return i < seq.videoTracks.size() ? seq.videoTracks[i] : seq.audioTracks[i - seq.videoTracks.size()];
    }

    const Clip* randomClip(const Sequence& seq) {
        std::vector<const Clip*> clips;
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
            for (const auto& t : *list) {
                for (const auto& c : t.clips) {
                    clips.push_back(&c);
                }
            }
        }
        return clips.empty() ? nullptr : clips[pick(static_cast<std::uint32_t>(clips.size()))];
    }

    const Project& m_project;
    std::mt19937 m_rng;
};

class RandomEditTest : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(RandomEditTest, UndoRedoReplaysEveryStateExactly) {
    test::BasicProject ids;
    Project project = ids.build();
    UndoStack stack(project);
    RandomEditor editor(project, GetParam());

    std::vector<Project> states{project};
    int accepted = 0;
    int rejected = 0;
    for (int i = 0; i < 400; ++i) {
        auto command = editor.next();
        const Project before = project;
        try {
            stack.push(std::move(command));
            ++accepted;
            ASSERT_TRUE(project.validate().empty()) << project.validate().front();
            if (stack.count() == states.size()) {
                states.push_back(project); // new history entry
            } else {
                states.back() = project; // merged into the previous entry
            }
        } catch (const std::exception&) {
            ++rejected;
            ASSERT_EQ(project, before) << "a rejected command modified the project";
        }
    }
    ASSERT_EQ(states.size(), stack.count() + 1);
    EXPECT_GT(accepted, 100);
    EXPECT_GT(rejected, 0);

    for (std::size_t i = states.size() - 1; i > 0; --i) {
        stack.undo();
        ASSERT_EQ(project, states[i - 1]) << "undo mismatch at step " << i;
    }
    for (std::size_t i = 1; i < states.size(); ++i) {
        stack.redo();
        ASSERT_EQ(project, states[i]) << "redo mismatch at step " << i;
    }

    // The final state must survive a JSON round trip too.
    EXPECT_EQ(parseProject(serializeProject(project)), project);
}

INSTANTIATE_TEST_SUITE_P(Seeds, RandomEditTest, ::testing::Values(1u, 7u, 42u, 1234u, 99991u));

} // namespace
