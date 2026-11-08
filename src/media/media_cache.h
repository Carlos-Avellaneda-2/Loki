#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace loki::media {

// On-disk cache for derived media data (thumbnails, waveforms, later proxies). Entries are keyed
// by the source file identity (path + size + modification time), so editing or replacing a file
// invalidates its entries automatically.
class MediaCache {
public:
    explicit MediaCache(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }

    // Stable key for the current version of `source` ("" if the file cannot be stat'ed).
    [[nodiscard]] static std::string sourceKey(const std::filesystem::path& source);

    // Path for an entry: <root>/<kind>/<key>.<extension>. Creates the kind folder.
    [[nodiscard]] std::filesystem::path entryPath(std::string_view kind, const std::string& key,
                                                  std::string_view extension) const;

    // Total size of the cache folder in bytes (slow: walks the folder).
    [[nodiscard]] std::uintmax_t diskUsage() const;
    // Deletes everything in the cache folder. Returns false if something could not be removed.
    bool clear() const;

private:
    std::filesystem::path m_root;
};

} // namespace loki::media
