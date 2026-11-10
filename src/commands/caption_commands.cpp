#include "commands/caption_commands.h"

#include "commands/project_commands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace loki::commands {

using namespace model;

void SetSequenceCaptionsCommand::redo(Project& project) {
    const auto* seq = project.findSequence(m_id);
    if (seq == nullptr) {
        throw CommandError("Sequence does not exist");
    }
    m_oldCaptions = seq->captions;
    m_oldStyle = seq->captionStyle;
    mutate(project).setSequenceCaptions(m_id, m_captions, m_style);
}

void SetSequenceCaptionsCommand::undo(Project& project) {
    mutate(project).setSequenceCaptions(m_id, m_oldCaptions, m_oldStyle);
}

namespace {

const Sequence& requireSequence(const Project& project, SequenceId id) {
    const auto* seq = project.findSequence(id);
    if (seq == nullptr) {
        throw CommandError("Sequence does not exist");
    }
    return *seq;
}

void sortCaptions(std::vector<Caption>& captions) {
    std::sort(captions.begin(), captions.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
}

bool overlapping(const std::vector<Caption>& captions) {
    for (std::size_t i = 1; i < captions.size(); ++i) {
        if (captions[i - 1].end() > captions[i].start) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<std::vector<Caption>> withCaption(std::vector<Caption> captions, Caption caption) {
    if (caption.id.isNull()) {
        caption.id = CaptionId::generate();
    }
    if (captionAt(captions, caption.start) != nullptr || caption.start.isNegative() || !caption.duration.isPositive()) {
        return std::nullopt;
    }
    for (const auto& c : captions) {
        if (caption.start < c.start && c.start < caption.end()) {
            caption.duration = c.start - caption.start; // stop at the next caption
        }
    }
    captions.push_back(std::move(caption));
    sortCaptions(captions);
    return captions;
}

const Caption* captionAt(const std::vector<Caption>& captions, const Rational& t) noexcept {
    for (const auto& c : captions) {
        if (c.start <= t && t < c.end()) {
            return &c;
        }
    }
    return nullptr;
}

std::unique_ptr<Command> makeAddCaption(const Project& project, SequenceId id, const Rational& start,
                                        const Rational& duration, std::string text, CaptionId* added) {
    const auto& seq = requireSequence(project, id);
    Caption c;
    c.id = CaptionId::generate();
    c.start = start;
    c.duration = duration;
    c.text = std::move(text);
    if (added != nullptr) {
        *added = c.id;
    }
    auto list = withCaption(seq.captions, std::move(c));
    if (!list) {
        throw CommandError("There is already a caption at this time");
    }
    return std::make_unique<SetSequenceCaptionsCommand>(id, std::move(*list), seq.captionStyle, "Add Caption");
}

std::unique_ptr<Command> makeEditCaption(const Project& project, SequenceId id, Caption caption) {
    const auto& seq = requireSequence(project, id);
    auto list = seq.captions;
    const auto it = std::find_if(list.begin(), list.end(), [&](const Caption& c) { return c.id == caption.id; });
    if (it == list.end()) {
        throw CommandError("Caption does not exist");
    }
    if (*it == caption) {
        return nullptr;
    }
    if (caption.start.isNegative() || !caption.duration.isPositive()) {
        throw CommandError("Invalid caption time");
    }
    *it = std::move(caption);
    sortCaptions(list);
    if (overlapping(list)) {
        throw CommandError("Captions cannot overlap");
    }
    return std::make_unique<SetSequenceCaptionsCommand>(id, std::move(list), seq.captionStyle, "Edit Caption");
}

std::unique_ptr<Command> makeRemoveCaption(const Project& project, SequenceId id, CaptionId caption) {
    const auto& seq = requireSequence(project, id);
    auto list = seq.captions;
    const auto removed = std::erase_if(list, [&](const Caption& c) { return c.id == caption; });
    if (removed == 0) {
        return nullptr;
    }
    return std::make_unique<SetSequenceCaptionsCommand>(id, std::move(list), seq.captionStyle, "Delete Caption");
}

std::unique_ptr<Command> makeSetCaptionStyle(const Project& project, SequenceId id, CaptionStyle style) {
    const auto& seq = requireSequence(project, id);
    if (style == seq.captionStyle) {
        return nullptr;
    }
    return std::make_unique<SetSequenceCaptionsCommand>(id, seq.captions, std::move(style), "Caption Style");
}

std::unique_ptr<Command> makeReplaceCaptions(const Project& project, SequenceId id, std::vector<Caption> captions) {
    const auto& seq = requireSequence(project, id);
    sortCaptions(captions);
    for (std::size_t i = 1; i < captions.size(); ++i) {
        if (captions[i - 1].end() > captions[i].start) {
            captions[i - 1].duration = captions[i].start - captions[i - 1].start;
        }
    }
    std::erase_if(captions, [](const Caption& c) { return !c.duration.isPositive(); });
    return std::make_unique<SetSequenceCaptionsCommand>(id, std::move(captions), seq.captionStyle, "Import Captions");
}

// --- SRT ------------------------------------------------------------------------------------------------

namespace {

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// "HH:MM:SS,mmm" -> milliseconds.
std::optional<std::int64_t> parseTimestamp(const std::string& text) {
    // HH:MM:SS,mmm with numbers of any width (no sscanf: /WX treats it as unsafe).
    std::int64_t fields[4] = {0, 0, 0, 0};
    std::size_t field = 0;
    bool digits = false;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            fields[field] = fields[field] * 10 + (ch - '0');
            digits = true;
        } else if ((ch == ':' && field < 2) || ((ch == ',' || ch == '.') && field == 2)) {
            if (!digits) {
                return std::nullopt;
            }
            ++field;
            digits = false;
        } else {
            return std::nullopt;
        }
    }
    if (field != 3 || !digits) {
        return std::nullopt;
    }
    const auto h = fields[0];
    const auto m = fields[1];
    const auto s = fields[2];
    const auto ms = fields[3];
    if (m > 59 || s > 59 || ms > 999) {
        return std::nullopt;
    }
    return ((static_cast<std::int64_t>(h) * 60 + m) * 60 + s) * 1000 + ms;
}

std::string formatTimestamp(const Rational& t) {
    const auto total = static_cast<std::int64_t>(std::llround(t.toDouble() * 1000.0));
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%02lld:%02lld:%02lld,%03lld", static_cast<long long>(total / 3600000),
                  static_cast<long long>(total / 60000 % 60), static_cast<long long>(total / 1000 % 60),
                  static_cast<long long>(total % 1000));
    return buffer;
}

} // namespace

std::vector<Caption> parseSrt(std::string_view text) {
    std::vector<Caption> out;
    std::istringstream in{std::string(text)};
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        // Optional numeric index line, then the timing line.
        if (line.find("-->") == std::string::npos) {
            if (!std::getline(in, line)) {
                break;
            }
            ++lineNo;
            line = trim(line);
        }
        const auto arrow = line.find("-->");
        if (arrow == std::string::npos) {
            throw CommandError("SRT line " + std::to_string(lineNo) + ": expected a timing like 00:00:01,000 --> 00:00:02,000");
        }
        const auto from = parseTimestamp(trim(line.substr(0, arrow)));
        auto toText = trim(line.substr(arrow + 3));
        if (const auto space = toText.find(' '); space != std::string::npos) {
            toText = toText.substr(0, space); // position hints after the end time are ignored
        }
        const auto to = parseTimestamp(toText);
        if (!from || !to || *to <= *from) {
            throw CommandError("SRT line " + std::to_string(lineNo) + ": invalid timing");
        }
        std::string body;
        while (std::getline(in, line)) {
            ++lineNo;
            const auto t = trim(line);
            if (t.empty()) {
                break;
            }
            body += body.empty() ? t : "\n" + t;
        }
        Caption c;
        c.id = CaptionId::generate();
        c.start = Rational(*from, 1000);
        c.duration = Rational(*to - *from, 1000);
        c.text = std::move(body);
        out.push_back(std::move(c));
    }
    sortCaptions(out);
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i - 1].end() > out[i].start) {
            out[i - 1].duration = out[i].start - out[i - 1].start;
        }
    }
    std::erase_if(out, [](const Caption& c) { return !c.duration.isPositive(); });
    return out;
}

std::string formatSrt(const std::vector<Caption>& captions) {
    std::string out;
    int index = 1;
    for (const auto& c : captions) {
        out += std::to_string(index++) + "\n" + formatTimestamp(c.start) + " --> " + formatTimestamp(c.end()) + "\n" +
               c.text + "\n\n";
    }
    return out;
}

// --- Track automation --------------------------------------------------------------------------------------

namespace {

std::vector<Keyframe>& keysOf(TrackState& state, AutomationKind kind) {
    return kind == AutomationKind::Volume ? state.volumeAutomation : state.panAutomation;
}

double valueOf(const TrackState& state, AutomationKind kind, const Rational& t) {
    return kind == AutomationKind::Volume ? state.volumeAt(t) : state.panAt(t);
}

double clampValue(AutomationKind kind, double v) {
    return kind == AutomationKind::Volume ? std::clamp(v, -96.0, 15.0) : std::clamp(v, -100.0, 100.0);
}

const TrackState& requireAudioTrack(const Project& project, TrackId id) {
    const auto ref = project.findTrack(id);
    if (ref.track == nullptr || ref.track->kind != TrackKind::Audio) {
        throw CommandError("Automation needs an audio track");
    }
    return ref.track->state;
}

const char* label(AutomationKind kind) {
    return kind == AutomationKind::Volume ? "Track Volume Keyframe" : "Track Pan Keyframe";
}

} // namespace

std::unique_ptr<Command> toggleTrackAutomationKey(const Project& project, TrackId track, AutomationKind kind,
                                                  const Rational& t) {
    auto state = requireAudioTrack(project, track);
    auto& keys = keysOf(state, kind);
    const auto it = std::find_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.time == t; });
    if (it != keys.end()) {
        keys.erase(it);
    } else {
        Keyframe k;
        k.time = t;
        k.value = valueOf(requireAudioTrack(project, track), kind, t);
        keys.push_back(k);
        std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
    return std::make_unique<SetTrackStateCommand>(track, std::move(state), label(kind));
}

std::unique_ptr<Command> setTrackAutomationValue(const Project& project, TrackId track, AutomationKind kind,
                                                 const Rational& t, double value, GestureId gesture) {
    auto state = requireAudioTrack(project, track);
    auto& keys = keysOf(state, kind);
    value = clampValue(kind, value);
    const auto it = std::find_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.time == t; });
    if (it != keys.end()) {
        it->value = value;
    } else {
        Keyframe k;
        k.time = t;
        k.value = value;
        keys.push_back(k);
        std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
    return std::make_unique<SetTrackStateCommand>(track, std::move(state), label(kind), gesture);
}

std::unique_ptr<Command> moveTrackAutomationKey(const Project& project, TrackId track, AutomationKind kind,
                                                const Rational& from, const Rational& to, GestureId gesture) {
    auto state = requireAudioTrack(project, track);
    auto& keys = keysOf(state, kind);
    const auto it = std::find_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.time == from; });
    if (it == keys.end() || to.isNegative()) {
        throw CommandError("No keyframe there");
    }
    if (from == to) {
        return nullptr;
    }
    if (std::any_of(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.time == to; })) {
        throw CommandError("There is already a keyframe at that time");
    }
    it->time = to;
    std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    return std::make_unique<SetTrackStateCommand>(track, std::move(state), "Move Track Keyframe", gesture);
}

std::unique_ptr<Command> clearTrackAutomation(const Project& project, TrackId track, AutomationKind kind) {
    auto state = requireAudioTrack(project, track);
    auto& keys = keysOf(state, kind);
    if (keys.empty()) {
        return nullptr;
    }
    keys.clear();
    return std::make_unique<SetTrackStateCommand>(track, std::move(state), "Clear Track Keyframes");
}

} // namespace loki::commands
