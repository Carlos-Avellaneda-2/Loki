#include "playback/render_previews.h"

#include "model/project_json.h"

#include <cstdio>
#include <functional>

namespace loki::playback {

namespace {

// FNV-1a 64-bit: stable across runs and platforms (std::hash is not).
std::uint64_t fnv1a(const std::string& text, std::uint64_t h = 1469598103934665603ULL) {
    for (const unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

void appendSequence(std::string& out, const SequenceSnapshot& s, const Rational& from, const Rational& to, int depth);

void appendMedia(std::string& out, const SequenceSnapshot& s, const model::MediaId& id, int depth) {
    const auto it = s.media.find(id);
    if (it == s.media.end()) {
        return;
    }
    out += model::canonicalMedia(it->second);
    if (it->second.isNest() && depth < 16) {
        if (const auto nested = s.nested.find(it->second.sequence); nested != s.nested.end() && nested->second) {
            // The whole nested sequence (its time mapping depends on the clip): simple and safe.
            appendSequence(out, *nested->second, Rational(), nested->second->sequence.duration() + Rational(1), depth + 1);
        }
    }
}

void appendSequence(std::string& out, const SequenceSnapshot& s, const Rational& from, const Rational& to, int depth) {
    const auto& seq = s.sequence;
    out += std::to_string(seq.settings.width) + 'x' + std::to_string(seq.settings.height) + '@' +
           std::to_string(seq.settings.frameRate.num()) + '/' + std::to_string(seq.settings.frameRate.den());
    for (std::size_t i = 0; i < seq.videoTracks.size(); ++i) {
        const auto& track = seq.videoTracks[i];
        out += "|V" + std::to_string(i) + (track.state.hidden ? "h" : "");
        if (track.state.hidden) {
            continue;
        }
        for (const auto& t : track.transitions) {
            out += "T" + t.type + t.from.toString() + t.to.toString() + std::to_string(t.duration.num()) + '/' +
                   std::to_string(t.duration.den()) + std::to_string(static_cast<int>(t.alignment));
        }
        for (const auto& clip : track.clips) {
            if (clip.end() <= from || clip.start >= to) {
                continue;
            }
            out += model::canonicalClip(clip);
            appendMedia(out, s, clip.media, depth);
        }
    }
}

} // namespace

Rational previewSegmentLength() {
    return Rational(kPreviewSegmentSeconds);
}

std::size_t previewSegmentCount(const SequenceSnapshot& snapshot) {
    const auto duration = snapshot.sequence.duration();
    if (!duration.isPositive()) {
        return 0;
    }
    return static_cast<std::size_t>((duration / previewSegmentLength()).ceil());
}

std::string previewSegmentKey(const SequenceSnapshot& snapshot, std::size_t index) {
    const Rational length = previewSegmentLength();
    const Rational start = length * Rational(static_cast<std::int64_t>(index));
    const Rational end = start + length;
    bool any = false;
    for (const auto& track : snapshot.sequence.videoTracks) {
        for (const auto& clip : track.clips) {
            any = any || (!track.state.hidden && clip.start < end && clip.end() > start);
        }
    }
    if (!any) {
        return {};
    }
    // Transition handles reach a little past the clips' edges: include clips near the segment.
    std::string text = "loki-preview-v1|" + std::to_string(index) + '|';
    appendSequence(text, snapshot, start - Rational(2), end + Rational(2), 0);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a(text)));
    return hex;
}

std::filesystem::path previewFile(const std::filesystem::path& cacheDir, const std::string& key) {
    return cacheDir / (key + ".mov");
}

void attachPreviews(SequenceSnapshot& snapshot, const std::filesystem::path& cacheDir) {
    snapshot.previews = {};
    if (cacheDir.empty()) {
        return;
    }
    const auto count = previewSegmentCount(snapshot);
    snapshot.previews.length = previewSegmentLength();
    snapshot.previews.files.resize(count);
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto key = previewSegmentKey(snapshot, i);
        if (key.empty()) {
            continue;
        }
        const auto file = previewFile(cacheDir, key);
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            snapshot.previews.files[i] = file;
            any = true;
        }
    }
    if (!any) {
        snapshot.previews = {};
    }
}

std::vector<PreviewState> previewStates(const SequenceSnapshot& snapshot, const std::filesystem::path& cacheDir) {
    std::vector<PreviewState> states(previewSegmentCount(snapshot), PreviewState::Empty);
    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto key = previewSegmentKey(snapshot, i);
        if (key.empty()) {
            continue;
        }
        std::error_code ec;
        states[i] = !cacheDir.empty() && std::filesystem::exists(previewFile(cacheDir, key), ec) ? PreviewState::Rendered
                                                                                                 : PreviewState::NotRendered;
    }
    return states;
}

} // namespace loki::playback
