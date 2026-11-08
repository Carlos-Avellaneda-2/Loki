#include "media/media_cache.h"

#include "model/project_json.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <system_error>

namespace loki::media {

namespace {

// FNV-1a 64-bit: stable across runs and platforms (std::hash is not).
std::uint64_t fnv1a(std::string_view data) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : data) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

} // namespace

MediaCache::MediaCache(std::filesystem::path root) : m_root(std::move(root)) {
    std::error_code ec;
    std::filesystem::create_directories(m_root, ec);
}

std::string MediaCache::sourceKey(const std::filesystem::path& source) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(source, ec);
    if (ec) {
        return {};
    }
    const auto mtime = std::filesystem::last_write_time(source, ec);
    if (ec) {
        return {};
    }
    const auto ticks = mtime.time_since_epoch().count();
    const std::string identity = model::pathToUtf8(std::filesystem::absolute(source, ec)) + "|" +
                                 std::to_string(size) + "|" + std::to_string(ticks);
    std::array<char, 17> hex{};
    std::snprintf(hex.data(), hex.size(), "%016llx", static_cast<unsigned long long>(fnv1a(identity)));
    return hex.data();
}

std::filesystem::path MediaCache::entryPath(std::string_view kind, const std::string& key,
                                            std::string_view extension) const {
    const auto folder = m_root / std::filesystem::path(std::string(kind));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    return folder / (key + std::string(extension));
}

std::uintmax_t MediaCache::diskUsage() const {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(m_root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
    }
    return total;
}

bool MediaCache::clear() const {
    bool ok = true;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_root, ec)) {
        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        ok = ok && !removeError;
    }
    return ok && !ec;
}

} // namespace loki::media
