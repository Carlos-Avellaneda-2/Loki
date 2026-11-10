#include "model/project_json.h"

#include "util/atomic_file.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <system_error>

namespace loki::model {

using nlohmann::json;

namespace {

// ---------------------------------------------------------------------------------------------
// Migrations: kMigrations[i] upgrades a document from version i+1 to i+2.
using Migration = void (*)(json&);

// v1 -> v2: only additions (transitions, generators, string parameters); v1 documents are valid.
void migrate1to2(json&) {}
// v2 -> v3: only additions (speed, reverse, markers, nested media); v2 documents are valid.
void migrate2to3(json&) {}
// v3 -> v4: only additions (effect masks, curves, analysis samples); v3 documents are valid.
void migrate3to4(json&) {}

constexpr std::array<Migration, kProjectFormatVersion - 1> kMigrations{&migrate1to2, &migrate2to3, &migrate3to4};

// ---------------------------------------------------------------------------------------------
// Writing

json rationalToJson(const Rational& r) {
    return json::array({r.num(), r.den()});
}

template <typename Tag>
json idToJson(const Id<Tag>& id) {
    return id.isNull() ? json(nullptr) : json(id.toString());
}

const char* interpolationName(Interpolation i) {
    switch (i) {
    case Interpolation::Hold: return "hold";
    case Interpolation::Linear: return "linear";
    case Interpolation::Bezier: return "bezier";
    }
    return "linear";
}

json parameterToJson(const Parameter& p) {
    json keys = json::array();
    for (const auto& k : p.keyframes) {
        keys.push_back({{"time", rationalToJson(k.time)},
                        {"value", k.value},
                        {"interpolation", interpolationName(k.interpolation)},
                        {"easeIn", k.easeIn},
                        {"easeOut", k.easeOut}});
    }
    return {{"key", p.key}, {"value", p.value}, {"keyframes", std::move(keys)}};
}

json pathToJson(const std::vector<MaskVertex>& path) {
    json list = json::array();
    for (const auto& v : path) {
        list.push_back(json::array({v.x, v.y, v.inX, v.inY, v.outX, v.outY}));
    }
    return list;
}

json maskToJson(const Mask& m) {
    json keys = json::array();
    for (const auto& k : m.keyframes) {
        keys.push_back({{"time", rationalToJson(k.time)}, {"path", pathToJson(k.path)}});
    }
    return {{"id", idToJson(m.id)},
            {"name", m.name},
            {"mode", m.mode == MaskMode::Subtract ? "subtract" : "add"},
            {"inverted", m.inverted},
            {"path", pathToJson(m.path)},
            {"pathKeys", std::move(keys)},
            {"parameters", json::array({parameterToJson(m.feather), parameterToJson(m.opacity),
                                        parameterToJson(m.expansion)})}};
}

json effectToJson(const Effect& effect) {
    json params = json::array();
    for (const auto& p : effect.parameters) {
        params.push_back(parameterToJson(p));
    }
    json j = {{"id", idToJson(effect.id)},
              {"type", effect.type},
              {"enabled", effect.enabled},
              {"parameters", std::move(params)}};
    if (!effect.strings.empty()) {
        j["strings"] = effect.strings;
    }
    // v4 (D33-D36): omitted when empty.
    if (!effect.masks.empty()) {
        json masks = json::array();
        for (const auto& m : effect.masks) {
            masks.push_back(maskToJson(m));
        }
        j["masks"] = std::move(masks);
    }
    if (!effect.curves.empty()) {
        json curves = json::object();
        for (const auto& [name, points] : effect.curves) {
            json list = json::array();
            for (const auto& p : points) {
                list.push_back(json::array({p.x, p.y}));
            }
            curves[name] = std::move(list);
        }
        j["curves"] = std::move(curves);
    }
    if (!effect.samples.empty()) {
        j["samples"] = effect.samples;
    }
    return j;
}

json markersToJson(const std::vector<Marker>& markers) {
    json list = json::array();
    for (const auto& m : markers) {
        json j = {{"id", idToJson(m.id)}, {"time", rationalToJson(m.time)}, {"color", static_cast<int>(m.color)}};
        if (m.duration.isPositive()) {
            j["duration"] = rationalToJson(m.duration);
        }
        if (!m.name.empty()) {
            j["name"] = m.name;
        }
        if (!m.comment.empty()) {
            j["comment"] = m.comment;
        }
        list.push_back(std::move(j));
    }
    return list;
}

const char* alignmentName(TransitionAlignment a) {
    switch (a) {
    case TransitionAlignment::Center: return "center";
    case TransitionAlignment::StartAtCut: return "start";
    case TransitionAlignment::EndAtCut: return "end";
    }
    return "center";
}

json clipToJson(const Clip& clip) {
    json effects = json::array();
    for (const auto& e : clip.effects) {
        effects.push_back(effectToJson(e));
    }
    json j = {{"id", idToJson(clip.id)},
              {"media", idToJson(clip.media)},
              {"name", clip.name},
              {"start", rationalToJson(clip.start)},
              {"sourceIn", rationalToJson(clip.sourceIn)},
              {"duration", rationalToJson(clip.duration)},
              {"enabled", clip.enabled},
              {"link", idToJson(clip.link)},
              {"effects", std::move(effects)}};
    // Optional (D17): omitted when there is no fade.
    if (clip.fadeIn.isPositive()) {
        j["fadeIn"] = rationalToJson(clip.fadeIn);
    }
    if (clip.fadeOut.isPositive()) {
        j["fadeOut"] = rationalToJson(clip.fadeOut);
    }
    // v3 (D28): omitted at 100 % forward.
    if (clip.speed != Rational(1)) {
        j["speed"] = rationalToJson(clip.speed);
    }
    if (clip.reverse) {
        j["reverse"] = true;
    }
    return j;
}

json trackToJson(const Track& track) {
    json clips = json::array();
    for (const auto& c : track.clips) {
        clips.push_back(clipToJson(c));
    }
    json j = {{"id", idToJson(track.id)},
              {"name", track.state.name},
              {"locked", track.state.locked},
              {"hidden", track.state.hidden},
              {"muted", track.state.muted},
              {"clips", std::move(clips)}};
    if (!track.transitions.empty()) {
        json transitions = json::array();
        for (const auto& t : track.transitions) {
            transitions.push_back({{"id", idToJson(t.id)},
                                   {"type", t.type},
                                   {"from", idToJson(t.from)},
                                   {"to", idToJson(t.to)},
                                   {"duration", rationalToJson(t.duration)},
                                   {"alignment", alignmentName(t.alignment)}});
        }
        j["transitions"] = std::move(transitions);
    }
    // Optional mixer fields (D16): omitted at their defaults.
    if (track.state.volumeDb != 0.0) {
        j["volumeDb"] = track.state.volumeDb;
    }
    if (track.state.pan != 0.0) {
        j["pan"] = track.state.pan;
    }
    if (track.state.solo) {
        j["solo"] = true;
    }
    return j;
}

json mediaToJson(const MediaItem& m, const std::filesystem::path& projectDir) {
    json j = {{"id", idToJson(m.id)},
              {"name", m.name},
              {"path", pathToUtf8(m.path)},
              {"bin", idToJson(m.bin)},
              {"info",
               {{"hasVideo", m.info.hasVideo},
                {"hasAudio", m.info.hasAudio},
                {"width", m.info.width},
                {"height", m.info.height},
                {"frameRate", rationalToJson(m.info.frameRate)},
                {"duration", rationalToJson(m.info.duration)},
                {"sampleRate", m.info.sampleRate},
                {"audioChannels", m.info.audioChannels},
                {"videoCodec", m.info.videoCodec},
                {"audioCodec", m.info.audioCodec}}}};
    if (!projectDir.empty() && m.path.is_absolute()) {
        std::error_code ec;
        const auto rel = std::filesystem::relative(m.path, projectDir, ec);
        if (!ec && !rel.empty()) {
            j["relativePath"] = pathToUtf8(rel);
        }
    }
    if (m.proxyPath) {
        j["proxyPath"] = pathToUtf8(*m.proxyPath);
    }
    if (m.isGenerated()) {
        j["generator"] = m.generator;
    }
    if (m.info.isStill) {
        j["info"]["isStill"] = true;
    }
    if (m.markIn) {
        j["markIn"] = rationalToJson(*m.markIn);
    }
    if (m.markOut) {
        j["markOut"] = rationalToJson(*m.markOut);
    }
    if (m.isNest()) {
        j["sequence"] = idToJson(m.sequence);
    }
    if (!m.markers.empty()) {
        j["markers"] = markersToJson(m.markers);
    }
    return j;
}

// ---------------------------------------------------------------------------------------------
// Reading. Every accessor reports the JSON pointer of the offending value.

class Reader {
public:
    explicit Reader(std::filesystem::path projectDir) : m_projectDir(std::move(projectDir)) {}

    Project read(const json& root) {
        ProjectBuilder builder;
        for (std::size_t i = 0; i < array(root, "bins", "").size(); ++i) {
            builder.addBin(readBin(root.at("bins")[i], "/bins/" + std::to_string(i)));
        }
        for (std::size_t i = 0; i < array(root, "media", "").size(); ++i) {
            builder.addMedia(readMedia(root.at("media")[i], "/media/" + std::to_string(i)));
        }
        for (std::size_t i = 0; i < array(root, "sequences", "").size(); ++i) {
            builder.addSequence(readSequence(root.at("sequences")[i], "/sequences/" + std::to_string(i)));
        }
        return builder.build();
    }

private:
    [[noreturn]] static void fail(const std::string& where, const std::string& what) {
        throw ProjectFormatError("Invalid project at '" + (where.empty() ? std::string("/") : where) + "': " + what);
    }

    static const json& field(const json& obj, const char* key, const std::string& where) {
        if (!obj.is_object()) {
            fail(where, "expected an object");
        }
        const auto it = obj.find(key);
        if (it == obj.end()) {
            fail(where + "/" + key, "missing");
        }
        return *it;
    }

    static const json::array_t& array(const json& obj, const char* key, const std::string& where) {
        const auto& v = field(obj, key, where);
        if (!v.is_array()) {
            fail(where + "/" + key, "expected an array");
        }
        return v.get_ref<const json::array_t&>();
    }

    static std::string string(const json& obj, const char* key, const std::string& where) {
        const auto& v = field(obj, key, where);
        if (!v.is_string()) {
            fail(where + "/" + key, "expected a string");
        }
        return v.get<std::string>();
    }

    static bool boolean(const json& obj, const char* key, const std::string& where) {
        const auto& v = field(obj, key, where);
        if (!v.is_boolean()) {
            fail(where + "/" + key, "expected a boolean");
        }
        return v.get<bool>();
    }

    static int integer(const json& obj, const char* key, const std::string& where) {
        const auto& v = field(obj, key, where);
        if (!v.is_number_integer()) {
            fail(where + "/" + key, "expected an integer");
        }
        const auto value = v.get<std::int64_t>();
        if (value < INT32_MIN || value > INT32_MAX) {
            fail(where + "/" + key, "integer out of range");
        }
        return static_cast<int>(value);
    }

    static double number(const json& obj, const char* key, const std::string& where) {
        const auto& v = field(obj, key, where);
        if (!v.is_number()) {
            fail(where + "/" + key, "expected a number");
        }
        const double d = v.get<double>();
        if (!std::isfinite(d)) {
            fail(where + "/" + key, "number is not finite");
        }
        return d;
    }

    static Rational rational(const json& obj, const char* key, const std::string& where) {
        const auto& v = field(obj, key, where);
        const std::string at = where + "/" + key;
        if (!v.is_array() || v.size() != 2 || !v[0].is_number_integer() || !v[1].is_number_integer()) {
            fail(at, "expected [numerator, denominator] integers");
        }
        try {
            return Rational(v[0].get<std::int64_t>(), v[1].get<std::int64_t>());
        } catch (const std::exception& e) {
            fail(at, e.what());
        }
    }

    template <typename IdT>
    static IdT id(const json& obj, const char* key, const std::string& where, bool nullable) {
        const auto& v = field(obj, key, where);
        if (v.is_null()) {
            if (!nullable) {
                fail(where + "/" + key, "id must not be null");
            }
            return IdT();
        }
        if (!v.is_string()) {
            fail(where + "/" + key, "expected a UUID string");
        }
        auto parsed = IdT::parse(v.get_ref<const std::string&>());
        if (!parsed || parsed->isNull()) {
            fail(where + "/" + key, "invalid UUID");
        }
        return *parsed;
    }

    static Interpolation interpolation(const json& obj, const std::string& where) {
        const auto name = string(obj, "interpolation", where);
        if (name == "hold") {
            return Interpolation::Hold;
        }
        if (name == "linear") {
            return Interpolation::Linear;
        }
        if (name == "bezier") {
            return Interpolation::Bezier;
        }
        fail(where + "/interpolation", "unknown interpolation '" + name + "'");
    }

    static Bin readBin(const json& j, const std::string& at) {
        Bin b;
        b.id = id<BinId>(j, "id", at, false);
        b.name = string(j, "name", at);
        b.parent = id<BinId>(j, "parent", at, true);
        return b;
    }

    MediaItem readMedia(const json& j, const std::string& at) const {
        MediaItem m;
        m.id = id<MediaId>(j, "id", at, false);
        m.name = string(j, "name", at);
        m.path = pathFromUtf8(string(j, "path", at));
        m.bin = id<BinId>(j, "bin", at, true);
        if (j.contains("relativePath") && !m_projectDir.empty()) {
            const auto candidate = (m_projectDir / pathFromUtf8(string(j, "relativePath", at))).lexically_normal();
            std::error_code ec;
            if (!std::filesystem::exists(m.path, ec) && std::filesystem::exists(candidate, ec)) {
                m.path = candidate; // project folder moved together with its media: relink
            }
        }
        if (j.contains("proxyPath")) {
            m.proxyPath = pathFromUtf8(string(j, "proxyPath", at));
        }
        if (j.contains("generator")) {
            m.generator = string(j, "generator", at);
        }
        const auto& info = field(j, "info", at);
        const std::string infoAt = at + "/info";
        m.info.hasVideo = boolean(info, "hasVideo", infoAt);
        m.info.hasAudio = boolean(info, "hasAudio", infoAt);
        m.info.width = integer(info, "width", infoAt);
        m.info.height = integer(info, "height", infoAt);
        m.info.frameRate = rational(info, "frameRate", infoAt);
        m.info.duration = rational(info, "duration", infoAt);
        m.info.sampleRate = integer(info, "sampleRate", infoAt);
        m.info.audioChannels = integer(info, "audioChannels", infoAt);
        m.info.videoCodec = string(info, "videoCodec", infoAt);
        m.info.audioCodec = string(info, "audioCodec", infoAt);
        // Optional fields (added within v1; absent in older files).
        if (info.contains("isStill")) {
            m.info.isStill = boolean(info, "isStill", infoAt);
        }
        if (j.contains("markIn")) {
            m.markIn = rational(j, "markIn", at);
        }
        if (j.contains("markOut")) {
            m.markOut = rational(j, "markOut", at);
        }
        if (j.contains("sequence")) {
            m.sequence = id<SequenceId>(j, "sequence", at, true);
        }
        m.markers = readMarkers(j, at);
        return m;
    }

    static std::vector<Marker> readMarkers(const json& j, const std::string& at) {
        std::vector<Marker> markers;
        if (!j.contains("markers")) {
            return markers;
        }
        const auto& list = array(j, "markers", at);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string mAt = at + "/markers/" + std::to_string(i);
            Marker m;
            m.id = id<MarkerId>(list[i], "id", mAt, false);
            m.time = rational(list[i], "time", mAt);
            const int color = integer(list[i], "color", mAt);
            if (color < 0 || color >= kMarkerColorCount) {
                fail(mAt + "/color", "unknown marker color");
            }
            m.color = static_cast<MarkerColor>(color);
            if (list[i].contains("duration")) {
                m.duration = rational(list[i], "duration", mAt);
            }
            if (list[i].contains("name")) {
                m.name = string(list[i], "name", mAt);
            }
            if (list[i].contains("comment")) {
                m.comment = string(list[i], "comment", mAt);
            }
            markers.push_back(std::move(m));
        }
        return markers;
    }

    static Parameter readParameter(const json& j, const std::string& pAt) {
        Parameter p;
        p.key = string(j, "key", pAt);
        p.value = number(j, "value", pAt);
        const auto& keys = array(j, "keyframes", pAt);
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const std::string kAt = pAt + "/keyframes/" + std::to_string(k);
            Keyframe kf;
            kf.time = rational(keys[k], "time", kAt);
            kf.value = number(keys[k], "value", kAt);
            kf.interpolation = interpolation(keys[k], kAt);
            kf.easeIn = number(keys[k], "easeIn", kAt);
            kf.easeOut = number(keys[k], "easeOut", kAt);
            p.keyframes.push_back(kf);
        }
        return p;
    }

    static std::vector<MaskVertex> readPath(const json& obj, const char* key, const std::string& at) {
        std::vector<MaskVertex> path;
        const auto& list = array(obj, key, at);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& v = list[i];
            const std::string vAt = at + "/" + key + "/" + std::to_string(i);
            if (!v.is_array() || v.size() != 6 || !std::all_of(v.begin(), v.end(), [](const json& n) { return n.is_number(); })) {
                fail(vAt, "expected [x, y, inX, inY, outX, outY]");
            }
            path.push_back({v[0].get<double>(), v[1].get<double>(), v[2].get<double>(), v[3].get<double>(),
                            v[4].get<double>(), v[5].get<double>()});
        }
        return path;
    }

    static Mask readMask(const json& j, const std::string& at) {
        Mask m;
        m.id = id<MaskId>(j, "id", at, false);
        m.name = string(j, "name", at);
        const auto mode = string(j, "mode", at);
        if (mode != "add" && mode != "subtract") {
            fail(at + "/mode", "unknown mask mode '" + mode + "'");
        }
        m.mode = mode == "subtract" ? MaskMode::Subtract : MaskMode::Add;
        m.inverted = boolean(j, "inverted", at);
        m.path = readPath(j, "path", at);
        const auto& keys = array(j, "pathKeys", at);
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const std::string kAt = at + "/pathKeys/" + std::to_string(k);
            m.keyframes.push_back({rational(keys[k], "time", kAt), readPath(keys[k], "path", kAt)});
        }
        const auto& params = array(j, "parameters", at);
        for (std::size_t i = 0; i < params.size(); ++i) {
            auto p = readParameter(params[i], at + "/parameters/" + std::to_string(i));
            if (p.key == "feather") {
                m.feather = std::move(p);
            } else if (p.key == "opacity") {
                m.opacity = std::move(p);
            } else if (p.key == "expansion") {
                m.expansion = std::move(p);
            }
        }
        return m;
    }

    static Effect readEffect(const json& j, const std::string& at) {
        Effect e;
        e.id = id<EffectId>(j, "id", at, false);
        e.type = string(j, "type", at);
        e.enabled = boolean(j, "enabled", at);
        const auto& params = array(j, "parameters", at);
        for (std::size_t i = 0; i < params.size(); ++i) {
            e.parameters.push_back(readParameter(params[i], at + "/parameters/" + std::to_string(i)));
        }
        if (j.contains("masks")) {
            const auto& masks = array(j, "masks", at);
            for (std::size_t i = 0; i < masks.size(); ++i) {
                e.masks.push_back(readMask(masks[i], at + "/masks/" + std::to_string(i)));
            }
        }
        if (j.contains("curves")) {
            const auto& curves = field(j, "curves", at);
            if (!curves.is_object()) {
                fail(at + "/curves", "expected an object");
            }
            for (const auto& [name, list] : curves.items()) {
                const std::string cAt = at + "/curves/" + name;
                if (!list.is_array()) {
                    fail(cAt, "expected an array of [x, y]");
                }
                std::vector<CurvePoint> points;
                for (const auto& pt : list) {
                    if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number()) {
                        fail(cAt, "expected [x, y] points");
                    }
                    points.push_back({pt[0].get<double>(), pt[1].get<double>()});
                }
                e.curves[name] = std::move(points);
            }
        }
        if (j.contains("samples")) {
            const auto& samples = array(j, "samples", at);
            for (const auto& v : samples) {
                if (!v.is_number()) {
                    fail(at + "/samples", "expected numbers");
                }
                e.samples.push_back(v.get<double>());
            }
        }
        if (j.contains("strings")) {
            const auto& strings = field(j, "strings", at);
            if (!strings.is_object()) {
                fail(at + "/strings", "expected an object");
            }
            for (const auto& [key, value] : strings.items()) {
                e.strings[key] = string(strings, key.c_str(), at + "/strings");
            }
        }
        return e;
    }

    static TransitionAlignment readAlignment(const json& j, const std::string& at) {
        const auto name = string(j, "alignment", at);
        if (name == "center") {
            return TransitionAlignment::Center;
        }
        if (name == "start") {
            return TransitionAlignment::StartAtCut;
        }
        if (name == "end") {
            return TransitionAlignment::EndAtCut;
        }
        fail(at + "/alignment", "unknown value " + name);
    }

    static Clip readClip(const json& j, const std::string& at) {
        Clip c;
        c.id = id<ClipId>(j, "id", at, false);
        c.media = id<MediaId>(j, "media", at, false);
        c.name = string(j, "name", at);
        c.start = rational(j, "start", at);
        c.sourceIn = rational(j, "sourceIn", at);
        c.duration = rational(j, "duration", at);
        c.enabled = boolean(j, "enabled", at);
        c.link = id<LinkId>(j, "link", at, true);
        if (j.contains("fadeIn")) {
            c.fadeIn = rational(j, "fadeIn", at);
        }
        if (j.contains("fadeOut")) {
            c.fadeOut = rational(j, "fadeOut", at);
        }
        if (j.contains("speed")) {
            c.speed = rational(j, "speed", at);
        }
        if (j.contains("reverse")) {
            c.reverse = boolean(j, "reverse", at);
        }
        const auto& effects = array(j, "effects", at);
        for (std::size_t i = 0; i < effects.size(); ++i) {
            c.effects.push_back(readEffect(effects[i], at + "/effects/" + std::to_string(i)));
        }
        return c;
    }

    static Track readTrack(const json& j, TrackKind kind, const std::string& at) {
        Track t;
        t.id = id<TrackId>(j, "id", at, false);
        t.kind = kind;
        t.state.name = string(j, "name", at);
        t.state.locked = boolean(j, "locked", at);
        t.state.hidden = boolean(j, "hidden", at);
        t.state.muted = boolean(j, "muted", at);
        if (j.contains("transitions")) {
            const auto& list = array(j, "transitions", at);
            for (std::size_t i = 0; i < list.size(); ++i) {
                const std::string tAt = at + "/transitions/" + std::to_string(i);
                Transition tr;
                tr.id = id<TransitionId>(list[i], "id", tAt, false);
                tr.type = string(list[i], "type", tAt);
                tr.from = id<ClipId>(list[i], "from", tAt, true);
                tr.to = id<ClipId>(list[i], "to", tAt, true);
                tr.duration = rational(list[i], "duration", tAt);
                tr.alignment = readAlignment(list[i], tAt);
                t.transitions.push_back(std::move(tr));
            }
        }
        if (j.contains("volumeDb")) {
            t.state.volumeDb = number(j, "volumeDb", at);
        }
        if (j.contains("pan")) {
            t.state.pan = number(j, "pan", at);
        }
        if (j.contains("solo")) {
            t.state.solo = boolean(j, "solo", at);
        }
        const auto& clips = array(j, "clips", at);
        for (std::size_t i = 0; i < clips.size(); ++i) {
            t.clips.push_back(readClip(clips[i], at + "/clips/" + std::to_string(i)));
        }
        return t;
    }

    static Sequence readSequence(const json& j, const std::string& at) {
        Sequence s;
        s.id = id<SequenceId>(j, "id", at, false);
        s.name = string(j, "name", at);
        const auto& settings = field(j, "settings", at);
        const std::string sAt = at + "/settings";
        s.settings.width = integer(settings, "width", sAt);
        s.settings.height = integer(settings, "height", sAt);
        s.settings.frameRate = rational(settings, "frameRate", sAt);
        s.settings.sampleRate = integer(settings, "sampleRate", sAt);
        const auto& video = array(j, "videoTracks", at);
        for (std::size_t i = 0; i < video.size(); ++i) {
            s.videoTracks.push_back(readTrack(video[i], TrackKind::Video, at + "/videoTracks/" + std::to_string(i)));
        }
        const auto& audio = array(j, "audioTracks", at);
        for (std::size_t i = 0; i < audio.size(); ++i) {
            s.audioTracks.push_back(readTrack(audio[i], TrackKind::Audio, at + "/audioTracks/" + std::to_string(i)));
        }
        if (j.contains("markIn")) {
            s.markIn = rational(j, "markIn", at);
        }
        if (j.contains("markOut")) {
            s.markOut = rational(j, "markOut", at);
        }
        if (j.contains("masterVolumeDb")) {
            s.masterVolumeDb = number(j, "masterVolumeDb", at);
        }
        s.markers = readMarkers(j, at);
        return s;
    }

    std::filesystem::path m_projectDir;
};

std::filesystem::path projectDirOf(const std::filesystem::path& projectFile) {
    if (projectFile.empty()) {
        return {};
    }
    return std::filesystem::absolute(projectFile).parent_path();
}

} // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto u8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()))
        .make_preferred();
}

json toJson(const Project& project, const std::filesystem::path& projectFile) {
    const auto projectDir = projectDirOf(projectFile);
    json bins = json::array();
    for (const auto& b : project.bins()) {
        bins.push_back({{"id", idToJson(b.id)}, {"name", b.name}, {"parent", idToJson(b.parent)}});
    }
    json media = json::array();
    for (const auto& m : project.media()) {
        media.push_back(mediaToJson(m, projectDir));
    }
    json sequences = json::array();
    for (const auto& s : project.sequences()) {
        json video = json::array();
        for (const auto& t : s.videoTracks) {
            video.push_back(trackToJson(t));
        }
        json audio = json::array();
        for (const auto& t : s.audioTracks) {
            audio.push_back(trackToJson(t));
        }
        json seqJson = {{"id", idToJson(s.id)},
                             {"name", s.name},
                             {"settings",
                              {{"width", s.settings.width},
                               {"height", s.settings.height},
                               {"frameRate", rationalToJson(s.settings.frameRate)},
                               {"sampleRate", s.settings.sampleRate}}},
                             {"videoTracks", std::move(video)},
                             {"audioTracks", std::move(audio)}};
        if (s.markIn) {
            seqJson["markIn"] = rationalToJson(*s.markIn);
        }
        if (s.markOut) {
            seqJson["markOut"] = rationalToJson(*s.markOut);
        }
        if (s.masterVolumeDb != 0.0) {
            seqJson["masterVolumeDb"] = s.masterVolumeDb;
        }
        if (!s.markers.empty()) {
            seqJson["markers"] = markersToJson(s.markers);
        }
        sequences.push_back(std::move(seqJson));
    }
    return {{"format", kProjectFormatName},
            {"version", kProjectFormatVersion},
            {"bins", std::move(bins)},
            {"media", std::move(media)},
            {"sequences", std::move(sequences)}};
}

int migrateProjectJson(json& root) {
    if (!root.is_object()) {
        throw ProjectFormatError("Invalid project: root is not a JSON object");
    }
    const auto format = root.find("format");
    if (format == root.end() || !format->is_string() || format->get_ref<const std::string&>() != kProjectFormatName) {
        throw ProjectFormatError("Not a Loki project (missing or wrong 'format')");
    }
    const auto version = root.find("version");
    if (version == root.end() || !version->is_number_integer()) {
        throw ProjectFormatError("Invalid project: missing integer 'version'");
    }
    const auto original = version->get<std::int64_t>();
    if (original < 1) {
        throw ProjectFormatError("Invalid project version " + std::to_string(original));
    }
    if (original > kProjectFormatVersion) {
        throw ProjectFormatError("Project was saved by a newer Loki (format version " + std::to_string(original) +
                                 ", this build supports up to " + std::to_string(kProjectFormatVersion) + ")");
    }
    for (auto v = original; v < kProjectFormatVersion; ++v) {
        kMigrations[static_cast<std::size_t>(v - 1)](root);
        root["version"] = v + 1;
    }
    return static_cast<int>(original);
}

Project fromJson(const json& input, const std::filesystem::path& projectFile) {
    json root = input;
    migrateProjectJson(root);
    try {
        return Reader(projectDirOf(projectFile)).read(root);
    } catch (const ModelError& e) {
        throw ProjectFormatError(std::string("Invalid project: ") + e.what());
    } catch (const json::exception& e) {
        throw ProjectFormatError(std::string("Invalid project: ") + e.what());
    }
}

std::string serializeProject(const Project& project, const std::filesystem::path& projectFile) {
    return toJson(project, projectFile).dump(2) + "\n";
}

Project parseProject(std::string_view text, const std::filesystem::path& projectFile) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error& e) {
        throw ProjectFormatError(std::string("Project is not valid JSON: ") + e.what());
    }
    return fromJson(root, projectFile);
}

void saveProjectFile(const Project& project, const std::filesystem::path& file) {
    util::writeFileAtomic(file, serializeProject(project, file));
}

Project loadProjectFile(const std::filesystem::path& file) {
    return parseProject(util::readFile(file), file);
}

} // namespace loki::model
