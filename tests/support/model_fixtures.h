#pragma once

#include "model/project.h"
#include "model/project_json.h"

#include <string>
#include <vector>

namespace loki::test {

inline model::MediaItem makeMedia(std::string name, Rational duration = Rational(60)) {
    model::MediaItem m;
    m.id = model::MediaId::generate();
    m.name = name;
    m.path = std::filesystem::path("C:/media") / (name + ".mp4");
    m.info.hasVideo = true;
    m.info.hasAudio = true;
    m.info.width = 1920;
    m.info.height = 1080;
    m.info.frameRate = Rational(30000, 1001);
    m.info.duration = duration;
    m.info.sampleRate = 48000;
    m.info.audioChannels = 2;
    m.info.videoCodec = "h264";
    m.info.audioCodec = "aac";
    return m;
}

inline model::Clip makeClip(model::MediaId media, Rational start, Rational duration, Rational sourceIn = {}) {
    model::Clip c;
    c.id = model::ClipId::generate();
    c.media = media;
    c.name = "clip";
    c.start = start;
    c.duration = duration;
    c.sourceIn = sourceIn;
    return c;
}

inline model::Effect makeOpacityEffect() {
    model::Effect e;
    e.id = model::EffectId::generate();
    e.type = "loki.opacity";
    e.parameters.push_back({"opacity", 100.0, {}});
    e.parameters.push_back({"blendMode", 0.0, {}});
    return e;
}

// Records every change event.
class RecordingObserver final : public model::ProjectObserver {
public:
    void onProjectChanged(const model::ChangeEvent& event) override { events.push_back(event); }
    [[nodiscard]] std::size_t count(model::ChangeKind kind) const {
        std::size_t n = 0;
        for (const auto& e : events) {
            n += e.kind == kind ? 1 : 0;
        }
        return n;
    }
    std::vector<model::ChangeEvent> events;
};

// Project with one media item and one sequence (V1, V2, A1); ids exposed for tests.
struct BasicProject {
    model::MediaId media;
    model::SequenceId sequence;
    model::TrackId v1;
    model::TrackId v2;
    model::TrackId a1;

    model::Project build() {
        auto m = makeMedia("interview");
        media = m.id;
        auto seq = model::makeDefaultSequence("Seq", {}, 2, 1);
        sequence = seq.id;
        v1 = seq.videoTracks[0].id;
        v2 = seq.videoTracks[1].id;
        a1 = seq.audioTracks[0].id;
        model::ProjectBuilder builder;
        builder.addMedia(std::move(m));
        builder.addSequence(std::move(seq));
        return builder.build();
    }
};

// Readable diff on failure: compares the canonical JSON form.
inline std::string dump(const model::Project& project) {
    return model::serializeProject(project);
}

} // namespace loki::test
