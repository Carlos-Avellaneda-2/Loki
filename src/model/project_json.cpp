#include "model/project_json.h"

#include "util/atomic_file.h"

#include <nlohmann/json.hpp>

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
constexpr std::array<Migration, kProjectFormatVersion - 1> kMigrations{};

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

json effectToJson(const Effect& effect) {
    json params = json::array();
    for (const auto& p : effect.parameters) {
        json keys = json::array();
        for (const auto& k : p.keyframes) {
            keys.push_back({{"time", rationalToJson(k.time)},
                            {"value", k.value},
                            {"interpolation", interpolationName(k.interpolation)},
                            {"easeIn", k.easeIn},
                            {"easeOut", k.easeOut}});
        }
        params.push_back({{"key", p.key}, {"value", p.value}, {"keyframes", std::move(keys)}});
    }
    return {{"id", idToJson(effect.id)},
            {"type", effect.type},
            {"enabled", effect.enabled},
            {"parameters", std::move(params)}};
}

json clipToJson(const Clip& clip) {
    json effects = json::array();
    for (const auto& e : clip.effects) {
        effects.push_back(effectToJson(e));
    }
    return {{"id", idToJson(clip.id)},
            {"media", idToJson(clip.media)},
            {"name", clip.name},
            {"start", rationalToJson(clip.start)},
            {"sourceIn", rationalToJson(clip.sourceIn)},
            {"duration", rationalToJson(clip.duration)},
            {"enabled", clip.enabled},
            {"link", idToJson(clip.link)},
            {"effects", std::move(effects)}};
}

json trackToJson(const Track& track) {
    json clips = json::array();
    for (const auto& c : track.clips) {
        clips.push_back(clipToJson(c));
    }
    return {{"id", idToJson(track.id)},
            {"name", track.state.name},
            {"locked", track.state.locked},
            {"hidden", track.state.hidden},
            {"muted", track.state.muted},
            {"clips", std::move(clips)}};
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
    if (m.info.isStill) {
        j["info"]["isStill"] = true;
    }
    if (m.markIn) {
        j["markIn"] = rationalToJson(*m.markIn);
    }
    if (m.markOut) {
        j["markOut"] = rationalToJson(*m.markOut);
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
        return m;
    }

    static Effect readEffect(const json& j, const std::string& at) {
        Effect e;
        e.id = id<EffectId>(j, "id", at, false);
        e.type = string(j, "type", at);
        e.enabled = boolean(j, "enabled", at);
        const auto& params = array(j, "parameters", at);
        for (std::size_t i = 0; i < params.size(); ++i) {
            const std::string pAt = at + "/parameters/" + std::to_string(i);
            Parameter p;
            p.key = string(params[i], "key", pAt);
            p.value = number(params[i], "value", pAt);
            const auto& keys = array(params[i], "keyframes", pAt);
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
            e.parameters.push_back(std::move(p));
        }
        return e;
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
